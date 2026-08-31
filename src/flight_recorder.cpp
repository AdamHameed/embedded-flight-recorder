#include "flight_recorder/flight_recorder.hpp"

#include <algorithm>
#include <chrono>

namespace flight_recorder {

FlightRecorder::FlightRecorder(RecorderConfig config)
    : config_(std::move(config)),
      buffer_(config_.buffer_size),
      simulator_(config_.simulator_seed),
      writer_(config_.output_path,
              config_.fault_config,
              WriterOptions {config_.batch_size, config_.sync_every_batches,
                             config_.preallocation_chunk_bytes,
                             config_.serialization_buffer_alignment}),
      drain_buffer_(config_.batch_size == 0 ? nullptr : std::make_unique<FlightRecord[]>(config_.batch_size)) {
    writer_batch_.reserve(config_.batch_size);
}

FlightRecorder::~FlightRecorder() {
    stop();
}

void FlightRecorder::set_start_sequence(std::uint64_t sequence) {
    start_sequence_ = sequence;
}

bool FlightRecorder::start() {
    if (config_.buffer_size == 0 || config_.sample_rate_hz == 0 || config_.batch_size == 0 ||
        config_.sync_every_batches == 0 || config_.output_path.empty()) {
        return false;
    }

    if (running_.exchange(true)) {
        return false;
    }

    stop_requested_.store(false);
    writer_error_.store(false);
    overflow_since_last_sample_.store(false);
    sequence_.store(start_sequence_);
    total_records_generated_.store(0);
    total_records_written_.store(0);
    total_records_committed_.store(0);
    dropped_records_.store(0);
    buffer_high_watermark_.store(0);
    buffer_.reset();

    if (!writer_.open()) {
        running_.store(false);
        return false;
    }
    writer_.reset_measurement_counters();

    sensor_thread_ = std::thread(&FlightRecorder::sensor_loop, this);
    writer_thread_ = std::thread(&FlightRecorder::writer_loop, this);
    return true;
}

void FlightRecorder::stop() {
    if (!running_.exchange(false) && !sensor_thread_.joinable() && !writer_thread_.joinable()) {
        return;
    }

    stop_requested_.store(true);

    if (sensor_thread_.joinable()) {
        sensor_thread_.join();
    }

    buffer_.close();

    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }

    writer_.flush();
    writer_.close();
}

void FlightRecorder::sensor_loop() {
    const auto sample_period = std::chrono::microseconds(
        std::max<std::uint64_t>(1, 1'000'000u / config_.sample_rate_hz));
    auto next_sample_deadline = std::chrono::steady_clock::now();

    while (!stop_requested_.load()) {
        next_sample_deadline += sample_period;

        const bool overflow_snapshot = overflow_since_last_sample_.exchange(false);
        FlightRecord record = simulator_.next_sample(now_microseconds(), overflow_snapshot);
        const auto push_status = buffer_.push_wait_for(record, sample_period);

        if (push_status == CircularBuffer::PushStatus::Pushed) {
            total_records_generated_.fetch_add(1);
            const auto buffer_stats = buffer_.stats();
            if (buffer_stats.high_watermark > buffer_high_watermark_.load()) {
                buffer_high_watermark_.store(buffer_stats.high_watermark);
            }
        } else if (push_status == CircularBuffer::PushStatus::Timeout) {
            total_records_generated_.fetch_add(1);
            dropped_records_.fetch_add(1);
            overflow_since_last_sample_.store(true);
        } else {
            break;
        }

        if (!config_.unpaced_producer) {
            std::this_thread::sleep_until(next_sample_deadline);
        }
    }
}

void FlightRecorder::writer_loop() {
    while (true) {
        FlightRecord record;
        const auto pop_status = buffer_.pop_wait_for(record, std::chrono::milliseconds(250));
        if (pop_status == CircularBuffer::PopStatus::Timeout) {
            continue;
        }
        if (pop_status == CircularBuffer::PopStatus::Closed) {
            break;
        }

        writer_batch_.clear();
        writer_batch_.push_back(SequencedRecord {record, sequence_.fetch_add(1) + 1});
        const auto drained = buffer_.try_pop_batch(drain_buffer_.get(), config_.batch_size - 1);
        for (std::size_t index = 0; index < drained; ++index) {
            writer_batch_.push_back(
                SequencedRecord {drain_buffer_[index], sequence_.fetch_add(1) + 1});
        }

        if (!writer_.append_batch(writer_batch_)) {
            // Stop acquisition if persistence fails so we do not pretend data is durable.
            stop_requested_.store(true);
            writer_error_.store(true);
            buffer_.close();
            break;
        }

        total_records_written_.fetch_add(writer_batch_.size());
        total_records_committed_.store(writer_.stats().records_committed);
    }

    if (!writer_.flush()) {
        writer_error_.store(true);
    }
    total_records_committed_.store(writer_.stats().records_committed);
}

RecorderStats FlightRecorder::stats() const {
    const auto buffer_stats = buffer_.stats();
    RecorderStats snapshot;
    snapshot.total_records_generated = total_records_generated_.load();
    snapshot.total_records_written = total_records_written_.load();
    snapshot.total_records_committed = total_records_committed_.load();
    snapshot.dropped_records = dropped_records_.load();
    snapshot.buffer_high_watermark =
        std::max<std::uint64_t>(buffer_high_watermark_.load(), buffer_stats.high_watermark);
    snapshot.writer_error = writer_error_.load();
    return snapshot;
}

std::uint64_t FlightRecorder::now_microseconds() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

}  // namespace flight_recorder
