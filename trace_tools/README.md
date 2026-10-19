# Collect Traces

Run all commands from the repository root on Linux.

## Install Dependencies

```bash
sudo apt-get install build-essential autoconf automake libtool perl python3-venv
```

## Install Application Dependencies

```bash
python3.10 -m venv .collection-venv
source .collection-venv/bin/activate
python -m pip install --upgrade pip
python -m pip install torch==1.13.1 torchvision==0.14.1 \
  --extra-index-url https://download.pytorch.org/whl/cpu
python -m pip install numpy==1.23.5 opencv-python==4.11.0.86 scikit-learn==1.0.2
```

## Collect

```bash
# Preview collection commands
bash collect.sh --dry-run

# Collect all nine workloads
bash collect.sh --output "$PWD/recollected"

# Collect selected workloads
bash collect.sh --traces OFLOW DL RT-SIFT \
  --output "$PWD/recollected_selected"

# Run in the background
nohup bash collect.sh --output "$PWD/recollected" \
  > collection.log 2>&1 < /dev/null &
```

Available names: `OFLOW`, `DL`, `SA-64`, `SA-128`, `DLRM`, `SSSP`, `RT-SIFT`, `RT-MSER`, `RT-TRACK`.

Use `--python /path/to/python` to select an application interpreter. Use a new output directory for each collection.

Each workload is saved to `OUTPUT/NAME/`, including `NAME.trace`, the raw trace, and logs.

## Convert a Raw Trace

```bash
trace_tools/translator/StreamTranslate input.vout output.trace
```


## Citation

Yang S. P., Kim M., Nam S., et al. *Overcoming the Memory Wall with CXL-Enabled SSDs*. USENIX ATC, 2023. [Paper](https://www.usenix.org/conference/atc23/presentation/yang-shao-peng).
