#!/usr/bin/env python3
"""Compare recovery observations/actions/torques and lifecycle with dancer Python."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import numpy as np

root = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, default=root.parent / 'dancer-robocupdemo')
args = p.parse_args()
sys.path.insert(0, str(args.source / 'src/source_motion'))
from source_motion.getup_mjlab import MjlabRecoveryPolicy

assets = root / 'Config/NeuralNets/Dancer'
cfg = json.loads((assets / 'getup.json').read_text())
assert cfg == json.loads((args.source / 'src/source_motion/config/getup_mjlab.json').read_text())
model = (assets / cfg['model_file']).read_bytes()
assert model == (args.source / 'models' / cfg['model_file']).read_bytes()
assert hashlib.sha256(model).hexdigest() == cfg['model_sha256']
reference = MjlabRecoveryPolicy(cfg, args.source / 'models')
process = subprocess.Popen([str(root / 'build_sim/rcss_dancer_policy_probe'), str(assets), 'getup'],
                           stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
rng = np.random.default_rng(20261025)
errors = {name: 0. for name in ['observation', 'action', 'tau', 'kd']}
cases = [('timeout', 310), ('success', 80), ('reset', 150)]
statuses = ['settling', 'executing', 'succeeded', 'failed']
try:
    for case, frames in cases:
        for tick in range(frames):
            reset = tick == 0 or (case == 'reset' and tick in (19, 53, 112))
            if reset: reference.reset()
            quat = rng.normal(0, .08, 4); quat[0] += 1.; quat /= np.linalg.norm(quat)
            if case == 'success': quat = np.array([1., 0., 0., 0.])
            w,x,y,z = quat
            gravity = [2*(w*y-x*z), -2*(w*x+y*z), 2*(x*x+y*y)-1]
            state = dict(q=(reference.default+rng.normal(0,.4,22)).astype(np.float32),
                         dq=rng.normal(0,2,22).astype(np.float32), gyro=rng.normal(0,1,3).astype(np.float32),
                         quat=quat, rpy=np.zeros(3), height=.56 if case == 'success' else .2)
            reference.step(state, None)
            packet = dict(q=state['q'].tolist(),dq=state['dq'].tolist(),gyro=state['gyro'].tolist(),
                          gravity=gravity,height=state['height'],reset=reset)
            process.stdin.write(json.dumps(packet)+'\n'); process.stdin.flush()
            actual = json.loads(process.stdout.readline())
            assert actual['row'] == reference.row, (case,tick,actual['row'],reference.row)
            assert statuses[actual['status']] == reference.state, (case,tick,actual['status'],reference.state)
            for key, expected in [('observation',reference.last_observation), ('action',reference.last_action),
                                  ('tau',reference.tau),('kd',reference.kd)]:
                np.testing.assert_allclose(actual[key],expected,atol=2e-4 if key=='tau' else 1e-5,rtol=1e-5,
                                           err_msg=f'{case} {tick} {key}')
                errors[key] = max(errors[key],float(np.max(np.abs(np.array(actual[key])-expected))))
finally:
    process.stdin.close()
    assert process.wait(timeout=10) == 0
print(json.dumps(dict(frames=sum(n for _,n in cases),cases=dict(cases),max_abs_error=errors),indent=2))
