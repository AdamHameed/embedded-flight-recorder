#pragma once

#include "flight_recorder/flight_record.hpp"

#include <cstdint>
#include <random>

namespace flight_recorder {

enum class FlightPhase : std::uint8_t {
    Startup,
    Takeoff,
    Climb,
    Cruise,
    Descent,
    Landing
};

class SensorSimulator {
public:
    explicit SensorSimulator(std::uint32_t seed = 42);

    FlightRecord next_sample(std::uint64_t timestamp_us, bool buffer_overrun);

private:
    FlightPhase current_phase() const;
    double phase_progress() const;
    bool anomaly_active(std::uint64_t start_sample, std::uint64_t duration_samples) const;

    std::mt19937 rng_;
    std::uniform_real_distribution<double> noise_ {-1.0, 1.0};

    std::uint64_t sample_index_ {0};
    double phase_rad_ {0.0};
    double altitude_m_ {0.0};
    double airspeed_kts_ {0.0};
    double heading_deg_ {0.0};
    double vertical_speed_fpm_ {0.0};
    double engine_temperature_c_ {32.0};
    double engine_rpm_ {0.0};

    std::uint64_t engine_spike_start_ {0};
    std::uint64_t altitude_drop_start_ {0};
    std::uint64_t sensor_glitch_start_ {0};
    FlightPhase last_phase_ {FlightPhase::Startup};
};

}  // namespace flight_recorder
