#!/usr/bin/env python3
"""Paired physics ablations, same exported policy and seeded starts; not a match metric."""
import argparse,json,math,sys
from pathlib import Path
import numpy as np
import mujoco
import onnxruntime as ort
ROOT=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);p.add_argument('--unfair-server',type=Path,required=True);p.add_argument('--trials',type=int,default=16);p.add_argument('--variants',default='current,unfair,contact_only,numerics_only');a=p.parse_args()
sys.path.insert(0,str(ROOT/'rcssservermj/src'))
from rcsssmj.games.soccer.sim.soccer_sim import SoccerSimulation
from rcsssmj.games.soccer.soccer_fields import create_soccer_field
cfg=json.loads((ROOT/'dancer-robocupdemo/models/student/student_equivariant_config.json').read_text())
default=np.array(cfg['default_dof_pos_22']);kp=np.array(cfg['kp_22']);kd=np.array(cfg['kd_22'])
names=['AAHead_yaw','Head_pitch']+cfg['equivariance']['spec']['description']['joint_names']
names=[dict(Left_Shoulder_Pitch='ALeft_Shoulder_Pitch',Right_Shoulder_Pitch='ARight_Shoulder_Pitch').get(n,n) for n in names]
options=ort.SessionOptions();options.intra_op_num_threads=1;options.inter_op_num_threads=1
actor=ort.InferenceSession(str(ROOT/'dancer-robocupdemo/models'/cfg['model_file']),options,providers=['CPUExecutionProvider'])
reference=mujoco.MjSpec.from_file(str(a.unfair_server/'src/rcsssmj/resources/environments/soccer/world.xml'))
def model(variant):
    world=SoccerSimulation(field=create_soccer_field('fifa7vs7'),bhuman_physics=True)._create_world()
    if variant in ('unfair','contact_only'):
        for name in ('ball','pitch'):
            for key in ('friction','condim','priority','solref','solimp','solmix'):
                setattr(world.geom(name),key,getattr(reference.geom(name),key))
    if variant == 'compliance_only':
        for key in ('solref','solimp'):
            setattr(world.geom('ball'),key,getattr(reference.geom('ball'),key))
    if variant == 'friction_only':
        world.geom('ball').friction=reference.geom('ball').friction
    if variant in ('unfair','numerics_only'):
        for key in ('timestep','integrator','solver','iterations','ls_iterations','tolerance','gravity','density','viscosity'):
            setattr(world.option,key,getattr(reference.option,key))
    robot=mujoco.MjSpec.from_file(str(ROOT/'rcssservermj/src/rcsssmj/resources/robots/K1/robot.xml'))
    world.worldbody.add_frame().attach_body(robot.body('torso'),'robot-','')
    m=world.compile()
    ji=np.array([m.joint('robot-'+n).id for n in names]);qa=m.jnt_qposadr[ji];va=m.jnt_dofadr[ji]
    pa=[]
    for j,k,d in zip(ji,kp,kd):
        actuators=np.flatnonzero(m.actuator_trnid[:,0]==j)
        pos=next(int(i) for i in actuators if m.actuator(i).name.endswith('_pos'))
        vel=next(int(i) for i in actuators if m.actuator(i).name.endswith('_vel'))
        m.actuator_gainprm[pos,0]=k;m.actuator_biasprm[pos,1]=-k
        m.actuator_gainprm[vel,0]=d;m.actuator_biasprm[vel,2]=-d
        pa.append(pos)
    return m,qa,va,np.array(pa)
models={v:model(v) for v in a.variants.split(',')}
snapshots=json.loads((a.out/'physics_profiles.json').read_text()) if (a.out/'physics_profiles.json').exists() else {}
for v,(m,*_) in models.items():
    snapshots[v]={'option':{k:np.asarray(getattr(m.opt,k)).tolist() for k in ('timestep','integrator','iterations','ls_iterations','tolerance','gravity','density','viscosity')},'geoms':{name:{k:np.asarray(getattr(m,'geom_'+k)[m.geom(name).id]).tolist() for k in ('friction','condim','priority','solref','solimp')} for name in ('ball','pitch')}}
