# Generated artifacts

Benchmark JSON, binary logs, journals, acknowledgement files, and crash-matrix
workspaces are written below this directory and ignored by Git.

Generate a Linux benchmark set with:

```bash
scripts/run_linux_benchmarks.sh artifacts/linux-benchmarks
```

Generate a process-interruption report with:

```bash
./build-release/crash_matrix artifacts/crash-matrix/local-release.json
```

Keep results with the machine and configuration that produced them. Performance
artifacts are not portable evidence across CPUs, kernels, filesystems, or
storage devices.
