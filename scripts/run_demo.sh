#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir="${repo_root}/build-release"
demo_temp_root=${TMPDIR:-/tmp}
demo_temp_root=${demo_temp_root%/}
demo_dir=$(mktemp -d "${demo_temp_root}/flight-recorder-demo.XXXXXX")

if [[ ! -d "$demo_dir" || "$demo_dir" != "${demo_temp_root}"/flight-recorder-demo.* ]]; then
    echo "failed to create a safe demo directory" >&2
    exit 1
fi

echo "[1/5] Building the Release binaries"
cmake --preset release -S "$repo_root"
cmake --build --preset release -j 4

echo "[2/5] Recording a deterministic one-second flight"
"${build_dir}/flight_recorder" \
    --output "${demo_dir}/flight.bin" \
    --duration-seconds 1 \
    --sample-rate-hz 200 \
    --buffer-size 1024 \
    --batch-size 32 \
    --sync-every-batches 2 \
    --seed 42

echo "[3/5] Replaying and summarizing the valid log"
"${build_dir}/replay_tool" "${demo_dir}/flight.bin" --summary

echo "[4/5] Injecting corruption and proving replay detects it"
cp "${demo_dir}/flight.bin" "${demo_dir}/corrupt.bin"
"${build_dir}/fault_injector" corrupt "${demo_dir}/corrupt.bin" 2 40
set +e
"${build_dir}/replay_tool" "${demo_dir}/corrupt.bin" --summary --show-invalid
replay_status=$?
set -e
if [[ "$replay_status" -ne 2 ]]; then
    echo "expected corrupt replay to exit 2, got ${replay_status}" >&2
    exit 1
fi
echo "corruption_detection=passed expected_exit=2"

echo "[5/5] Running all deterministic crash/recovery scenarios"
"${build_dir}/crash_matrix" "${demo_dir}/crash-matrix.json"

echo "demo_complete=true"
echo "demo_artifacts=${demo_dir}"
