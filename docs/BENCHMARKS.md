# Benchmark methodology

Performance results are meaningful only with their configuration and platform.
Run benchmarks with a Release build on the filesystem being evaluated.

## Metrics

- **Throughput:** records successfully produced, transferred, serialized, and
  written during the measured interval, divided by steady-clock elapsed time.
- **Batch-write latency:** steady-clock time around each real main-log
  `pwrite()` call. This excludes synchronization time.
- **Synchronization latency:** steady-clock time around each main-log data-sync
  call. It is reported separately from write latency.
- **Committed records:** records covered by the newest checkpoint acknowledged
  after both the log and journal synchronization steps complete.
- **Valid replay:** one forward scan verifies metadata, CRC32, exact sequence
  continuity, record count, and file size.

A valid benchmark run requires:

- zero dropped records
- no writer errors
- equal generated, written, committed, and replayed counts
- an exact expected file size
- a healthy replay result

## Reproducible builds

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

cmake --preset release
cmake --build --preset release
ctest --preset release

cmake --preset ubsan
cmake --build --preset ubsan
ctest --preset ubsan
```

## End-to-end benchmark

The benchmark rejects non-Release builds. A typical run is:

```bash
./build-release/recorder_benchmark \
  --output artifacts/benchmarks/trial.bin \
  --artifact artifacts/benchmarks/trial.json \
  --warmup-seconds 2 \
  --duration-seconds 30 \
  --buffer-size 262144 \
  --batch-size 1024 \
  --sync-every-batches 64 \
  --preallocation-bytes 268435456 \
  --alignment 4096 \
  --seed 42
```

The JSON result contains the compiler, build type, kernel, CPU, filesystem,
storage device, Git revision, worktree state, configuration, raw counters,
one-second windows, write/sync latency summaries, and replay status.

The warm-up and measured output paths must not already exist. Results and raw
logs are ignored by Git because they are machine-specific and can be large.

## Linux benchmark suite

```bash
scripts/run_linux_benchmarks.sh artifacts/linux-benchmarks
```

The script runs Release tests, five 30-second end-to-end trials, four shorter
one-factor experiments, and the 180-case process-interruption matrix. It then
checks that every trial is a valid Linux run and prints a compact JSON summary.

The one-factor sequence compares:

1. per-record, minimally aligned, non-preallocated writes
2. batching and group commit
3. 4096-byte serialization-buffer alignment
4. Linux keep-size preallocation

These comparisons show how each configuration changes measured behavior on the
same host. They do not establish a universal performance guarantee.

## Queue-only diagnostic

```bash
./build-release/queue_benchmark 5000000 65536 256
```

This isolates SPSC transfer and ordering. It does not include serialization,
file I/O, checkpointing, or replay and therefore must not be presented as
recorder throughput.

## Interpreting results

Do not compare results unless the platform, storage path, build type, batch
configuration, duration, and validity checks are comparable. Short runs are
useful for development but should not be treated as sustained performance
evidence.
