#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "Linux is required for this benchmark suite" >&2
    exit 1
fi

benchmark_root=${1:-artifacts/linux-benchmarks}
run_stamp=$(date -u +%Y%m%dT%H%M%SZ)
run_dir="${benchmark_root}/${run_stamp}"
mkdir -p "$run_dir"

cmake --preset release
cmake --build --preset release -j "$(nproc)"
ctest --preset release

for trial in 1 2 3 4 5; do
    ./build-release/recorder_benchmark \
        --output "${run_dir}/trial-${trial}.bin" \
        --artifact "${run_dir}/trial-${trial}.json" \
        --warmup-seconds 2 \
        --duration-seconds 30 \
        --buffer-size 262144 \
        --batch-size 1024 \
        --sync-every-batches 64 \
        --preallocation-bytes 268435456 \
        --alignment 4096 \
        --seed "$((41 + trial))"
done

# These short runs change one I/O setting at a time. They are diagnostic
# comparisons, not qualification trials.
./build-release/recorder_benchmark \
    --output "${run_dir}/attribution-baseline.bin" \
    --artifact "${run_dir}/attribution-baseline.json" \
    --warmup-seconds 2 --duration-seconds 5 --buffer-size 262144 \
    --batch-size 1 --sync-every-batches 1 --preallocation-bytes 0 --alignment 8 --seed 101
./build-release/recorder_benchmark \
    --output "${run_dir}/attribution-batched.bin" \
    --artifact "${run_dir}/attribution-batched.json" \
    --warmup-seconds 2 --duration-seconds 5 --buffer-size 262144 \
    --batch-size 1024 --sync-every-batches 64 --preallocation-bytes 0 --alignment 8 --seed 102
./build-release/recorder_benchmark \
    --output "${run_dir}/attribution-aligned.bin" \
    --artifact "${run_dir}/attribution-aligned.json" \
    --warmup-seconds 2 --duration-seconds 5 --buffer-size 262144 \
    --batch-size 1024 --sync-every-batches 64 --preallocation-bytes 0 --alignment 4096 --seed 103
./build-release/recorder_benchmark \
    --output "${run_dir}/attribution-preallocated.bin" \
    --artifact "${run_dir}/attribution-preallocated.json" \
    --warmup-seconds 2 --duration-seconds 5 --buffer-size 262144 \
    --batch-size 1024 --sync-every-batches 64 --preallocation-bytes 268435456 --alignment 4096 --seed 104

./build-release/crash_matrix "${run_dir}/crash-matrix-release.json"

jq -e -s '
  (length == 5) and
  all(.[];
    .valid_run and
    (.platform.os_kernel | startswith("Linux ")) and
    .configuration.warmup_seconds >= 2 and
    .configuration.duration_seconds >= 30
  )
' "${run_dir}"/trial-*.json >/dev/null

jq -s '{
  valid_trials: length,
  records_per_second: map(.records_per_second),
  minimum_window_records_per_second:
    map(.one_second_window_min_records_per_second),
  batch_write_p99_ns: map(.batch_write_latency.p99_ns),
  main_sync_p99_ns: map(.main_sync_latency.p99_ns)
}' "${run_dir}"/trial-*.json

echo "linux_benchmark_artifacts=${run_dir}"
