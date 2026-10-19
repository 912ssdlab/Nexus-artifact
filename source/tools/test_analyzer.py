#!/usr/bin/env python3
"""Brute-force regression including a neighbor exactly 4096 events ahead."""
import csv
import math
from pathlib import Path
import random
import statistics
import subprocess
import tempfile

EXE = Path(__file__).resolve().parent/'feature_analyzer'
with tempfile.TemporaryDirectory(prefix='scope4096_regression_') as directory:
    root = Path(directory)
    random.seed(37)
    cases = [([(0, 0)] + [(1, 10)] * 4095 + [(0, 1)] + [(2, 30)] * 5),
             [(random.randrange(5), random.randrange(64)) for _ in range(250)],
             [(0, i) for i in range(64)]]
    for num, events in enumerate(cases):
        trace, summary, temporal = [root/(str(num)+suffix) for suffix in ('.trace', '.csv', '_bins.csv')]
        with trace.open('w') as stream:
            for i, (page, cl) in enumerate(events):
                stream.write(f'{i} 0 {page*4096+cl*64} 64 {i%2}\n')
        subprocess.run([str(EXE), str(num), str(trace), str(len(events)), str(summary), str(temporal)], check=True)
        active, spans, q, moran = [], [], [], []
        for i, (page, seed) in enumerate(events):
            mask = {cl for p, cl in events[i:i+4097] if p == page}
            left = right = seed
            while left-1 in mask:
                left -= 1
            while right+1 in mask:
                right += 1
            a, s = len(mask), right-left+1
            active.append(a)
            spans.append(s)
            q.append(s/a)
            if a != 64:
                x = [float(cl in mask) for cl in range(64)]
                mean = a/64
                moran.append(64/63 * sum((x[j]-mean)*(x[j+1]-mean) for j in range(63)) /
                             sum((v-mean)**2 for v in x))
        with summary.open() as stream:
            result = next(csv.DictReader(stream))
        for field, values in [('mean_active_cl', active), ('mean_seed_span', spans), ('mean_q', q),
                              ('moran_i_mean_standard', moran)]:
            assert math.isclose(float(result[field]), statistics.mean(values), abs_tol=1e-9), field
        for field, values in [('median_active_cl', active), ('median_seed_span', spans)]:
            assert float(result[field]) == sorted(values)[math.ceil(len(values)*.5)-1], field
        assert int(result['window']) == 4096
        if num == 0:
            assert active[0] == 2
print('PASS: brute-force A/S/Q/Moran; full-page handling; seed plus exactly next 4096')
