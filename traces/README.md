# Input Traces

Trace files are distributed separately from the Git repository. To collect them locally, follow [../trace_tools/README.md](../trace_tools/README.md).

Place these files in this directory:

```text
OFLOW.trace
DL.trace
SA-64.trace
SA-128.trace
DLRM.trace
SSSP.trace
RT-SIFT.trace
RT-MSER.trace
RT-TRACK.trace
```

Run from the repository root:

```bash
bash run.sh --output "$PWD/results"
bash run.sh --traces OFLOW DL RT-SIFT --output "$PWD/selected_results"
```

To collect new traces, follow [../trace_tools/README.md](../trace_tools/README.md).

To use newly collected inputs, copy the artifact to a separate directory, replace its trace files, and update the corresponding paths, request counts, file sizes, and SHA-256 hashes in `manifest.json`.
