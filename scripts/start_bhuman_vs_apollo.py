#!/usr/bin/env python3
"""Start B-Human K1 vs the supplied Apollo binary, optionally with the server."""
import argparse
from datetime import datetime
import math
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import time


def apollo_connected(pid, port):
    """Check the owned process's established agent socket on Linux."""
    try:
        inodes = set()
        for fd in (Path('/proc') / str(pid) / 'fd').iterdir():
            try:
                target = fd.readlink().as_posix()
            except OSError:
                continue
            if target.startswith('socket:['):
                inodes.add(target[8:-1])
        for family in ['tcp', 'tcp6']:
            table = Path('/proc') / str(pid) / 'net' / family
            for line in table.read_text().splitlines()[1:]:
                fields = line.split()
                if fields[3] == '01' and fields[9] in inodes and int(fields[2].split(':')[1], 16) == port:
                    return True
    except OSError:
        pass
    return False


def trace_time(path):
    """Return the last complete native sensor-frame timestamp in a trace."""
    try:
        with path.open('rb') as file:
            file.seek(max(0, path.stat().st_size - 8192))
            rows = file.read().split(b'\n')
        for row in reversed(rows[:-1]):
            try:
                return float(row.split(b',', 1)[0])
            except ValueError:
                continue
    except OSError:
        pass
    return None


def tcp_open(host, port):
    try:
        with socket.create_connection((host, port), timeout=.2):
            return True
    except OSError:
        return False


def parse_args(argv=None):
    repo = Path(__file__).resolve().parents[1]
    workspace = repo.parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=60000)
    parser.add_argument('--monitor-port', type=int, default=60001)
    parser.add_argument('--players', type=int, choices=range(1, 8), default=7)
    parser.add_argument('--robot', choices=['K1', 'T1'], default='K1', help='B-Human robot; Apollo uses its own model.')
    parser.add_argument('--team-bhuman', help='Default: BHumanK1 or BHumanT1, matching --robot.')
    parser.add_argument('--team-apollo', default='Apollo3D')
    parser.add_argument('--team-id', type=int, default=81, help='B-Human UDP team ID (1..99).')
    parser.add_argument('--localization', choices=['truth', 'measured'], default='truth')
    parser.add_argument('--kickoff', choices=['none', 'left', 'right'], default='none')
    parser.add_argument('--apollo-dir', type=Path, default=workspace / 'apollo')
    parser.add_argument('--apollo-loader', type=Path, help='Override the isolated glibc loader.')
    parser.add_argument('--start-server', action='store_true', help='Also start and manage the local rcssservermj server.')
    parser.add_argument('--server-repo', type=Path, default=workspace / 'rcssservermj')
    conda_python = Path.home() / 'miniconda3/envs/robocup_sim/bin/python'
    parser.add_argument('--server-python', type=Path,
                        default=Path(os.environ.get('RCSS_PYTHON', str(conda_python if conda_python.is_file() else sys.executable))))
    parser.add_argument('--render', action=argparse.BooleanOptionalAction, default=True, help='Viewer for --start-server.')
    parser.add_argument('--realtime', action=argparse.BooleanOptionalAction, default=True, help='Pacing for --start-server.')
    parser.add_argument('--sim-speed', type=float, default=1.)
    parser.add_argument('--ready-timeout', type=float, default=60.)
    parser.add_argument('--duration', type=float, help='Optional wall-clock seconds after both teams are ready.')
    parser.add_argument('--log-dir', type=Path, default=Path('/tmp') / ('bhuman-vs-apollo-' + datetime.now().strftime('%Y%m%d-%H%M%S-%f')))
    args = parser.parse_args(argv)
    args.team_bhuman = args.team_bhuman or 'BHuman' + args.robot
    allowed = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-'
    for team in [args.team_bhuman, args.team_apollo]:
        if not team or any(c not in allowed for c in team):
            parser.error('Team names must contain only ASCII letters, digits, _ or -.')
    if args.team_bhuman == args.team_apollo:
        parser.error('The two teams need different names.')
    if not 1 <= args.team_id <= 99:
        parser.error('--team-id must be between 1 and 99.')
    if not all(1 <= port <= 65535 for port in [args.port, args.monitor_port]) or args.port == args.monitor_port:
        parser.error('Agent and monitor ports must be distinct, between 1 and 65535.')
    if any(not math.isfinite(value) or value <= 0 for value in [args.sim_speed, args.ready_timeout] + ([] if args.duration is None else [args.duration])):
        parser.error('Speed, timeout and duration must be finite positive numbers.')
    args.repo = repo
    args.apollo_dir = args.apollo_dir.resolve()
    args.server_repo = args.server_repo.resolve()
    args.server_python = args.server_python.resolve()
    args.log_dir = args.log_dir.resolve()
    if args.apollo_loader is None:
        loader = workspace / 'dancer-robocupdemo/third_party/apollo_runtime/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2'
        if loader.is_file():
            args.apollo_loader = loader
    elif not args.apollo_loader.is_file():
        parser.error(f'Apollo loader does not exist: {args.apollo_loader}')
    if args.apollo_loader is not None:
        args.apollo_loader = args.apollo_loader.resolve()
    return args


