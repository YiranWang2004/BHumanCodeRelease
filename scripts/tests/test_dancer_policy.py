"""Run with ZJUDancer's motion Python to exercise the actual ONNX contract."""
import importlib.util
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from dancer_kick_worker import Controller
from test_dancer_kick import REPO, frame


@unittest.skipUnless(importlib.util.find_spec('onnxruntime'), 'requires motion Python with ONNX Runtime')
class RealPolicyTest(unittest.TestCase):
    def test_split_server_frames_do_not_double_inference(self):
        import numpy as np
        c = Controller(REPO)
        cfg = c.bank.cfg['visualkick']
        state = dict(q=np.asarray(cfg['default_dof_pos_22']), dq=np.zeros(22), rpy=np.zeros(3), gyro=np.zeros(3))
        for t in [0., .6, .61, .62, .65, .66]:
            f = frame(t, 1.5)
            f.update(state=state, head=[0., .7])
            c.step(f)
            if t == .61:
                self.assertEqual(c.inferences, 1)
        self.assertEqual(c.inferences, 4)

    def test_normal_walk_and_stand_use_shared_amp_with_history(self):
        import numpy as np
        c = Controller(REPO)
        from source_motion.core import Commands
        from source_motion.policies import AmpVelPolicy
        cfg = c.bank.cfg['loco']
        direct = AmpVelPolicy(cfg, REPO / 'models')
        command = Commands()
        state = dict(q=np.asarray(c.bank.cfg['visualkick']['default_dof_pos_22'], np.float32),
                     dq=np.zeros(22), rpy=np.zeros(3), gyro=np.zeros(3))
        for i, velocity in enumerate([[.7, .2, -.3], [.7, .2, -.3], [0., 0., 0.], [-.2, -.1, .4]]):
            f = frame(i * .02)
            f.update(state=state, head=[.2, .7], allowed=False, loco_enabled=True, velocity=velocity)
            result = c.step(f)
            command.velocity = velocity
            q, kp, kd = direct.step(state, command)
            np.testing.assert_allclose(result['q'][2:], q[2:], rtol=0, atol=1e-7)
            np.testing.assert_allclose(result['kp'], kp)
            np.testing.assert_allclose(result['kd'], kd)
            self.assertEqual(result['phase'], 'idle')
            self.assertFalse(result['holding'])
        self.assertEqual(c.amp_inferences, 4)
        self.assertEqual(c.inferences, 0)
        f.update(time=.08, loco_enabled=False)
        self.assertNotIn('q', c.step(f))  # native get-up/prepare retains motors
        f.update(time=.1, loco_enabled=True, state=dict(state, rpy=[0., 1.2, 0.]))
        self.assertNotIn('q', c.step(f))  # AMP refuses unsafe gravity

    def test_same_body_action_as_shared_policy(self):
        import numpy as np
        controller = Controller(REPO)
        from source_motion.core import Commands
        from source_motion.policies import VisualKickPolicy
        cfg = controller.bank.cfg['visualkick']
        direct = VisualKickPolicy(cfg, REPO / 'models')
        commands = Commands()
        state = dict(q=np.asarray(cfg['default_dof_pos_22'], np.float32), dq=np.zeros(22, np.float32),
                     rpy=np.zeros(3, np.float32), gyro=np.zeros(3, np.float32))
        commands.visual_ball = [1.5, .1]
        commands.set_reference(1.5, 0., 0., 6., goal=[7., 1.])
        active_frames = 0
        for tick in range(35):
            f = frame(tick * .02, 1.5)
            f.update(state=state, visual_ball=commands.visual_ball, goal=[7., 1.], head=[0., .7])
            result = controller.step(f)
            if result['phase'] == 'active':
                active_frames += 1
                q, kp, kd = direct.step(state, commands)
                np.testing.assert_allclose(result['q'][2:], q[2:], rtol=0, atol=1e-7)
                np.testing.assert_allclose(result['kp'], kp)
                np.testing.assert_allclose(result['kd'], kd)
        self.assertGreater(active_frames, 1)
        self.assertEqual(controller.inferences, active_frames)


if __name__ == '__main__':
    unittest.main()
