#include "flight_recorder/circular_buffer.hpp"
#include "flight_recorder/platform_info.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    std::size_t total_records = 5'000'000;
    std::size_t capacity = 65'536;
    std::size_t batch_size = 256;
    try {
        if (argc > 1) total_records = std::stoull(argv[1]);
        if (argc > 2) capacity = std::stoull(argv[2]);
        if (argc > 3) batch_size = std::stoull(argv[3]);
    } catch (...) {
        std::cerr << "Usage: queue_benchmark [records>0] [capacity>0] [batch_size>0]\n";
        return 1;
    }
    if (total_records == 0 || capacity == 0 || batch_size == 0) {
        std::cerr << "Usage: queue_benchmark [records>0] [capacity>0] [batch_size>0]\n";
        return 1;
    }

    flight_recorder::CircularBuffer buffer(capacity);
    auto batch = std::make_unique<flight_recorder::FlightRecord[]>(batch_size);
    const auto start = std::chrono::steady_clock::now();
    std::thread producer([&] {
        for (std::size_t index = 1; index <= total_records; ++index) {
            flight_recorder::FlightRecord record;
            record.timestamp_us = index;
            if (buffer.push_blocking(record) != flight_recorder::CircularBuffer::PushStatus::Pushed) {
                break;
            }
        }
        buffer.close();
    });

    std::size_t consumed = 0;
    bool ordered = true;
    while (true) {
        flight_recorder::FlightRecord first;
        const auto status = buffer.pop_blocking(first);
        if (status == flight_recorder::CircularBuffer::PopStatus::Closed) break;
        ordered = ordered && first.timestamp_us == consumed + 1;
        ++consumed;
        const auto count = buffer.try_pop_batch(batch.get(), batch_size);
        for (std::size_t index = 0; index < count; ++index) {
            ordered = ordered && batch[index].timestamp_us == consumed + 1;
            ++consumed;
        }
    }
    producer.join();
    const auto end = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(end - start).count();
    const auto stats = buffer.stats();
    const auto platform = flight_recorder::platform_info_json(
        flight_recorder::collect_platform_info("."));

    std::cout << "{\"benchmark_kind\":\"queue_only_diagnostic\","
              << "\"records\":" << consumed << ','
              << "\"elapsed_seconds\":" << elapsed << ','
              << "\"records_per_second\":"
              << static_cast<std::uint64_t>(static_cast<double>(consumed) / elapsed) << ','
              << "\"capacity\":" << capacity << ','
              << "\"batch_size\":" << batch_size << ','
              << "\"dropped_records\":" << stats.dropped_records << ','
              << "\"ordered\":" << (ordered ? "true" : "false") << ','
              << "\"platform\":" << platform << "}\n";
    return consumed == total_records && ordered && stats.dropped_records == 0 ? 0 : 2;
}
