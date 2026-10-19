#!/usr/bin/env python3
"""Build collection tools locally, without sudo or system installation."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent
def run(args, cwd=None): subprocess.run(args, cwd=cwd, check=True)
def main():
    p = argparse.ArgumentParser()
    p.add_argument('--jobs', type=int, default=2)
    p.add_argument('--components', nargs='+', choices=['valgrind', 'translator', 'gapbs', 'rtbench', 'roi'],
                   default=['valgrind', 'translator', 'gapbs', 'rtbench', 'roi'])
    a = p.parse_args()
    if a.jobs < 1: p.error('--jobs must be positive')
    vg = ROOT / 'vendor/valgrind_cachetrace'
    if 'valgrind' in a.components:
        if not (vg / 'configure').exists(): run(['sh', 'autogen.sh'], vg)
        if not (vg / 'Makefile').exists(): run(['sh', 'configure'], vg)
        run(['make', f'-j{a.jobs}'], vg)
    if 'translator' in a.components:
        run(['g++', '-std=c++17', '-O3', str(ROOT / 'translator/StreamTranslate.cpp'), '-o', str(ROOT / 'translator/StreamTranslate')])
    if 'gapbs' in a.components: run(['make', f'-j{a.jobs}', 'sssp'], ROOT / 'vendor/gapbs')
    if 'rtbench' in a.components:
        for app in ('sift', 'mser', 'tracking'):
            run(['make', 'compile'], ROOT / f'vendor/rt-bench/vision/benchmarks/{app}/data/fullhd')
    if 'roi' in a.components:
        run(['cc', '-shared', '-fPIC', '-O2', '-I' + str(vg / 'callgrind'), '-I' + str(vg / 'include'),
             str(ROOT / 'workloads/roi_control.c'), '-o', str(ROOT / 'workloads/libcachetrace_roi.so')])
if __name__ == '__main__': main()
