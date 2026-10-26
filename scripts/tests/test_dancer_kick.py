import uuid
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from dancer_kick_worker import Session, TeamSlot, parameters
from start_sim_game import arguments, commands

REPO = Path(__file__).resolve().parents[3] / 'dancer-robocupdemo'


def frame(t=0., distance=.6):
    return dict(time=t, number=2, allowed=True, owner=True, intent=True,
                seen=True, vision=True, ball_stamp=t, ball=[distance, 0.],
                ball_velocity=[0., 0.], ball_field=[distance, 0.],
                visual_ball=[distance, 0.], goal=[7., 0.], ball_out=False,
                state=dict(rpy=[0., 0., 0.]))


class SessionTest(unittest.TestCase):
    def setUp(self):
        self.s = Session(parameters(REPO))

    def active(self):
        self.assertEqual(self.s.step(frame()), 'starting')
        self.assertEqual(self.s.step(frame(.61)), 'active')

    def test_timestamp_conversion_roundoff(self):
        f = frame(57.8)
        f['ball_stamp'] = 57.800000000000004
        self.assertEqual(self.s.step(f), 'starting')
        f.update(time=58.42, ball_stamp=58.42000000000001)
        self.assertEqual(self.s.step(f), 'active')

    def test_set_play_enters_before_native_kick_motion(self):
        # PassToTeammate is still walking to / adjusting its waiting pose.
        f = frame(0., 3.)
        f.update(owner=False, intent=False, set_play_kicker=True)
        self.assertEqual(self.s.step(f), 'starting')
        f.update(time=.61, ball_stamp=.61)
        self.assertEqual(self.s.step(f), 'active')
        f.update(time=.63, ball_stamp=.63)
        self.assertEqual(self.s.step(f), 'active')

    def test_set_play_keeps_common_entry_guards(self):
        for change in [dict(allowed=False), dict(ball=[6.1, 0.]),
                       dict(ball=[0., 3.]), dict(seen=False, ball_stamp=-2.)]:
            f = frame(0., 3.)
            f.update(owner=False, intent=False, set_play_kicker=True)
            f.update(change)
            self.assertEqual(self.s.step(f), 'idle')

    def test_goalkeeper_restart_uses_common_gate_and_keeps_session(self):
        f = frame(0., 5.5)
        f.update(number=1, owner=False, intent=False, set_play_kicker=True)
        self.assertEqual(self.s.step(f), 'starting')
        f.update(time=.61, ball_stamp=.61)
        self.assertEqual(self.s.step(f), 'active')
        # A touch ends the restart. Do not suddenly apply goalkeeper's
        # 3.5 m exit range or no-progress/session timeout to this same kick.
        f.update(time=.63, ball_stamp=.63, set_play_kicker=False, owner=True)
        self.assertEqual(self.s.step(f), 'active')
        f.update(time=20., ball_stamp=20.)
        self.assertEqual(self.s.step(f), 'active')
        self.assertFalse(self.s.goalie_session)

    def test_current_config(self):
        self.assertEqual(self.s.p('startup_decelerate_ms'), 600)
        self.assertEqual(self.s.cfg['auto_visual_kick_enable_angle'], .6)

    def test_geometry_and_permission(self):
        for change in [dict(allowed=False), dict(owner=False),
                       dict(ball=[6., 0.]), dict(ball=[2., 3.]),
                       dict(seen=False, ball_stamp=-2.), dict(state=dict(rpy=[1.4, 0, 0]))]:
            f = frame(); f.update(change)
            self.assertEqual(self.s.step(f), 'idle', change)
        self.assertEqual(self.s.step(frame()), 'starting')

    def test_startup_brief_visual_loss_preserves_timer(self):
        self.s.step(frame())
        f = frame(.4); f.update(seen=False)
        self.assertEqual(self.s.step(f), 'starting')
        self.assertEqual(self.s.step(frame(.61)), 'active')

    def test_startup_sustained_visual_loss_stops(self):
        self.s.step(frame())
        for t in [.1, .2, .3]:
            f = frame(t); f.update(seen=False)
            self.assertEqual(self.s.step(f), 'starting')
        f = frame(.32); f.update(seen=False)
        self.assertEqual(self.s.step(f), 'idle')
        self.assertEqual(self.s.reason, 'startup visual reacquisition timeout')

    def test_boundary_requires_distinct_observation_confirmation(self):
        self.active()
        for t in [.65, .69]:
            f = frame(t); f['ball_out'] = True
            self.assertEqual(self.s.step(f), 'active')
            self.assertEqual(self.s.step(f), 'active')
        f = frame(.77); f['ball_out'] = True
        self.assertEqual(self.s.step(f), 'idle')
        self.assertEqual(self.s.reason, 'confirmed ball out')

    def test_server_ball_on_line_is_not_out_even_with_estimate_bias(self):
        for mode in ['PlayOn', 'KickIn_Left', 'corner_kick_left', 'goal_kick_left']:
            self.s = Session(parameters(REPO))
            for t in [0., .2, .4, .61]:
                f = frame(t); f.update(mode=mode, ball_out=True, set_play_kicker=True)
                phase = self.s.step(f)
            self.assertEqual(phase, 'active', mode)

    def test_goalie_does_not_need_native_kick_request(self):
        f = frame(); f.update(number=1, intent=False, owner=False,
                              control_owner=1, goalie_area=True)
        self.assertEqual(self.s.step(f), 'starting')

    def test_interleaved_nonvision_frame(self):
        self.active()
        f = frame(.63, .3)
        f.update(vision=False, vision_fresh=True, seen=True, ball_stamp=.61)
        self.assertEqual(self.s.step(f), 'active')

    def test_goalie_committed_session_keeps_owner_outside_entry_area(self):
        key = 'test-' + uuid.uuid4().hex
        slot = TeamSlot(key)
        try:
            f = frame(); f.update(number=1, goalie_area=True)
            self.assertEqual(slot.select(f, self.s), 1)
            self.s.phase = 'active'; self.s.goalie_session = True
            f.update(time=.02, ball_stamp=.02, goalie_area=False)
            self.assertEqual(slot.select(f, self.s), 1)
            self.s.phase = 'idle'
            self.assertEqual(slot.select(f, self.s), 0)
        finally:
            slot.file.close()
            Path(slot.file.name).unlink()

    def test_existing_kicker_owns_slot(self):
        key = 'test-' + uuid.uuid4().hex
        first, second = TeamSlot(key), TeamSlot(key)
        try:
            self.assertTrue(first.acquire())
            self.assertFalse(second.acquire())
            first.release()
            self.assertTrue(second.acquire())
        finally:
            first.file.close()
            second.file.close()
            Path(first.file.name).unlink()

    def test_no_striker_timeout(self):
        self.active()
        self.assertEqual(self.s.step(frame(80.)), 'active')

    def test_four_distinct_images(self):
        self.active()
        for i in range(3):
            f = frame(.7 + i*.02, 6.1)
            self.assertEqual(self.s.step(f), 'active')
            self.assertEqual(self.s.step(f), 'active')
        self.assertEqual(self.s.step(frame(.8, 6.1)), 'exiting')
        self.assertEqual(self.s.reason, 'ball range over limit')
        self.assertEqual(self.s.step(frame(.95)), 'idle')
        self.assertEqual(self.s.step(frame(1.)), 'idle')
        self.assertEqual(self.s.step(frame(1.41)), 'starting')

    def test_ball_loss_cancel_preserves_session(self):
        self.active()
        f = frame(1.7); f.update(seen=False, ball_stamp=.61)
        self.assertEqual(self.s.step(f), 'exiting')
        self.assertEqual(self.s.step(frame(1.72)), 'active')
        self.assertEqual(self.s.kick_at, .61)

    def test_missed_ball_hold(self):
        self.active()
        f = frame(1.3); f.update(seen=False, ball_stamp=.61, ball=[.3, 0])
        self.assertEqual(self.s.step(f), 'active')
        f['time'] = 1.5
        self.assertEqual(self.s.step(f), 'exiting')

    def test_moving_away_vs_incoming(self):
        self.active()
        for t in [.7, .72]:
            f=frame(t, 2.); f['ball_velocity']=[-2., 0.]
            self.assertEqual(self.s.step(f), 'active')
        f=frame(.74, 2.); f['ball_velocity']=[2., 0.]
        self.assertEqual(self.s.step(f), 'active')
        f['time']=f['ball_stamp']=.76
        self.assertEqual(self.s.step(f), 'exiting')
        self.assertTrue(self.s.release)

    def test_penalty_immediate(self):
        self.active()
        f=frame(.7); f['allowed']=False
        self.assertEqual(self.s.step(f), 'idle')

    def test_goalie_bounded(self):
        for t in [0., .61, 4.2]:
            f=frame(t); f['number']=1
            phase=self.s.step(f)
        self.assertEqual(phase, 'exiting')
        self.assertEqual(self.s.reason, 'no progress')


