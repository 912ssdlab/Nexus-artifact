"""Adapt newly generated paper_data; never import archived result values."""
import csv
import hashlib
import json
import math
from pathlib import Path

WORKLOADS = [
 ('OFLOW','OFLOW','OFLOW'),
 ('DL','DL','DL'),
 ('RT-SIFT','RT-SIFT','RT-SIFT'),
 ('RT-MSER','RT-MSER','RT-MSER'),
 ('RT-TRACK','RT-TRACK','RT-TRACK'),
 ('SA-64','SA-64','SA-64'),
 ('SA-128','SA-128','SA-128'),
 ('DLRM','DLRM','DLRM'),
 ('SSSP','SSSP','SSSP'),
]
MOTIVATION = [x for x in WORKLOADS if x[0] not in ('RT-MSER','RT-TRACK','SSSP')]
METHODS = {'Page':'Page','Cacheline':'Cacheline','skybyte':'SkyByte','mywork':'Nexus',
           'Optimal 1':'Page-Optimal','Optimal 2':'Cacheline-Optimal'}
FIGURES = {'2':'fig02_motivation_performance','3':'fig03_access_coverage','4':'fig04_access_temporal',
           '7':'fig07_average_latency','8':'fig08_p99_latency','9':'fig09_hit_ratio',
           '10':'fig10_admission_distribution','11':'fig11_admission_temporal','extra':'extra_io_amplification'}

def num(value, minimum=None, maximum=None):
    n=float(value)
    if not math.isfinite(n) or (minimum is not None and n<minimum) or (maximum is not None and n>maximum):
        raise ValueError(f'Invalid value {value!r}')
    return n

def integer(value):
    n=num(value,0)
    if not n.is_integer():raise ValueError(f'Non-integer count {value!r}')
    return int(n)

def indexed(rows,key):
    result={r[key]:r for r in rows}
    if len(result)!=len(rows):raise ValueError(f'Duplicate {key}')
    return result

