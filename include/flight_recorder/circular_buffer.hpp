#pragma once

#include "flight_recorder/flight_record.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>

namespace flight_recorder {

// Single-producer/single-consumer ring. The producer owns tail and the consumer
// owns head. Release publishes a slot/cursor update; acquire observes it before
// reading or reusing that slot. The wait mutex pairs cursor publication with
// condition-variable notification so blocking calls cannot miss a wake-up.
class CircularBuffer {
public:
    enum class PushStatus { Pushed, Timeout, Closed };
    enum class PopStatus { Popped, Timeout, Closed };

    struct Stats {
        std::size_t dropped_records {0};
        std::size_t current_size {0};
        std::size_t high_watermark {0};
    };

    explicit CircularBuffer(std::size_t capacity)
        : capacity_(capacity), storage_(capacity == 0 ? nullptr : std::make_unique<FlightRecord[]>(capacity)) {}

    PushStatus try_push(FlightRecord record) {
        if (closed_.load(std::memory_order_acquire)) {
            return PushStatus::Closed;
        }
        const auto tail = producer_.cursor.load(std::memory_order_relaxed);
        const auto head = consumer_.cursor.load(std::memory_order_acquire);
        if (tail - head >= capacity_) {
            return PushStatus::Timeout;
        }
        storage_[tail % capacity_] = record;
        {
            // Pair cursor publication with the wait mutex so a waiter cannot
            // miss a notification between checking its predicate and sleeping.
            std::lock_guard<std::mutex> lock(wait_mutex_);
            producer_.cursor.store(tail + 1, std::memory_order_release);
        }
        update_high_watermark(tail + 1 - head);
        data_available_.notify_one();
        return PushStatus::Pushed;
    }

    PushStatus push_blocking(FlightRecord record) {
        while (true) {
            const auto status = try_push(record);
            if (status != PushStatus::Timeout) {
                return status;
            }
            std::unique_lock<std::mutex> lock(wait_mutex_);
            space_available_.wait(lock, [this] { return closed() || !full(); });
        }
    }

    template <class Rep, class Period>
    PushStatus push_wait_for(FlightRecord record, const std::chrono::duration<Rep, Period>& timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            const auto status = try_push(record);
            if (status != PushStatus::Timeout) {
                return status;
            }
            std::unique_lock<std::mutex> lock(wait_mutex_);
            if (!space_available_.wait_until(lock, deadline, [this] { return closed() || !full(); })) {
                dropped_records_.fetch_add(1, std::memory_order_relaxed);
                return PushStatus::Timeout;
            }
        }
    }

    PopStatus try_pop_status(FlightRecord& out) {
        const auto head = consumer_.cursor.load(std::memory_order_relaxed);
        const auto tail = producer_.cursor.load(std::memory_order_acquire);
        if (head == tail) {
            return closed_.load(std::memory_order_acquire) ? PopStatus::Closed : PopStatus::Timeout;
        }
        out = storage_[head % capacity_];
        {
            std::lock_guard<std::mutex> lock(wait_mutex_);
            consumer_.cursor.store(head + 1, std::memory_order_release);
        }
        space_available_.notify_one();
        return PopStatus::Popped;
    }

    bool try_pop(FlightRecord& out) {
        return try_pop_status(out) == PopStatus::Popped;
    }

    std::size_t try_pop_batch(FlightRecord* output, std::size_t max_records) {
        if (max_records == 0) {
            return 0;
        }
        const auto head = consumer_.cursor.load(std::memory_order_relaxed);
        const auto tail = producer_.cursor.load(std::memory_order_acquire);
        const auto count = std::min<std::size_t>(max_records, tail - head);
        for (std::size_t index = 0; index < count; ++index) {
            output[index] = storage_[(head + index) % capacity_];
        }
        if (count != 0) {
            {
                std::lock_guard<std::mutex> lock(wait_mutex_);
                consumer_.cursor.store(head + count, std::memory_order_release);
            }
            space_available_.notify_one();
        }
        return count;
    }

    PopStatus pop_blocking(FlightRecord& out) {
        while (true) {
            const auto status = try_pop_status(out);
            if (status != PopStatus::Timeout) {
                return status;
            }
            std::unique_lock<std::mutex> lock(wait_mutex_);
            data_available_.wait(lock, [this] { return closed() || !empty(); });
        }
    }

    template <class Rep, class Period>
    PopStatus pop_wait_for(FlightRecord& out, const std::chrono::duration<Rep, Period>& timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            const auto status = try_pop_status(out);
            if (status != PopStatus::Timeout) {
                return status;
            }
            std::unique_lock<std::mutex> lock(wait_mutex_);
            if (!data_available_.wait_until(lock, deadline, [this] { return closed() || !empty(); })) {
                return PopStatus::Timeout;
            }
        }
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(wait_mutex_);
            closed_.store(true, std::memory_order_release);
        }
        notify_all();
    }

    void reset() {
        consumer_.cursor.store(0, std::memory_order_relaxed);
        producer_.cursor.store(0, std::memory_order_relaxed);
        dropped_records_.store(0, std::memory_order_relaxed);
        high_watermark_.store(0, std::memory_order_relaxed);
        closed_.store(false, std::memory_order_release);
    }

    void notify_all() {
        data_available_.notify_all();
        space_available_.notify_all();
    }

    Stats stats() const {
        const auto head = consumer_.cursor.load(std::memory_order_acquire);
        const auto tail = producer_.cursor.load(std::memory_order_acquire);
        return Stats {
            dropped_records_.load(std::memory_order_relaxed),
            tail - head,
            high_watermark_.load(std::memory_order_relaxed)
        };
    }

    bool closed() const { return closed_.load(std::memory_order_acquire); }

private:
    struct alignas(64) Cursor {
        std::atomic<std::size_t> cursor {0};
    };

    bool empty() const {
        return consumer_.cursor.load(std::memory_order_acquire) ==
               producer_.cursor.load(std::memory_order_acquire);
    }

    bool full() const {
        const auto head = consumer_.cursor.load(std::memory_order_acquire);
        const auto tail = producer_.cursor.load(std::memory_order_acquire);
        return tail - head >= capacity_;
    }

    void update_high_watermark(std::size_t size) {
        auto previous = high_watermark_.load(std::memory_order_relaxed);
        while (previous < size &&
               !high_watermark_.compare_exchange_weak(
                   previous, size, std::memory_order_relaxed, std::memory_order_relaxed)) {}
    }

    const std::size_t capacity_;
    std::unique_ptr<FlightRecord[]> storage_;
    Cursor producer_;
    Cursor consumer_;
    std::atomic<std::size_t> dropped_records_ {0};
    std::atomic<std::size_t> high_watermark_ {0};
    std::atomic<bool> closed_ {false};
    mutable std::mutex wait_mutex_;
    std::condition_variable data_available_;
    std::condition_variable space_available_;
};

}  // namespace flight_recorder
