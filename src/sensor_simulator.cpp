#include "flight_recorder/sensor_simulator.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace flight_recorder {

namespace {

constexpr std::uint64_t kStartupSamples = 25;
constexpr std::uint64_t kTakeoffSamples = 20;
constexpr std::uint64_t kClimbSamples = 70;
constexpr std::uint64_t kCruiseSamples = 120;
constexpr std::uint64_t kDescentSamples = 65;
constexpr std::uint64_t kLandingSamples = 35;
constexpr std::uint64_t kTotalProfileSamples =
    kStartupSamples + kTakeoffSamples + kClimbSamples +
    kCruiseSamples + kDescentSamples + kLandingSamples;

double wrap_degrees(double value) {
    while (value >= 360.0) {
        value -= 360.0;
    }
    while (value < 0.0) {
        value += 360.0;
    }
    return value;
}

double smoothstep(double x) {
    const double clamped = std::clamp(x, 0.0, 1.0);
    return clamped * clamped * (3.0 - (2.0 * clamped));
}

double lerp(double start, double end, double t) {
    return start + ((end - start) * t);
}

}  // namespace

SensorSimulator::SensorSimulator(std::uint32_t seed)
    : rng_(seed),
      altitude_m_(340.0),
      airspeed_kts_(0.0),
      heading_deg_(74.0),
      vertical_speed_fpm_(0.0),
      engine_temperature_c_(30.0),
      engine_rpm_(0.0) {
    std::uniform_int_distribution<std::uint64_t> engine_spike_dist(110, 150);
    std::uniform_int_distribution<std::uint64_t> altitude_drop_dist(220, 250);
    std::uniform_int_distribution<std::uint64_t> sensor_glitch_dist(155, 205);

    engine_spike_start_ = engine_spike_dist(rng_);
    altitude_drop_start_ = altitude_drop_dist(rng_);
    sensor_glitch_start_ = sensor_glitch_dist(rng_);
}

FlightPhase SensorSimulator::current_phase() const {
    if (sample_index_ < kStartupSamples) {
        return FlightPhase::Startup;
    }
    if (sample_index_ < kStartupSamples + kTakeoffSamples) {
        return FlightPhase::Takeoff;
    }
    if (sample_index_ < kStartupSamples + kTakeoffSamples + kClimbSamples) {
        return FlightPhase::Climb;
    }
    if (sample_index_ < kStartupSamples + kTakeoffSamples + kClimbSamples + kCruiseSamples) {
        return FlightPhase::Cruise;
    }
    if (sample_index_ < kStartupSamples + kTakeoffSamples + kClimbSamples + kCruiseSamples + kDescentSamples) {
        return FlightPhase::Descent;
    }
    return FlightPhase::Landing;
}

double SensorSimulator::phase_progress() const {
    const auto phase = current_phase();
    std::uint64_t phase_start = 0;
    std::uint64_t phase_duration = kLandingSamples;

    switch (phase) {
    case FlightPhase::Startup:
        phase_start = 0;
        phase_duration = kStartupSamples;
        break;
    case FlightPhase::Takeoff:
        phase_start = kStartupSamples;
        phase_duration = kTakeoffSamples;
        break;
    case FlightPhase::Climb:
        phase_start = kStartupSamples + kTakeoffSamples;
        phase_duration = kClimbSamples;
        break;
    case FlightPhase::Cruise:
        phase_start = kStartupSamples + kTakeoffSamples + kClimbSamples;
        phase_duration = kCruiseSamples;
        break;
    case FlightPhase::Descent:
        phase_start = kStartupSamples + kTakeoffSamples + kClimbSamples + kCruiseSamples;
        phase_duration = kDescentSamples;
        break;
    case FlightPhase::Landing:
        phase_start = kStartupSamples + kTakeoffSamples + kClimbSamples + kCruiseSamples + kDescentSamples;
        phase_duration = kLandingSamples;
        break;
    }

    return smoothstep(static_cast<double>(sample_index_ - phase_start) /
                      static_cast<double>(std::max<std::uint64_t>(phase_duration - 1, 1)));
}

bool SensorSimulator::anomaly_active(std::uint64_t start_sample, std::uint64_t duration_samples) const {
    return sample_index_ >= start_sample && sample_index_ < (start_sample + duration_samples);
}

