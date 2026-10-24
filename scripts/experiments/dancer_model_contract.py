#!/usr/bin/env python3
"""Check K1 and K1BHuman geometry against MuJoCo, independent of policy inference."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import mujoco
import numpy as np

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--server-repo', type=Path, default=root.parent / 'rcssservermj')
args = parser.parse_args()
sys.path.insert(0, str(args.server_repo / 'src'))
from rcsssmj.resources.spec_provider import ModelSpecProvider

models = [ModelSpecProvider().load_robot_spec(name).compile() for name in ('K1', 'K1BHuman')]
data = [mujoco.MjData(model) for model in models]
camera_error = 0.; joint_error = 0.
rng = np.random.default_rng(222)
with subprocess.Popen([str(root / 'build_sim/rcss_dancer_policy_probe'), '-', 'geometry'],
                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True) as probe:
    for tick in range(100):
        joints = {models[0].joint(i).name: rng.uniform(-.3, .3) for i in range(models[0].njnt)
                  if models[0].jnt_type[i] == mujoco.mjtJoint.mjJNT_HINGE}
        yaw, pitch = rng.uniform(-1, 1), rng.uniform(-.3, .85)
        joints['AAHead_yaw'], joints['Head_pitch'] = yaw, pitch
        quat = rng.normal(size=4); quat /= np.linalg.norm(quat)
        for model, d in zip(models, data):
            d.qpos[:3] = rng.uniform(-5, 5, 3); d.qpos[3:7] = quat
            for name, angle in joints.items():
                d.qpos[model.joint(name).qposadr[0]] = angle
            mujoco.mj_forward(model, d)
        for name in ('Left_Arm_2', 'Left_Arm_3', 'left_hand_link', 'Right_Arm_2', 'Right_Arm_3',
                     'right_hand_link', 'Left_Hip_Pitch', 'Head_2'):
            joint_error = max(joint_error, float(np.max(np.abs(data[0].body(name).xmat - data[1].body(name).xmat))))
        for model, d in zip(models, data):
            world_ball = d.body('torso').xpos + rng.uniform([-1, -2, -.4], [6, 2, .1])
            camera = d.site('camera')
            xyz = camera.xmat.reshape(3, 3).T @ (world_ball - camera.xpos)
            distance = np.linalg.norm(xyz)
            packet = dict(yaw=yaw, pitch=pitch, distance=distance, azimuth=np.arctan2(xyz[1], xyz[0]),
                          elevation=np.arctan2(xyz[2], np.hypot(xyz[0], xyz[1])), standard_k1=model is models[0])
            probe.stdin.write(json.dumps(packet) + '\n'); probe.stdin.flush()
            actual = np.array(json.loads(probe.stdout.readline()))
            expected = d.body('torso').xmat.reshape(3, 3).T @ (world_ball - d.body('torso').xpos)
            camera_error = max(camera_error, float(np.max(np.abs(actual - expected))))
            np.testing.assert_allclose(actual, expected, atol=1.e-10)
    probe.stdin.close()
    assert probe.wait(timeout=10) == 0
assert joint_error < 1.e-10
print(json.dumps(dict(samples_per_model=100, serial_joint_rotation_max_abs=joint_error,
                      camera_ball_max_abs_m=camera_error), indent=2))
