#!/usr/bin/env python3
"""Steady-state sparse embedding training with PyTorch SparseAdam."""

from __future__ import annotations

import argparse
import os

os.environ.setdefault("OMP_NUM_THREADS", "1")
os.environ.setdefault("MKL_NUM_THREADS", "1")

import torch

from roi_control import ROIControl


def train_step(table, optimizer, rows, lookups, generator):
    indices = torch.randint(0, rows, (lookups,), generator=generator, dtype=torch.long)
    optimizer.zero_grad(set_to_none=True)
    output = table(indices)
    loss = output.square().mean() + output[:, ::8].mean()
    loss.backward()
    optimizer.step()
    return loss


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--roi-library", required=True)
    parser.add_argument("--rows", type=int, default=2_000_000)
    parser.add_argument("--dim", type=int, default=32)
    parser.add_argument("--lookups", type=int, default=8192)
    parser.add_argument("--steps", type=int, default=128)
    parser.add_argument("--warmup-steps", type=int, default=2)
    args = parser.parse_args()

    torch.set_num_threads(1)
    torch.manual_seed(20260914)
    table = torch.nn.Embedding(args.rows, args.dim, sparse=True)
    optimizer = torch.optim.SparseAdam(table.parameters(), lr=1.0e-3)
    generator = torch.Generator().manual_seed(915)

    checksum = 0.0
    for _ in range(args.warmup_steps):
        loss = train_step(table, optimizer, args.rows, args.lookups, generator)
        checksum += float(loss.detach())

    roi = ROIControl(args.roi_library)
    roi.start()
    print("[roi-start] sparseadam", flush=True)
    for _ in range(args.steps):
        loss = train_step(table, optimizer, args.rows, args.lookups, generator)
        checksum += float(loss.detach())
    print("[roi-stop] sparseadam", flush=True)
    roi.stop()
    print(f"checksum={checksum:.9e}")


if __name__ == "__main__":
    main()

