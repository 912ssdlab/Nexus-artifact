"""Shared data-driven plotting for the archived Nexus manuscript figures.

All inputs are read from the selected figure's data/ directory. Nothing is read
from Overleaf, git history, a temporary directory, or a reference PDF.
"""
import argparse
import csv
import json
import os
from pathlib import Path
import tempfile
import warnings

os.environ.setdefault('MPLCONFIGDIR', str(Path(tempfile.gettempdir())/'nexus-matplotlib'))
import matplotlib
matplotlib.use('Agg')
import matplotlib.font_manager as fm
import matplotlib.pyplot as plt
from matplotlib.collections import LineCollection
from matplotlib.legend_handler import HandlerPatch
from matplotlib.lines import Line2D
from matplotlib.patches import Patch, Rectangle
from matplotlib.ticker import FormatStrFormatter
import numpy as np

ROOT=Path(__file__).resolve().parent
WORKLOADS=['OFLOW','DL','RT-SIFT','RT-MSER','RT-TRACK','SA-64','SA-128','DLRM','SSSP']
MOTIVATION=['OFLOW','DL','RT-SIFT','SA-64','SA-128','DLRM']
RAW_NAMES=['OFLOW','DL','RT-SIFT','RT-MSER','RT-TRACK',
           'SA-64','SA-128','DLRM','SSSP']
TRACE_NAMES=['OFLOW','DL','RT-SIFT',
             'RT-MSER','RT-TRACK','SA-64',
             'SA-128','DLRM','SSSP']
RAW=dict(zip(WORKLOADS,RAW_NAMES))
TRACE=dict(zip(WORKLOADS,TRACE_NAMES))
METHODS=['Page','Page-OPT','Cacheline','Cacheline-OPT','SkyByte','Nexus']
FIELDS=['Page','Optimal 1','Cacheline','Optimal 2','skybyte','mywork']
BLUE='#E1EBF6'
GREEN='#E1EFDB'
ORANGE='#F8E5D8'
SALMON='#F7BDB2'
COLORS=[BLUE,BLUE,GREEN,GREEN,ORANGE,SALMON]
LINE_COLORS=['#2F5597','#808080','#C58B39']
RED='#C00000'
EDGE=1.0
CONFIG={
 'fig02_motivation_performance':('Motivation1.pdf','motivation'),
 'fig03_access_coverage':('Motivation2.pdf','coverage'),
 'fig04_access_temporal':('Motivation3.pdf','access_temporal'),
 'fig07_average_latency':('latency.pdf','latency'),
 'fig08_p99_latency':('tail-latency.pdf','p99'),
 'fig09_hit_ratio':('hitratio.pdf','hit'),
 'fig10_admission_distribution':('admission-distribution.pdf','distribution'),
 'fig11_admission_temporal':('admission-granularity.pdf','admission_temporal'),
 'extra_io_amplification':('IO-amplification.pdf','io'),
}

def table(path):
    with Path(path).open(encoding='utf-8-sig',newline='') as f:
        return list(csv.DictReader(f))

def workload_table(path):
    rows=table(path)
    result={r['Workload']:r for r in rows}
    if len(result)!=len(rows):raise ValueError(f'Duplicate workload in {path}')
    return result

def setup_font(font_file=None, family='Calibri'):
    if font_file:
        fm.fontManager.addfont(str(Path(font_file).expanduser()))
        family=fm.FontProperties(fname=str(Path(font_file).expanduser())).get_name()
    else:
        # The font itself is not redistributed. Use a licensed local installation.
        folders=[Path.home()/'Library/Fonts', Path('/Library/Fonts'),
                 Path('/Applications/Microsoft PowerPoint.app/Contents/Resources/DFonts')]
        for folder in folders:
            if folder.exists():
                for p in folder.glob('*'):
                    if p.name.lower().startswith('calibri') and p.suffix.lower() in ('.ttf','.otf'):
                        fm.fontManager.addfont(str(p))
    try:fm.findfont(family,fallback_to_default=False)
    except ValueError:
        warnings.warn(f'{family} unavailable; using DejaVu Sans. Use --font for manuscript typography.')
        family='DejaVu Sans'
    plt.rcParams.update({'font.family':family,'font.size':9,'axes.labelsize':9,
        'xtick.labelsize':8.5,'ytick.labelsize':8.5,'legend.fontsize':9,
        'axes.linewidth':EDGE,'xtick.major.size':0,'ytick.major.size':0,
        'xtick.major.pad':5,'ytick.major.pad':5,'pdf.fonttype':42,'ps.fonttype':42,
        'figure.facecolor':'white','savefig.facecolor':'white','hatch.linewidth':.45})
    return family

def canvas(width,height):
    return plt.figure(figsize=(width/72,height/72))

