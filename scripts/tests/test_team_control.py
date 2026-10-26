import os
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import unittest
from team_control import elect, support_velocity

class ArbitrationTest(unittest.TestCase):
    def report(self, t, cost, phase='idle', eligible=True):
        return dict(time=t, cost=cost, phase=phase, eligible=eligible, pid=os.getpid())

    def test_committed_session_survives_cheaper_challenger(self):
        state = {}
        self.assertEqual(elect(state, 2, self.report(0, 3, 'active'), 0), 2)
        self.assertEqual(elect(state, 3, self.report(.02, 1), .02), 2)
        self.assertEqual(elect(state, 2, self.report(.04, 3, eligible=False), .04), 3)

    def test_approach_handover_has_hysteresis(self):
        state = {}
        elect(state, 2, self.report(0, 3), 0)
        for t in [.02, .2, .4]:
            elect(state, 2, self.report(t, 3), t)
            self.assertEqual(elect(state, 3, self.report(t, 1), t), 2)
        elect(state, 2, self.report(.54, 3), .54)
        self.assertEqual(elect(state, 3, self.report(.54, 1), .54), 3)

    def test_stale_owner_cannot_block_team(self):
        state = {}
        elect(state, 2, self.report(0, 3, 'active'), 0)
        self.assertEqual(elect(state, 3, self.report(.32, 1), .32), 3)

    def test_losing_striker_supports_instead_of_chasing(self):
        f = dict(number=2, allowed=True, ball=[1.,0.], owner=True, intent=False,
                 velocity=[.8,0.,0.], ball_field=[1.,0.], pose=[0.,0.,0.], field=[7.,4.])
        velocity = support_velocity(f, 3)
        self.assertLess(velocity[0], 0.)
        self.assertGreater(velocity[1], 0.)
        self.assertEqual(support_velocity(f, 2), f['velocity'])