(a.out/'physics_profiles.json').write_text(json.dumps(snapshots,indent=2))
def run(variant,seed):
    m,qa,va,pa=models[variant];d=mujoco.MjData(m)
    ra=int(m.joint('robot-root').qposadr[0]);rv=int(m.joint('robot-root').dofadr[0]);ba=int(m.joint('ball-root').qposadr[0]);bv=int(m.joint('ball-root').dofadr[0]);body=m.body('robot-torso').id;bgeom=m.geom('ball').id
    rng=np.random.default_rng(seed)
    d.qpos[ra:ra+7]=[0,0,.555,1,0,0,0];d.qpos[qa]=default+rng.normal(0,.02,22)
    distance=rng.uniform(.8,2.);bearing=rng.uniform(-.35,.35);direction_angle=rng.uniform(-.5,.5)
    d.qpos[ba:ba+7]=[distance*math.cos(bearing),distance*math.sin(bearing),.115,1,0,0,0]
    history=np.zeros((50,75),np.float32);last=np.zeros(20);held=np.zeros(3);phase=0.
    mujoco.mj_forward(m,d)
    samples=[];first_fall=None
    for tick in range(500):
        rotation=d.xmat[body].reshape(3,3)
        if tick%2==0:held=rotation.T@(d.qpos[ba:ba+3]-d.xpos[body])
        direction=(rotation.T@np.array([math.cos(direction_angle),math.sin(direction_angle),0.]))[:2];direction/=max(1.e-8,np.linalg.norm(direction))
        obs=np.r_[rotation.T@np.array([0,0,-1]),d.qvel[rv+3:rv+6],cfg['default_command'],math.cos(2*math.pi*phase),math.sin(2*math.pi*phase),d.qpos[qa[2:]]-default[2:],.1*d.qvel[va[2:]],last,held[:2],direction]
        history[:-1]=history[1:];history[-1]=obs
        last=np.clip(actor.run(None,{'history':history.reshape(1,-1)})[0].reshape(-1),-1,1)
        target=default.copy();target[2:]+=last
        target[0]=np.clip(math.atan2(held[1],held[0]),-1,1);target[1]=np.clip(math.atan2(-held[2]+.3,math.hypot(*held[:2]))-.1,-.349,.855)
        contact=False
        for sub in range(round(.02/m.opt.timestep)):
            d.ctrl[pa]=target;mujoco.mj_step(m,d);mujoco.mj_forward(m,d)
            contact|=any(bgeom in (c.geom1,c.geom2) and 'foot' in (m.geom(c.geom1 if c.geom2==bgeom else c.geom2).name or '') for c in d.contact)
        speed=float(np.linalg.norm(d.qvel[bv:bv+2]));samples.append(dict(t=float(d.time),contact=bool(contact),speed=speed))
        q=d.qpos[ra+3:ra+7]
        if d.qpos[ra+2]<.35 or 1-2*(q[1]**2+q[2]**2)<math.cos(math.pi/4):first_fall=float(d.time);break
        if abs(d.qpos[ba])>15 or abs(d.qpos[ba+1])>10:break
        phase=(phase+.02*cfg['default_gait_frequency'])%1
    episodes=[]
    for i,s in enumerate(samples):
        if s['contact']:
            if not episodes or s['t']-samples[episodes[-1][-1]]['t']>.2+1e-6:episodes.append([])
            episodes[-1].append(i)
    releases=[]
    for e in episodes:
        end=e[-1]+1
        if end<len(samples):releases.append(dict(t=samples[end]['t'],speed=samples[end]['speed']))
    kicks=[e for e in releases if e['speed']>=1.]
    result=dict(variant=variant,seed=seed,distance=distance,bearing=bearing,direction=direction_angle,first_fall=first_fall,contact=bool(episodes),release_speed=kicks[0]['speed'] if kicks else None,releases=releases,peak_speed=max(s['speed'] for s in samples))
    return result
rows=json.loads((a.out/'physics_trials.json').read_text()) if (a.out/'physics_trials.json').exists() else []
rows=[r for r in rows if r['variant'] not in models]
for variant in models:
    batch=[run(variant,seed) for seed in range(a.trials)]
    rows+=batch
    (a.out/'physics_trials.json').write_text(json.dumps(rows,indent=2))
    speeds=[r['release_speed'] for r in batch if r['release_speed'] is not None]
    print(json.dumps(dict(variant=variant,trials=len(batch),kicks=len(speeds),mean=float(np.mean(speeds)) if speeds else None,falls=sum(r['first_fall'] is not None for r in batch))),flush=True)