def axes(fig,box):
    width,height=fig.get_size_inches()*72
    ax=fig.add_axes([box[0]/width,box[1]/height,box[2]/width,box[3]/height])
    ax.grid(axis='y',color='#D0CECE',linewidth=.6,linestyle=(0,(4,3)))
    ax.set_axisbelow(True)
    return ax

def sparse_hatch(ax,bars):
    """45-degree strokes with a fixed 10-pt pitch, matching the OPT style."""
    inv=ax.transData.inverted()
    for bar in bars:
        p=ax.transData.transform((bar.get_x(),bar.get_y()))
        q=ax.transData.transform((bar.get_x()+bar.get_width(),bar.get_y()+bar.get_height()))
        w,h=q-p
        h=min(h,ax.get_window_extent().y1-p[1])
        segments=[]
        for t in np.arange(-w,h+1,10*ax.figure.dpi/72):
            segments.append(inv.transform([p+[0,t],p+[w,t+w]]))
        lc=LineCollection(segments,colors='#333333',linewidths=.45,zorder=3.1)
        lc.set_clip_path(bar)
        ax.add_collection(lc)

class SquareHandler(HandlerPatch):
    def __init__(self,scale=.895,**kwargs):
        super().__init__(**kwargs);self.scale=scale
    def create_artists(self,legend,orig,xd,yd,width,height,fontsize,trans):
        side=fontsize*self.scale
        p=Rectangle((-xd+(width-side)/2,-yd+(height-side)/2),side,side)
        self.update_prop(p,orig,legend);p.set_transform(trans)
        return [p]

def legend(ax,handles,columns,small=False):
    leg=ax.legend(handles=handles,loc='lower right',bbox_to_anchor=(1,1),
        ncol=columns,borderaxespad=0,frameon=True,fancybox=False,
        edgecolor='black',framealpha=1,borderpad=.25,
        columnspacing=.6 if small else .8,handletextpad=.35,
        handlelength=.8 if small else 1.0,handleheight=1,
        handler_map={Patch:SquareHandler(.8 if small else .895)})
    leg.get_frame().set_linewidth(EDGE)
    return leg

def patch_handles(labels,colors,opt=False):
    return [Patch(facecolor=c,edgecolor='black',linewidth=EDGE,label=n,
                  hatch='/' if opt and n.endswith('-OPT') else None)
            for n,c in zip(labels,colors)]

def category_axis(ax,names,ylabel,ymax,step,rotation=0):
    ax.set_xlim(-.6,len(names)-.4)
    ax.set_ylim(0,ymax)
    ax.set_xticks(range(len(names)),names,rotation=rotation,
                  ha='right' if rotation else 'center',rotation_mode='anchor')
    ax.set_yticks(np.arange(0,ymax+1e-7,step))
    ax.yaxis.set_major_formatter(FormatStrFormatter('%.1f'))
    ax.set_ylabel(ylabel,labelpad=3)

def capped_bars(ax,x,y,width,color,ymax,opt=False):
    y=np.asarray(y,float)
    if not np.isfinite(y).all() or (y<0).any():raise ValueError('Invalid bar data')
    # Keep the actual values in the bar artists; the axes clip values above ymax.
    bars=ax.bar(x,y,width=width,color=color,edgecolor='black',linewidth=EDGE,zorder=3)
    if opt:sparse_hatch(ax,bars)
    for xx,yy in zip(x,y):
        if yy>ymax:
            ax.text(xx,ymax-.06,f'×{yy:.2f}',rotation=90,ha='center',va='top',fontsize=6,zorder=5)
    return bars

def plot_grouped(folder,kind):
    fig=canvas(394.56,222)
    left,width=(51.2928,339.3216) if kind=='io' else (41.4288,349.1856)
    ymax=1 if kind=='hit' else 2
    ylabel={'latency':'Average I/O latency','p99':'P99 I/O latency',
            'hit':'Cache hit ratio','io':'I/O amplification'}[kind]
    for index,cap in enumerate([32,64]):
        rows=workload_table(folder/'data'/f'{cap}MB.csv')
        ax=axes(fig,[left,[140,32][index],width,54])
        category_axis(ax,WORKLOADS,ylabel,ymax,.2 if kind=='hit' else .5)
        fields=['Page','Page-OPT','Cacheline','Cacheline-OPT','SkyByte','Mywork'] if kind=='io' else FIELDS
        for j,(method,field,color) in enumerate(zip(METHODS,fields,COLORS)):
            vals=np.array([float(rows[RAW[n]][field]) for n in WORKLOADS])
            if kind in ('latency','p99'):
                vals/=np.array([float(rows[RAW[n]]['Page']) for n in WORKLOADS])
            capped_bars(ax,np.arange(9)+(j-2.5)*(.7/6),vals,.7/6,color,ymax,method.endswith('-OPT'))
        ax.set_xlabel(f'({chr(97+index)}) {cap}MB',labelpad=4)
        legend(ax,patch_handles(METHODS,COLORS,True),6,small=True)
    return fig

