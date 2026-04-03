#pragma once

#include "flight_recorder/binary_log_writer.hpp"
#include "flight_recorder/circular_buffer.hpp"
#include "flight_recorder/recorder_config.hpp"
#include "flight_recorder/sensor_simulator.hpp"

#include <atomic>
#include <cstdint>
#include <thread>

namespace flight_recorder {

struct RecorderStats {
    std::uint64_t total_records_generated {0};
    std::uint64_t total_records_written {0};
    std::uint64_t dropped_records {0};
    std::uint64_t buffer_high_watermark {0};
    bool writer_error {false};
};

class FlightRecorder {
public:
    explicit FlightRecorder(RecorderConfig config);
    ~FlightRecorder();

    FlightRecorder(const FlightRecorder&) = delete;
    FlightRecorder& operator=(const FlightRecorder&) = delete;

    void set_start_sequence(std::uint64_t sequence);
    bool start();
    void stop();
    RecorderStats stats() const;

private:
    void sensor_loop();
    void writer_loop();
    static std::uint64_t now_microseconds();

    RecorderConfig config_;
    CircularBuffer buffer_;
    SensorSimulator simulator_;
    BinaryLogWriter writer_;

    std::atomic<bool> running_ {false};
    std::atomic<bool> stop_requested_ {false};
    std::atomic<bool> overflow_since_last_sample_ {false};
    std::atomic<std::uint64_t> sequence_ {0};
    std::atomic<std::uint64_t> total_records_generated_ {0};
    std::atomic<std::uint64_t> total_records_written_ {0};
    std::atomic<std::uint64_t> dropped_records_ {0};
    std::atomic<std::uint64_t> buffer_high_watermark_ {0};
    std::atomic<bool> writer_error_ {false};
    std::uint64_t start_sequence_ {0};

    std::thread sensor_thread_;
    std::thread writer_thread_;
};

}  // namespace flight_recorder
