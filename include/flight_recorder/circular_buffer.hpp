#pragma once

#include "flight_recorder/flight_record.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <chrono>

namespace flight_recorder {

class CircularBuffer {
public:
    enum class PushStatus {
        Pushed,
        Timeout,
        Closed
    };

    enum class PopStatus {
        Popped,
        Timeout,
        Closed
    };

    struct Stats {
        std::size_t dropped_records {0};
        std::size_t current_size {0};
        std::size_t high_watermark {0};
    };

    explicit CircularBuffer(std::size_t capacity) : capacity_(capacity) {}

    PushStatus push_blocking(FlightRecord record) {
        std::unique_lock<std::mutex> lock(mutex_);
        space_available_.wait(lock, [this] {
            return closed_ || queue_.size() < capacity_;
        });
        return push_locked(std::move(record));
    }

    template <class Rep, class Period>
    PushStatus push_wait_for(FlightRecord record, const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!space_available_.wait_for(lock, timeout, [this] {
                return closed_ || queue_.size() < capacity_;
            })) {
            ++dropped_records_;
            return PushStatus::Timeout;
        }

        return push_locked(std::move(record));
    }

    PopStatus pop_blocking(FlightRecord& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        data_available_.wait(lock, [this] {
            return closed_ || !queue_.empty();
        });

        if (queue_.empty()) {
            return PopStatus::Closed;
        }

        return pop_locked(out);
    }

    template <class Rep, class Period>
    PopStatus pop_wait_for(FlightRecord& out, const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!data_available_.wait_for(lock, timeout, [this] {
                return closed_ || !queue_.empty();
            })) {
            return PopStatus::Timeout;
        }

        if (queue_.empty()) {
            return PopStatus::Closed;
        }

        return pop_locked(out);
    }

    bool try_pop(FlightRecord& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }

        pop_locked(out);
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        data_available_.notify_all();
        space_available_.notify_all();
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        dropped_records_ = 0;
        high_watermark_ = 0;
        closed_ = false;
    }

    void notify_all() {
        data_available_.notify_all();
        space_available_.notify_all();
    }

    Stats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return Stats {
            dropped_records_,
            queue_.size(),
            high_watermark_
        };
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    PushStatus push_locked(FlightRecord record) {
        if (closed_) {
            return PushStatus::Closed;
        }

        queue_.push_back(std::move(record));
        if (queue_.size() > high_watermark_) {
            high_watermark_ = queue_.size();
        }
        data_available_.notify_one();
        return PushStatus::Pushed;
    }

    PopStatus pop_locked(FlightRecord& out) {
        out = queue_.front();
        queue_.pop_front();
        space_available_.notify_one();
        return PopStatus::Popped;
    }

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable data_available_;
    std::condition_variable space_available_;
    std::deque<FlightRecord> queue_;
    std::size_t dropped_records_ {0};
    std::size_t high_watermark_ {0};
    bool closed_ {false};
};

}  // namespace flight_recorder
