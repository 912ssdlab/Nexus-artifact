#!/usr/bin/env python3
"""Isolated, resumable reproduction; never reads archived experimental results."""
import argparse
import csv
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
import xml.etree.ElementTree as ET
from build import ROOT, MECHANISMS, build

TOOLS = ROOT / 'source/tools'
FIELDS = {
 'overall_mean_us': 'Average host request response time (us)',
 'read_mean_us': 'Average host read response time (us)',
 'write_mean_us': 'Average host write response time (us)',
 'hit_ratio': 'Cache Hit ratio', 'read_hit_ratio': 'Read Hit ratio',
 'write_hit_ratio': 'Write Hit ratio', 'flash_read_count': 'Flash Read Count',
 'flush_count': 'Flush count', 'requests': 'total requests',
}

def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(8 << 20), b''): h.update(block)
    return h.hexdigest()

def save(path, value):
    path = Path(path)
    tmp = path.with_suffix(path.suffix + '.tmp')
    tmp.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')
    tmp.replace(path)

def field(lines, label):
    for line in lines:
        if line.startswith(label + ':'):
            return line.split(':', 1)[1].strip()
    raise ValueError(f'Missing overall field: {label}')

def configure(t, mechanism, size, run):
    source = ROOT / 'source' / mechanism
    run.mkdir(parents=True, exist_ok=True)
    (run / 'Results').mkdir(exist_ok=True)
    config = (source / 'config.txt').read_text()
    config = re.sub(r'^DRAM_size .*$', f'DRAM_size {size * 1024**2}', config, flags=re.M)
    config = re.sub(r'^Total_number_of_requests .*$', f'Total_number_of_requests {t["requests"]}', config, flags=re.M)
    (run / 'config.txt').write_text(config)
    tree = ET.parse(source / 'workload.xml')
    tree.find('.//File_Path').text = str(t['input_path'])
    tree.write(run / 'workload.xml', encoding='us-ascii', xml_declaration=True)
    shutil.copy2(source / 'ssdconfig.xml', run / 'ssdconfig.xml')
    env = {k: v for k, v in os.environ.items() if not k.startswith(('CXL_', 'MQSIM_OPTIMAL_', 'MQSIM_CL_OPTIMAL_'))}
    env.update(CXL_ENABLE_LATENCY_LOG='1', CXL_ENABLE_GRANULARITY_TIMELINE='1',
               CXL_GRANULARITY_TIMELINE_FILE=str(run / 'granularity_timeline.csv'),
               CXL_GRANULARITY_FUTURE_ALIGNMENT_FILE=str(run / 'granularity_future_alignment.csv'),
               OMP_NUM_THREADS='1', OPENBLAS_NUM_THREADS='1', MKL_NUM_THREADS='1', LC_ALL='C')
    return [str(source / 'MQSim'), '-i', str(run / 'ssdconfig.xml'), '-w', str(run / 'workload.xml')], env

