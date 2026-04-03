#include "flight_recorder/sensor_simulator.hpp"

#include <algorithm>
#include <cmath>

namespace flight_recorder {

namespace {

double wrap_degrees(double value) {
    while (value >= 360.0) {
        value -= 360.0;
    }
    while (value < 0.0) {
        value += 360.0;
    }
    return value;
}

}  // namespace

SensorSimulator::SensorSimulator() = default;

FlightRecord SensorSimulator::next_sample(std::uint64_t timestamp_us, bool buffer_overrun) {
    phase_rad_ += 0.08;

    altitude_m_ += 1.2 + std::sin(phase_rad_) * 0.6;
    airspeed_kts_ = std::clamp(136.0 + std::sin(phase_rad_ * 0.6) * 8.0, 110.0, 185.0);
    heading_deg_ = wrap_degrees(heading_deg_ + 0.7 + std::sin(phase_rad_ * 0.2) * 0.15);
    vertical_speed_fpm_ = 240.0 + std::sin(phase_rad_ * 1.5) * 120.0;
    engine_temperature_c_ = std::clamp(630.0 + std::sin(phase_rad_ * 0.9) * 18.0, 580.0, 690.0);
    engine_rpm_ = std::clamp(2180.0 + std::sin(phase_rad_ * 0.75) * 85.0, 1900.0, 2400.0);

    std::uint32_t status = StatusNominal;
    if (engine_temperature_c_ > 660.0) {
        status |= StatusEngineWarning;
    }
    if (buffer_overrun) {
        status |= StatusRecorderBufferOverrun;
    }

    return FlightRecord {
        timestamp_us,
        altitude_m_,
        airspeed_kts_,
        heading_deg_,
        vertical_speed_fpm_,
        engine_temperature_c_,
        engine_rpm_,
        status
    };
}

}  // namespace flight_recorder
