import argparse, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
MECHANISMS=['Page','Cacheline','SkyByte','Nexus','Page-Optimal','Cacheline-Optimal']
def build(mechanisms,jobs=2):
 for m in mechanisms:
  print('Building',m,flush=True)
  subprocess.run(['make',f'-j{jobs}'],cwd=ROOT/'source'/m,check=True)
 for tool in ('feature_analyzer','hot_share','read_hot_share','latency_stats'):
  src=ROOT/'source/tools'/f'{tool}.cpp';out=src.with_suffix('')
  if not out.exists() or src.stat().st_mtime>out.stat().st_mtime:
   subprocess.run(['g++','-std=c++17','-O3',str(src),'-o',str(out)],check=True)
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--jobs',type=int,default=2);p.add_argument('--mechanisms',nargs='+',choices=MECHANISMS,default=MECHANISMS)
 a=p.parse_args();build(a.mechanisms,a.jobs)
