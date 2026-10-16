#!/usr/bin/env python3
"""Test-only server fixture used by kick_alignment.py; never used by normal startup."""
import json
import logging
import sys
import math
from rcsssmj.games.soccer.soccer_rules import create_soccer_rule_book
from rcsssmj.games.soccer.soccer_fields import create_soccer_field
from rcsssmj.games.soccer.sim.soccer_sim import SoccerSimulation
from rcsssmj.games.soccer.server.soccer_server import SoccerSimServer
from rcsssmj.games.soccer.server.soccer_command_parser import SoccerCommandParser
from rcsssmj.games.teams import TeamSide
from rcsssmj.sim.commands import MonitorCommand
scene = json.load(open(sys.argv[3]))
out = open(sys.argv[4], 'w', buffering=1)

class Restore(MonitorCommand):

    def perform(self, ci):
        ci.request_place_ball((*scene['ball'], 0.11), (0, 0, 0))
        if scene['kind'] == 'kickin':
            ci.referee.throw_in(TeamSide.LEFT)
        else:
            ci.referee.free_kick(TeamSide.LEFT)
        ci.started = ci.sim_time
        out.write(json.dumps(dict(start=ci.sim_time, ball=list(ci.ball.xpos))) + '\n')

class Parser(SoccerCommandParser):

    def parse_node(self, n):
        return Restore() if n[0] == b'testAlignment' else super().parse_node(n)

class Sim(SoccerSimulation):
    started = None

    def _post_step(self, commands):
        super()._post_step(commands)
        if self.started is None:
            self.request_place_ball((*scene['ball'], 0.11), (0, 0, 0))
            for actor in self.sim_agents:
                p = scene['poses'][actor.agent_id.player_no - 1]
                if actor.agent_id.player_no == 4 or math.hypot(actor.xpos[0] - p['x'], actor.xpos[1] - p['y']) > 0.2:
                    self.request_place_player(p['n'], 'AlignmentTest', (p['x'], p['y'], p['z']), (math.cos(p['yaw'] / 2), 0, 0, math.sin(p['yaw'] / 2)))
                if actor.agent_id.player_no == 4:
                    dx = scene['ball'][0] - actor.xpos[0]
                    dy = scene['ball'][1] - actor.xpos[1]
                    pan = (math.atan2(dy, dx) - p['yaw'] + math.pi) % (2 * math.pi) - math.pi
                    for j in actor._joints:
                        if 'head_yaw' in j.name.lower():
                            j.qpos[0] = max(-1.0, min(1.0, pan))
                            j.qvel[0] = 0
                        if 'head_pitch' in j.name.lower():
                            j.qpos[0] = 0.65
                            j.qvel[0] = 0
        if self.started is not None:
            actor = next((p for p in self.sim_agents if p.agent_id.player_no == 4))
            out.write(json.dumps(dict(t=self.sim_time, ball=self.ball.xpos.tolist(), vel=self.ball._qvel[:3].tolist(), actor=actor.xpos.tolist(), contact=str(self.ball.active_contact), parts=sorted(self.ball.active_contact_body_parts or []), mode=self.game_state.play_mode.value)) + '\n')
logging.basicConfig(level=logging.INFO)
sim = Sim(field=create_soccer_field('fifa7vs7'), rules=create_soccer_rule_book('ssim'), bhuman_physics=True)
server = SoccerSimServer(sim, agent_port=int(sys.argv[1]), monitor_port=int(sys.argv[2]), sync_mode=True, real_time=False, render=False)
server._command_parser = Parser()
server.run()
