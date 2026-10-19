#!/usr/bin/env python3
"""Synthetic schema/render tests; temporary data are NOT experimental results."""
import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from prepare_data import WORKLOADS, MOTIVATION, METHODS, FIGURES, prepare

def write(path,rows):
    path.parent.mkdir(parents=True,exist_ok=True)
    with path.open('w',encoding='utf-8-sig',newline='') as f:
        w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)

def main():
    with tempfile.TemporaryDirectory(prefix='nexus_plot_schema_test_') as directory:
        base=Path(directory);paper=base/'paper_data';paper.mkdir()
        (paper/'completion.json').write_text(json.dumps({'smoke':True,'all_simulations_complete':False}))
        for cap in (32,64):
            for metric in ('mean_us','p99_us','hit_ratio','io_amplification_4k_equiv'):
                rows=[]
                for i,(alias,_,_) in enumerate(reversed(WORKLOADS)):
                    factor=.01 if metric=='hit_ratio' else 1.0
                    rows.append({'Workload':alias,**{m:(10+i+j)*factor for j,m in enumerate(METHODS.values())}})
                write(paper/f'Main_Experiments/{cap}MB_{metric}.csv',rows)
                if cap==64 and metric in ('mean_us','hit_ratio'):
                    write(paper/f'Motivation1/{cap}MB_{metric}.csv',[r for r in rows if r['Workload'] in {a for a,_,_ in MOTIVATION}])
        write(paper/'Motivation2/scope_moran_plot_ready.csv',[{'Workload':a,'Small':.2,'Medium':.3,'Large':.5,
              'Morans_I':-.1 if a=='DL' else .6} for a,_,_ in reversed(MOTIVATION)])
        for a,_,trace in MOTIVATION:
            write(paper/f'Motivation3/{trace}_coverage_temporal.csv',[{'Progress_percent':i+.5,'Mean_active_CL':10+i%20,
                  'Trace_mean_active_CL':19.5} for i in range(100)])
        distributions=[];curves=[]
        for a,_,_ in WORKLOADS:
            for cap in (32,64):
                for g in (1,2,4,8,16,32,64,'Other'):
                    distributions.append({'Workload':a,'Cache_MiB':cap,'Selected_CL':g,'Admission_count':2})
                for i in range(100):
                    curves.append({'Workload':a,'Cache_MiB':cap,'bin':i,'all_access_count':10,
                                   'admission_count':0 if i==5 else 3,
                                   'mean_future_active_cl_all_accesses':10+i%20,
                                   'mean_logical_selected_cl':12+i%20+(cap==64)})
        write(paper/'Main_Experiments/Nexus_Admission/granularity_distribution.csv',distributions)
        write(paper/'Main_Experiments/Nexus_Admission/temporal_alignment_long.csv',curves)
        dest=base/'inputs';report=prepare(paper,dest)
        assert set(report['ready'])==set(FIGURES) and not report['skipped']
        assert all(r['fraction']==.125 for r in report['excluded_fraction'])
        with (dest/'fig07_average_latency/data/32MB.csv').open(encoding='utf-8-sig') as f:
            first=next(csv.DictReader(f));assert first['Workload']=='OFLOW' and float(first['Page'])==18
        with (dest/'fig11_admission_temporal/data/OFLOW_32MB.csv').open(encoding='utf-8-sig') as f:
            r=list(csv.DictReader(f));assert r[5]['mean_logical_selected_CL']=='nan'
        script=Path(__file__).resolve().parent/'plot_all.py'
        subprocess.run([sys.executable,str(script),'--paper-data',str(paper),'--output-dir',str(base/'rendered'),
                        '--font-family','DejaVu Sans','--no-png'],check=True)
        assert len(list((base/'rendered').glob('*/*.pdf')))==9
        (paper/'Main_Experiments/64MB_p99_us.csv').unlink()
        report=prepare(paper,dest)
        assert '8' in report['skipped'] and '8' not in report['ready']
        p=subprocess.run([sys.executable,str(script),'--paper-data',str(paper),'--figures','8',
                          '--output-dir',str(base/'rendered')],capture_output=True,text=True)
        assert p.returncode!=0 and 'Missing/invalid inputs' in p.stderr
        print('PASS: name mapping, normalization input, conditional distribution, zero-admission gaps, 9 renders, explicit missing-data failure')

if __name__=='__main__':main()