FlightRecord SensorSimulator::next_sample(std::uint64_t timestamp_us, bool buffer_overrun) {
    const auto phase = current_phase();
    const double progress = phase_progress();
    phase_rad_ += 0.065;

    double target_altitude_m = altitude_m_;
    double target_airspeed_kts = airspeed_kts_;
    double target_vertical_speed_fpm = vertical_speed_fpm_;
    double target_engine_temperature_c = engine_temperature_c_;
    double target_engine_rpm = engine_rpm_;
    double heading_rate_deg = 0.0;

    switch (phase) {
    case FlightPhase::Startup:
        target_altitude_m = 340.0;
        target_airspeed_kts = 0.0;
        target_vertical_speed_fpm = 0.0;
        target_engine_temperature_c = 35.0 + (progress * 210.0);
        target_engine_rpm = 650.0 + (progress * 350.0);
        heading_rate_deg = 0.02;
        break;
    case FlightPhase::Takeoff:
        target_altitude_m = lerp(340.0, 720.0, progress);
        target_airspeed_kts = lerp(25.0, 155.0, progress);
        target_vertical_speed_fpm = lerp(400.0, 2200.0, progress);
        target_engine_temperature_c = lerp(250.0, 645.0, progress);
        target_engine_rpm = lerp(1800.0, 2550.0, progress);
        heading_rate_deg = 0.12;
        break;
    case FlightPhase::Climb:
        target_altitude_m = lerp(720.0, 3650.0, progress);
        target_airspeed_kts = lerp(150.0, 205.0, progress);
        target_vertical_speed_fpm = lerp(1900.0, 950.0, progress);
        target_engine_temperature_c = lerp(640.0, 690.0, progress);
        target_engine_rpm = lerp(2500.0, 2350.0, progress);
        heading_rate_deg = 0.20;
        break;
    case FlightPhase::Cruise:
        target_altitude_m = 3650.0 + (std::sin(phase_rad_ * 0.35) * 35.0);
        target_airspeed_kts = 228.0 + (std::sin(phase_rad_ * 0.55) * 4.5);
        target_vertical_speed_fpm = std::sin(phase_rad_ * 0.28) * 65.0;
        target_engine_temperature_c = 662.0 + (std::sin(phase_rad_ * 0.42) * 8.0);
        target_engine_rpm = 2220.0 + (std::sin(phase_rad_ * 0.40) * 28.0);
        heading_rate_deg = 0.08;
        break;
    case FlightPhase::Descent:
        target_altitude_m = lerp(3600.0, 780.0, progress);
        target_airspeed_kts = lerp(215.0, 138.0, progress);
        target_vertical_speed_fpm = lerp(-750.0, -1450.0, progress);
        target_engine_temperature_c = lerp(645.0, 555.0, progress);
        target_engine_rpm = lerp(2050.0, 1680.0, progress);
        heading_rate_deg = -0.06;
        break;
    case FlightPhase::Landing:
        target_altitude_m = lerp(780.0, 345.0, progress);
        target_airspeed_kts = lerp(132.0, 18.0, progress);
        target_vertical_speed_fpm = lerp(-900.0, -120.0, progress);
        target_engine_temperature_c = lerp(520.0, 180.0, progress);
        target_engine_rpm = lerp(1550.0, 760.0, progress);
        heading_rate_deg = -0.03;
        break;
    }

    const double small_noise = noise_(rng_);
    altitude_m_ = std::max(0.0, lerp(altitude_m_, target_altitude_m, 0.12) + (small_noise * 0.6));
    airspeed_kts_ = std::max(0.0, lerp(airspeed_kts_, target_airspeed_kts, 0.18) + (noise_(rng_) * 0.45));
    vertical_speed_fpm_ = lerp(vertical_speed_fpm_, target_vertical_speed_fpm, 0.22) + (noise_(rng_) * 8.0);
    engine_temperature_c_ =
        std::max(20.0, lerp(engine_temperature_c_, target_engine_temperature_c, 0.16) + (noise_(rng_) * 0.8));
    engine_rpm_ = std::max(0.0, lerp(engine_rpm_, target_engine_rpm, 0.20) + (noise_(rng_) * 4.0));
    heading_deg_ = wrap_degrees(heading_deg_ + heading_rate_deg + (std::sin(phase_rad_ * 0.18) * 0.05));

    std::uint32_t status = StatusNominal;
    const bool phase_transition = phase != last_phase_;
    if (phase_transition) {
        status |= StatusFlightPhaseTransition;
    }

    if (anomaly_active(engine_spike_start_, 9)) {
        engine_temperature_c_ += 28.0 - (std::abs(4.0 - static_cast<double>(sample_index_ - engine_spike_start_)) * 4.2);
        status |= StatusEngineWarning;
    }

    if (anomaly_active(altitude_drop_start_, 4)) {
        altitude_m_ = std::max(0.0, altitude_m_ - 45.0);
        vertical_speed_fpm_ -= 650.0;
        status |= StatusAltitudeDeviation;
    }

    if (anomaly_active(sensor_glitch_start_, 3)) {
        airspeed_kts_ += (sample_index_ % 2 == 0) ? 18.0 : -14.0;
        heading_deg_ = wrap_degrees(heading_deg_ + 8.0);
        status |= StatusSensorGlitch | StatusPitotDisagree;
    }

    if (engine_temperature_c_ > 675.0) {
        status |= StatusEngineWarning;
    }
    if (phase == FlightPhase::Startup && engine_rpm_ > 850.0 && engine_temperature_c_ < 80.0) {
        status |= StatusLowOilPressure;
    }
    if (buffer_overrun) {
        status |= StatusRecorderBufferOverrun;
    }

    last_phase_ = phase;
    ++sample_index_;
    if (sample_index_ >= kTotalProfileSamples) {
        sample_index_ = kStartupSamples + kTakeoffSamples + kClimbSamples;
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