def prepare(paper_data,dest):
    paper_data,dest=Path(paper_data).resolve(),Path(dest).resolve()
    completion=json.loads((paper_data/'completion.json').read_text())
    if dest==paper_data or dest in paper_data.parents:raise ValueError('Do not overwrite paper_data')
    report={'source':str(paper_data),'completion':completion,'ready':{},'skipped':{},'input_hashes':{},
            'notes':{'p99':'p99_us, not tm99_us',
              'admission_distribution':'Power-of-two masks only; Other excluded and reported separately',
              'admission_reference':'Trace future Active CL, not an optimal-policy result'}}
    dest.mkdir(parents=True,exist_ok=True)
    def load(relative):
        path=paper_data/relative
        with path.open(encoding='utf-8-sig',newline='') as f:rows=list(csv.DictReader(f))
        report['input_hashes'][relative]=hashlib.sha256(path.read_bytes()).hexdigest()
        return rows
    for figure,directory in FIGURES.items():
        files={}
        try:
            if figure in ('2','7','8','9','extra'):
                metrics={'2':['mean_us','hit_ratio'],'7':['mean_us'],'8':['p99_us'],
                         '9':['hit_ratio'],'extra':['io_amplification_4k_equiv']}[figure]
                subset=MOTIVATION if figure=='2' else WORKLOADS
                methods={'Page':'Page','Cacheline':'Cacheline'} if figure=='2' else METHODS
                folder='Motivation1' if figure=='2' else 'Main_Experiments'
                for metric in metrics:
                    for cap in ([64] if figure=='2' else [32,64]):
                        rows=indexed(load(f'{folder}/{cap}MB_{metric}.csv'),'Workload')
                        data=[{'Workload':label,**{old:num(rows[a][new],0,1 if metric=='hit_ratio' else None)
                              for old,new in methods.items()}} for a,label,_ in subset]
                        if metric!='hit_ratio' and any(r['Page']<=0 for r in data):raise ValueError('Zero Page denominator')
                        if figure=='extra':
                            rename={'Page':'Page','Cacheline':'Cacheline','SkyByte':'skybyte','Mywork':'mywork',
                                    'Page-OPT':'Optimal 1','Cacheline-OPT':'Optimal 2'}
                            data=[{'Workload':r['Workload'],**{new:r[old]/r['Page'] for new,old in rename.items()}} for r in data]
                        files[f'{cap}MB_{metric}.csv' if figure=='2' else f'{cap}MB.csv']=data
            elif figure=='3':
                rows=indexed(load('Motivation2/scope_moran_plot_ready.csv'),'Workload');data=[]
                for a,label,_ in MOTIVATION:
                    r=rows[a];fractions=[num(r[k],0,1) for k in ('Small','Medium','Large')]
                    if not math.isclose(sum(fractions),1,abs_tol=1e-8):raise ValueError('Scope fractions do not sum to 1')
                    moran=num(r['Morans_I']) if r['Morans_I'] else float('nan')
                    data.append(dict(Workload=label,Small_1_4CL=fractions[0],Medium_5_16CL=fractions[1],Large_17_64CL=fractions[2],Moran_I=moran))
                files['coverage.csv']=data
            elif figure=='4':
                for _,_,trace in MOTIVATION:
                    records=load(f'Motivation3/{trace}_coverage_temporal.csv')
                    if len(records)!=100:raise ValueError(f'Expected 100 temporal bins: {trace}')
                    files[trace+'.csv']=[{'progress_pct':num(r['Progress_percent'],0,100),
                       'mean_active_cl':num(r['Mean_active_CL'],1,64),
                       'global_mean_active_cl':num(r['Trace_mean_active_CL'],1,64)} for r in records]
            elif figure=='10':
                rows=load('Main_Experiments/Nexus_Admission/granularity_distribution.csv');data=[];excluded=[]
                for cap in (32,64):
                    for a,_,_ in WORKLOADS:
                        selected=[r for r in rows if r['Workload']==a and int(r['Cache_MiB'])==cap]
                        r=indexed(selected,'Selected_CL')
                        counts={f'c{g}':integer(r[str(g)]['Admission_count']) for g in (1,2,4,8,16,32,64)}
                        other=integer(r['Other']['Admission_count']);total=sum(counts.values())+other
                        if sum(counts.values())==0:raise ValueError('No power-of-two admissions')
                        data.append({'workload':a,'cache_mb':cap,**counts,'other_non_power_of_two':other})
                        excluded.append({'workload':a,'cache_mb':cap,'fraction':other/total})
                files['admission_counts.csv']=data;report['excluded_fraction']=excluded
            elif figure=='11':
                rows=load('Main_Experiments/Nexus_Admission/temporal_alignment_long.csv')
                for cap in (32,64):
                    for a,_,trace in WORKLOADS:
                        selected=sorted([r for r in rows if r['Workload']==a and int(r['Cache_MiB'])==cap],key=lambda r:int(r['bin']))
                        if [int(r['bin']) for r in selected]!=list(range(100)):raise ValueError(f'Missing or duplicate bins: {a} {cap}')
                        files[f'{trace}_{cap}MB.csv']=[{'bin':int(r['bin']),'progress_pct':int(r['bin'])+.5,
                           'all_access_count':integer(r['all_access_count']),'admission_count':integer(r['admission_count']),
                           'trace_mean_active_CL':num(r['mean_future_active_cl_all_accesses'],1,64),
                           'mean_logical_selected_CL':num(r['mean_logical_selected_cl'],0,64) if integer(r['admission_count']) else float('nan')}
                           for r in selected]
                for _,_,trace in WORKLOADS:
                    for r,s in zip(files[f'{trace}_32MB.csv'],files[f'{trace}_64MB.csv']):
                        if not math.isclose(r['trace_mean_active_CL'],s['trace_mean_active_CL'],abs_tol=1e-8,rel_tol=0):
                            raise ValueError(f'Trace curves differ: {trace}')
            for name,records in files.items():
                path=dest/directory/'data'/name;path.parent.mkdir(parents=True,exist_ok=True)
                with path.open('w',encoding='utf-8-sig',newline='') as f:
                    w=csv.DictWriter(f,fieldnames=list(records[0]));w.writeheader();w.writerows(records)
            report['ready'][figure]=directory
        except (OSError,ValueError,KeyError,TypeError) as e:
            report['skipped'][figure]=f'{type(e).__name__}: {e}'
    (dest/'preparation.json').write_text(json.dumps(report,ensure_ascii=False,indent=2))
    return report

if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser();p.add_argument('--paper-data',required=True,type=Path);p.add_argument('--output',required=True,type=Path)
    a=p.parse_args();print(json.dumps(prepare(a.paper_data,a.output),ensure_ascii=False,indent=2))
