#!/usr/bin/env python3
"""Prepare new experiment outputs and render eligible manuscript figures."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
from prepare_data import FIGURES, prepare

ROOT=Path(__file__).resolve().parent

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--paper-data',required=True,type=Path,help='OUTPUT/paper_data from run.sh')
    p.add_argument('--figures',nargs='+',choices=list(FIGURES))
    p.add_argument('--output-dir',type=Path,help='Default: OUTPUT/figures')
    p.add_argument('--font')
    p.add_argument('--font-family',default='Calibri')
    p.add_argument('--no-png',action='store_true')
    a=p.parse_args()
    out=(a.output_dir or a.paper_data.resolve().parent/'figures').resolve()
    report=prepare(a.paper_data,out/'plot_inputs')
    render_report={'rendered':[],'skipped':report['skipped'],'source':report['source'],
                   'source_completion':report['completion'],'historical_results_used':False,'status':'preparing'}
    def save_report():
        (out/'render_report.json').write_text(json.dumps(render_report,ensure_ascii=False,indent=2))
    save_report()
    if a.figures:
        missing={k:report['skipped'][k] for k in a.figures if k not in report['ready']}
        if missing:
            render_report['status']='missing_requested_inputs';save_report()
            p.error('Missing/invalid inputs: '+json.dumps(missing,ensure_ascii=False))
    for k,reason in report['skipped'].items():print(f'SKIP Fig.{k}: {reason}',flush=True)
    rendered=[]
    for number,directory in report['ready'].items():
        if a.figures and number not in a.figures:continue
        cmd=[sys.executable,str(ROOT/directory/'plot.py'),'--data-root',str(out/'plot_inputs'),'--output-dir',str(out/directory),
             '--font-family',a.font_family]
        if a.font:cmd+=['--font',str(Path(a.font).expanduser().resolve())]
        if a.no_png:cmd+=['--no-png']
        if report['completion'].get('smoke'):cmd+=['--smoke-watermark']
        try:subprocess.run(cmd,check=True)
        except subprocess.CalledProcessError:
            render_report['status']='render_failed';render_report['failed_figure']=number;save_report();raise
        rendered.append(number)
        render_report['rendered']=rendered;save_report()
    render_report['status']='complete_for_available_inputs';save_report()
    print('Figures:',out,'; generated:',', '.join(rendered) or 'none (incomplete inputs)',flush=True)

if __name__=='__main__':main()
