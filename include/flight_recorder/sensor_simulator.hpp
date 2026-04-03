#pragma once

#include "flight_recorder/flight_record.hpp"

#include <cstdint>

namespace flight_recorder {

class SensorSimulator {
public:
    SensorSimulator();

    FlightRecord next_sample(std::uint64_t timestamp_us, bool buffer_overrun);

private:
    double phase_rad_ {0.0};
    double altitude_m_ {1200.0};
    double airspeed_kts_ {135.0};
    double heading_deg_ {72.0};
    double vertical_speed_fpm_ {250.0};
    double engine_temperature_c_ {625.0};
    double engine_rpm_ {2150.0};
};

}  // namespace flight_recorder
