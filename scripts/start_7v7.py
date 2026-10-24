#!/usr/bin/env python3
"""Start two native B-Human teams against the rcssservermj 7v7 backend."""
import argparse
from datetime import datetime
from pathlib import Path
import signal
import socket
import subprocess
import time


def main():
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=60000)
    parser.add_argument('--monitor-port', type=int, default=60001)
    parser.add_argument('--server-model', choices=['K1', 'K1BHuman', 'T1BHuman'], help='Override both teams server model; default follows run_player.')
    parser.add_argument('--robot', choices=['K1', 'T1'], default='K1')
    parser.add_argument('--robot-a', choices=['K1', 'T1'], help='Override the left team robot type.')
    parser.add_argument('--robot-b', choices=['K1', 'T1'], help='Override the right team robot type.')
    parser.add_argument('--players', type=int, choices=range(1, 8), default=7)
    parser.add_argument('--team-a', default='BHumanBlue')
    parser.add_argument('--team-b', default='BHumanRed')
    parser.add_argument('--team-id-a', type=int, default=81)
    parser.add_argument('--team-id-b', type=int, default=82)
    parser.add_argument('--localization', choices=['truth', 'measured'], default='truth')
    parser.add_argument('--strategy', choices=['current', 'balanced', 'attacking'], default='current')
    parser.add_argument('--motion-policy', choices=['native', 'dancer'], default='native')
    parser.add_argument('--kick-mode', choices=['fast-max', 'native'], default='fast-max',
                        help='Measured localization only; truth always uses strong/max speed with pose constraints.')
    parser.add_argument('--kickoff', choices=['none', 'left', 'right'], default='none')
    parser.add_argument('--sim-speed', type=float, default=1.)
    parser.add_argument('--duration', type=float, help='Optional wall-clock seconds for diagnostics.')
    parser.add_argument('--log-dir', type=Path, default=Path('/tmp') / ('bhuman-rcss7v7-' + datetime.now().strftime('%Y%m%d-%H%M%S-%f')))
    args = parser.parse_args()
    if args.server_model and any(args.server_model not in (['K1', 'K1BHuman'] if robot == 'K1' else ['T1BHuman']) for robot in (args.robot_a or args.robot, args.robot_b or args.robot)):
        parser.error('--server-model does not match both teams robot types')
    if args.motion_policy == 'dancer' and any(robot != 'K1' for robot in (args.robot_a or args.robot, args.robot_b or args.robot)):
        parser.error('--motion-policy dancer requires K1 on both teams')
    if args.team_a == args.team_b or args.team_id_a == args.team_id_b:
        parser.error('The two teams need different names and UDP team IDs.')
    if any(not 1 <= n <= 99 for n in [args.team_id_a, args.team_id_b]):
        parser.error('Team IDs must be between 1 and 99.')
    if args.sim_speed <= 0 or (args.duration is not None and args.duration <= 0):
        parser.error('Simulation speed and duration must be positive.')
    executable = repo / 'build_sim/run_player'
    if not executable.is_file():
        parser.error('Build the client first: scripts/build_sim.sh')
    args.log_dir.mkdir(parents=True, exist_ok=False)
    children, files = [], []

    def stop(_signum, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    def spawn(team, team_id, number):
        log = (args.log_dir / f'{team}_{number}.log').open('w')
        files.append(log)
        child = subprocess.Popen([
            str(executable), '--host', args.host, '--port', str(args.port),
            '--team', team, '--team-id', str(team_id), '--num', str(number),
            '--num-players', str(args.players), '--field', 'fifa7vs7',
            '--robot', (args.robot_a or args.robot) if team == args.team_a else (args.robot_b or args.robot),
            '--localization', args.localization, '--sim-speed', str(args.sim_speed),
            '--strategy', args.strategy,
            '--motion-policy', args.motion_policy,
            '--kick-mode', args.kick_mode,
            '--trace', str(args.log_dir / f'{team}_{number}.csv'),
        ] + (['--server-model', args.server_model] if args.server_model else []),
            cwd=repo, stdout=log, stderr=subprocess.STDOUT)
        children.append(child)

    def ready(team, number):
        trace = args.log_dir / f'{team}_{number}.csv'
        return trace.exists() and trace.stat().st_size > 12000

    def check():
        for child in children:
            if child.poll() is not None:
                raise RuntimeError(f'Client {child.pid} exited with {child.returncode}; see {args.log_dir}')

    try:
        # The first initialized team becomes Left. Wait for its sensor frames
        # before any Right client can connect; never rely on process launch order.
        spawn(args.team_a, args.team_id_a, 1)
        deadline = time.monotonic() + 45
        while not ready(args.team_a, 1):
            check()
            if time.monotonic() > deadline:
                raise RuntimeError('First player did not receive frames within 45 seconds.')
            time.sleep(.1)
        for number in range(2, args.players + 1):
            spawn(args.team_a, args.team_id_a, number)
        for number in range(1, args.players + 1):
            spawn(args.team_b, args.team_id_b, number)
        deadline = time.monotonic() + 60
        while not all(ready(team, n) for team in [args.team_a, args.team_b] for n in range(1, args.players + 1)):
            check()
            if time.monotonic() > deadline:
                raise RuntimeError('Roster did not become ready within 60 seconds.')
            time.sleep(.1)
        print(f'{args.players}v{args.players} connected; Left={args.team_a}, Right={args.team_b}. Logs: {args.log_dir}', flush=True)
        if args.kickoff != 'none':
            payload = f'(kickOff {args.kickoff.title()})'.encode('ascii')
            with socket.create_connection((args.host, args.monitor_port), timeout=5) as monitor:
                monitor.sendall(len(payload).to_bytes(4, 'big') + payload)
        start = time.monotonic()
        while args.duration is None or time.monotonic() - start < args.duration:
            check()
            time.sleep(.2)
    except KeyboardInterrupt:
        pass
    finally:
        for child in children:
            if child.poll() is None:
                child.send_signal(signal.SIGINT)
        deadline = time.monotonic() + 5
        for child in children:
            try:
                child.wait(timeout=max(.1, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        for file in files:
            file.close()


if __name__ == '__main__':
    main()
