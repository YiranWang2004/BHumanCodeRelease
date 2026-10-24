#!/usr/bin/env python3
"""Compare prebuilt K1/T1 alignment variants on an isolated seven-player server.

The initial replay fixture uses root poses from play_time=219.46 in the reported
match. It does not replay joint states or velocities. Before starting, hold the
actor root and point its head at the ball to remove perception cold starts;
require a fresh native ball estimate. After starting, all normal perception and
physics run unchanged. Other players execute native stand as fixed fixtures.

Only launch variants built as described in reports/kick_alignment.md. Each
concurrent experiment needs a distinct --team-id (native team messages use UDP).
All server and telemetry TCP/UDP ports are ephemeral, and only owned processes
are cleaned up. --out defaults to the ignored repository logs/ directory.
"""
import argparse
import csv
import hashlib
import json
import math
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
P = argparse.ArgumentParser()
P.add_argument('--cases', default='replay')
P.add_argument('--incoming-speed', type=float, default=0., help='Test rolling incoming ball in PlayOn (m/s); initial speed, not a maintained force.')
P.add_argument('--robot', choices=['K1', 'T1'], default='K1')
P.add_argument('--repeats', type=int, default=1)
P.add_argument('--seconds', type=float, default=12)
P.add_argument('--team-id', type=int, default=94)
P.add_argument('--out')
P.add_argument('--server-repo', type=Path, default=Path(__file__).resolve().parents[3] / 'rcssservermj')
P.add_argument('--server-python', default=sys.executable)
P.add_argument('--baseline', type=Path, required=True)
P.add_argument('--candidate', type=Path, required=True)
P.add_argument('--candidate-motion-policy', choices=['native', 'dancer'], default='native')
P.add_argument('--candidate-assets', type=Path, help='Isolated configuration for the direct variant.')
P.add_argument('--actor-only', action='store_true', help='Only player 4; removes teammate ownership competition.')
P.add_argument('--force-exit-after-contact', action='store_true', help='Test fixture: move ball 12 m ahead .5 s after first foot contact to exercise normal Kick exit.')
P.add_argument('--accurate', type=Path)
P.add_argument('--modes', default='baseline,direct')
a = P.parse_args()
if a.repeats < 1 or a.seconds <= 0 or (not 1 <= a.team_id <= 99):
    P.error('Invalid repeats, seconds, or team ID')
BINARIES = dict(baseline=a.baseline.resolve(), direct=a.candidate.resolve(), accurate=a.accurate.resolve() if a.accurate else None)
for mode in a.modes.split(','):
    if mode not in BINARIES or BINARIES[mode] is None or (not BINARIES[mode].is_file()):
        P.error('Missing executable for ' + mode)
REPO = Path(__file__).resolve().parents[2]
SERVER = a.server_repo.resolve()
log_root = REPO / 'logs'
log_root.mkdir(exist_ok=True)
OUT = Path(a.out or tempfile.mkdtemp(prefix='kick-alignment-', dir=log_root)).resolve()
OUT.mkdir(parents=True, exist_ok=True)
original = [{'n': 1, 'x': -25.898, 'y': 0.881, 'z': 0.55, 'yaw': 0.443337}, {'n': 2, 'x': -14.435, 'y': 6.804, 'z': 0.55, 'yaw': 0.84285}, {'n': 3, 'x': -16.311, 'y': -8.3, 'z': 0.55, 'yaw': 1.16918}, {'n': 4, 'x': 5.382, 'y': 17.49, 'z': 0.55, 'yaw': 1.5471}, {'n': 5, 'x': 6.512, 'y': 0.549, 'z': 0.55, 'yaw': 0.230009}, {'n': 6, 'x': -1.331, 'y': -11.851, 'z': 0.55, 'yaw': 0.548772}, {'n': 7, 'x': 17.154, 'y': 0.58, 'z': 0.55, 'yaw': 2.4958}]
original[3]['z'] = 0.67 if a.robot == 'T1' else 0.55
bootstrap = Path(__file__).with_name('kick_alignment_server.py')

def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]

