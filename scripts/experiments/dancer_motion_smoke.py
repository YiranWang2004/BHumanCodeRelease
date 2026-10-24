#!/usr/bin/env python3
"""Isolated stand/walk smoke; never connects to a user's match ports."""
import argparse
import csv
import json
import math
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--server-python', required=True)
parser.add_argument('--server-repo', type=Path, default=ROOT.parent / 'rcssservermj')
parser.add_argument('--policies', default='native,dancer')
parser.add_argument('--commands', default='0,0,0;0.8,0,0;0,0.4,0;0,0,0.6')
parser.add_argument('--tip', action='store_true', help='After entering PlayOn, topple the robot to exercise recovery.')
parser.add_argument('--tip-direction', choices=['front', 'back', 'left', 'right'], default='front')
parser.add_argument('--server-model', choices=['K1', 'K1BHuman'])
parser.add_argument('--assets-dir', type=Path, help='Optional isolated configuration for experiments.')
parser.add_argument('--beam-y', type=float, default=0., help='Offset straight runs away from goalposts.')
parser.add_argument('--beam-x', type=float, default=-10.)
parser.add_argument('--max-frames', type=int, help='Override trial duration.')
parser.add_argument('--repeat', type=int, default=1)
args = parser.parse_args()
(ROOT / 'logs').mkdir(exist_ok=True)
output = Path(tempfile.mkdtemp(prefix='dancer-motion-', dir=ROOT / 'logs'))

def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]

