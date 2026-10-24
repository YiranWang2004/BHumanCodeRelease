#!/usr/bin/env python3
"""Test the unmodified dancer recovery/AMP actors on server model physics.

Uses SoccerSimulation's compiled field/robot and actual motor controller at
every physics substep. No referee, network or B-Human behavior is involved in
this first feasibility test. A handoff counts only after five stable AMP seconds.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import mujoco
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, default=ROOT.parent / 'dancer-robocupdemo')
p.add_argument('--server', type=Path, default=ROOT.parent / 'rcssservermj')
p.add_argument('--models', default='K1BHuman')
p.add_argument('--repeats', type=int, default=2)
p.add_argument('--cases', default='front,back,left,right')
args = p.parse_args()
sys.path[:0] = [str(args.source / 'src/source_motion'), str(args.server / 'src')]
from source_motion.getup_mjlab import MjlabRecoveryPolicy
from source_motion.policies import AmpVelPolicy
from source_motion.core import Commands
from rcsssmj.games.soccer.sim.soccer_sim import SoccerSimulation
from rcsssmj.games.soccer.soccer_fields import create_soccer_field

config = json.loads((args.source / 'src/source_motion/config/getup_mjlab.json').read_text())
amp_config = json.loads((args.source / 'src/source_motion/config/policies.json').read_text())['loco']
recovery = MjlabRecoveryPolicy(config, args.source / 'models')
amp = AmpVelPolicy(amp_config, args.source / 'models')
names = ['AAHead_yaw', 'Head_pitch', 'ALeft_Shoulder_Pitch', 'Left_Shoulder_Roll', 'Left_Elbow_Pitch',
         'Left_Elbow_Yaw', 'ARight_Shoulder_Pitch', 'Right_Shoulder_Roll', 'Right_Elbow_Pitch', 'Right_Elbow_Yaw',
         'Left_Hip_Pitch', 'Left_Hip_Roll', 'Left_Hip_Yaw', 'Left_Knee_Pitch', 'Left_Ankle_Pitch', 'Left_Ankle_Roll',
         'Right_Hip_Pitch', 'Right_Hip_Roll', 'Right_Hip_Yaw', 'Right_Knee_Pitch', 'Right_Ankle_Pitch', 'Right_Ankle_Roll']
motors = ['he1','he2'] + [f'{side}ae{i}' for side in ['l','r'] for i in range(1,5)] + [f'{side}le{i}' for side in ['l','r'] for i in range(1,7)]
out = Path(tempfile.mkdtemp(prefix='dancer-getup-physics-', dir=ROOT / 'logs'))
print('ARTIFACTS', out, flush=True)
(out / 'manifest.json').write_text(json.dumps(dict(source=str(args.source), server=str(args.server),
    config=config, model_sha256=hashlib.sha256((args.source / 'models' / config['model_file']).read_bytes()).hexdigest(),
    mujoco=mujoco.__version__, models=args.models, repeats=args.repeats), indent=2))
reports = []
for model_name in args.models.split(','):
    for case in args.cases.split(','):
        for repeat in range(args.repeats):
            sim = SoccerSimulation(create_soccer_field('fifa7vs7'), bhuman_physics=True)
            assert sim.init()
            robot, = sim.add_players([SimpleNamespace(model_name=model_name, team_name='GetupProbe', player_no=1)])
            model, data = sim.mj_model, sim.mj_data
            prefix = robot.agent_id.prefix
            qp = np.array([model.joint(prefix+n).qposadr[0] for n in names])
            qv = np.array([model.joint(prefix+n).dofadr[0] for n in names])
            root = model.joint(prefix+'root').qposadr[0]
            roll, pitch = dict(front=(0., math.pi/2), back=(0.,-math.pi/2),
                               left=(math.pi/2,0.), right=(-math.pi/2,0.))[case]
            yaw = repeat * .4
            cr,sr,cp,sp,cy,sy = math.cos(roll/2),math.sin(roll/2),math.cos(pitch/2),math.sin(pitch/2),math.cos(yaw/2),math.sin(yaw/2)
            data.qpos[root:root+3] = [-8., 0., .42]
            data.qpos[root+3:root+7] = [cr*cp*cy+sr*sp*sy, sr*cp*cy-cr*sp*sy,
                                        cr*sp*cy+sr*cp*sy, cr*cp*sy-sr*sp*cy]
            data.qpos[qp] = np.array(amp_config['default_dof_pos_22']) + np.random.default_rng(100+repeat).normal(0, .025, 22)
            data.qvel[:] = 0
            mujoco.mj_forward(model, data)
            def command(q, kp, kd, tau):
                for i, name in enumerate(motors):
                    sim.ctrl_motor(prefix+name, float(q[i]), 0., float(kp[i]), float(kd[i]), float(tau[i]))
                for _ in range(sim.n_substeps):
                    sim._simrobot_motors.apply(data)
                    mujoco.mj_step(model, data)
            # Land and settle before timing; no artificial root hold during recovery.
            for _ in range(40): command(np.zeros(22), np.zeros(22), np.ones(22), np.zeros(22))
            recovery.reset(); amp.reset()
            mode = 'getup'; retries = 0; handoff = None; stable = 0; success = False; failure = None
            start = data.time
            initial_height = float(data.sensor(prefix+'torso_pos').data[2])
            trace = (out / f'{model_name}_{case}_{repeat}.csv').open('w')
            writer = csv.writer(trace); writer.writerow(['t','mode','height','gravity_z','gyro_norm','attempt','row','state','max_abs_tau'])
            for tick in range(1500):
                quat = data.sensor(prefix+'torso_quat').data.copy(); w,x,y,z = quat / np.linalg.norm(quat)
                gravity_z = 2*(x*x+y*y)-1
                rpy = np.array([math.atan2(2*(w*x+y*z),1-2*(x*x+y*y)), math.asin(np.clip(2*(w*y-z*x),-1,1)),0.])
                state = dict(q=data.qpos[qp].astype(np.float32), dq=data.qvel[qv].astype(np.float32),
                             gyro=data.sensor(prefix+'torso_gyro').data.astype(np.float32).copy(),
                             quat=quat, rpy=rpy, height=float(data.sensor(prefix+'torso_pos').data[2]))
                try:
                    if mode == 'getup':
                        q,kp,kd = recovery.step(state, Commands()); tau = recovery.tau.copy()
                        # Actual dancer SimController completion override, not
                        # the stricter unused actor-only .53 m / 50-step rule.
                        if recovery.state != 'failed' and gravity_z < -.85 and state['height'] >= .30:
                            mode = 'amp'; handoff = tick*.02; amp.reset()
                        elif recovery.state == 'failed':
                            retries += 1; recovery.reset()
                            q=kp=kd=tau=np.zeros(22)
                    else:
                        if gravity_z > -.5 or state['height'] < .30:
                            failure = 'fell_after_handoff'; break
                        q,kp,kd = amp.step(state, Commands()); tau = np.zeros(22)
                        stable = stable+1 if state['height'] > .45 and gravity_z < -.85 else 0
                        if stable >= 250: success=True; break
                    writer.writerow([tick*.02,mode,state['height'],gravity_z,np.linalg.norm(state['gyro']),retries+1,
                                     recovery.row,recovery.state,float(np.max(np.abs(tau)))])
                    command(q,kp,kd,tau)
                except Exception as e:
                    failure=f'{type(e).__name__}: {e}'; break
            trace.close()
            result=dict(model=model_name,case=case,repeat=repeat,success=success,handoff_s=handoff,
                        elapsed_s=round(data.time-start,3),attempts=retries+1,initial_height=initial_height,
                        final_height=state['height'],final_gravity_z=float(gravity_z),failure=failure)
            reports.append(result); print(json.dumps(result),flush=True)
            (out/'report.json').write_text(json.dumps(reports,indent=2))
