# Demo guide

Run the complete deterministic demonstration from the repository root:

```bash
scripts/run_demo.sh
```

The script:

1. builds the Release binaries
2. records one second of seeded telemetry
3. replays and summarizes the valid log
4. corrupts one record and verifies that replay exits with status 2
5. runs 180 deterministic process-interruption and recovery cases

All generated files are written to a new temporary directory whose path is
printed at the end.

## What the output demonstrates

- The bounded SPSC ring separates telemetry production from storage work.
- Every fixed-size record carries a sequence number and CRC32.
- Replay stops at the first malformed, corrupt, or non-contiguous record.
- A commit is acknowledged only after main-log and checkpoint-journal
  synchronization.
- Recovery selects a checkpoint-consistent prefix and is idempotent.

The crash matrix uses child-process termination and controlled partial writes.
It tests recorder state transitions deterministically; it does not simulate the
full behavior of a physical power cut or failing storage device.

## Manual inspection

The demo prints the temporary directory. You can inspect its valid log with:

```bash
./build-release/replay_tool /path/from/demo/flight.bin --show-invalid
```

The corrupted copy should report a CRC mismatch at sequence 2:

```bash
./build-release/replay_tool /path/from/demo/corrupt.bin --show-invalid
```
