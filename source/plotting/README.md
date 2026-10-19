# Generate Figures

Run from the repository root after installing `requirements.txt`.

```bash
# Generate all available figures
python3 source/plotting/plot_all.py --paper-data results/paper_data

# Select figures
python3 source/plotting/plot_all.py --paper-data results/paper_data --figures 2 3 4

# Change the output directory
python3 source/plotting/plot_all.py --paper-data results/paper_data \
  --output-dir "$PWD/figures"

# Export PDF only
python3 source/plotting/plot_all.py --paper-data results/paper_data --no-png
```

| Selection | Figure |
|---|---|
| 2 | Motivation: performance |
| 3 | Motivation: scope distribution and Moran's I |
| 4 | Motivation: temporal evolution |
| 7 | Average latency |
| 8 | P99 latency |
| 9 | Hit ratio |
| 10 | Admission distribution |
| 11 | Admission over time |
| extra | I/O amplification |

Edit `source/plotting/plot_common.py` to change colors, fonts, and layout.

To select a font, add `--font-family "DejaVu Sans"` or `--font /path/to/font.ttf`.

After generating the plotting inputs, redraw an individual figure with:

```bash
python3 source/plotting/fig07_average_latency/plot.py \
  --data-root results/figures/plot_inputs \
  --output-dir results/figures/fig07_average_latency
```

Editable CSVs are saved under `results/figures/plot_inputs/`.
