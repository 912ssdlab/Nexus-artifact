#!/usr/bin/env python3
"""Regenerate extra_io_amplification using newly prepared experiment data."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from plot_common import main

if __name__ == '__main__':
    main('extra_io_amplification')