def main(argv=None):
    args = parse_args(argv)
    executable = args.repo / 'build_sim/run_player'
    apollo = args.apollo_dir / 'Apollo'
    for binary in [executable, apollo]:
        if not binary.is_file() or not os.access(binary, os.X_OK):
            raise RuntimeError(f'Executable missing: {binary}')
    for asset in ['walk/policy.onnx', 'kick/policy.onnx', 'getup/policy.onnx', 'dribble/policy.onnx', 'keyframes/neutral.yaml']:
        if not (args.apollo_dir / 'assets/apollo' / asset).is_file():
            raise RuntimeError(f'Apollo asset missing: {asset}')
    apollo_env = os.environ.copy()
    libraries = str(args.apollo_dir / 'libs')
    if args.apollo_loader is not None:
        libraries = str(args.apollo_loader.resolve().parent) + os.pathsep + libraries
        # Resolve dependencies without running Apollo or connecting an agent.
        probe = subprocess.run([str(args.apollo_loader), '--library-path', libraries, '--list', str(apollo)],
                               capture_output=True, text=True, timeout=10)
        if probe.returncode:
            raise RuntimeError('Apollo runtime check failed:\n' + probe.stderr)
        apollo_command = [str(args.apollo_loader), '--library-path', libraries, str(apollo)]
    else:
        apollo_command = [str(apollo)]
    apollo_env['LD_LIBRARY_PATH'] = libraries + (os.pathsep + apollo_env['LD_LIBRARY_PATH'] if apollo_env.get('LD_LIBRARY_PATH') else '')
    if args.start_server:
        if not args.server_python.is_file() or not (args.server_repo / 'src/rcsssmj/__main__.py').is_file():
            raise RuntimeError('Server interpreter or source missing; set --server-python and --server-repo.')
        if tcp_open(args.host, args.port) or tcp_open(args.host, args.monitor_port):
            raise RuntimeError('A requested port is already in use; omit --start-server or choose different ports.')
    elif not tcp_open(args.host, args.port):
        raise RuntimeError('Server unavailable. Start it first or add --start-server.')
    args.log_dir.mkdir(parents=True, exist_ok=False)
    children, logs, bhuman, apollo_players = [], [], [], []

    def spawn(command, name, cwd, env=None):
        file = (args.log_dir / name).open('w')
        logs.append(file)
        child = subprocess.Popen(command, cwd=cwd, env=env, stdout=file, stderr=subprocess.STDOUT, start_new_session=True)
        children.append((child, name))
        return child

    def check():
        for child, name in children:
            if child.poll() is not None:
                tail = (args.log_dir / name).read_text(errors='replace')[-2500:]
                raise RuntimeError(f'{name} exited with {child.returncode}. Log: {args.log_dir / name}\n{tail}')

    def wait_until(condition, description):
        deadline = time.monotonic() + args.ready_timeout
        while not condition():
            check()
            if time.monotonic() >= deadline:
                raise RuntimeError(f'Timeout waiting for {description}; logs: {args.log_dir}')
            time.sleep(.1)
        check()

    def start_bhuman(number):
        trace = args.log_dir / f'{args.team_bhuman}_{number}.csv'
        spawn([str(executable), '--robot', args.robot, '--host', args.host, '--port', str(args.port),
               '--team', args.team_bhuman, '--team-id', str(args.team_id), '--num', str(number),
               '--num-players', str(args.players), '--field', 'fifa7vs7', '--localization', args.localization,
               '--sim-speed', str(args.sim_speed), '--trace', str(trace)], f'{args.team_bhuman}_{number}.log', args.repo)
        bhuman.append(trace)

    def stop(_signum, _frame):
        raise KeyboardInterrupt

    def signal_group(child, sig):
        if child.poll() is None:
            try:
                os.killpg(child.pid, sig)
            except ProcessLookupError:
                pass

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    try:
        print(f'Logs: {args.log_dir}', flush=True)
        if args.start_server:
            env = os.environ.copy()
            env['PYTHONPATH'] = str(args.server_repo / 'src') + (os.pathsep + env['PYTHONPATH'] if env.get('PYTHONPATH') else '')
            spawn([str(args.server_python), '-m', 'rcsssmj', '--host', args.host, '--aport', str(args.port),
                   '--mport', str(args.monitor_port), '--field', 'fifa7vs7', '--rules', 'ssim', '--sync', '--bhuman-physics',
                   '--render' if args.render else '--no-render', '--realtime' if args.realtime else '--no-realtime'],
                  'server.log', args.log_dir, env)
            wait_until(lambda: tcp_open(args.host, args.port), 'the server')
        # Confirm an actual B-Human sensor frame before Apollo can register:
        # the first registered team becomes Left on an empty server.
        start_bhuman(1)
        wait_until(lambda: trace_time(bhuman[0]) is not None, 'B-Human player 1')
        for number in range(2, args.players + 1):
            start_bhuman(number)
        for number in range(1, args.players + 1):
            apollo_players.append(spawn(apollo_command + ['--team', args.team_apollo, '--player-number', str(number),
                                  '--host', args.host, '--port', str(args.port), '--asset-root', str(args.apollo_dir / 'assets/apollo')],
                                  f'{args.team_apollo}_{number}.log', args.apollo_dir, apollo_env))
        wait_until(lambda: all(trace_time(path) is not None for path in bhuman)
                   and all(apollo_connected(child.pid, args.port) for child in apollo_players), 'both rosters to connect')
        baselines = [trace_time(path) for path in bhuman]
        wait_until(lambda: all((trace_time(path) or 0) >= baseline + .5 for path, baseline in zip(bhuman, baselines)),
                   'sensor frames after Apollo connected')
        print(f'{args.players}v{args.players} ready: {args.team_bhuman} ({args.robot}) vs {args.team_apollo} (Apollo).', flush=True)
        print('On an empty server: B-Human is Left (K), Apollo is Right (J). Ctrl+C stops this launch.', flush=True)
        if args.kickoff != 'none':
            payload = f'(kickOff {args.kickoff.title()})'.encode('ascii')
            with socket.create_connection((args.host, args.monitor_port), timeout=5) as monitor:
                monitor.sendall(len(payload).to_bytes(4, 'big') + payload)
        started = time.monotonic()
        while args.duration is None or time.monotonic() - started < args.duration:
            check()
            time.sleep(.2)
    except KeyboardInterrupt:
        pass
    finally:
        # Signal only the process groups created by this invocation, including
        # the server only when --start-server was requested.
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        for sig, grace in [(signal.SIGINT, 5), (signal.SIGTERM, 2), (signal.SIGKILL, 1)]:
            for child, _ in reversed(children):
                signal_group(child, sig)
            deadline = time.monotonic() + grace
            for child, _ in reversed(children):
                try:
                    child.wait(timeout=max(.01, deadline - time.monotonic()))
                except subprocess.TimeoutExpired:
                    pass
            if all(child.poll() is not None for child, _ in children):
                break
        for file in logs:
            file.close()


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f'Error: {error}', file=sys.stderr)
        sys.exit(1)
