#!/usr/bin/env bash
set -euo pipefail

recorder=$1
replay=$2
session_dir=$(mktemp -d)
child_pid=
cleanup() {
    if [[ -n "$child_pid" ]]; then
        kill -KILL "$child_pid" 2>/dev/null || true
        wait "$child_pid" 2>/dev/null || true
    fi
    # Keep diagnostic output and recordings available when an assertion fails.
    echo "CLI test artifacts: $session_dir"
}
trap cleanup EXIT

wait_for_status() {
    for ((attempt=0; attempt<200; attempt++)); do
        if grep -Eq '^status generated=[1-9][0-9]* written=[1-9][0-9]* committed=0 ' "$session_dir/output.txt"; then
            return
        fi
        kill -0 "$child_pid"
        sleep 0.05
    done
    echo "Timed out waiting for uncommitted live records" >&2
    return 1
}

previous_sequence=0
for signal in INT TERM; do
    # Exercise continuous recording and early termination of a timed recording.
    mode=(--run-until-signal)
    if [[ "$signal" == TERM ]]; then
        mode=(--duration-seconds 60)
    fi
    "$recorder" --output "$session_dir/session.bin" "${mode[@]}" \
        --sample-rate-hz 100 --sync-every-batches 1000000 \
        --stats-interval-ms 10 >"$session_dir/output.txt" 2>&1 &
    child_pid=$!
    wait_for_status
    grep -q " last_sequence=$previous_sequence$" "$session_dir/output.txt"
    kill -"$signal" "$child_pid"
    # Bound shutdown waiting independently of the outer CTest timeout.
    for ((attempt=0; attempt<200; attempt++)); do
        if ! kill -0 "$child_pid" 2>/dev/null; then
            break
        fi
        sleep 0.05
    done
    if kill -0 "$child_pid" 2>/dev/null; then
        echo "Timed out waiting for graceful shutdown" >&2
        exit 1
    fi
    wait "$child_pid"
    child_pid=
    grep -q '^shutdown_signal=' "$session_dir/output.txt"
    grep -q '^replay_validation healthy=true checksum_failures=0 ' "$session_dir/output.txt"
    final=$(grep '^generated=' "$session_dir/output.txt")
    generated=$(echo "$final" | sed -E 's/^generated=([0-9]+).*/\1/')
    written=$(echo "$final" | sed -E 's/.* written=([0-9]+).*/\1/')
    committed=$(echo "$final" | sed -E 's/.* committed=([0-9]+).*/\1/')
    [[ "$generated" -gt 0 && "$generated" == "$written" && "$written" == "$committed" ]]
    [[ "$final" == *" dropped=0 "* && "$final" == *"writer_error=false" ]]
    previous_sequence=$((previous_sequence + committed))
    grep -q " valid_records=$previous_sequence$" "$session_dir/output.txt"
    "$replay" "$session_dir/session.bin" --summary
done

# Existing zero-duration behavior stays finite; reporting remains opt-in.
"$recorder" --output "$session_dir/session.bin" --duration-seconds 0 >"$session_dir/output.txt"
if grep -q '^status ' "$session_dir/output.txt"; then
    exit 1
fi

expect_invalid() {
    local result=0
    "$recorder" --output "$session_dir/invalid.bin" "$@" >"$session_dir/invalid.txt" 2>&1 || result=$?
    [[ "$result" == 1 && ! -e "$session_dir/invalid.bin" ]]
}
expect_invalid --run-until-signal --duration-seconds 1
expect_invalid --duration-seconds 1 --run-until-signal
for value in -1 1x 4294967296 ''; do
    expect_invalid --stats-interval-ms "$value"
done
expect_invalid --stats-interval-ms
expect_invalid --duration-seconds 1x

rm "$session_dir/session.bin" "$session_dir/session.bin.journal" \
    "$session_dir/output.txt" "$session_dir/invalid.txt"
rmdir "$session_dir"
trap - EXIT
echo "CLI session tests passed"
