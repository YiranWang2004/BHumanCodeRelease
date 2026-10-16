#!/usr/bin/env python3
"""Check native 2026 kick inference, foot/ball contact, and real ball movement.

Uses an isolated truth-localization match with one native B-Human striker.
The server child only records MuJoCo states; it does not modify game or motion.
"""
import argparse
import csv
import math
import os
from pathlib import Path
import resource
import signal
import socket
import subprocess
import sys
import tempfile
import time


def server_child(port, work):
    import mujoco
    from rcsssmj.__main__ import soccer_sim

    original_step = mujoco.mj_step
    with (work / 'ball.csv').open('w', buffering=1) as output:
        writer = csv.writer(output)
        writer.writerow(['time', 'x', 'y', 'z', 'speed', 'foot_contact'])

        def record_step(model, data, nstep=1):
            original_step(model, data, nstep)
            if not any(name.endswith('simrobot_servo_lle1') for name in (model.numeric(i).name for i in range(model.nnumeric))):
                return
            ball = model.geom('ball').id
            feet = []
            for contact in data.contact:
                if ball in contact.geom:
                    other = contact.geom[1] if contact.geom[0] == ball else contact.geom[0]
                    body = model.body(model.geom_bodyid[other]).name
                    if body.startswith('r-0-3-') and body.endswith(('left_foot_link', 'right_foot_link')):
                        feet.append(body)
            joint = data.joint('ball-root')
            writer.writerow([data.time, *joint.qpos[:3], math.hypot(*joint.qvel[:2]), '|'.join(feet)])

        mujoco.mj_step = record_step
        sys.argv = ['rcssservermj', '--host', '127.0.0.1', '--aport', str(port), '--mport', str(port + 1),
                    '--field', 'hsl_m_26', '--no-render', '--no-realtime', '--sync']
        soccer_sim()


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def check(args):
    root = Path(__file__).resolve().parents[1]
    server_python = args.server_repo / '.venv/bin/python'
    executable = root / 'build_sim/run_player'
    work = Path(tempfile.mkdtemp(prefix='bhuman2026-kick-'))
    print('Logs:', work, flush=True)
    processes, handles = [], []
    try:
        # Fail before starting a client if a user's server occupies either port.
        for port in [args.port, args.port + 1]:
            with socket.socket() as probe:
                probe.bind(('127.0.0.1', port))
        for command, name, cwd in [
            ([str(server_python), str(Path(__file__).resolve()), '--server-child', '--port', str(args.port), '--work', str(work)], 'server', work),
            ([str(executable), '--port', str(args.port), '--team', 'BHumanKickCheck', '--team-id', '97', '--num', '3', '--num-players', '3',
              '--field', 'hsl_m', '--localization', 'truth', '--beam-x', '-1.8', '--beam-y', '0', '--beam-theta', '0',
              '--sim-speed', '10', '--max-frames', '4000', '--trace', str(work / 'client.csv')], 'client', root),
        ]:
            handle = (work / (name + '.log')).open('w')
            handles.append(handle)
            process = subprocess.Popen(command, cwd=cwd, stdout=handle, stderr=subprocess.STDOUT, start_new_session=True, preexec_fn=no_core)
            processes.append(process)
            if name == 'server':
                deadline = time.monotonic() + 15
                while True:
                    try:
                        with socket.create_connection(('127.0.0.1', args.port), timeout=.2):
                            break
                    except OSError:
                        if process.poll() is not None or time.monotonic() > deadline:
                            raise RuntimeError('Server failed to start; see server.log')
                        time.sleep(.1)
        client = processes[-1]
        deadline = time.monotonic() + 20
        while not (work / 'client.csv').exists() or (work / 'client.csv').stat().st_size < 2000:
            if client.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('Client failed to initialize; see client.log')
            time.sleep(.02)
        payload = b'(kickOff Left)'
        with socket.create_connection(('127.0.0.1', args.port + 1), timeout=3) as monitor:
            monitor.sendall(len(payload).to_bytes(4, 'big') + payload)
        code = client.wait(timeout=90)
        if code:
            raise RuntimeError(f'Client exited with {code}; see client.log')
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGINT)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
        for handle in handles:
            handle.close()
    client_rows = list(csv.DictReader((work / 'client.csv').open()))
    ball_rows = list(csv.DictReader((work / 'ball.csv').open()))
    previous, kick_times = 0, []
    for row in client_rows:
        count = int(row['kick_inferences'])
        if count > previous:
            kick_times.append(float(row['time']))
        previous = count
    contacts = [row for row in ball_rows if row['foot_contact'] and any(abs(float(row['time']) - t) < .06 for t in kick_times)]
    movements = []
    first_movement = 0.
    for contact in contacts:
        previous_row = contact
        contact_movement = 0.
        for row in ball_rows:
            elapsed = float(row['time']) - float(contact['time'])
            if elapsed <= 0:
                continue
            if elapsed > .5:
                break
            dt = float(row['time']) - float(previous_row['time'])
            jump = math.hypot(float(row['x']) - float(previous_row['x']), float(row['y']) - float(previous_row['y']))
            if jump > .02 + 2 * dt * max(float(row['speed']), float(previous_row['speed'])):
                break  # Exclude referee ball placement after a goal/out of bounds.
            contact_movement = max(contact_movement, math.hypot(float(row['x']) - float(contact['x']), float(row['y']) - float(contact['y'])))
            previous_row = row
        if not movements:
            first_movement = contact_movement
        movements.append(contact_movement)
    movement = max(movements, default=0.)
    print(f'Kick inferences: {previous}; physics samples with foot contact during kick: {len(contacts)}; '
          f'first contact following 0.5 s ball movement: {first_movement:.3f} m; max continuous movement: {movement:.3f} m')
    if not contacts or movement < .1:
        raise RuntimeError('No verified kick with ball movement; inspect the recorded traces')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server-repo', type=Path, default=Path(__file__).resolve().parents[2] / 'dancer-rcssservermj')
    parser.add_argument('--port', type=int, default=60220)
    parser.add_argument('--server-child', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--work', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.server_child:
        server_child(args.port, args.work)
    else:
        check(args)
