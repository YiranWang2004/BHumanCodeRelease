#!/usr/bin/env python3
"""Start B-Human vs B-Human; one selectable team uses ZJUDancer AMP + VisualKick."""
import argparse
import math
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def arguments(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host', default='127.0.0.1')
    p.add_argument('--port', type=int, default=60000)
    p.add_argument('--team-port-base', type=int, help='UDP ports for teams A/B; default server port and port+1')
    p.add_argument('--field', choices=['hsl_s', 'hsl_m', 'hsl_l', 'hsl_whrg'], default='hsl_m')
    p.add_argument('--level', choices=['foundation', 'advanced', '11v11'], default='foundation')
    p.add_argument('--num-players', type=int, help='Override roster size for each team')
    p.add_argument('--localization', choices=['truth', 'measured'], default='truth')
    p.add_argument('--sim-speed', type=float, default=1.)
    p.add_argument('--team-a', default='BHumanZJU')
    p.add_argument('--team-b', default='BHumanNative')
    p.add_argument('--policy-team', choices=['a', 'b', 'none'], default='a')
    p.add_argument('--dancer-repo', type=Path, default=ROOT.parent / 'dancer-robocupdemo')
    p.add_argument('--dancer-python', help='Python with NumPy/ONNX Runtime (or K1_PYTHON)')
    p.add_argument('--log-dir', type=Path)
    p.add_argument('--dry-run', action='store_true', help='Print commands without connecting to server')
    args = p.parse_args(argv)
    if args.team_a == args.team_b or any(not re.fullmatch(r'[A-Za-z0-9_-]+', t) for t in [args.team_a, args.team_b]):
        p.error('Team names must differ and contain only ASCII letters, digits, _ or -')
    if not 0 < args.port <= 65535 or not math.isfinite(args.sim_speed) or args.sim_speed <= 0:
        p.error('Invalid port or sim-speed')
    if args.level == '11v11' and args.field != 'hsl_l':
        p.error('11v11 requires hsl_l')
    default = 11 if args.level == '11v11' else 5 if args.field == 'hsl_whrg' else (4 if args.level == 'foundation' else 7) if args.field == 'hsl_s' else (3 if args.level == 'foundation' else 5)
    args.num_players = args.num_players if args.num_players is not None else default
    if not 1 <= args.num_players <= (11 if args.field == 'hsl_l' else 7):
        p.error('Invalid num-players for selected field')
    args.team_port_base = args.port if args.team_port_base is None else args.team_port_base
    if not 0 < args.team_port_base < 65535:
        p.error('team-port-base must be 1..65534 (two UDP ports)')
    args.dancer_repo = args.dancer_repo.resolve()
    if args.dancer_python is None:
        candidates = [args.dancer_repo / '.venv/bin/python', args.dancer_repo / '.venv-sim-motion/bin/python']
        args.dancer_python = os.environ.get('K1_PYTHON') or next((str(x) for x in candidates if x.is_file()), 'python3')
    if '/' in args.dancer_python:
        args.dancer_python = os.path.abspath(args.dancer_python)
    return args


def commands(args):
    for side, team in enumerate([args.team_a, args.team_b], 1):
        for number in range(1, args.num_players + 1):
            policy = args.policy_team == ('a' if side == 1 else 'b')
            cmd = [str(ROOT / 'build_sim/run_player'), '--host', args.host, '--port', str(args.port),
                   '--team', team, '--team-id', str(side), '--team-port', str(args.team_port_base+side-1), '--num', str(number),
                   '--num-players', str(args.num_players), '--field', args.field,
                   '--localization', args.localization, '--sim-speed', str(args.sim_speed),
                   '--kick-policy', 'zjudancer' if policy else 'native']
            if policy:
                cmd += ['--dancer-repo', str(args.dancer_repo), '--dancer-python', args.dancer_python]
            yield team, number, cmd


def main(argv=None):
    args = arguments(argv)
    if args.dry_run:
        for _, _, cmd in commands(args):
            print(shlex.join(cmd))
        return 0
    if not (ROOT / 'build_sim/run_player').is_file():
        raise SystemExit('请先运行 bash scripts/build_sim.sh')
    if args.policy_team != 'none':
        # Fail before connecting any robots when model/dependency setup is bad.
        code = 'from pathlib import Path; from dancer_kick_worker import Controller; import sys; Controller(Path(sys.argv[1]))'
        subprocess.run([args.dancer_python, '-c', code, str(args.dancer_repo)], cwd=ROOT / 'scripts', check=True)
    directory = args.log_dir or Path(tempfile.mkdtemp(prefix='bhuman-selfplay-'))
    directory = directory.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault('HSL_TRACE_DIR', str(directory / 'csv'))
    os.environ.setdefault('HSL_DECISION_DIR', str(directory / 'decisions'))
    print(f'日志目录：{directory}', flush=True)
    children, logs = [], []
    stopped = False

    def stop(_signum, _frame):
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    try:
        for team, number, cmd in commands(args):
            if stopped:
                break
            handle = (directory / f'{team}_{number}.log').open('w')
            logs.append(handle)
            child = subprocess.Popen(cmd, cwd=ROOT, stdout=handle, stderr=subprocess.STDOUT, start_new_session=True)
            children.append(child)
            print(f'{team} #{number}: pid={child.pid}', flush=True)
            # Registration order defines server left/right team identity.
            # Python policy loading must not let the native team overtake A.
            startup_deadline = time.monotonic() + 30
            log_path = directory / f'{team}_{number}.log'
            while '[HSL] native' not in log_path.read_text():
                if stopped or any(c.poll() is not None for c in children):
                    break
                if time.monotonic() >= startup_deadline:
                    raise RuntimeError(f'客户端注册超时：{team} #{number}，请查看 {directory}')
                time.sleep(.05)
            if any(c.poll() is not None for c in children):
                raise RuntimeError(f'客户端提前退出，请查看 {directory}')
        while not stopped:
            if any(c.poll() is not None for c in children):
                raise RuntimeError(f'客户端提前退出，请查看 {directory}')
            time.sleep(.2)
    finally:
        for child in children:
            if child.poll() is None:
                # run_player closes/reaps its Python worker during normal exit.
                child.terminate()
        deadline = time.monotonic() + 5
        for child in children:
            try:
                child.wait(timeout=max(.01, deadline-time.monotonic()))
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()
        for handle in logs:
            handle.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
