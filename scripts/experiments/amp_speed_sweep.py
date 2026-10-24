#!/usr/bin/env python3
"""Offline AMP command sweep using the real C++ policy and current server physics.

Does not alter production assets or connect to a running match. The pitch is
unbounded for this experiment; other world geoms cannot collide with the robot.
"""
import argparse
from concurrent.futures import ProcessPoolExecutor
import json
import math
from pathlib import Path
import re
import subprocess
import sys

import mujoco
import numpy as np

ROOT = Path(__file__).resolve().parents[2]


def run(job):
    output, name, velocity, seed, pattern, seconds, head_scan, continue_low, bhuman_limits = job
    sys.path.insert(0, str(ROOT.parent / 'rcssservermj/src'))
    from rcsssmj.games.soccer.sim.soccer_sim import SoccerSimulation
    from rcsssmj.games.soccer.soccer_fields import create_soccer_field
    cfg = json.loads((Path(output) / 'assets/amp.json').read_text())
    neutral = np.array(cfg['default_dof_pos_22'])
    bounds = np.array([[-np.inf, np.inf]] * 22)
    if bhuman_limits:
        limits = dict((name, [float(lo) * math.pi / 180, float(hi) * math.pi / 180]) for name, lo, hi in
                      re.findall(r'(\w+)\s*=\s*\{[^{}]*?min\s*=\s*([-\d.]+)deg;\s*max\s*=\s*([-\d.]+)deg;',
                                 (ROOT / 'Config/Robots/Default/K1/jointLimits.cfg').read_text()))
        order = ['headYaw', 'headPitch'] + [side + joint for side in ('l', 'r')
                  for joint in ('ShoulderPitch', 'ShoulderRoll', 'ElbowPitch', 'ElbowYaw')] + [side + joint for side in ('l', 'r')
                  for joint in ('HipPitch', 'HipRoll', 'HipYaw', 'KneePitch', 'AnklePitch', 'AnkleRoll')]
        bounds = np.array([limits[n] for n in order])
        cfg['kp_22'][:2] = [10., 10.]
        cfg['kd_22'][:2] = [1.2, 1.2]
    world = SoccerSimulation(field=create_soccer_field('fifa7vs7'), bhuman_physics=True)._create_world()
    for geom in world.geoms:
        if geom.name != 'pitch':
            geom.contype = geom.conaffinity = 0
    robot = mujoco.MjSpec.from_file(str(ROOT.parent / 'rcssservermj/src/rcsssmj/resources/robots/K1/robot.xml'))
    world.worldbody.add_frame().attach_body(robot.body('torso'), 'robot-', '')
    m = world.compile()
    joint_names = ['AAHead_yaw', 'Head_pitch', 'ALeft_Shoulder_Pitch', 'Left_Shoulder_Roll',
                   'Left_Elbow_Pitch', 'Left_Elbow_Yaw', 'ARight_Shoulder_Pitch', 'Right_Shoulder_Roll',
                   'Right_Elbow_Pitch', 'Right_Elbow_Yaw', 'Left_Hip_Pitch', 'Left_Hip_Roll',
                   'Left_Hip_Yaw', 'Left_Knee_Pitch', 'Left_Ankle_Pitch', 'Left_Ankle_Roll',
                   'Right_Hip_Pitch', 'Right_Hip_Roll', 'Right_Hip_Yaw', 'Right_Knee_Pitch',
                   'Right_Ankle_Pitch', 'Right_Ankle_Roll']
    ji = np.array([m.joint('robot-' + n).id for n in joint_names])
    qa, va = m.jnt_qposadr[ji], m.jnt_dofadr[ji]
    pa = []
    for j, kp, kd in zip(ji, cfg['kp_22'], cfg['kd_22']):
        actuators = np.flatnonzero(m.actuator_trnid[:, 0] == j)
        pos = next(int(i) for i in actuators if m.actuator(i).name.endswith('_pos'))
        vel = next(int(i) for i in actuators if m.actuator(i).name.endswith('_vel'))
        m.actuator_gainprm[pos, 0] = kp
        m.actuator_biasprm[pos, 1] = -kp
        m.actuator_gainprm[vel, 0] = kd
        m.actuator_biasprm[vel, 2] = -kd
        pa.append(pos)
    d = mujoco.MjData(m)
    ra = int(m.joint('robot-root').qposadr[0])
    rv = int(m.joint('robot-root').dofadr[0])
    body = m.body('robot-torso').id
    rng = np.random.default_rng(seed)
    roll, pitch = rng.uniform(-.025, .025, 2)
    quat = np.array([1., roll / 2, pitch / 2, 0.]); quat /= np.linalg.norm(quat)
    d.qpos[ra:ra+7] = [0, 0, .555, *quat]
    d.qpos[qa] = neutral + rng.normal(0, .015, 22)
    mujoco.mj_forward(m, d)
    actor = subprocess.Popen([str(ROOT / 'build_sim/rcss_dancer_policy_probe'),
                              str(Path(output) / 'assets'), 'walk'],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    samples = []
    fallen = None
    low_height = None
    clip_counts = np.zeros(22, dtype=int)
    try:
        for tick in range(round(seconds / .02)):
            t = tick * .02
            command = np.array(velocity, dtype=float)
            phase = 'steady'
            if t < 2:
                command[:] = 0; phase = 'warmup'
            elif t < 3 and pattern != 'abrupt':
                command *= t - 2; phase = 'ramp'
            elif t >= seconds - 4:
                command[:] = 0; phase = 'stop'
            elif pattern == 'stress' and t >= 7:
                phase = 'stress'
                # Turning at speed, braking, reverse, and a second hard start.
                if t < 10: command = np.array([velocity[0], .4, 1.])
                elif t < 11: command[:] = 0
                elif t < 13: command = np.array([-1., -.4, -.8])
                else: command = np.array([velocity[0], -.4, -1.])
            rotation = d.xmat[body].reshape(3, 3)
            state = dict(q=d.qpos[qa].tolist(), dq=d.qvel[va].tolist(),
                         gyro=d.qvel[rv+3:rv+6].tolist(),
                         gravity=(rotation.T @ np.array([0., 0., -1.])).tolist(),
                         velocity=command.tolist(), reset=tick == 0)
            actor.stdin.write(json.dumps(state) + '\n'); actor.stdin.flush()
            line = actor.stdout.readline()
            if not line:
                raise RuntimeError(actor.stderr.read())
            out = json.loads(line)
            assert np.allclose(out['observation'][66:69], command, atol=1.e-6)
            if head_scan:
                out['q'][0] = .8 * math.sin(2 * math.pi * t / 3)
                out['q'][1] = .25 + .2 * math.sin(2 * math.pi * t / 4)
            target = np.asarray(out['q'])
            clip_counts += (target < bounds[:, 0]) | (target > bounds[:, 1])
            d.ctrl[pa] = np.clip(target, bounds[:, 0], bounds[:, 1])
            for _ in range(round(.02 / m.opt.timestep)):
                mujoco.mj_step(m, d)
            mujoco.mj_forward(m, d)
            rotation = d.xmat[body].reshape(3, 3)
            speed = rotation.T @ d.qvel[rv:rv+3]
            tilt = math.degrees(math.acos(np.clip(rotation[2, 2], -1, 1)))
            samples.append([t + .02, *command, *speed[:2], d.qvel[rv+5], d.qpos[ra+2], tilt])
            if d.qpos[ra+2] < .35 and low_height is None:
                low_height = dict(t=t+.02, phase=phase, height=float(d.qpos[ra+2]), tilt=tilt)
            if d.qpos[ra+2] < (.25 if continue_low else .35) or tilt > 45:
                fallen = dict(t=t+.02, phase=phase, height=float(d.qpos[ra+2]), tilt=tilt)
                break
    finally:
        actor.stdin.close()
        actor.wait(timeout=5)
        actor.stdout.close(); actor.stderr.close()
    trace = np.array(samples)
    np.savez_compressed(Path(output) / f'{name}_{seed}_{pattern}.npz', samples=trace)
    steady = trace[(trace[:, 0] >= 4) & (trace[:, 0] <= (7 if pattern == 'stress' else 15))]
    result = dict(case=name, command=velocity, seed=seed, pattern=pattern,
                  fall=fallen, low_height=low_height, seconds=float(trace[-1, 0]),
                  duration=seconds, head_scan=head_scan, height_threshold=.25 if continue_low else .35,
                  bhuman_limits=bhuman_limits, clipped_frames=dict(zip(joint_names, clip_counts.tolist())),
                  actual_mean=steady[:, 4:7].mean(axis=0).tolist() if len(steady) else None,
                  min_height=float(trace[:, 7].min()), max_tilt=float(trace[:, 8].max()))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--cases', required=True, help='JSON object of case name: [vx,vy,yaw]')
    parser.add_argument('--trials', type=int, default=3)
    parser.add_argument('--pattern', choices=['steady', 'stress', 'abrupt'], default='steady')
    parser.add_argument('--seconds', type=float, default=20.)
    parser.add_argument('--head-scan', action='store_true')
    parser.add_argument('--continue-low', action='store_true', help='Continue past .35 m, stop at .25 m or 45 degree tilt.')
    parser.add_argument('--bhuman-limits', action='store_true', help='Apply current B-Human joint target limits and head gains.')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    assets = args.out / 'assets'; assets.mkdir(exist_ok=True)
    production = ROOT / 'Config/NeuralNets/Dancer'
    cfg = json.loads((production / 'amp.json').read_text())
    cfg['velocity_limits'] = [6., 6., 6.]
    (assets / 'amp.json').write_text(json.dumps(cfg, indent=2))
    if not (assets / 'amp_vel').exists(): (assets / 'amp_vel').symlink_to(production / 'amp_vel', target_is_directory=True)
    cases = json.loads(args.cases)
    jobs = [(str(args.out), name, velocity, seed, args.pattern, args.seconds, args.head_scan, args.continue_low, args.bhuman_limits)
            for name, velocity in cases.items() for seed in range(args.trials)]
    results_path = args.out / f'{args.pattern}.json'
    rows = json.loads(results_path.read_text()) if results_path.exists() else []
    rows = [r for r in rows if r['case'] not in cases]
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for result in pool.map(run, jobs):
            rows.append(result)
            results_path.write_text(json.dumps(rows, indent=2))
            print(json.dumps(result), flush=True)


if __name__ == '__main__':
    main()
