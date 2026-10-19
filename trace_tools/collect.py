#!/usr/bin/env python3
"""Recollect application traces, separately from the fixed reproduction inputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent
W = ROOT / 'workloads'
ALIASES = {name: name for name in (
    'OFLOW', 'DL', 'RT-SIFT', 'SA-128', 'SA-64', 'DLRM', 'RT-MSER', 'SSSP', 'RT-TRACK')}
INPUTS = {'RT-SIFT': 'rtb_sift_5600', 'RT-MSER': 'rtb_mser_5440',
          'RT-TRACK': 'rtb_tracking_6400'}

def command(alias, python):
    roi = ['--roi-library', str(W / 'libcachetrace_roi.so')]
    if alias == 'OFLOW': return [python, str(W / 'opencv_optflow_natural.py'), '--size', '2048', '--pairs', '2'], 'yes'
    if alias == 'DL': return [python, str(W / 'torch_resnet34_infer.py'), '--size', '896', '--batch', '2', '--rounds', '1'], 'yes'
    if alias in ('SA-128', 'SA-64'):
        rows, dim = ('1000000', '128') if alias == 'SA-128' else ('2000000', '64')
        return [python, str(W / 'torch_sparseadam_roi.py'), *roi, '--rows', rows, '--dim', dim,
                '--lookups', '8192', '--warmup-steps', '2', '--steps', '128'], 'no'
    if alias == 'DLRM':
        return [python, str(W / 'run_meta_dlrm_roi.py'), '--dlrm-script', str(ROOT / 'vendor/dlrm/dlrm_s_pytorch.py'),
                *roi, '--warmup-batches', '4', '--mini-batch-size=512', '--num-batches=164', '--data-generation=random',
                '--arch-mlp-bot=64-128-128', '--arch-sparse-feature-size=128', '--arch-embedding-size=2000000-2000000',
                '--num-indices-per-lookup=8', '--num-indices-per-lookup-fixed=True', '--arch-interaction-op=dot',
                '--arch-mlp-top=128-64-1', '--loss-function=bce', '--numpy-rand-seed=20260930', '--print-freq=164'], 'no'
    if alias == 'SSSP': return [str(ROOT / 'vendor/gapbs/sssp'), '-g', '22', '-k', '8', '-n', '1', '-d', '4'], 'yes'
    app = {'RT-SIFT': 'sift', 'RT-MSER': 'mser', 'RT-TRACK': 'tracking'}[alias]
    return [str(ROOT / f'vendor/rt-bench/vision/benchmarks/{app}/data/fullhd/{app}'),
            '-d', '10', '-p', '10', '-t', '1', '-l', '2', '-b', '.'], 'yes'

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--traces', nargs='+', choices=list(ALIASES), default=list(ALIASES))
    p.add_argument('--python', default=sys.executable)
    p.add_argument('--output', type=Path, default=ROOT.parent / 'recollected')
    p.add_argument('--dry-run', action='store_true')
    p.add_argument('--minimum-free-gib', type=float, default=40)
    a = p.parse_args(); out = a.output.resolve()
    if out == ROOT.parent / 'traces' or (ROOT.parent / 'traces') in out.parents:
        p.error('Never recollect into the bundled reproduction traces directory')
    vg = ROOT / 'vendor/valgrind_cachetrace/vg-in-place'
    translator = ROOT / 'translator/StreamTranslate'
    for alias in a.traces:
        name = ALIASES[alias]; dest = out / name
        app, start = command(alias, a.python)
        args = [str(vg), '--tool=callgrind', '--simulate-wb=yes', '--simulate-hwpref=yes',
                '--instr-atstart=' + start, '--callgrind-out-file=' + str(dest / 'callgrind.out'), *app]
        print(json.dumps({'workload': alias, 'cwd': str(dest), 'command': args}, ensure_ascii=False), flush=True)
        if a.dry_run: continue
        out.mkdir(parents=True, exist_ok=True)
        if shutil.disk_usage(out).free < a.minimum_free_gib * 1024**3:
            raise RuntimeError('Insufficient collection space; no existing data deleted')
        if dest.exists(): raise RuntimeError(f'Refusing to overwrite {dest}; use a new --output')
        if not vg.exists() or not translator.exists(): raise RuntimeError('Run trace_tools/build.py first')
        dest.mkdir()
        inputs = ROOT / 'inputs' / INPUTS.get(alias, name)
        if inputs.exists():
            for image in inputs.glob('*.bmp'): shutil.copy2(image, dest / image.name)
        env = dict(os.environ, OMP_NUM_THREADS='1', OPENBLAS_NUM_THREADS='1', MKL_NUM_THREADS='1',
                   NUMEXPR_NUM_THREADS='1', VECLIB_MAXIMUM_THREADS='1', PYTHONHASHSEED='0')
        metadata = {'alias': alias, 'trace': name, 'command': args, 'roi_start_at_program_start': start == 'yes',
                    'python': a.python, 'started_at': time.strftime('%FT%T%z'), 'not_byte_identical_to_archived_input': True}
        (dest / 'collection.json').write_text(json.dumps(metadata, indent=2))
        with (dest / 'application.log').open('w') as log, (dest / 'raw.vout').open('w') as raw:
            child = subprocess.Popen(args, cwd=dest, env=env, stdout=log, stderr=raw)
            try:
                while True:
                    try: rc = child.wait(timeout=30); break
                    except subprocess.TimeoutExpired:
                        if shutil.disk_usage(out).free < 2 * 1024**3: raise RuntimeError('Disk guard: collection stopped; partial data preserved')
                if rc: raise RuntimeError(f'Collection failed ({rc}); inspect {dest}')
            except BaseException:
                child.terminate()
                try: child.wait(timeout=10)
                except subprocess.TimeoutExpired: child.kill(); child.wait()
                raise
        trace = dest / (name + '.trace')
        with (dest / 'translation.log').open('w') as log:
            subprocess.run([str(translator), str(dest / 'raw.vout'), str(trace)], stdout=log, stderr=subprocess.STDOUT, check=True)
        h = hashlib.sha256(); count = 0
        with trace.open('rb') as stream:
            for line in stream:
                if len(line.split()) != 5: raise RuntimeError('Malformed translated trace')
                h.update(line); count += 1
        if count == 0: raise RuntimeError('Empty collection; do not use this trace')
        metadata.update(requests=count, trace_sha256=h.hexdigest(), completed=True)
        (dest / 'collection.json').write_text(json.dumps(metadata, indent=2))
        print('Completed', alias, count, 'requests', flush=True)

if __name__ == '__main__': main()
