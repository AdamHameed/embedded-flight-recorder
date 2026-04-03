# embedded-flight-recorder

`embedded-flight-recorder` is a Linux-hosted C++17 simulation of an embedded flight data recorder pipeline. It is structured like a small systems project: sensor production runs on one thread, durable logging runs on another thread, records move through a bounded circular buffer, and offline tools support replay and recovery.

## Highlights

- C++17 + CMake project with `include/`, `src/`, `tools/`, and `tests/`
- Simulated aircraft sensor stream with evolving values and status flags
- Fixed-size in-memory circular buffer guarded by mutex/condition variable
- Packed binary file header plus fixed-size records with magic, version, sequence, timestamp, payload, and CRC32
- Sidecar write-ahead journal for crash-safe appends
- Explicit flush, `fdatasync()`, and startup recovery flow for crash resilience
- Replay and recovery CLI utilities

## Directory Layout

```text
embedded-flight-recorder/
├── CMakeLists.txt
├── README.md
├── include/flight_recorder/
│   ├── binary_log_writer.hpp
│   ├── circular_buffer.hpp
│   ├── crc32.hpp
│   ├── flight_record.hpp
│   ├── flight_recorder.hpp
│   ├── recorder_config.hpp
│   ├── recovery_manager.hpp
│   └── sensor_simulator.hpp
├── src/
│   ├── binary_log_writer.cpp
│   ├── flight_recorder.cpp
│   ├── main.cpp
│   ├── recovery_manager.cpp
│   └── sensor_simulator.cpp
├── tests/
│   └── flight_recorder_tests.cpp
└── tools/
    ├── recovery_tool.cpp
    └── replay_tool.cpp
```

## Architecture

### `SensorSimulator`
Generates plausible aircraft telemetry at a configurable sample rate. The simulator intentionally evolves values gradually rather than emitting random noise so replay output looks like a believable flight segment.

### `FlightRecord`
Represents one telemetry sample in memory. It contains the fields the recorder pipeline cares about: time, altitude, airspeed, heading, vertical speed, engine temperature, engine RPM, and a bitmask of status flags.

### `CircularBuffer`
A bounded producer/consumer queue. In an embedded design this acts like a small RAM-backed staging area between time-sensitive data acquisition and slower persistent storage. When full, the implementation drops the oldest sample and tracks that event so the system fails in a bounded, observable way instead of allocating unbounded memory.

### `FlightRecorder`
Owns the runtime pipeline: start/stop control, sensor thread, writer thread, buffer, backpressure handling, and graceful shutdown.

### `BinaryLogWriter`
Serializes `FlightRecord` data to a packed binary format with a single file header followed by fixed-size append-only records. Each append is protected by a sidecar journal file (`<log>.journal`) so interrupted writes can be recovered on the next startup.

## File Format

### Endianness and Packing

The current implementation assumes a little-endian Linux target and IEEE-754 `double`. It does not serialize the in-memory `FlightRecord` directly. Instead, it writes dedicated packed on-disk structs composed only of fixed-width integer types and explicitly ordered `double` fields:

- compiler padding is disabled with `#pragma pack(push, 1)`
- field sizes are guarded with `static_assert`
- the code rejects big-endian builds at compile time

This is a deliberate embedded-style tradeoff: the format is compact and fast on the intended target, while the assumptions are documented clearly instead of being left implicit.

### File Header

Written once at offset `0`:

```text
+----------------------+--------------------------------------+
| Field                | Notes                                |
+----------------------+--------------------------------------+
| magic                | file magic (`FLOG`)                  |
| version              | format version                       |
| header_size          | packed file header size              |
| recorder_start_time  | wall-clock start time in microseconds|
| record_size          | fixed on-disk size of each record    |
| header_crc32         | CRC32 over the file header           |
+----------------------+--------------------------------------+
```

### Record Layout

Each appended record contains:

```text
+----------------+-------------------------------+
| Field          | Notes                         |
+----------------+-------------------------------+
| magic          | 32-bit constant               |
| version        | 16-bit format version         |
| header_size    | 16-bit packed header bytes    |
| payload_size   | 32-bit payload bytes          |
| sequence       | 64-bit monotonic sample id    |
| timestamp_us   | 64-bit sample timestamp       |
| payload        | packed telemetry payload      |
| crc32          | CRC over header+payload       |
+----------------+-------------------------------+
```

