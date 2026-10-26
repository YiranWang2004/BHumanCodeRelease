#!/usr/bin/env python3
"""B-Human transport for ZJUDancer's unmodified source_motion policy.

Session rules follow StrikerDecide/RLVisionKick (brain_tree.cpp). B-Human
continues to supply ownership, game permissions, localization and ball filter.
Only the target is deliberately overridden: always the opponent goal centre.
"""
import argparse
import csv
from team_control import elect, support_velocity
import fcntl
import hashlib
import os
import tempfile
import json
import math
from pathlib import Path
import re
import socket
import sys


def parameters(repo):
    """Read scalar strategy defaults and YAML overrides without requiring PyYAML.

    This reads only the numeric/bool scalars consumed here, not arbitrary YAML.
    Unsupported values for those keys fail instead of silently using defaults.
    """
    source = (repo / 'src/brain/src/brain_node_init.cpp').read_text()
    values = {}
    for key, value in re.findall(r'declare_parameter<(?:double|int|bool)>\("strategy\.([^"\n]+)",\s*([^);]+)\);', source):
        try:
            values[key] = json.loads(value.strip())
        except ValueError:
            pass
    stack = []
    for line in (repo / 'src/brain/config/config.yaml').read_text().splitlines():
        line = line.split('#', 1)[0].rstrip()
        if not line.strip() or ':' not in line:
            continue
        indent = len(line) - len(line.lstrip())
        key, value = line.strip().split(':', 1)
        while stack and stack[-1][0] >= indent:
            stack.pop()
        path = [item[1] for item in stack] + [key]
        if not value.strip():
            stack.append((indent, key))
        elif 'strategy' in path:
            name = '.'.join(path[path.index('strategy') + 1:])
            if name in values:
                values[name] = json.loads(value.strip())
    return values


