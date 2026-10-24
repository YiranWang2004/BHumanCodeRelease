#!/usr/bin/env python3
"""Isolated 7v7 comparison against the same Apollo; retain server truth for kick speeds."""
import argparse, fcntl, hashlib, json, os, signal, socket, subprocess, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[3]
P = argparse.ArgumentParser()
P.add_argument('--out', type=Path, required=True)
P.add_argument('--unfair-server', type=Path, required=True)
P.add_argument('--dancer-binary',type=Path,default=ROOT/'dancer-robocupdemo/build_sim/run_player')
P.add_argument('--seconds', type=float, default=120.)
P.add_argument('--wait-for-match-lock',action='store_true')
P.add_argument('--runs', default='bhuman_current_101,dancer_unfair_101,bhuman_current_202,dancer_unfair_202')
a = P.parse_args()
D = ROOT/'dancer-robocupdemo'; B = ROOT/'BHumanCodeRelease'
PY = '/home/robotics/miniconda3/envs/robocup_sim/bin/python'
lock = open('/tmp/k1_sim_match_team12.lock', 'w')
fcntl.flock(lock, fcntl.LOCK_EX | (0 if a.wait_for_match_lock else fcntl.LOCK_NB))
def ports():
    for p in range(62100, 62990, 2):
        try:
            with socket.socket() as s, socket.socket() as t:
                s.bind(('127.0.0.1',p)); t.bind(('127.0.0.1',p+1)); return p
        except OSError: pass
    raise RuntimeError('No test ports')
for run in a.runs.split(','):
    client, physics, seed = run.split('_'); seed = int(seed)
    out = a.out/run; out.mkdir(exist_ok=False)
    server = ROOT/'rcssservermj' if physics == 'current' else a.unfair_server
    port = ports(); processes=[]; files=[]
    scenario=dict(kind='match', players=14, seed=seed, duration=a.seconds, settle=5., opening='kickoff', right=False, kickoff='left')
    (out/'scenario.json').write_text(json.dumps(scenario,indent=2))
    env = {k:v for k,v in os.environ.items() if not k.startswith('K1_') and k not in ('PYTHONPATH','LD_LIBRARY_PATH')}
    env.update(OMP_NUM_THREADS='1',OPENBLAS_NUM_THREADS='1',MKL_NUM_THREADS='1',PYTHONHASHSEED=str(seed),K1_PYTHON=str(D/'.venv-sim-motion/bin/python'))
    def spawn(cmd,name,extra=None):
        cmd=list(map(str,cmd)); f=(out/(name+'.log')).open('w'); files.append(f)
        p=subprocess.Popen(cmd,cwd=out,env=env| (extra or {}),stdout=f,stderr=subprocess.STDOUT,start_new_session=True)
        processes.append(p)
        with (out/'commands.jsonl').open('a') as log:log.write(json.dumps(dict(name=name,cmd=cmd,extra=extra))+'\n')
    def wait(test, seconds):
        start=time.monotonic(); printed=start
        while not test():
            if any(p.poll() is not None for p in processes):raise RuntimeError(f'Child failed: {out}')
            if time.monotonic()-start>seconds:raise TimeoutError(str(out))
            if time.monotonic()-printed>30:
                print('RUNNING',run,round(time.monotonic()-start),'wall seconds',flush=True);printed=time.monotonic()
            time.sleep(.1)
    def listening():
        try:
            with socket.create_connection(('127.0.0.1',port+1),.1):return True
        except OSError:return False
    try:
        spawn([PY,D/'scripts/stage0/server.py','--scenario',out/'scenario.json','--server-root',server,
               '--host','127.0.0.1','--aport',port,'--mport',port+1,'--rules','ssim','--field','fifa7vs7','--sync','--no-render','--no-realtime'] +
              (['--bhuman-physics'] if physics=='current' else []),'server')
        wait(listening,30)
        for n in range(1,8):
            if client=='bhuman':
                spawn([B/'build_sim/run_player','--assets-dir',B,'--port',port,'--team','Dancer','--team-id',92,'--num',n,
                       '--motion-policy','dancer','--server-model','K1','--sim-speed',20,'--trace',out/f'BHuman_{n}.csv'],f'Dancer{n}')
            else:
                spawn([a.dancer_binary,'--assets-dir',D,'--host','127.0.0.1','--port',port,'--team','Dancer','--team-id',232,
                       '--num',n,'--server-profile','ssim','--sim-speed',20],f'Dancer{n}',
                      dict(K1_SIM_SEED=str(seed+n),K1_SIM_KICK_BACKEND='student',K1_SIM_STUDENT_VARIANT='normal',K1_SIM_MOTION_TRACE_DIR=str(out)))
            if n==1:wait(lambda:(out/'connected_Dancer').exists(),30)
        folder=ROOT/'apollo';loader=D/'third_party/apollo_runtime/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2'
        for n in range(1,8):
            spawn([loader,'--library-path',str(loader.parent)+':'+str(folder/'libs'),folder/'Apollo',
                   '--host','127.0.0.1','--port',port,'--team','Apollo','--player-number',n,'--asset-root',folder/'assets/apollo'],f'Apollo{n}')
        wait(lambda:(out/'done.json').exists(),900)
        print('DONE',run,flush=True)
    finally:
        for p in reversed(processes):
            if p.poll() is None:
                try:os.killpg(p.pid,signal.SIGTERM)
                except ProcessLookupError:pass
        for p in processes:
            try:p.wait(timeout=6)
            except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait()
        for f in files:f.close()
    # Keep raw client evidence compressed; match traces can be hundreds of MB.
    import gzip, shutil
    for path in list(out.glob('Dancer_*.jsonl'))+list(out.glob('Dancer*.log'))+list(out.glob('BHuman_*.csv')):
        with path.open('rb') as src,gzip.open(str(path)+'.gz','wb',compresslevel=1) as dst:shutil.copyfileobj(src,dst)
        path.unlink()