class LauncherTest(unittest.TestCase):
    def test_separate_matches_have_separate_team_channels(self):
        ports=[]
        for server_port in [61340,61350]:
            result=list(commands(arguments(['--port',str(server_port)])))
            ports.append({cmd[cmd.index('--team-port')+1] for _,_,cmd in result})
        self.assertEqual(ports,[{'61340','61341'},{'61350','61351'}])
        self.assertFalse(ports[0] & ports[1])

    def test_exactly_one_team(self):
        for side in ['a', 'b']:
            rows=list(commands(arguments(['--policy-team', side])))
            self.assertEqual(len(rows), 6)
            policies=[cmd[cmd.index('--kick-policy')+1] for _, _, cmd in rows]
            self.assertEqual(policies.count('zjudancer'), 3)
            self.assertEqual(policies[:3], ['zjudancer' if side=='a' else 'native']*3)

    def test_keep_virtualenv_executable_path(self):
        python = REPO / '.venv-sim-motion/bin/python'
        self.assertEqual(arguments(['--dancer-python', str(python)]).dancer_python, str(python))

    def test_native_only(self):
        rows=list(commands(arguments(['--policy-team','none'])))
        self.assertTrue(all('--dancer-repo' not in cmd for _, _, cmd in rows))


if __name__ == '__main__':
    unittest.main()