class Session:
    def __init__(self, cfg):
        self.cfg = cfg
        self.phase = 'idle'
        self.reason = ''
        self.last_exit = -math.inf
        self.started = self.phase_at = self.kick_at = 0.
        self.over_range = self.lost_owner = self.moving_frames = 0
        self.observation = None
        self.unstable_at = None
        self.progress = None
        self.rounds = 0
        self.release = False
        self.goalie_session = False
        self.last_tick = None
        self.missing_since = None
        self.out_since = None
        self.out_observation = None
        self.out_count = 0
        self.gate = "idle"

    def p(self, name):
        return self.cfg['auto_visual_kick.' + name]

    def exit(self, now, reason, immediate=False):
        self.last_exit, self.reason = now, reason
        self.release = reason in ('moving ball requires chase', 'unstable visual-kick approach')
        self.phase, self.phase_at = ('idle' if immediate else 'exiting'), now

    def step(self, f):
        now = f['time']
        x, y = f['ball']
        distance = math.hypot(x, y)
        # Wire time is seconds, native timestamps are integer milliseconds.
        # Remove only floating-point roundoff at their common boundary.
        age = round((now - f['ball_stamp']) * 1000, 6)
        fresh = f['seen'] and f.get('vision_fresh', f['vision']) and 0 <= age <= self.p('current_real_ball_max_age_ms') and distance > .05
        usable = fresh or (0 <= age <= self.p('real_ball_hold_ms') and
                          distance > self.p('near_ball_memory_reject_range') and
                          (self.p('allow_memory_kick_msg') or self.p('allow_predicted_kick_msg')))
        hold = self.phase in ('active', 'exiting') and f.get('vision_fresh', f['vision']) and 0 <= age <= self.p('active_ball_hold_ms') and distance > .05
        roll, pitch = f['state']['rpy'][:2]
        tilt = math.acos(max(-1., min(1., math.cos(roll) * math.cos(pitch))))
        set_play = f.get('set_play_kicker', False)
        owner = f.get('control_owner', f['number'] if f['owner'] or set_play else 0) == f['number']
        # A restart session keeps the common field-player rules after its
        # first touch changes the game state back to PlayOn (also for #1).
        goalie = (f['number'] == 1 and not set_play) if self.phase == 'idle' else self.goalie_session
        # The simulation referee is authoritative about ball out. A persistent
        # camera bias at a touchline cannot overrule PlayOn or a legal restart.
        # Legacy inputs without a server mode use observation confirmation.
        if not f['ball_out'] or bool(f.get('mode')):
            self.out_since, self.out_count = None, 0
        elif fresh and f['ball_stamp'] != self.out_observation:
            self.out_observation = f['ball_stamp']
            self.out_since = now if self.out_since is None else self.out_since
            self.out_count += 1
        confirmed_out = self.out_count >= 3 and now-self.out_since >= .1-1e-8
        hard_reason = f.get('block_reason', 'game state forbids motion') if not f['allowed'] else (
            'recovery tilt limit' if tilt >= math.pi/3 else 'confirmed ball out' if confirmed_out else '')
        self.gate = hard_reason
        if hard_reason:
            if self.phase != 'idle':
                self.exit(now, hard_reason, immediate=True)
            return self.phase
        if self.phase == 'idle':
            cooldown = self.cfg['goalie_visual_kick_cooldown_ms'] if goalie else self.p('striker_reentry_cooldown_ms')
            geometry = distance <= .8 or (self.cfg['auto_visual_kick_enable_dist_min'] < distance < self.cfg['auto_visual_kick_enable_dist_max'] and abs(math.atan2(y, x)) < self.cfg['auto_visual_kick_enable_angle'] * 1.3)
            # Keep B-Human's goalkeeper tactical permission rather than making
            # its goalie abandon the goal whenever it sees a distant ball.
            if goalie:
                geometry = f.get('goalie_area', f['intent']) and distance <= self.cfg['goalie_visual_kick_max_ball_range']
            self.gate = ('not control owner' if not owner else 'ball unavailable' if not usable else
                         'outside entry geometry' if not geometry else
                         'reentry cooldown' if (now-self.last_exit)*1000 < cooldown else 'ready')
            if self.cfg['enable_auto_visual_kick'] and owner and usable and geometry and (now - self.last_exit) * 1000 >= cooldown:
                self.goalie_session = goalie
                self.phase, self.started, self.phase_at = 'starting', now, now
                self.reason = ''
                self.missing_since = None
                self.over_range = self.lost_owner = self.moving_frames = self.rounds = 0
                self.observation = None
                self.progress = None
                self.unstable_at = None
                self.release = False
        elif self.phase == 'starting':
            if not owner:
                self.exit(now, 'lost ownership during startup')
            else:
                # Pause at zero speed on a brief miss. The stable-body work
                # is not discarded; entry still needs a real visual ball.
                self.missing_since = None if fresh else (now if self.missing_since is None else self.missing_since)
                if self.missing_since is not None and now-self.missing_since > .2+1e-8:
                    self.exit(now, 'startup visual reacquisition timeout', immediate=True)
                elif fresh and (now-self.phase_at)*1000 >= self.p('startup_decelerate_ms'):
                    self.phase, self.kick_at = 'active', now
                    self.progress_at = now
        elif self.phase == 'exiting':
            if self.reason == 'no usable/fresh ball source' and (usable or hold):
                self.phase, self.reason = 'active', ''
            else:
                duration = self.p('approach_settle_ms') if self.release else self.p('exit_decelerate_ms')
                if (now - self.phase_at) * 1000 >= duration:
                    self.phase = 'idle'
        else:
            # Distinct actual images, never repeated ticks or predicted balls.
            if not fresh:
                self.over_range = self.moving_frames = 0
            elif f['ball_stamp'] != self.observation:
                self.observation = f['ball_stamp']
                self.over_range = self.over_range + 1 if distance > (3.5 if goalie else 6.) else 0
                vx, vy = f['ball_velocity']
                away = (x * vx + y * vy) / max(distance, .05)
                moving = distance > self.p('near_ball_risk_exemption_range') and math.hypot(vx, vy) >= self.p('moving_ball_speed') and away >= self.p('moving_ball_away_speed')
                self.moving_frames = self.moving_frames + 1 if moving else 0
            # Brain ticks at 100 Hz; ownership loss is confirmed for 3 ticks.
            self.lost_owner = 0 if owner else self.lost_owner + max(1, round((now - (self.last_tick or now - .02)) / .01))
            unstable = distance > self.p('near_ball_risk_exemption_range') and tilt > self.p('approach_tilt_max')
            self.unstable_at = (now if self.unstable_at is None else self.unstable_at) if unstable else None
            reason = ''
            if not goalie and self.moving_frames >= 2:
                reason = 'moving ball requires chase'
            elif not goalie and unstable and (tilt >= .5 or now - self.unstable_at >= .04 - 1e-9):
                reason = 'unstable visual-kick approach'
            elif not usable and not hold:
                reason = 'no usable/fresh ball source'
            elif self.lost_owner >= 3:
                reason = 'lost team ball ownership'
            elif self.over_range >= 4 and (not goalie or now - self.kick_at > 1.5):
                reason = 'ball range over limit'
            # Strikers intentionally have NO duration/progress timeout.
            if goalie:
                gx, gy = f['goal']
                direction = math.atan2(gy-y, gx-x)
                alignment = abs(math.atan2(math.sin(direction-math.atan2(y,x)), math.cos(direction-math.atan2(y,x))))
                if self.progress is None:
                    self.progress = (distance, f['ball_field'], alignment, direction + f['state']['rpy'][2])
                d0, b0, a0, heading = self.progress
                forward = (f['ball_field'][0]-b0[0])*math.cos(heading)+(f['ball_field'][1]-b0[1])*math.sin(heading)
                if d0-distance >= self.p('progress_range_delta') or forward >= self.p('progress_ball_forward_delta') or a0-alignment >= .15:
                    self.progress = (distance, f['ball_field'], alignment, direction + f['state']['rpy'][2])
                    self.progress_at = now
                if (now-self.started)*1000 >= self.p('max_total_ms'):
                    reason = reason or 'session timeout'
                elif (now-self.progress_at)*1000 >= self.p('no_progress_timeout_ms'):
                    reason = reason or 'no progress'
                elif now-self.kick_at > 7.:
                    if self.rounds < self.p('max_extra_rounds') and usable:
                        self.rounds += 1
                        self.kick_at = now
                    else:
                        reason = reason or 'maximum kick duration reached'
            if reason:
                self.exit(now, reason)
        self.last_tick = now
        return self.phase


