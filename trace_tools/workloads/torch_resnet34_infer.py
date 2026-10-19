#!/usr/bin/env python3
"""Standard torchvision ResNet-34 inference."""

import argparse
import os

os.environ.setdefault("OMP_NUM_THREADS", "1")
os.environ.setdefault("MKL_NUM_THREADS", "1")
os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")

import torch
from torchvision.models import resnet34


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--size", type=int, default=896)
    parser.add_argument("--batch", type=int, default=2)
    parser.add_argument("--rounds", type=int, default=1)
    args = parser.parse_args()

    torch.set_num_threads(1)
    torch.manual_seed(20260915)
    model = resnet34(weights=None).eval()
    image = torch.randn(args.batch, 3, args.size, args.size)
    checksum = 0.0
    with torch.no_grad():
        for _ in range(args.rounds):
            output = model(image)
            checksum += float(output.sum())
    print(f"checksum={checksum:.9e}")


if __name__ == "__main__":
    main()
