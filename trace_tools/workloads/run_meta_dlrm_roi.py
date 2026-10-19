#!/usr/bin/env python3
"""Run upstream Meta DLRM and enable Cachetrace at a training batch boundary.

Only optional TensorBoard logging is stubbed.  Model/data construction and the
training implementation remain in the upstream script.  A Python line hook is
active only before the ROI and removes itself immediately after issuing the
Callgrind start request.
"""

from __future__ import annotations

import argparse
import runpy
import sys
import types
from pathlib import Path

from roi_control import ROIControl


class _UnusedSummaryWriter:
    def __init__(self, *args, **kwargs):
        pass

    def add_scalar(self, *args, **kwargs):
        pass


def main() -> None:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--dlrm-script", required=True)
    parser.add_argument("--roi-library", required=True)
    parser.add_argument("--warmup-batches", type=int, default=4)
    known, remaining = parser.parse_known_args()

    dlrm_script = Path(known.dlrm_script).resolve()
    roi = ROIControl(known.roi_library)
    started = False

    def start_at_batch(frame, event, arg):
        nonlocal started
        if (
            not started
            and event == "line"
            and frame.f_code.co_name == "run"
            and Path(frame.f_code.co_filename).resolve() == dlrm_script
            and "inputBatch" in frame.f_locals
            and frame.f_locals.get("j") == known.warmup_batches
        ):
            roi.start()
            started = True
            print(f"[roi-start] meta-dlrm batch={known.warmup_batches}", flush=True)
            sys.settrace(None)
            return None
        return start_at_batch

    optional = types.ModuleType("torch.utils.tensorboard")
    optional.SummaryWriter = _UnusedSummaryWriter
    sys.modules["torch.utils.tensorboard"] = optional
    sys.path.insert(0, str(dlrm_script.parent))
    sys.argv = [str(dlrm_script), *remaining]
    sys.settrace(start_at_batch)
    runpy.run_path(str(dlrm_script), run_name="__main__")
    if not started:
        raise RuntimeError("Meta-DLRM ROI start batch was never reached")


if __name__ == "__main__":
    main()

