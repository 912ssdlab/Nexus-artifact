#!/usr/bin/env python3
"""Dense OpenCV optical-flow pipeline using the standard Farneback algorithm."""

import argparse
import os

os.environ.setdefault("OMP_NUM_THREADS", "1")

import cv2
import numpy as np


def make_frame(size: int, shift_x: int, shift_y: int) -> np.ndarray:
    y, x = np.indices((size, size), dtype=np.float32)
    base = (
        96.0
        + 48.0 * np.sin((x + shift_x) * 0.011)
        + 44.0 * np.cos((y + shift_y) * 0.013)
        + 32.0 * np.sin((x + y + shift_x + shift_y) * 0.007)
    )
    return np.clip(base, 0, 255).astype(np.uint8)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--size", type=int, default=2048)
    parser.add_argument("--pairs", type=int, default=2)
    args = parser.parse_args()

    cv2.setNumThreads(1)
    previous = make_frame(args.size, 0, 0)
    checksum = 0.0
    for pair_id in range(args.pairs):
        current = make_frame(args.size, 5 * (pair_id + 1), 3 * (pair_id + 1))
        flow = cv2.calcOpticalFlowFarneback(
            previous,
            current,
            None,
            pyr_scale=0.5,
            levels=4,
            winsize=21,
            iterations=4,
            poly_n=7,
            poly_sigma=1.5,
            flags=0,
        )
        magnitude, angle = cv2.cartToPolar(flow[..., 0], flow[..., 1])
        checksum += float(magnitude[::127, ::131].sum(dtype=np.float64))
        checksum += float(angle[::137, ::139].sum(dtype=np.float64))
        previous = current
    print(f"checksum={checksum:.9e}")


if __name__ == "__main__":
    main()