def plot_motivation(folder):
    fig=canvas(394.56,114)
    for index,metric in enumerate(['mean_us','hit_ratio']):
        rows=workload_table(folder/'data'/f'64MB_{metric}.csv')
        ax=axes(fig,[[33.5376,230.419488][index],32,161.378592,54])
        category_axis(ax,MOTIVATION,['Average I/O latency','Cache hit ratio'][index],
                      [2,1][index],[.5,.2][index])
        ax.set_xlim(-.46,5.46)
        for j,(method,color) in enumerate(zip(['Page','Cacheline'],[BLUE,GREEN])):
            vals=np.array([float(rows[RAW[n]][method]) for n in MOTIVATION])
            if index==0:vals/=np.array([float(rows[RAW[n]]['Page']) for n in MOTIVATION])
            capped_bars(ax,np.arange(6)+(j-.5)*.39,vals,.39,color,[2,1][index])
        ax.set_xlabel(['(a) Average I/O latency','(b) Cache hit ratio'][index],labelpad=4)
        legend(ax,patch_handles(['Page','Cacheline'],[BLUE,GREEN]),2)
    return fig

def plot_stacked(folder,distribution=False):
    fig=canvas(394.56,124)
    names=WORKLOADS if distribution else MOTIVATION
    box=[92.272,32,236.736,72] if distribution else [76.9392,24,236.736,72]
    ax=axes(fig,box)
    if distribution:
        records=table(folder/'data/admission_counts.csv')
        records={r['workload']:r for r in records if r['cache_mb']=='64'}
        vals=[]
        for n in names:
            r=records[n]
            counts=np.array([int(r[f'c{g}']) for g in [1,2,4,8,16,32,64]])
            vals.append([sum(counts[:3]),sum(counts[3:5]),sum(counts[5:])]/sum(counts))
        vals=np.array(vals).T
    else:
        rows=workload_table(folder/'data/coverage.csv')
        vals=np.array([[float(rows[RAW[n]][f]) for n in names]
                      for f in ['Small_1_4CL','Medium_5_16CL','Large_17_64CL']])
    assert np.allclose(vals.sum(axis=0),1)
    category_axis(ax,names,'Admission fraction' if distribution else 'Access fraction',1.04,.2,
                  rotation=35 if distribution else 0)
    if distribution:ax.tick_params(axis='x',pad=4)
    labels=['(0, 4]','(4, 16]','(16, 64]'];colors=[BLUE,GREEN,ORANGE]
    bottom=np.zeros(len(names));width=.26*len(names)/6
    if distribution:width=.26*9.2/6.2
    for y,c in zip(vals,colors):
        ax.bar(range(len(names)),y,width=width,bottom=bottom,color=c,edgecolor='black',linewidth=EDGE,zorder=3)
        bottom+=y
    handles=patch_handles(labels,colors)
    if not distribution:
        right=ax.twinx();right.set_ylim(0,1.04);right.set_yticks(np.arange(0,1.01,.2))
        right.yaxis.set_major_formatter(FormatStrFormatter('%.1f'))
        right.set_ylabel("Moran's I value",labelpad=3)
        for spine in right.spines.values():spine.set_visible(False)
        values=[float(rows[RAW[n]]['Moran_I']) for n in names]
        finite=[v for v in values if np.isfinite(v)]
        if finite and min(finite)<0:
            lower=np.floor(min(finite)*5)/5
            right.set_ylim(lower,1.04);right.set_yticks(np.arange(lower,1.01,.2))
        if len(finite)!=len(values):warnings.warn('Undefined Moran means omitted, not replaced by zero.')
        right.plot(range(len(names)),values,color=RED,linestyle='--',linewidth=1.2,
                   marker='^',markersize=8,markerfacecolor='#EFEAE3',markeredgecolor='black',markeredgewidth=1)
        handles.append(Line2D([],[],color=RED,linestyle='--',linewidth=1.2,marker='^',markersize=8,
                             markerfacecolor='#EFEAE3',markeredgecolor='black',markeredgewidth=1,label="Moran's I"))
    legend(ax,handles,len(handles))
    return fig

