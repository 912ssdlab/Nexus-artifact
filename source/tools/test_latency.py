#!/usr/bin/env python3
"""Exact nearest-rank percentiles and slow-tail means, including ties/small N."""
import json
import math
from pathlib import Path
import random
import subprocess

exe = Path(__file__).resolve().parent / 'latency_stats'
rng = random.Random(11)
for n in (1, 2, 3, 7, 10, 100, 10001):
    values = sorted(rng.randrange(100) * 1000 for _ in range(n))
    data = ''.join(str(v) + '\n' for v in values)
    r = json.loads(subprocess.check_output([str(exe), str(n)], input=data, text=True))
    assert int(r['sum_ns']) == sum(values)
    assert math.isclose(r['mean_us'], sum(values)/n/1000, abs_tol=1e-10)
    for name, numerator in [('90',9000),('95',9500),('99',9900),('99_9',9990),('99_99',9999)]:
        rank = (n * numerator + 9999)//10000
        count = (n * (10000-numerator) + 9999)//10000
        assert r['p'+name+'_us'] == values[rank-1]/1000
        assert math.isclose(r['tm'+name+'_us'], sum(values[-count:])/count/1000, abs_tol=1e-10)
for data, n in [('3\n2\n',2),('2\n',2),('1\n2\n',1),('-1\n',1)]:
    assert subprocess.run([str(exe), str(n)], input=data, text=True, capture_output=True).returncode != 0
print('PASS: exact tail means, percentiles, count and sorted-input validation')
