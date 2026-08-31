#pragma once

#include "flight_recorder/crc32.hpp"
#include "flight_recorder/flight_record.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace flight_recorder {

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "embedded-flight-recorder log format currently supports little-endian targets only."
#endif

static_assert(std::numeric_limits<double>::is_iec559,
              "The log format requires IEEE-754 double representation.");

// The file format uses dedicated packed structs rather than serializing the in-memory
// FlightRecord directly. That keeps the on-disk layout explicit and stable even if the
// runtime struct changes or if a compiler would otherwise insert padding.
#pragma pack(push, 1)
struct PersistedFileHeader {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t header_size;
    std::uint64_t recorder_start_time_us;
    std::uint32_t record_size;
    std::uint32_t header_crc32;
};

struct PersistedFlightPayload {
    double altitude_m;
    double airspeed_kts;
    double heading_deg;
    double vertical_speed_fpm;
    double engine_temperature_c;
    double engine_rpm;
    std::uint32_t system_status;
};

struct PersistedRecordHeader {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t header_size;
    std::uint32_t payload_size;
    std::uint64_t sequence;
    std::uint64_t timestamp_us;
};

struct PersistedJournalEntry {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t header_size;
    std::uint32_t state;
    std::uint32_t entry_size;
    std::uint64_t sequence;
    PersistedRecordHeader record_header;
    PersistedFlightPayload payload;
    std::uint32_t record_crc32;
    std::uint32_t journal_crc32;
};

struct PersistedCheckpointSlot {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t slot_size;
    std::uint64_t generation;
    std::uint64_t recorder_start_time_us;
    std::uint64_t committed_length;
    std::uint64_t record_count;
    std::uint64_t last_sequence;
    std::uint32_t checkpoint_crc32;
};
#pragma pack(pop)

static_assert(sizeof(PersistedFileHeader) == 24u, "Unexpected file header size");
static_assert(sizeof(PersistedFlightPayload) == 52u, "Unexpected payload packing");
static_assert(sizeof(PersistedRecordHeader) == 28u, "Unexpected record header size");
static_assert(sizeof(PersistedJournalEntry) == 112u, "Unexpected journal entry size");
static_assert(sizeof(PersistedCheckpointSlot) == 52u, "Unexpected checkpoint slot size");

constexpr std::uint32_t kLogFileMagic = 0x464C4F47u;     // "FLOG"
constexpr std::uint32_t kLogRecordMagic = 0x46524344u;   // "FRCD"
constexpr std::uint32_t kJournalMagic = 0x464A4E4Cu;     // "FJNL"
constexpr std::uint16_t kLogFormatVersion = 1u;
constexpr std::uint32_t kJournalStatePending = 1u;
constexpr std::uint16_t kCheckpointJournalVersion = 2u;
constexpr std::size_t kCheckpointSlotCount = 2u;
constexpr std::size_t kCheckpointJournalSize =
    kCheckpointSlotCount * sizeof(PersistedCheckpointSlot);

constexpr std::size_t kPersistedRecordSize =
    sizeof(PersistedRecordHeader) + sizeof(PersistedFlightPayload) + sizeof(std::uint32_t);

inline PersistedFlightPayload make_persisted_payload(const FlightRecord& record) {
    return PersistedFlightPayload {
        record.altitude_m,
        record.airspeed_kts,
        record.heading_deg,
        record.vertical_speed_fpm,
        record.engine_temperature_c,
        record.engine_rpm,
        record.system_status
    };
}

inline FlightRecord make_runtime_record(const PersistedRecordHeader& header,
                                        const PersistedFlightPayload& payload) {
    FlightRecord record;
    record.timestamp_us = header.timestamp_us;
    record.altitude_m = payload.altitude_m;
    record.airspeed_kts = payload.airspeed_kts;
    record.heading_deg = payload.heading_deg;
    record.vertical_speed_fpm = payload.vertical_speed_fpm;
    record.engine_temperature_c = payload.engine_temperature_c;
    record.engine_rpm = payload.engine_rpm;
    record.system_status = payload.system_status;
    return record;
}

inline std::uint32_t compute_file_header_crc(const PersistedFileHeader& header) {
    PersistedFileHeader crc_input = header;
    crc_input.header_crc32 = 0;
    return compute_crc32(&crc_input, sizeof(crc_input));
}

inline std::uint32_t compute_record_crc(const PersistedRecordHeader& header,
                                        const PersistedFlightPayload& payload) {
    return compute_crc32_segments(&header, sizeof(header), &payload, sizeof(payload));
}

inline std::uint32_t compute_journal_crc(const PersistedJournalEntry& entry) {
    PersistedJournalEntry crc_input = entry;
    crc_input.journal_crc32 = 0;
    return compute_crc32(&crc_input, sizeof(crc_input));
}

inline std::uint32_t compute_checkpoint_crc(const PersistedCheckpointSlot& slot) {
    PersistedCheckpointSlot crc_input = slot;
    crc_input.checkpoint_crc32 = 0;
    return compute_crc32(&crc_input, sizeof(crc_input));
}

inline bool checkpoint_metadata_valid(const PersistedCheckpointSlot& slot) {
    return slot.magic == kJournalMagic &&
           slot.version == kCheckpointJournalVersion &&
           slot.slot_size == sizeof(PersistedCheckpointSlot) &&
           slot.generation != 0 &&
           compute_checkpoint_crc(slot) == slot.checkpoint_crc32;
}

}  // namespace flight_recorder
