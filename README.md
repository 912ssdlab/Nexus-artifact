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

Place the input files listed below in `traces/`, then run:

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
