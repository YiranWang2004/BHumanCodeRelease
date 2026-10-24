#!/usr/bin/env python3
"""Compare the port against dancer's actual AMP and VisualKick deployments."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace
import numpy as np

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=root.parent / 'dancer-robocupdemo')
parser.add_argument('--reference-probe', type=Path, help='Use an independently built current-source dancer probe')
args = parser.parse_args()
source = args.source.resolve()
sys.path.insert(0, str(source / 'src/source_motion'))
from source_motion.policies import AmpVelPolicy
from source_motion.core import gravity_from_rpy

assets = root / 'Config/NeuralNets/Dancer'
manifest = json.loads((assets / 'manifest.json').read_text())
for name, expected in manifest['sha256'].items():
    assert hashlib.sha256((assets / name).read_bytes()).hexdigest() == expected, name
for name in ('amp_vel/2026-10-04_10-24-37.onnx', 'student/5070Ti_equivariant_student.onnx'):
    assert (assets / name).read_bytes() == (source / 'models' / name).read_bytes()

def send(process, data):
    process.stdin.write(json.dumps(data) + '\n')
    process.stdin.flush()
    line = process.stdout.readline()
    if not line:
        raise RuntimeError(f'Probe exited: {process.poll()}')
    return json.loads(line)

def probe(kind):
    return subprocess.Popen([str(root / 'build_sim/rcss_dancer_policy_probe'), str(assets), kind],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

def close(process):
    process.stdin.close()
    assert process.wait(timeout=10) == 0

rng = np.random.default_rng(20261025)
errors = {}
cfg = json.loads((source / 'src/source_motion/config/policies.json').read_text())['loco']
# B-Human deliberately raises the AMP forward command ceiling. Compare the
# actor and observation construction with identical deployment limits.
cfg['velocity_limits'] = json.loads((assets / 'amp.json').read_text())['velocity_limits']
reference = AmpVelPolicy(cfg, source / 'models')
process = probe('walk')
maximum = 0.
try:
    for tick in range(200):
        reset = tick in (0, 57, 113)
        if reset: reference.reset()
        state = dict(q=reference.default + rng.normal(0, .1, 22), dq=rng.normal(0, .5, 22),
                     gyro=rng.normal(0, .2, 3), rpy=rng.uniform(-.3, .3, 3))
        command = SimpleNamespace(velocity=rng.uniform(-4, 4, 3))
        observation = reference.compute_observation(state, command)
        q, kp, kd = reference.step(state, command)
        actual = send(process, dict(q=state['q'].tolist(), dq=state['dq'].tolist(),
                                   gyro=state['gyro'].tolist(), gravity=gravity_from_rpy(state['rpy']).tolist(),
                                   velocity=command.velocity.tolist(), reset=reset))
        for key, expected in [('observation', observation), ('action', reference.last_action),
                              ('q', q), ('kp', kp), ('kd', kd)]:
            np.testing.assert_allclose(actual[key], expected, atol=1.e-5, rtol=1.e-5, err_msg=f'AMP {tick} {key}')
            maximum = max(maximum, float(np.max(np.abs(np.asarray(actual[key]) - expected))))
finally:
    close(process)
errors['amp_200_frames_max_abs'] = maximum

process = probe('kick')
reference = subprocess.Popen([str(args.reference_probe or source / 'build_sim/fast_student_deploy_probe'), str(source / 'models'),
                              str(source / 'models/student/student_equivariant_config.json')],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
cfg = json.loads((assets / 'visualkick.json').read_text())
maximum = 0.
try:
    for tick in range(200):
        quat = rng.normal(0, .08, 4); quat[0] += 1.; quat /= np.linalg.norm(quat)
        w, x, y, z = quat
        gravity = [2*(w*y-x*z), -2*(w*x+y*z), 2*(x*x+y*y)-1]
        q = np.array(cfg['default_dof_pos_22']) + rng.normal(0, .08, 22)
        dq = rng.normal(0, .5, 22); gyro = rng.normal(0, .2, 3)
        visible = not (tick % 2 or 50 <= tick < 80 or 110 <= tick < 116)
        ball = rng.uniform([-1, -2, -.6], [8, 2, .5], 3) if visible else np.zeros(3)
        if tick == 34: ball[:] = 0  # a zero sample does not erase a real detection
        direction = rng.uniform(-2, 2, 2)
        packet = dict(q=q.tolist(), dq=dq.tolist(), gyro=gyro.tolist(), reset=tick in (0, 55, 110, 170),
                      direction=direction.tolist())
        actual = send(process, dict(packet, gravity=gravity, ball=ball.tolist(), sees_ball=visible))
        expected = send(reference, dict(packet, quat=quat.tolist(), ball=ball.tolist() if visible else None))
        for key, wanted in [('observation', expected['history']), ('action', expected['action'][2:]),
                            ('q', expected['q']), ('kp', expected['kp']), ('kd', expected['kd'])]:
            got = actual[key]
            if key == 'q': got, wanted = got[2:], wanted[2:]  # head ownership stays with B-Human
            np.testing.assert_allclose(got, wanted, atol=1.e-5, rtol=1.e-5, err_msg=f'Kick {tick} {key}')
            maximum = max(maximum, float(np.max(np.abs(np.asarray(got) - wanted))))
finally:
    close(process); close(reference)
errors['visualkick_200_frames_max_abs'] = maximum
print(json.dumps(errors, indent=2))