def scene(name, rep):
    poses = [dict(p) for p in original]
    ball = [5.85, 18.0]
    kind = 'kickin'
    if name != 'replay':
        kind = 'free'
        ball = [5.0, 5.0]
        angle = math.atan2(-5, 22.5)
        dist, bearing, yaw = map(float, name.split('_'))
        b = angle + math.radians(bearing)
        poses[3].update(x=ball[0] - dist * math.cos(b), y=ball[1] - dist * math.sin(b), yaw=angle + math.radians(yaw))
        poses[6].update(x=15.0, y=3.0, yaw=0.0)
    if rep:
        poses[3]['yaw'] += math.radians(3 if rep % 2 else -3)
        poses[3]['x'] += 0.02 * (1 if rep % 2 else -1)
    velocity = [0., 0., 0.]
    if a.incoming_speed:
        actor = poses[3]
        distance = math.hypot(actor['x'] - ball[0], actor['y'] - ball[1])
        velocity[:2] = [a.incoming_speed * (actor[k] - ball[i]) / distance for i, k in enumerate(('x', 'y'))]
        kind = 'incoming'
    return dict(poses=poses, ball=ball, kind=kind, velocity=velocity, force_exit_after_contact=a.force_exit_after_contact)

def run(name, rep, mode):
    d = OUT / f'{name}-r{rep}-{mode}'
    d.mkdir()
    (d / 'scene.json').write_text(json.dumps(scene(name, rep)))
    children = []
    files = []
    latest = {}
    packets = []
    running = True
    monitor = None
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(('127.0.0.1', 0))
    udp.settimeout(0.1)

    def receive():
        while running:
            try:
                p = json.loads(udp.recvfrom(65536)[0])
                packets.append(p)
                latest[p['player']] = p
            except socket.timeout:
                continue
            except OSError:
                break
    threading.Thread(target=receive, daemon=True).start()

    def spawn(cmd, name, cwd=REPO, env=None):
        f = (d / (name + '.log')).open('w')
        files.append(f)
        p = subprocess.Popen(list(map(str, cmd)), cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT)
        children.append(p)
        return p

    def wait_for(test, timeout=40):
        end = time.monotonic() + timeout
        while not test():
            assert all((p.poll() is None for p in children)), 'child exited: ' + str(d)
            assert time.monotonic() < end, 'timeout: ' + str(d)
            time.sleep(0.02)
    try:
        ap, mp = (port(), port())
        env = os.environ.copy()
        env['PYTHONPATH'] = str(SERVER / 'src')
        server = spawn([a.server_python, bootstrap, ap, mp, d / 'scene.json', d / 'truth.jsonl'], 'server', d, env)
        end = time.monotonic() + 25
        while True:
            assert server.poll() is None, 'server exited: ' + str(d)
            try:
                monitor = socket.create_connection(('127.0.0.1', mp), timeout=0.2)
                break
            except OSError:
                assert time.monotonic() < end
                time.sleep(0.05)
        monitor.settimeout(None)

        def drain():
            try:
                while monitor.recv(65536):
                    pass
            except OSError:
                pass
        threading.Thread(target=drain, daemon=True).start()
        player_numbers = [4] if a.actor_only else list(range(1, 8))
        for n in player_numbers:
            pose = scene(name, rep)['poses'][n - 1]
            assets = a.candidate_assets.resolve() if mode == 'direct' and a.candidate_assets else REPO
            spawn([BINARIES[mode], '--port', ap, '--team', 'AlignmentTest', '--team-id', a.team_id, '--num', n, '--robot', a.robot if n == 4 else 'K1', '--beam-x', pose['x'], '--beam-y', pose['y'], '--beam-theta', math.degrees(pose['yaw']), '--assets-dir', assets, '--monitor', '--monitor-port', udp.getsockname()[1], '--sim-speed', 2, '--trace', d / f'{n}.csv', '--max-frames', 3000] + (['--test-stand'] if n != 4 else ['--motion-policy', a.candidate_motion_policy if mode == 'direct' else 'native']), str(n))
            if n == player_numbers[0]:
                wait_for(lambda: n in latest)
        wait_for(lambda: len(latest) == len(player_numbers) and min((p['sequence'] for p in latest.values())) >= 150)
        wait_for(lambda: latest[4]['ball_age'] < 0.1 and abs(math.hypot(latest[4]['ball_x'], latest[4]['ball_y']) - math.hypot(scene(name, rep)['ball'][0] - latest[4]['truth_x'], scene(name, rep)['ball'][1] - latest[4]['truth_y'])) < 0.1)
        (d / 'warmup.json').write_text(json.dumps(latest[4]))
        b = b'(testAlignment)'
        monitor.sendall(len(b).to_bytes(4, 'big') + b)
        wait_for(lambda: (d / 'truth.jsonl').exists() and (d / 'truth.jsonl').stat().st_size > 0)
        t0 = json.loads((d / 'truth.jsonl').open().readline())['start']
        wait_for(lambda: latest[4]['time'] >= t0 + a.seconds, timeout=60)
    finally:
        running = False
        udp.close()
        if monitor:
            try:
                monitor.shutdown(socket.SHUT_RDWR)
                monitor.close()
            except OSError:
                pass
        for p in children[1:]:
            if p.poll() is None:
                p.send_signal(signal.SIGINT)
        for p in children[1:]:
            try:
                p.wait(timeout=6)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        for p in children[:1]:
            if p.poll() is None:
                p.terminate()
                p.wait(timeout=6)
        for f in files:
            f.close()
    (d / 'telemetry.json').write_text(json.dumps([p for p in packets if p['player'] == 4]))
    truth = [json.loads(l) for l in (d / 'truth.jsonl').read_text().splitlines()]
    t0 = truth[0]['start']
    truth = truth[1:]
    rows = list(csv.DictReader((d / '4.csv').open()))
    rows = [r for r in rows if float(r['time']) >= t0]
    kicks = [r for i, r in enumerate(rows) if i and int(r['kick_inferences']) > int(rows[i - 1]['kick_inferences'])]
    # An incoming ball already has speed before any robot kicks it. Measure
    # launch only at/after the actor's first contact in that case.
    contact_time = next((v['t'] for v in truth if any('r-0-4-' in part for part in v['parts'])), float('inf')) if a.incoming_speed else -float('inf')
    launch = next((v for v in truth if v['t'] >= contact_time and math.hypot(*v['vel'][:2]) >= 1.0), None)
    report = dict(case=name, repeat=rep, mode=mode, t0=t0, first_policy=round(float(kicks[0]['time']) - t0, 3) if kicks else None, launch=None, falls=any((v['actor'][2] < 0.35 for v in truth)), penalized=any((r['penalized'] == '1' for r in rows)), walkclose_s=0.02 * sum(('GoToBallWaitAndKick:notAtBall' in r['behavior'] and 'WalkToPoint:walkClose' in r['behavior'] for r in rows)))
    report['kick_native_constraint_frames'] = sum(int(r.get('dancer_native_flags', '0')) != 0 for r in kicks)
    report['entry_settle_s'] = .02 * sum(r.get('dancer_gate') == 'entry_settle' for r in rows)
    report['transition_events'] = [dict(time=round(float(r['time']) - t0, 3), state=r.get('dancer_gate'))
                                   for i, r in enumerate(rows) if i == 0 or r.get('dancer_gate') != rows[i-1].get('dancer_gate')]
    if launch:
        lt = launch['t']
        row = min(rows, key=lambda r: abs(float(r['time']) - (lt - 0.02)))
        direction = float(row['dancer_kick_direction_world']) if mode == 'direct' and a.candidate_motion_policy == 'dancer' else float(row['truth_yaw']) + float(row['kick_direction'])
        end = min(truth, key=lambda v: abs(v['t'] - lt - 0.12))
        vx, vy = end['vel'][:2]
        err = abs((math.atan2(vy, vx) - direction + math.pi) % (2 * math.pi) - math.pi)
        report.update(launch=round(lt - t0, 3), speed_120ms=round(math.hypot(vx, vy), 3), error_deg=round(math.degrees(err), 2), command_world_deg=round(math.degrees(direction), 2), contact_parts=sorted({p for v in truth if lt - 0.1 <= v['t'] <= lt + 0.1 for p in v['parts']}), mode_at_launch=launch['mode'])
        report['falls_before_kick'] = any((v['actor'][2] < 0.35 for v in truth if v['t'] <= lt + 0.3))
        report['success'] = report['speed_120ms'] >= 1.5 and report['error_deg'] <= 30 and (not report['falls_before_kick'])
    else:
        report['success'] = False
    (d / 'report.json').write_text(json.dumps(report, indent=2))
    return report
(OUT / 'manifest.json').write_text(json.dumps(dict(binaries={k: dict(path=str(v), sha256=hashlib.sha256(v.read_bytes()).hexdigest()) for k, v in BINARIES.items() if v}, server=str(SERVER), robot=a.robot, candidate_motion_policy=a.candidate_motion_policy, candidate_assets=str(a.candidate_assets.resolve()) if a.candidate_assets else str(REPO), actor_only=a.actor_only, incoming_speed=a.incoming_speed, seconds=a.seconds, repeats=a.repeats, cases=a.cases, modes=a.modes, note='Matched stationary fixtures, not joint-state replays; only actor 4 moves. Before timing, server holds its root pose and points head at ball. Native visual ball model must be fresh.'), indent=2))
reports = []
for rep in range(a.repeats):
    for name in a.cases.split(','):
        for mode in a.modes.split(',') if rep % 2 == 0 else list(reversed(a.modes.split(','))):
            r = run(name, rep, mode)
            reports.append(r)
            (OUT / 'report.json').write_text(json.dumps(reports, indent=2))
            print(json.dumps(r), flush=True)
print('ARTIFACTS', OUT, flush=True)
