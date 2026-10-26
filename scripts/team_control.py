"""Atomic same-host team arbitration using a short-held flock and sim time.

The launcher already keeps each team on one host. No first-come session lock:
all recent player reports are considered, and ownership includes approach.
"""
import math
import os


def elect(state, number, report, now):
    players = state.setdefault('players', {})
    players[str(number)] = report
    live = {}
    for key, p in players.items():
        if not -.05 <= now-p['time'] <= .3:
            continue
        try:
            os.kill(p['pid'], 0)
        except ProcessLookupError:
            continue
        if p['eligible']:
            live[int(key)] = p
    previous = int(state.get('owner', 0))
    best = min(live, key=lambda n: (live[n]['cost'], n)) if live else 0
    if previous in live:
        current = live[previous]
        # A committed session cannot be stolen by an upstream role flicker.
        if current['phase'] != 'idle' or best == previous:
            best = previous
            state.pop('challenger', None)
        elif best and live[best]['cost'] < .7 * current['cost']:
            contender = state.get('challenger')
            if not contender or contender['id'] != best:
                state['challenger'] = {'id': best, 'since': now}
                best = previous
            elif now-contender['since'] < .5:
                best = previous
        else:
            best = previous
            state.pop('challenger', None)
    else:
        state.pop('challenger', None)
    state['owner'] = best
    state['players'] = {k: p for k,p in players.items() if -.05 <= now-p['time'] <= 2.}
    return best


def support_velocity(f, owner):
    """Replace only competing ball approaches; preserve other native tactics."""
    native = list(f.get('velocity', [0., 0., 0.]))
    if not owner or owner == f['number'] or not f['allowed'] or f['number'] == 1:
        return native
    x,y = f['ball']
    distance = math.hypot(x,y)
    approaching = native[0]*x + native[1]*y > .05
    if not (f['owner'] or f['intent'] or f.get('set_play_kicker') or (distance < 1.8 and approaching)):
        return native
    # Support behind and to the side of the ball in team field coordinates.
    bx,by = f['ball_field']
    px,py,yaw = f['pose']
    half_x,half_y = f['field']
    side = 1 if f['number'] % 2 == 0 else -1
    tx = max(-half_x+.6, min(half_x-.6, bx-1.8))
    ty = max(-half_y+.6, min(half_y-.6, by+side*2.))
    dx,dy = tx-px,ty-py
    cx,cy = math.cos(yaw)*dx+math.sin(yaw)*dy, -math.sin(yaw)*dx+math.cos(yaw)*dy
    scale = min(1., .65/max(math.hypot(cx,cy), .01))
    return [cx*scale,cy*scale,max(-1., min(1., math.atan2(y,x)))]