Because every record has the same packed size, replay and recovery can scan linearly without needing separators, indexes, or variable-length parsing.

The writer uses low-level file descriptors, retries short writes, and calls `fdatasync()` after flushes. This is slower than a buffered desktop logger, but it models the durability choices often made in safety-oriented data capture systems.

## Journal Format

The journal is a single-entry write-ahead log stored beside the main file as `<log>.journal`. It contains:

- journal magic and version
- pending state
- copied record header
- copied record payload
- copied record CRC32
- journal CRC32 over the whole journal entry

Only one in-flight append is journaled at a time. That keeps the implementation small and realistic for an embedded recorder with a single writer thread.

### Append Sequence

For each record:

1. Write the pending journal entry and `fdatasync()` the journal.
2. Append the record to the main log and `fdatasync()` the main file.
3. Clear the journal and `fdatasync()` the clear.

This means a crash can leave the system in one of two recoverable states:

- the journal exists but the record never reached the main log: recovery rolls the record forward
- the journal exists and the record already reached the main log: recovery clears the stale journal

### Consistency Guarantees

The journal protects against:

- process crashes during an append
- power loss between journal write and main-log write
- power loss after main-log flush but before journal clear
- torn or partial main-log record writes, which are still detected by the main-log CRC and valid-prefix scan

The current design does not protect against:

- simultaneous corruption of both the main log and the journal
- filesystem or hardware reordering beyond the durability guarantees of `fdatasync()`
- directory entry loss if the filesystem does not durably persist newly created files without syncing the parent directory

Those limitations are deliberate for a compact internship-scale project, and they are called out explicitly rather than hidden.

### `RecoveryManager`
Scans a log from the beginning, validates structure and CRC, inspects the sidecar journal, and detects:

- trailing partial records
- bad magic/version fields
- payload size mismatches
- CRC failures
- pending or corrupt journal entries

Startup recovery is intentionally conservative:

- if the main log has a damaged tail, it truncates back to the last valid prefix
- if the journal contains a valid pending record that is not yet in the log, it rolls that record forward
- if the journal contains a stale committed record, it clears the journal
- if the journal is corrupt, it discards the journal and preserves the validated main log

### Corruption Detection Examples

- If power is lost halfway through a record write, replay will hit EOF before a full record is present and report a truncated tail.
- If power is lost after journaling intent but before the main log append is durable, startup recovery replays the journaled record into the main log.
- If power is lost after the main log flush but before clearing the journal, startup recovery sees that the record already exists and clears the stale journal entry.
- If a bit flip changes any header or payload byte, CRC32 validation fails and the record is rejected.
- If a parser lands on garbage data, wrong file magic, wrong record magic, wrong version, or unexpected packed sizes cause an immediate rejection.
- If a damaged log repeats or rewinds the sequence counter, recovery rejects the non-monotonic record stream.

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

## Run

Record a short session:

```bash
./build/flight_recorder --output flight_log.bin --duration-seconds 5 --sample-rate-hz 20 --buffer-size 128
```

At startup the recorder prints a recovery summary before it begins sampling, for example:

```text
startup_recovery healthy=true log_truncated=false journal_found=true journal_replayed=true journal_cleared=true last_sequence=1042
rolled pending journal entry forward and cleared journal
```

Replay the captured data:

```bash
./build/replay_tool flight_log.bin
```

Validate and optionally truncate a corrupted tail:

```bash
./build/recovery_tool flight_log.bin --truncate
```

## Design Notes

- The on-disk structures are packed to keep the binary format stable and compact.
- CRC32 is used as a lightweight integrity check that fits embedded-style log validation well.
- The recorder separates acquisition from persistence so sensor timing is less impacted by storage latency.
- Safe shutdown flushes remaining in-memory records before exit.
- Recovery operates on a valid-prefix model because embedded recorders often favor salvageable data over perfect reconstruction.

## Future Improvements

- Dual-file journal or segment rotation for stronger crash consistency guarantees
- Record batching with configurable sync intervals
- Endianness tagging and cross-platform decoding helpers
- More detailed fault injection tests for torn writes and disk-full conditions
- Additional aircraft phases and fault scenarios in the simulator