reports = []
for policy in args.policies.split(','):
    for trial, command in [(trial, command) for trial in range(args.repeat) for command in args.commands.split(';')]:
        directory = output / (policy + '_' + command.replace(',', '_') + (f'_trial{trial}' if args.repeat > 1 else ''))
        directory.mkdir()
        processes, files = [], []
        monitor = None
        monitor_thread = None
        latest = {}
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp.bind(('127.0.0.1', 0)); udp.settimeout(.2)
        alive = True
        def receive():
            while alive:
                try: latest.update(json.loads(udp.recv(65536)))
                except socket.timeout: pass
                except OSError: break
        receiver = threading.Thread(target=receive, daemon=True)
        receiver.start()
        def spawn(command, name, env=None):
            stream = (directory / (name + '.log')).open('w'); files.append(stream)
            process = subprocess.Popen(list(map(str, command)), cwd=directory, env=env,
                                       stdout=stream, stderr=subprocess.STDOUT)
            processes.append(process)
            return process
        try:
            ap, mp = port(), port()
            env = dict(os.environ, PYTHONPATH=str(args.server_repo.resolve() / 'src'))
            server = spawn([args.server_python, '-m', 'rcsssmj', '--field', 'fifa7vs7', '--rules', 'ssim',
                            '--sync', '--no-realtime', '--bhuman-physics', '--aport', ap, '--mport', mp], 'server', env)
            end = time.monotonic() + 20
            while monitor is None:
                assert server.poll() is None, 'server failed'
                try: monitor = socket.create_connection(('127.0.0.1', mp), timeout=.2)
                except OSError:
                    assert time.monotonic() < end, 'server startup timeout'
                    time.sleep(.05)
            monitor.settimeout(None)
            def drain():
                try:
                    while monitor.recv(65536): pass
                except OSError: pass
            monitor_thread = threading.Thread(target=drain, daemon=True)
            monitor_thread.start()
            client = spawn([ROOT / 'build_sim/run_player', '--team', 'DancerSmoke', '--team-id', 91,
                            '--num', 1, '--port', ap, '--motion-policy', policy,
                            '--beam-x', args.beam_x, '--beam-y', args.beam_y, '--beam-theta', 0, '--sim-speed', 4,
                            '--max-frames', args.max_frames or (1800 if args.tip else 600), '--trace', directory / 'client.csv', '--monitor',
                            '--monitor-port', udp.getsockname()[1]] +
                           (['--server-model', args.server_model] if args.server_model else []) +
                           (['--assets-dir', args.assets_dir.resolve()] if args.assets_dir else []) +
                           (['--test-stand'] if command == '0,0,0' else ['--test-cmd', command]), 'client')
            end = time.monotonic() + 40
            while latest.get('sequence', 0) < 100:
                if client.poll() is not None: break
                assert time.monotonic() < end, 'client warmup timeout'
                time.sleep(.02)
            packet = b'(dropBall)'
            monitor.sendall(len(packet).to_bytes(4, 'big') + packet)
            if args.tip:
                while latest.get('sequence', 0) < 200 and client.poll() is None:
                    assert time.monotonic() < end, 'tip setup timeout'
                    time.sleep(.02)
                axis = dict(front=(0, 1, 0), back=(0, -1, 0), left=(1, 0, 0), right=(-1, 0, 0))[args.tip_direction]
                x, y, z = [v * math.sin(.7) for v in axis]
                packet = f'(agent (unum 1) (team DancerSmoke) (move3d {args.beam_x} {args.beam_y} .55 {math.cos(.7)} {x} {y} {z}))'.encode()
                monitor.sendall(len(packet).to_bytes(4, 'big') + packet)
            rc = client.wait(timeout=45)
        finally:
            alive = False; udp.close()
            if monitor:
                try: monitor.shutdown(socket.SHUT_RDWR); monitor.close()
                except OSError: pass
            receiver.join(timeout=1)
            if monitor_thread:
                monitor_thread.join(timeout=1)
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try: process.wait(timeout=5)
                    except subprocess.TimeoutExpired: process.kill(); process.wait()
            for stream in files: stream.close()
        rows = list(csv.DictReader((directory / 'client.csv').open()))
        active = [r for r in rows if r['mode'] == 'PlayOn']
        report = dict(policy=policy, command=command, exit_code=rc, frames=len(rows), active_frames=len(active), server_models=sorted({r.get('server_model', 'unknown') for r in rows}))
        if active:
            start, stop = active[0], active[-1]
            duration = float(stop['time']) - float(start['time'])
            report.update(seconds=duration, min_height=min(float(r['height']) for r in active),
                          dx=(float(stop['truth_x']) - float(start['truth_x'])) / 1000.,
                          dy=(float(stop['truth_y']) - float(start['truth_y'])) / 1000.,
                          yaw_delta=float(stop['truth_yaw']) - float(start['truth_yaw']),
                          phases=sorted({r['phase'] for r in active}),
                          final_height=float(stop['height']),
                          policies=sorted({r['policy'] for r in active}))
            if args.tip:
                getup = [r for r in active if r['phase'] == '4']
                handoffs = [r for r in getup if r.get('dancer_recovery_state') == 'handoff']
                report.update(tip_direction=args.tip_direction, getup_frames=len(getup),
                              getup_models=sorted({r['policy'] for r in getup}),
                              recovery_attempts=max((int(r.get('dancer_recovery_attempts', 0)) for r in getup), default=0))
                if handoffs:
                    first, last = getup[0], handoffs[0]
                    after = [r for r in active if float(r['time']) > float(last['time'])]
                    report.update(handoff_s=float(last['time']) - float(first['time']),
                                  handoff_height=float(last['height']),
                                  amp_after_handoff=sum('2026-10-04' in r['policy'] for r in after),
                                  final_5s_min_height=min(float(r['height']) for r in after[-250:]),
                                  post_handoff_fall=any(r['phase'] in ('3','4') for r in after),
                                  stable_after_handoff=len(after) >= 250 and all(float(r['height']) > .45 and r['phase'] in ('1','2') and float(r.get('gravity_z', -1)) < -.85 for r in after[-250:]))
        if args.tip and policy == 'dancer':
            report['success'] = (rc == 0 and report.get('stable_after_handoff', False) and
                                 not report.get('post_handoff_fall', True) and
                                 report.get('getup_models') == ['model_4999.onnx'])
        reports.append(report)
        (output / 'report.json').write_text(json.dumps(reports, indent=2))
        print(json.dumps(report), flush=True)
        assert rc == 0 and active and report.get('success', True), f'Incomplete smoke run; inspect {directory}'
print('ARTIFACTS', output, flush=True)
