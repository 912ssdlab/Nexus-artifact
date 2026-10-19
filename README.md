# Nexus

## Setup

Run all commands from the repository root.

```bash
sudo apt-get install build-essential python3 python3-venv
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r requirements.txt
```

## Run Experiments

Download the input files as described in [Input Traces](#input-traces), place them in `traces/`, then run:

```bash
bash run.sh --output "$PWD/results"
```

The runner builds the simulators, runs the 32MB and 64MB experiments, and generates tables and figures.

```bash
# Select workloads and mechanisms
bash run.sh --traces OFLOW DL RT-SIFT --mechanisms Page Cacheline SkyByte Nexus \
  --sizes 32 64 --output "$PWD/selected_results"

# Run in the background
nohup bash run.sh --output "$PWD/results" > reproduction.log 2>&1 < /dev/null &

# Collect trace characteristics only
bash run.sh --features-only --output "$PWD/features_results"

# Export tables again
bash run.sh --export-only --output "$PWD/results"
```

Repeat the same command to resume. Use a new output directory for a different configuration.

| Name | Application category | Input file |
|---|---|---|
| OFLOW | Computer vision | `OFLOW.trace` |
| DL | Deep learning | `DL.trace` |
| SA-64 | Sparse training | `SA-64.trace` |
| SA-128 | Sparse training | `SA-128.trace` |
| DLRM | Recommendation systems | `DLRM.trace` |
| SSSP | Graph processing | `SSSP.trace` |
| RT-SIFT | Real-time tasks | `RT-SIFT.trace` |
| RT-MSER | Real-time tasks | `RT-MSER.trace` |
| RT-TRACK | Real-time tasks | `RT-TRACK.trace` |

Tables are saved to `results/paper_data/`; figures are saved to `results/figures/`.

## Input Traces

Download the nine traces from [Zenodo: Nexus Memory-Access Traces](https://zenodo.org/records/23463071) ([DOI: 10.5281/zenodo.23463071](https://doi.org/10.5281/zenodo.23463071)). The dataset is released by **swu-912** under **CC BY 4.0**.

Download all `.trace.gz` and `.trace.gz.partNNN` files, plus `SHA256SUMS`, into `traces/`. From the repository root, verify, reassemble, and extract them:

```bash
cd traces
sha256sum -c SHA256SUMS
for first in *.trace.gz.part001; do
  [ -f "$first" ] || continue
  archive="${first%.part001}"
  cat "$archive".part[0-9][0-9][0-9] > "$archive"
done
gzip -dk *.trace.gz
cd ..
bash run.sh --output "$PWD/results"
```

Alternatively, collect the traces using `bash collect.sh --output "$PWD/recollected"` after following [trace_tools/README.md](trace_tools/README.md).

## Generate Figures

```bash
python3 source/plotting/plot_all.py --paper-data results/paper_data
python3 source/plotting/plot_all.py --paper-data results/paper_data --figures 2 3 4
```

Edit `source/plotting/plot_common.py` to adjust the plotting style.

## Collect Traces

Follow [trace_tools/README.md](trace_tools/README.md) to install the collection dependencies and build the tools, then run:

```bash
bash collect.sh --output "$PWD/recollected"
bash collect.sh --traces OFLOW DLRM --output "$PWD/recollected_selected"
```

See [source/README.md](source/README.md) for experiment commands and [source/plotting/README.md](source/plotting/README.md) for plotting commands.

## Citation

The simulator and trace collection tools are based on:

Yang S. P., Kim M., Nam S., et al. *Overcoming the Memory Wall with CXL-Enabled SSDs*. USENIX Annual Technical Conference (ATC), 2023. [Paper](https://www.usenix.org/conference/atc23/presentation/yang-shao-peng).

Third-party licenses and copyright notices are retained in their source directories.
