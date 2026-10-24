#!/usr/bin/env python3
"""Analyze real client AMP/Kick handoffs against server truth, not command speed."""
import argparse
import csv
import json
import math
from pathlib import Path
import statistics


def analyze(folder):
    scene = json.loads((folder / 'scene.json').read_text())
    summary = json.loads((folder / 'report.json').read_text())
    truth = [json.loads(line) for line in (folder / 'truth.jsonl').read_text().splitlines()]
    start = truth[0]['start']; truth = truth[1:]
    rows = [r for r in csv.DictReader((folder / '4.csv').open()) if float(r['time']) >= start]
    for v in truth:
        q = v['actor_quaternion']
        v['tilt'] = math.degrees(math.acos(max(-1., min(1., 1 - 2 * (q[1]**2 + q[2]**2)))))
        v['speed'] = math.hypot(*v['actor_velocity'][:2])
        v['bad'] = v['actor'][2] < .35 or v['tilt'] > 45

    def window(lo, hi):
        selected = [v for v in truth if lo - .011 <= v['t'] <= hi + .011]
        native = [r for r in rows if lo <= float(r['time']) <= hi]
        return dict(min_height=min(v['actor'][2] for v in selected),
                    max_tilt=max(v['tilt'] for v in selected),
                    max_angular_rate=max(math.sqrt(sum(w*w for w in v['actor_velocity'][3:6])) for v in selected),
                    fall=any(v['bad'] for v in selected) or any(r['phase'] in ('3', '4') for r in native),
                    observed_seconds=selected[-1]['t']-selected[0]['t'])

    def speed(lo, hi):
        selected = [v['speed'] for v in truth if lo <= v['t'] <= hi]
        return statistics.mean(selected) if selected else None

    entries = []
    for i, r in enumerate(rows):
        if r.get('dancer_gate') != 'kick' or (i and rows[i-1].get('dancer_gate') == 'kick'):
            continue
        t = float(r['time']); j = i - 1
        while j >= 0 and rows[j].get('dancer_gate') == 'entry_settle': j -= 1
        settle = float(rows[j+1]['time'])
        entry = dict(t=t-start, settle_s=t-settle,
                     before_settle_speed=speed(settle-.2, settle),
                     at_kick_speed=speed(t-.1, t),
                     max_approach_command=max(float(x['dancer_walk_vx']) for x in rows[:i+1]),
                     **window(settle, min(t+2, truth[-1]['t'])))
        entries.append(entry)
    exits = []
    for i, r in enumerate(rows[1:], 1):
        if '2026-10-04' not in r['policy'] or '5070Ti' not in rows[i-1]['policy']:
            continue
        t = float(r['time'])
        next_entry = next((start+e['t'] for e in entries if start+e['t'] > t), truth[-1]['t'])
        hi = min(t+2, next_entry-.02, truth[-1]['t'])
        if hi <= t: continue
        exits.append(dict(t=t-start, reason=r['dancer_gate'],
                          exit_reason=r.get('dancer_exit_reason'),
                          initial_speed=speed(t-.1, t),
                          max_next_command=max(abs(float(x['dancer_walk_vx'])) for x in rows if t <= float(x['time']) <= hi),
                          **window(t, hi)))
    return dict(path=str(folder), case=summary['case'], repeat=summary['repeat'],
                speed_profile=float(folder.parent.name.rsplit('-', 1)[1]),
                controlled_exit=bool(scene.get('force_exit_after_contact')),
                duration=truth[-1]['t']-start, first_kick=summary['first_policy'],
                launch=summary['launch'], foot_contact=any(any('r-0-4-' in p and 'foot' in p for p in v['parts']) for v in truth),
                peak_ball_speed=max(math.hypot(*v['vel'][:2]) for v in truth),
                overall_fall=any(v['bad'] for v in truth) or any(r['phase'] in ('3','4') for r in rows),
                entries=entries, exits=exits)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=Path)
    args = p.parse_args()
    trials = []
    for folder in sorted(args.directory.glob('handoff-*/*-direct')):
        if folder.parent.name == 'handoff-pilot' or not (folder / 'report.json').exists(): continue
        trials.append(analyze(folder))
    groups = []
    for controlled in (False, True):
        for profile in sorted({r['speed_profile'] for r in trials}):
            rs = [r for r in trials if r['controlled_exit'] == controlled and r['speed_profile'] == profile]
            if not rs: continue
            first = [r['entries'][0] for r in rs if r['entries']]
            entries = [e for r in rs for e in r['entries']]
            exits = [e for r in rs for e in r['exits']]
            group = dict(profile=profile, controlled_exit=controlled, trials=len(rs),
                         first_entries=len(first), first_entry_falls=sum(e['fall'] for e in first),
                         foot_contact_trials=sum(r['foot_contact'] for r in rs), overall_falls=sum(r['overall_fall'] for r in rs),
                         entry_events=len(entries), entry_event_falls=sum(e['fall'] for e in entries),
                         exit_events=len(exits), exit_event_falls=sum(e['fall'] for e in exits),
                         exit_reasons=sorted({e['exit_reason'] or e['reason'] for e in exits}))
            if first:
                group.update(before_settle_speed_mean=statistics.mean(e['before_settle_speed'] for e in first),
                             at_kick_speed_mean=statistics.mean(e['at_kick_speed'] for e in first),
                             at_kick_speed_max=max(e['at_kick_speed'] for e in first),
                             first_entry_max_tilt=max(e['max_tilt'] for e in first),
                             first_entry_min_height=min(e['min_height'] for e in first),
                             first_entry_mean_peak_tilt=statistics.mean(e['max_tilt'] for e in first),
                             first_entry_max_angular_rate=max(e['max_angular_rate'] for e in first),
                             settle_seconds=sorted({round(e['settle_s'], 3) for e in first}))
            if exits:
                group.update(exit_max_tilt=max(e['max_tilt'] for e in exits),
                             exit_min_height=min(e['min_height'] for e in exits))
            groups.append(group)
    (args.directory / 'handoff-analysis.json').write_text(json.dumps(dict(groups=groups, trials=trials), indent=2))
    print(json.dumps(groups, indent=2))


if __name__ == '__main__': main()
