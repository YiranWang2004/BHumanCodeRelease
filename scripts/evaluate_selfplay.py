#!/usr/bin/env python3
"""Run two complete 2x20-minute games with sides/kickoff balanced.

The server ends each half at GameOver. Restart at phase 1 with swapped sides
and carried scores. Observe referee data without changing rules or physics.
"""
import argparse
import csv
import concurrent.futures
import threading
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT.parent / 'dancer-rcssservermj'


def observed_server():
    from rcsssmj.games.soccer.sim.soccer_referee import SoccerReferee
    from rcsssmj.__main__ import soccer_sim
    from rcsssmj.games.teams import TeamSide
    directory = Path(os.environ['EVAL_DIRECTORY'])
    handle = (directory / 'server.csv').open('w', buffering=1)
    writer = csv.writer(handle)
    writer.writerow(['time','play_time','mode','left','right','ball_x','ball_y','ball_z','contact','last_contact'])
    original = SoccerReferee.referee
    previous = [-1., None]
    def observe(self):
        original(self)
        g, s = self.game, self.game.game_state
        now = g.sim_time
        mode = s.play_mode.value
        if now-previous[0] >= .1-1e-8 or mode != previous[1]:
            previous[:] = [now, mode]
            writer.writerow([now,s.play_time,mode,s.left_team_score,s.right_team_score,
                             *g.ball.xpos, str(g.ball.active_contact or ''),str(g.ball.last_contact or '')])
            status = dict(time=now,play_time=s.play_time,mode=mode,left=s.left_team_score,right=s.right_team_score,
                          left_team=s.get_team_name(TeamSide.LEFT),right_team=s.get_team_name(TeamSide.RIGHT))
            pending = directory / 'status.tmp'
            pending.write_text(json.dumps(status))
            pending.replace(directory / 'status.json')
    SoccerReferee.referee = observe
    sys.argv = [sys.argv[0], *sys.argv[2:]]
    soccer_sim()


def stop(process):
    if process and process.poll() is None:
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=12)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()


