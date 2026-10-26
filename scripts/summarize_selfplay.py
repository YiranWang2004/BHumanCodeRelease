#!/usr/bin/env python3
"""Aggregate full-match logs, separating counters from causal interpretations."""
import argparse
import collections
import csv
import json
import statistics
from pathlib import Path


def rows(path):
    with path.open() as f:
        yield from csv.DictReader(f)


def summarize(directory):
    result = dict(halves=[])
    for half in sorted(directory.glob('match*-half*')):
        item = dict(name=half.name,players={},decisions={})
        if (half/'result.json').exists():
            item['result'] = json.loads((half/'result.json').read_text())
        previous = None
        contacts, modes = collections.Counter(), collections.Counter()
        goals=[]; previous_score=None
        for r in rows(half/'server.csv'):
            if previous:
                dt = max(0., min(.3,float(r['time'])-float(previous['time'])))
                modes[previous['mode']] += dt
            if r['contact'] and (not previous or r['contact'] != previous['contact']):
                contacts[r['contact']] += 1
            score = (int(r['left']),int(r['right']))
            if previous_score is not None and score != previous_score:
                goals.append(dict(play_time=float(r['play_time']),left=score[0],right=score[1],last_contact=r['last_contact']))
            previous, previous_score = r, score
        item.update(contacts=dict(contacts),mode_seconds=dict(modes),goals=goals,last_server=previous)
        for p in sorted((half/'players/csv').glob('*.csv')):
            previous=None; penalties=falls=0; seconds=collections.Counter(); last=None
            for r in rows(p):
                if None in r.values():
                    continue  # a running writer may have one incomplete final line
                if float(r['play_time']) <= (1200 if half.name.endswith('half2') else 0):
                    previous=r; continue
                if previous:
                    dt=max(0.,min(.05,float(r['time'])-float(previous['time'])))
                    seconds['observed']+=dt
                    if previous['penalized']=='1':seconds['penalized']+=dt
                    if float(previous['height'])<.3:seconds['low_body']+=dt
                    seconds['policy_'+previous['dancer_phase']]+=dt
                    penalties+=r['penalized']=='1' and previous['penalized']!='1'
                    falls+=float(r['height'])<.3 and float(previous['height'])>=.3
                previous=last=r
            item['players'][p.stem]=dict(penalty_entries=penalties,low_body_entries=falls,seconds=dict(seconds),
                 counters={k:last[k] for k in ['walk_inferences','kick_inferences','amp_inferences','dancer_inferences']} if last else {})
        active_events=[]
        for p in sorted((half/'players/decisions').glob('*.csv')):
            previous=None; gates=collections.Counter(); exits=collections.Counter(); starts=active=0; last=None
            startup_at=None; startup_delays=[]
            for r in rows(p):
                if None in r.values(): continue
                if previous:
                    dt=max(0.,min(.05,float(r['time'])-float(previous['time'])))
                    gates[previous['gate']]+=dt
                    if r['phase']=='starting' and previous['phase']!='starting':
                        startup_at=float(r['time'])
                    if r['phase']=='active' and previous['phase']=='starting' and startup_at is not None:
                        startup_delays.append(float(r['time'])-startup_at)
                    if (r['phase']=='active') != (previous['phase']=='active'):
                        active_events.append((float(r['time']), 1 if r['phase']=='active' else -1))
                    starts+=r['phase']=='starting' and previous['phase']!='starting'
                    active+=r['phase']=='active' and previous['phase']=='starting'
                    if r['reason'] and r['reason']!=previous['reason']:exits[r['reason']]+=1
                previous=last=r
            if last:
                item['decisions'][last['number']]=dict(startups=starts,active_entries=active,gate_seconds=dict(gates),exit_events=dict(exits),startup_delay_median=statistics.median(startup_delays) if startup_delays else None,
                     startup_delay_max=max(startup_delays,default=None))
        active_count=0; overlap=longest=0.; began=None; previous_time=None
        for t,delta in sorted(active_events):
            if active_count>1 and previous_time is not None:
                overlap+=t-previous_time
            active_count+=delta
            if active_count>1 and began is None:began=t
            if active_count<=1 and began is not None:
                longest=max(longest,t-began);began=None
            previous_time=t
        item['simultaneous_active_seconds']=overlap
        item['longest_simultaneous_active_seconds']=longest
        result['halves'].append(item)
    if (directory/'results.json').exists():
        matches=json.loads((directory/'results.json').read_text());result['matches']=matches
        result['total']=dict(matches=len(matches),wins=sum(m['score'][0]>m['score'][1] for m in matches),
                            draws=sum(m['score'][0]==m['score'][1] for m in matches),
                            losses=sum(m['score'][0]<m['score'][1] for m in matches),
                            goals_for=sum(m['score'][0] for m in matches),goals_against=sum(m['score'][1] for m in matches))
    return result

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path)
    a=p.parse_args();data=summarize(a.directory)
    output=a.directory/'summary.json';output.write_text(json.dumps(data,indent=2))
    print(output)
    print(json.dumps(data.get('total',{'complete_matches':0}),ensure_ascii=False))