def plot_temporal(folder,admission=False):
    height=263 if admission else 177
    fig=canvas(394.56,height)
    names=WORKLOADS if admission else MOTIVATION
    panels=[]
    for i,name in enumerate(names):
        col,row=i%3,i//3
        ax=axes(fig,[39.456+127.8997927461*col,(204 if admission else 118)-86*row,89.4404145078,54])
        if admission:
            records={cap:table(folder/'data'/f'{TRACE[name]}_{cap}MB.csv') for cap in [32,64]}
            for cap,color in zip([32,64],LINE_COLORS):
                r=records[cap];assert len(r)==100
                ax.plot([float(t['progress_pct']) for t in r],
                        [float(t['mean_logical_selected_CL']) for t in r],color=color,lw=1.1,label=f'{cap}MB')
            r=records[32]
            assert np.allclose([float(t['trace_mean_active_CL']) for t in r],
                               [float(t['trace_mean_active_CL']) for t in records[64]],rtol=0,atol=1e-8)
            ax.plot([float(t['progress_pct']) for t in r],[float(t['trace_mean_active_CL']) for t in r],
                    color=LINE_COLORS[2],lw=1.1,label='Trace active CL')
        else:
            r=table(folder/'data'/f'{TRACE[name]}.csv');assert len(r)==100
            ax.plot([float(t['progress_pct']) for t in r],[float(t['mean_active_cl']) for t in r],
                    color=LINE_COLORS[0],lw=1.1,label='Access coverage')
            ax.axhline(float(r[0]['global_mean_active_cl']),color=RED,lw=1.1,
                       linestyle=(0,(4,3)),label='Average access coverage')
        ax.set_xlim(0,100);ax.set_xticks([0,20,40,60,80,100])
        ax.set_ylim(0,64 if admission or row==0 else 21)
        ax.set_yticks([0,20,40,60] if admission or row==0 else [0,5,10,15,20])
        if not admission and row>0:
            maximum=max(float(t['mean_active_cl']) for t in r)
            if maximum>21:
                upper=min(64,np.ceil(maximum/5)*5+1)
                ax.set_ylim(0,upper);ax.set_yticks(np.arange(0,upper+1,10))
        ax.set_ylabel('Cacheline count',labelpad=3)
        ax.set_xlabel(f'({chr(97+i)}) {name}',labelpad=4)
        panels.append(ax)
    target=panels[3] if admission else panels[0]
    handles,labels=target.get_legend_handles_labels()
    if not admission:handles[1]=Line2D([],[],color=RED,lw=1.1,linestyle=(0,(2.5,2.5)))
    leg=target.legend(handles,labels,loc='upper left',bbox_to_anchor=(0,0,1,1) if not admission else (0,1),
        mode='expand' if not admission else None,
        ncol=1,fontsize=7.5,frameon=True,fancybox=False,edgecolor='black',framealpha=1,
        borderaxespad=0,borderpad=.1,labelspacing=.05,handlelength=1.1,handletextpad=.3)
    leg.get_frame().set_linewidth(EDGE)
    for t in leg.get_texts():t.set_linespacing(.9)
    return fig

def main(key):
    parser=argparse.ArgumentParser(description=f'Render {key} from new experiment data.')
    parser.add_argument('--data-root',type=Path,required=True,help='Generated figures/plot_inputs directory')
    parser.add_argument('--smoke-watermark',action='store_true')
    parser.add_argument('--output-dir',type=Path,help='Default: this figure\'s output/ directory')
    parser.add_argument('--font',help='Path to a locally licensed Calibri TTF/OTF')
    parser.add_argument('--font-family',default='Calibri')
    parser.add_argument('--no-png',action='store_true')
    args=parser.parse_args()
    preparation=json.loads((args.data_root/'preparation.json').read_text())
    if key not in preparation['ready'].values():
        parser.error('This figure is not ready in the latest preparation; do not reuse stale CSVs')
    family=setup_font(args.font,args.font_family)
    folder=args.data_root.resolve()/key
    pdf,kind=CONFIG[key]
    if kind in ('latency','p99','hit','io'):fig=plot_grouped(folder,kind)
    elif kind=='motivation':fig=plot_motivation(folder)
    elif kind in ('coverage','distribution'):fig=plot_stacked(folder,kind=='distribution')
    elif kind in ('access_temporal','admission_temporal'):fig=plot_temporal(folder,kind=='admission_temporal')
    else:raise ValueError(f'Unknown plot kind: {kind}')
    if args.smoke_watermark or preparation['completion'].get('smoke'):
        fig.text(.5,.5,'SMOKE TEST — NOT PAPER RESULTS',ha='center',va='center',rotation=20,color='red',alpha=.35,fontsize=14)
    out=args.output_dir or folder/'output';out.mkdir(parents=True,exist_ok=True)
    fig.savefig(out/pdf,metadata={'Title':key,'Creator':'Nexus figure source archive','CreationDate':None})
    if not args.no_png:fig.savefig(out/Path(pdf).with_suffix('.png'),dpi=300)
    plt.close(fig)
    print(f'{key}: {out/pdf} [font={family}]')
