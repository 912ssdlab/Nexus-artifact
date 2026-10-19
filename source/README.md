# Run Experiments

Run these commands from the repository root after installing `requirements.txt`.

```bash
# Build
bash run.sh --build-only --build-jobs 4

# Preview commands
bash run.sh --dry-run

# Run all workloads
bash run.sh --output "$PWD/results"

# Select workloads, mechanisms, and cache sizes
bash run.sh --traces OFLOW DL RT-SIFT \
  --mechanisms Page Cacheline SkyByte Nexus Page-Optimal Cacheline-Optimal \
  --sizes 32 64 --output "$PWD/selected_results"

# Collect trace characteristics
bash run.sh --features-only --output "$PWD/features_results"

# Export tables from completed runs
bash run.sh --export-only --output "$PWD/results"

# Generate figures
python3 source/plotting/plot_all.py --paper-data results/paper_data
```

Repeat the original command to resume an interrupted run. Use a new output directory for a different selection.

Tables: `OUTPUT/paper_data/`. Figures: `OUTPUT/figures/`.

Add `--no-plots` to export tables without generating figures.