def simulate(command, env, run):
    with (run / 'mqsim.log').open('w') as log:
        p = subprocess.Popen(command, cwd=run, env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        start = time.monotonic()
        try:
            while True:
                try:
                    rc = p.wait(timeout=30)
                    break
                except subprocess.TimeoutExpired:
                    if shutil.disk_usage(run).free < 1024**3: raise RuntimeError('Less than 1 GiB free; stopping this child only')
                    if time.monotonic() - start > 48 * 3600: raise RuntimeError('48 hour job timeout')
            if rc: raise RuntimeError(f'Simulator exit {rc}; see {run / "mqsim.log"}')
        except BaseException:
            p.terminate()
            try: p.wait(timeout=10)
            except subprocess.TimeoutExpired: p.kill(); p.wait()
            raise

def latency_stats(raw, count, run):
    with (run / 'sort.stderr').open('w') as err:
        p = subprocess.Popen(['sort', '-n', '--parallel=1', '-S', '256M', '-T', str(run), str(raw)],
                             stdout=subprocess.PIPE, stderr=err, env={**os.environ, 'LC_ALL': 'C'})
        try:
            q = subprocess.run([str(TOOLS / 'latency_stats'), str(count)], stdin=p.stdout, capture_output=True, text=True)
            p.stdout.close()
            rc = p.wait()
            if rc or q.returncode: raise RuntimeError(f'Latency reduction failed: sort={rc}; {q.stderr}')
            return json.loads(q.stdout)
        finally:
            if p.poll() is None: p.terminate(); p.wait()

def collect(t, mechanism, size, out, keep):
    base = out / 'runs' / t['trace'] / f'{mechanism}-{size}MB'
    source = ROOT / 'source' / mechanism
    signature = {'trace_sha256': t['trace_sha256'], 'requests': t['requests'], 'mechanism': mechanism,
                 'cache_mb': size, 'smoke': t.get('smoke', False),
                 'files': {n: digest(source / n) for n in ('MQSim', 'config.txt', 'ssdconfig.xml', 'workload.xml')},
                 'runner_sha256': digest(__file__), 'latency_stats_sha256': digest(TOOLS / 'latency_stats')}
    result_file = base / 'result.json'
    if result_file.exists():
        previous = json.loads(result_file.read_text())
        if previous['signature'] != signature:
            raise RuntimeError(f'Changed input/code/config at {base}; use a NEW --output directory')
        print('Resume: already complete', t['alias'], mechanism, size, flush=True)
        return
    # Never overwrite failed/interrupted evidence. Each retry has its own directory.
    base.mkdir(parents=True, exist_ok=True)
    attempt = 1
    while (base / f'attempt-{attempt}').exists(): attempt += 1
    run = base / f'attempt-{attempt}'
    free = shutil.disk_usage(out).free
    required = t['requests'] * 36 + 2 * 1024**3
    if free < required: raise RuntimeError(f'Need about {required / 1024**3:.1f} GiB scratch free for this trace; have {free / 1024**3:.1f}')
    cmd, env = configure(t, mechanism, size, run)
    save(run / 'run_metadata.json', signature)
    print('Run:', t['alias'], mechanism, size, 'MiB', flush=True)
    simulate(cmd, env, run)
    files = list((run / 'Results').glob('*_overall.txt'))
    if len(files) != 1: raise RuntimeError(f'Expected one overall file at {run}')
    lines = files[0].read_text().splitlines()
    metrics = {k: float(field(lines, v)) for k, v in FIELDS.items()}
    metrics['requests'] = int(metrics['requests'])
    if metrics['requests'] != t['requests']: raise RuntimeError('Incomplete request count')
    for k in ('hit_ratio', 'read_hit_ratio', 'write_hit_ratio'):
        if not math.isfinite(metrics[k]) or not 0 <= metrics[k] <= 1: raise RuntimeError(f'Invalid {k}')
    raws = list((run / 'Results').glob('*_latency_result.txt'))
    if len(raws) != 1: raise RuntimeError('Per-request latency file missing or ambiguous')
    metrics.update(latency_stats(raws[0], t['requests'], run))
    if not math.isclose(metrics['mean_us'], metrics['overall_mean_us'], rel_tol=6e-6, abs_tol=1e-6):
        raise RuntimeError('Latency-log mean does not match overall')
    metrics['io_events_per_request'] = (metrics['flash_read_count'] + metrics['flush_count']) / t['requests']
    metrics['io_amplification_4k_equiv'] = 64 * metrics['io_events_per_request']
    granularity = {}
    for line in lines:
        if line.startswith(('Admission ', 'Metadata ', 'Cacheline metadata', 'Dynamic data')) and ':' in line:
            k, v = line.split(':', 1); granularity[k] = v.strip()
    result = {'signature': signature, 'trace': t['trace'], 'alias': t['alias'], 'mechanism': mechanism,
              'cache_mb': size, 'metrics': metrics, 'admission_fields': granularity,
              'run_directory': str(run.relative_to(out)), 'raw_latency_sha256': digest(raws[0]),
              'raw_latency_retained': keep, 'sampling': 'none'}
    save(result_file, result)
    if not keep:
        # Only this invocation's new log, after exact statistics and completion validation.
        raws[0].unlink()

def characterize(t, out):
    dest = out / 'features' / t['trace']
    dest.mkdir(parents=True, exist_ok=True)
    stamp = {'trace_sha256': t['trace_sha256'], 'requests': t['requests'],
             'analyzer_sha256': digest(TOOLS / 'feature_analyzer'),
             'hot_share_sha256': digest(TOOLS / 'hot_share'),
             'read_hot_share_sha256': digest(TOOLS / 'read_hot_share')}
    if (dest / 'complete.json').exists() and json.loads((dest / 'complete.json').read_text()) == stamp: return
    print('Full-trace characterization:', t['alias'], flush=True)
    subprocess.run([str(TOOLS / 'feature_analyzer'), t['trace'], str(t['input_path']), str(t['requests']),
                    str(dest / 'summary.csv'), str(dest / 'temporal.csv')], check=True)
    with (dest / 'summary.csv').open() as f: r = next(csv.DictReader(f))
    if int(r['accesses']) != t['requests'] or r['count_match'] not in ('1', 'true', 'yes'): raise RuntimeError('Feature count mismatch')
    for tool in ('hot_share', 'read_hot_share'):
        result = subprocess.run([str(TOOLS / tool), str(t['input_path']), str(t['requests'])],
                                check=True, capture_output=True, text=True)
        save(dest / (tool + '.json'), json.loads(result.stdout))
    save(dest / 'complete.json', stamp)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, help='New output directory; default results/ or smoke_results/')
    p.add_argument('--traces', nargs='+', help='Aliases (OFLOW DL SIFT SA-128 SA-64 DLRM MSER SSSP TRACK) or full names')
    p.add_argument('--mechanisms', nargs='+', choices=MECHANISMS, default=MECHANISMS)
    p.add_argument('--sizes', nargs='+', type=int, choices=[32, 64], default=[32, 64])
    p.add_argument('--build-jobs', type=int, default=2)
    p.add_argument('--smoke', action='store_true', help='Use ONLY a 100,000-request DLRM prefix by default; not paper results')
    p.add_argument('--dry-run', action='store_true')
    p.add_argument('--build-only', action='store_true')
    p.add_argument('--features-only', action='store_true')
    p.add_argument('--export-only', action='store_true')
    p.add_argument('--keep-latencies', action='store_true', help='Keep newly produced raw latency logs (large disk requirement)')
    p.add_argument('--no-plots', action='store_true', help='Export data only; skip automatic manuscript figures')
    a = p.parse_args()
    traces = json.loads((ROOT / 'traces/manifest.json').read_text())
    chosen = a.traces or (['DLRM'] if a.smoke else [t['alias'] for t in traces])
    unknown = set(chosen) - {x for t in traces for x in (t['alias'], t['trace'])}
    if unknown: p.error(f'Unknown workloads: {sorted(unknown)}')
    traces = [t for t in traces if t['alias'] in chosen or t['trace'] in chosen]
    if len(a.mechanisms) != len(set(a.mechanisms)) or len(a.sizes) != len(set(a.sizes)): p.error('Duplicate mechanisms/sizes')
    out = (a.output or ROOT / ('smoke_results' if a.smoke else 'results')).resolve()
    if a.dry_run:
        print(json.dumps({'output': str(out), 'workloads': [t['alias'] for t in traces], 'mechanisms': a.mechanisms,
                          'cache_mib': a.sizes, 'jobs': len(traces)*len(a.sizes)*len(a.mechanisms),
                          'smoke': a.smoke, 'optimal_windows': {'Page': 262144, 'Cacheline': 524288},
                          'historical_results_used': False}, indent=2)); return
    if a.build_jobs < 1: p.error('--build-jobs must be positive')
    if not a.no_plots and not a.build_only:
        from importlib.util import find_spec
        for module in ('matplotlib', 'numpy'):
            if find_spec(module) is None: p.error(f'Missing {module}; install requirements.txt or use --no-plots')
    import openpyxl  # fail before a long experiment if the exporter dependency is missing
    from export import export
    if a.build_only: build(a.mechanisms, a.build_jobs); return
    out.mkdir(parents=True, exist_ok=True)
    with (out / '.run.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        manifest_path = out / 'run_manifest.json'
        if a.export_only:
            if not manifest_path.exists(): p.error('No run manifest in this output directory')
            export(out)
            if not a.no_plots: plot(out)
            return
        build(a.mechanisms, a.build_jobs)
        for t in traces:
            path = ROOT / t['path']
            if not path.is_file() or path.stat().st_size != t['bytes'] or digest(path) != t['trace_sha256']:
                raise RuntimeError(f'Missing/changed input: {path}')
            if a.smoke:
                prefix = out / 'inputs' / (t['trace'] + '.trace'); prefix.parent.mkdir(exist_ok=True)
                with path.open('rb') as src, prefix.open('wb') as dst:
                    for _ in range(min(100000, t['requests'])): dst.write(next(src))
                path = prefix; t['requests'] = min(100000, t['requests']); t['trace_sha256'] = digest(path)
                t['smoke'] = True
            t['input_path'] = str(path)
        manifest = {'traces': traces, 'mechanisms': a.mechanisms, 'cache_sizes_mib': a.sizes,
                    'expected_jobs': len(traces)*len(a.mechanisms)*len(a.sizes), 'smoke': a.smoke,
                    'features_only': a.features_only, 'historical_results_used': False}
        if manifest_path.exists() and json.loads(manifest_path.read_text()) != manifest:
            raise RuntimeError('Output manifest differs; choose a new --output directory')
        save(manifest_path, manifest)
        for t in traces: characterize(t, out)
        export(out)
        if not a.features_only:
            for t in traces:
                for size in a.sizes:
                    for m in a.mechanisms:
                        collect(t, m, size, out, a.keep_latencies)
                        export(out)
        print('Generated data:', out / 'paper_data', flush=True)
        if not a.no_plots: plot(out)

def plot(out):
    import sys
    subprocess.run([sys.executable, str(ROOT / 'source/plotting/plot_all.py'),
                    '--paper-data', str(out / 'paper_data')], check=True)

if __name__ == '__main__': main()
