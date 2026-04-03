#pragma once

#include <cstdint>

namespace flight_recorder {

enum SystemStatusFlag : std::uint32_t {
    StatusNominal = 1u << 0,
    StatusEngineWarning = 1u << 1,
    StatusLowOilPressure = 1u << 2,
    StatusPitotDisagree = 1u << 3,
    StatusRecorderBufferOverrun = 1u << 4,
    StatusSensorGlitch = 1u << 5,
    StatusAltitudeDeviation = 1u << 6,
    StatusFlightPhaseTransition = 1u << 7
};

struct FlightRecord {
    std::uint64_t timestamp_us {0};
    double altitude_m {0.0};
    double airspeed_kts {0.0};
    double heading_deg {0.0};
    double vertical_speed_fpm {0.0};
    double engine_temperature_c {0.0};
    double engine_rpm {0.0};
    std::uint32_t system_status {StatusNominal};
};

}  // namespace flight_recorder