def run_half(directory, port, half, policy_left, scores, speed, cancel=None):
    directory.mkdir(parents=True)
    for p in [port, port+1]:
        with socket.socket() as s:
            s.bind(('127.0.0.1', p))
    env = os.environ.copy()
    env['EVAL_DIRECTORY'] = str(directory)
    left, right = scores if policy_left else scores[::-1]
    server_cmd = [str(SERVER/'.venv/bin/python'), str(Path(__file__).resolve()), '--server',
                  '--host','127.0.0.1','--aport',str(port),'--mport',str(port+1),
                  '--field','hsl_m_26','--level','foundation','--no-render','--no-realtime','--sync',
                  '--phase',str(half),'--score-left',str(left),'--score-right',str(right)]
    client_cmd = [sys.executable,str(ROOT/'scripts/start_sim_game.py'),'--port',str(port),
                  '--sim-speed',str(speed),'--policy-team','a' if policy_left else 'b',
                  '--team-a','BHumanZJU' if policy_left else 'BHumanNative',
                  '--team-b','BHumanNative' if policy_left else 'BHumanZJU',
                  '--log-dir',str(directory/'players')]
    (directory/'commands.json').write_text(json.dumps(dict(server=server_cmd,clients=client_cmd),indent=2))
    server = clients = None
    start = time.monotonic()
    with (directory/'server.log').open('w') as sh, (directory/'launcher.log').open('w') as ch:
        try:
            server = subprocess.Popen(server_cmd,cwd=directory,env=env,stdout=sh,stderr=subprocess.STDOUT,start_new_session=True)
            for _ in range(200):
                if server.poll() is not None:
                    raise RuntimeError(f'Server startup failed: {directory}')
                try:
                    with socket.create_connection(('127.0.0.1',port),timeout=.2):
                        break
                except OSError:
                    time.sleep(.1)
            else:
                raise RuntimeError('Server startup timeout')
            env.pop('HSL_TRACE_DIR',None); env.pop('HSL_DECISION_DIR',None)
            clients = subprocess.Popen(client_cmd,cwd=ROOT,env=env,stdout=ch,stderr=subprocess.STDOUT,start_new_session=True)
            for _ in range(300):
                if clients.poll() is not None:
                    raise RuntimeError(f'Client startup failed: {directory}')
                logs = list((directory/'players').glob('*.log'))
                if len(logs) == 6 and all('[HSL] native' in p.read_text() for p in logs):
                    break
                time.sleep(.2)
            else:
                raise RuntimeError('Six-player startup timeout')
            identity = json.loads((directory/'status.json').read_text())
            expected_left = 'BHumanZJU' if policy_left else 'BHumanNative'
            if identity['left_team'] != expected_left:
                raise RuntimeError(f'Team registration order mismatch: {identity}')
            with socket.create_connection(('127.0.0.1',port+1),timeout=3) as s:
                message = b'(kickOff Left)'
                s.sendall(len(message).to_bytes(4,'big')+message)
            last_print = 0
            while True:
                if cancel and cancel.is_set():
                    raise RuntimeError('Evaluation cancelled')
                if clients.poll() is not None or server.poll() is not None:
                    raise RuntimeError(f'Premature process exit: {directory}')
                status = json.loads((directory/'status.json').read_text())
                if time.monotonic()-last_print > 30:
                    print(directory.name, status, flush=True)
                    last_print = time.monotonic()
                if status['mode'] == 'GameOver':
                    if status['play_time'] < (half+1)*1200-.05:
                        raise RuntimeError('Premature GameOver')
                    final = [status['left'],status['right']]
                    result = dict(complete=True,half=half+1,policy_left=policy_left,
                                  score=final if policy_left else final[::-1],
                                  status=status,wall_seconds=time.monotonic()-start)
                    (directory/'result.json').write_text(json.dumps(result,indent=2))
                    return result
                if time.monotonic()-start > 7200:
                    raise RuntimeError('Half exceeded 2 wall hours')
                time.sleep(1)
        finally:
            stop(clients)
            stop(server)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--port',type=int,default=61300)
    p.add_argument('--matches',type=int,default=2)
    p.add_argument('--speed',type=float,default=100.)
    p.add_argument('--jobs',type=int,choices=[1,2],default=1)
    a = p.parse_args()
    directory = a.output.resolve(); directory.mkdir(parents=True,exist_ok=False)
    files = [ROOT/'build_sim/run_player',ROOT/'scripts/dancer_kick_worker.py',ROOT/'scripts/team_control.py']
    files += list((ROOT.parent/'dancer-robocupdemo/models').glob('student_20/model_7900_bhuman_size3.onnx'))
    manifest = dict(args=vars(a)|{'output':str(directory)},sha256={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in files},
                    half_seconds=1200,comparison='same HSL adapter and K1BHuman model; native motions versus ZJU AMP + VisualKick')
    (directory/'manifest.json').write_text(json.dumps(manifest,indent=2))
    results = []
    cancel = threading.Event()
    def run_match(match):
        scores = [0,0]; halves = []
        for half in range(2):
            result = run_half(directory/f'match{match+1}-half{half+1}',a.port+10*match,half,(match+half)%2==0,scores,a.speed,cancel)
            scores = result['score']; halves.append(result)
        return dict(match=match+1,score=scores,halves=halves)
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        futures = [pool.submit(run_match, match) for match in range(a.matches)]
        try:
            for future in concurrent.futures.as_completed(futures):
                results.append(future.result()); results.sort(key=lambda r:r['match'])
                (directory/'results.json').write_text(json.dumps(results,indent=2))
                print('COMPLETE', future.result(), flush=True)
        finally:
            cancel.set()


if __name__ == '__main__':
    if '--server' in sys.argv:
        observed_server()
    else:
        main()