class TeamSlot:
    """One local self-play team may own one visual-kick session at a time.

    Kernel releases the lock on client crash. Existing session wins, matching
    ZJU's incumbent-kicker priority without changing B-Human's wire format.
    """
    def __init__(self, key):
        directory = Path(tempfile.gettempdir()) / f'bhuman-visualkick-{os.getuid()}'
        directory.mkdir(mode=0o700, exist_ok=True)
        self.file = (directory / hashlib.sha256(key.encode()).hexdigest()).open('a+')

    def acquire(self):
        try:
            fcntl.flock(self.file, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return True
        except BlockingIOError:
            return False

    def select(self, f, session):
        now = f['time']
        distance = math.hypot(*f['ball'])
        age = max(0., now-f['ball_stamp'])
        roll,pitch = f['state']['rpy'][:2]
        eligible = (f['allowed'] and age <= 1.0 and distance > .05 and
                    math.cos(roll)*math.cos(pitch) > .5 and
                    (f['number'] != 1 or f.get('goalie_area', False) or f.get('set_play_kicker', False) or
                     session.phase != 'idle') and
                    (session.phase != 'idle' or now-session.last_exit >= .6))
        yaw = abs(math.atan2(f['ball'][1], f['ball'][0]))
        cost = distance/.8 + yaw*.4 + (0. if f['owner'] or f.get('set_play_kicker') else .2)
        report = dict(time=now, pid=os.getpid(), eligible=eligible, cost=cost, phase=session.phase)
        fcntl.flock(self.file, fcntl.LOCK_EX)
        try:
            self.file.seek(0)
            content = self.file.read()
            state = json.loads(content) if content else {}
            owner = elect(state, f['number'], report, now)
            self.file.seek(0); self.file.truncate()
            self.file.write(json.dumps(state)); self.file.flush()
            return owner
        finally:
            fcntl.flock(self.file, fcntl.LOCK_UN)

    def release(self):
        fcntl.flock(self.file, fcntl.LOCK_UN)


class Controller:
    def __init__(self, repo, slot=None):
        self.slot = slot
        sys.path.insert(0, str(repo / 'src/source_motion'))
        from source_motion.core import Commands
        from source_motion.policies import PolicyBank
        self.bank = PolicyBank(repo / 'src/source_motion/config', repo / 'models')
        model = self.bank.cfg['visualkick']['model_file']
        print(f'[DancerKick] shared policy={repo / "models" / model}', file=sys.stderr, flush=True)
        print(f'[DancerWalk] shared policy={repo / "models" / self.bank.cfg["loco"]["model_file"]}', file=sys.stderr, flush=True)
        self.commands = Commands()
        self.session = Session(parameters(repo))
        self.previous_kind = None
        self.last_time = None
        self.last_reply = None
        self.inferences = 0
        self.amp_inferences = 0
        self.next_inference_at = None
        self.last_targets = {}

    def step(self, f):
        import numpy as np
        from source_motion.core import validate_target
        now = float(f['time'])
        if self.last_time is not None:
            dt = now - self.last_time
            if abs(dt) < 1e-8:
                return self.last_reply
            if dt < 0:
                raise ValueError('Simulation clock moved backwards')
        self.last_time = now
        if self.slot:
            f['control_owner'] = self.slot.select(f, self.session)
        phase = self.session.step(f)
        result = dict(phase=phase, reason=self.session.reason, holding=phase != 'idle', gate=self.session.gate,
                      control_owner=f.get('control_owner', 0))
        state = {key: np.asarray(value, np.float32) for key, value in f['state'].items()}
        amp_safe = math.cos(float(state['rpy'][0])) * math.cos(float(state['rpy'][1])) >= .5
        if not amp_safe or (phase == 'idle' and not f.get('loco_enabled', False)):
            self.previous_kind = None
            self.next_inference_at = None
        else:
            state = {key: np.asarray(value, np.float32) for key, value in f['state'].items()}
            if any(not np.isfinite(v).all() for v in state.values()):
                raise ValueError('Non-finite policy sensor')
            if f['vision']:
                self.commands.visual_ball = f['visual_ball'] if f['seen'] else None
            self.commands.ball = f['ball']
            self.commands.set_reference(*f['ball'], 0., 6., goal=f['goal'], goalkeeper=f['number'] == 1)
            use_kick = phase == 'active' or (phase == 'exiting' and not self.session.release and self.previous_kind == 'kick')
            kind = 'kick' if use_kick else 'loco'
            elapsed = now - self.session.phase_at
            # Same startup command as RLVisionKick::stepDecelerate: distant
            # entry .9 m/s for 400 ms then .3; close field player holds zero.
            close = not self.session.goalie_session and math.hypot(*f['ball']) <= 1.2
            vx = (.9 if elapsed < .4 else .3) if phase == 'starting' and not close and self.session.missing_since is None else 0.
            self.commands.velocity = support_velocity(f, f.get('control_owner', 0)) if phase == 'idle' else [vx, 0., 0.]
            reset = kind != self.previous_kind
            # The server can split one nominal 20 ms agent period into 10/30
            # ms packets when clients join or become desynchronized. Sample
            # inference at 50 Hz; never invent intermediate sensor states.
            if reset or self.next_inference_at is None or now + 1e-8 >= self.next_inference_at:
                q, kp, kd, _ = self.bank.step(kind, state, self.commands, reset=reset)
                if reset or self.next_inference_at is None:
                    self.next_inference_at = now + .02
                else:
                    while self.next_inference_at <= now + 1e-8:
                        self.next_inference_at += .02
                self.previous_kind = kind
                if kind == 'kick':
                    self.inferences += 1
                    q[:2] = [0., .4 if now-self.session.kick_at < .3 else .7]
                else:
                    self.amp_inferences += 1
                    q[:2] = f['head']
                    if phase == 'starting':
                        # Native pass waiting scans must not turn away mid-start.
                        x,y = f['ball']
                        q[:2] = [max(-1., min(1., math.atan2(y,x))),
                                 min(.855, max(.15, math.atan2(.65, max(.1, math.hypot(x,y)))))]
                validate_target(q, kp, kd)
                self.last_targets = dict(q=q.tolist(), kp=kp.tolist(), kd=kd.tolist())
            result.update(self.last_targets)
        result['amp_inferences'] = self.amp_inferences
        result['inferences'] = self.inferences
        self.last_reply = result
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fd', type=int, required=True)
    parser.add_argument('--dancer-repo', type=Path, required=True)
    parser.add_argument('--slot-key', required=True)
    args = parser.parse_args()
    with socket.socket(fileno=args.fd) as connection, connection.makefile('rwb', buffering=0) as stream:
        try:
            controller = Controller(args.dancer_repo, TeamSlot(args.slot_key))
            trace_dir = os.environ.get('HSL_DANCER_TRACE_DIR')
            trace = None
            if trace_dir:
                Path(trace_dir).mkdir(parents=True, exist_ok=True)
                trace = (Path(trace_dir) / f'{os.getpid()}.jsonl').open('w', buffering=1)
            decision = None
            if os.environ.get('HSL_DECISION_DIR'):
                directory = Path(os.environ['HSL_DECISION_DIR'])
                directory.mkdir(parents=True, exist_ok=True)
                decision = (directory / f'{os.getpid()}.csv').open('w', buffering=1)
                writer = csv.writer(decision)
                writer.writerow(['time','play_time','number','mode','phase','gate','reason','control_owner','native_owner','intent','set_play','seen','ball_age','range','ball_yaw','tilt','ball_out','block','amp','kick'])
            stream.write(b'{"loaded":true}\n')
            while line := stream.readline(1 << 20):
                if not line.endswith(b'\n'):
                    raise ValueError('Incomplete worker input')
                packet = json.loads(line)
                reply = controller.step(packet)
                if decision:
                    f = packet
                    writer.writerow([f['time'],f.get('play_time'),f['number'],f.get('mode'),reply['phase'],reply['gate'],reply['reason'],reply['control_owner'],f['owner'],f['intent'],f.get('set_play_kicker'),f['seen'],round(f['time']-f['ball_stamp'],4),math.hypot(*f['ball']),math.atan2(f['ball'][1],f['ball'][0]),math.acos(max(-1.,min(1.,math.cos(f['state']['rpy'][0])*math.cos(f['state']['rpy'][1])))),f['ball_out'],f.get('block_reason'),reply['amp_inferences'],reply['inferences']])
                if trace:
                    trace.write(json.dumps({'input': packet, 'output': reply}, allow_nan=False) + '\n')
                stream.write(json.dumps(reply, allow_nan=False).encode() + b'\n')
        except Exception as exc:
            stream.write(json.dumps({'error': str(exc)}).encode() + b'\n')
            raise


if __name__ == '__main__':
    main()
