#!/usr/bin/env python3
"""Contact-grounded release speeds; do not differentiate noisy vision positions."""
import argparse,bisect,csv,gzip,json,math,statistics
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('out',type=Path);a=p.parse_args()
def open_text(path):return gzip.open(path,'rt') if path.suffix=='.gz' else path.open()
def norm(v):return math.hypot(*v[:2])
def policies(folder):
    result={}
    for n in range(1,8):
        files=list(folder.glob(f'BHuman_{n}.csv*')) or list(folder.glob(f'Dancer_{n}.jsonl*'))
        if not files:continue
        f=files[0];rows=[]
        with open_text(f) as stream:
            if 'BHuman' in f.name:
                for r in csv.DictReader(stream):
                    rows.append((float(r['time']),r['policy'] if r['phase']=='2' else 'other_phase'))
            else:
                for line in stream:
                    r=json.loads(line);rows.append((r['server_time'] if 'server_time' in r else r['input']['time'],r.get('action',{}).get('policy','unknown')))
        result[n]=([r[0] for r in rows],[r[1] for r in rows])
    return result
def analyze(folder):
    scenario=json.loads((folder/'scenario.json').read_text());rows=[]
    with (folder/'truth.jsonl').open() as f:
        for line in f:
            r=json.loads(line)
            if r['elapsed']<=scenario['duration']+.0001:rows.append(r)
    episodes=[]
    for i,r in enumerate(rows):
        if not r['contact']:continue
        if not episodes or episodes[-1]['who']!=r['contact'] or r['t']-rows[episodes[-1]['indices'][-1]]['t']>.2+1.e-6:
            episodes.append(dict(who=r['contact'],indices=[]))
        episodes[-1]['indices'].append(i)
    policy=policies(folder); events=[]
    for e in episodes:
        if not e['who'].startswith('r-0-'):continue
        start,end=e['indices'][0],e['indices'][-1]; release=end+1
        if release>=len(rows):continue
        parts=sorted({p for i in e['indices'] for p in (rows[i].get('contact_parts') or [])})
        if not any('foot' in p for p in parts):continue
        r=rows[release];last=rows[end];before=rows[max(0,start-1)]
        if r['contact'] or r['play_mode']!=last['play_mode']:continue
        if math.dist(r['ball'],last['ball'])>1.:continue
        robot=next(x for x in last['robots'] if x['id']==e['who']);q=robot['quat']
        upright=robot['pos'][2]>=.35 and 1-2*(q[1]**2+q[2]**2)>=math.cos(math.pi/4)
        n=int(e['who'].rsplit('-',1)[-1]);name='unknown'
        if n in policy:
            times,labels=policy[n];j=bisect.bisect_right(times,last['t'])-1
            if j>=0 and last['t']-times[j]<.08:name=labels[j]
        feet_speed=[norm(v) for i in e['indices'] for k,v in rows[i].get('contact_part_center_velocity',{}).items() if 'foot' in k]
        # V120 is reported only before any next contact, referee reset or teleport.
        tail=next((x for x in rows[release:] if x['t']>=r['t']+.12-1e-6),None)
        tail_ok=tail and all(not x['contact'] and x['play_mode']==r['play_mode'] for x in rows[release:] if x['t']<=tail['t'])
        events.append(dict(player=n,t=last['elapsed'],speed=norm(r['ball_velocity']),speed3d=math.sqrt(sum(v*v for v in r['ball_velocity'])),incoming=norm(before['ball_velocity']),speed120=norm(tail['ball_velocity']) if tail_ok else None,upright=upright,policy=name,parts=parts,foot_center_speed=max(feet_speed) if feet_speed else None))
    (folder/'kick_release_events.json').write_text(json.dumps(events,indent=2))
    valid=[e for e in events if e['upright'] and e['speed']>=1.]
    def stats(data):
        speeds=[e['speed'] for e in data]
        return dict(n=len(speeds),mean=statistics.mean(speeds) if speeds else None,median=statistics.median(speeds) if speeds else None,sd=statistics.stdev(speeds) if len(speeds)>1 else None,mean3d=statistics.mean(e['speed3d'] for e in data) if data else None,speeds=speeds)
    summary=dict(run=folder.name,duration=rows[-1]['elapsed'],score=rows[-1]['score'],all_foot_releases=len(events),upright_ge1=stats(valid),upright_ge2=stats([e for e in valid if e['speed']>=2]),upright_delta_ge05=stats([e for e in valid if e['speed']-e['incoming']>=.5]),policies=sorted({e['policy'] for e in valid}))
    (folder/'kick_speed_summary.json').write_text(json.dumps(summary,indent=2));return summary
all_summaries=[]
for folder in sorted(a.out.iterdir()):
    if (folder/'done.json').exists() and (folder/'truth.jsonl').exists():
        s=analyze(folder);all_summaries.append(s);print(json.dumps(s),flush=True)
(a.out/'match_speed_summaries.json').write_text(json.dumps(all_summaries,indent=2))
