#!/usr/bin/env python3
"""Measure real server See packets against MuJoCo truth, without running a match.

Requires the server's Python environment. This tests perception/serialization and
float32 reconstruction with a known camera pose, not the online pose estimator,
tracker association or stale observations. No server source or live state changes.
"""
import argparse
import csv
import json
import math
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import mujoco
import numpy as np


def main():
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server-repo', type=Path, default=repo.parents[1] / 'rcssservermj')
    parser.add_argument('--decoder', type=Path, default=repo / 'build_sim/rcss_decode_players')
    parser.add_argument('--output', type=Path, default=repo / 'reports/opponent_range')
    parser.add_argument('--samples', type=int, default=30)
    parser.add_argument('--seed', type=int, default=20261016)
    parser.add_argument('--tolerance', type=float, default=.25,
                        help='Maximum horizontal error in metres for the upright first-marker proxy.')
    args = parser.parse_args()
    if args.samples < 1 or args.tolerance <= 0:
        parser.error('--samples and --tolerance must be positive')
    sys.path.insert(0, str(args.server_repo / 'src'))
    from rcsssmj.games.soccer.sim.soccer_sim import SoccerSimulation
    from rcsssmj.games.soccer.soccer_fields import create_soccer_field
    from rcsssmj.games.soccer.soccer_rules import create_soccer_rule_book
    from rcsssmj.sim.perceptions import VisionPerception

    rng = np.random.default_rng(args.seed)
    diagonal = math.hypot(55, 36)
    distances = [5, 9, 15, 25, 40, 55, 60, 65, diagonal, 70, 80, 100]
    rows, packets, contexts = [], [], []
    for observer_model in ('K1BHuman', 'T1BHuman'):
        for target_model in ('K1BHuman', 'T1BHuman', 'T1'):
            sim = SoccerSimulation(create_soccer_field('fifa7vs7'), create_soccer_rule_book('ssim'), bhuman_physics=True)
            if not sim.init():
                raise RuntimeError('Cannot initialize simulation')
            observer, target = sim.add_players([
                SimpleNamespace(model_name=observer_model, team_name='RangeObserver', player_no=1),
                SimpleNamespace(model_name=target_model, team_name='RangeTarget', player_no=1)])
            model, data = sim.mj_model, sim.mj_data
            half_fov = float(observer.spec.site(observer.agent_id.prefix + 'camera').userdata[0])
            for distance in distances:
                for sample in range(args.samples):
                    # The diagonal positions stay inside the field through 65.734m.
                    offset = np.array([55., 36.]) / diagonal * distance / 2
                    observer.drop_at(*(-offset), math.atan2(36, 55))
                    target.drop_at(*offset, rng.uniform(-math.pi, math.pi))
                    # Sample central vision, near-FOV edges, and outside-FOV controls.
                    bearing = (-1 if sample % 2 else 1) * (0, .4, .8, .98, 1.05)[sample % 5] * half_fov
                    for agent, roll, pitch in ((observer, rng.uniform(-5, 5), rng.uniform(-5, 5)),
                                               (target, rng.uniform(-5, 5), rng.uniform(-5, 5))):
                        root = data.joint(agent.root_joint_name).qpos
                        original = root[3:7].copy()
                        tilt = np.zeros(4)
                        mujoco.mju_euler2Quat(tilt, np.radians([roll, pitch, 0]), 'xyz')
                        mujoco.mju_mulQuat(root[3:7], original, tilt)
                    for joint in observer.spec.joints:
                        if 'head' in joint.name.lower():
                            angle = bearing if 'yaw' in joint.name.lower() else rng.uniform(-10, 10)
                            data.joint(joint.name).qpos[0] = math.radians(angle)
                    mujoco.mj_forward(model, data)
                    sim.generate_perceptions(gen_vision=True)
                    vision = next(p for p in observer.perceptions if isinstance(p, VisionPerception))
                    packets.append(vision.to_sexp())
                    camera = data.site(observer.agent_id.prefix + 'camera')
                    contexts.append((camera.xpos.copy(), camera.xmat.reshape(3, 3).copy(),
                                     {name: data.site(site).xpos.copy() for site, name in target.markers},
                                     target.xpos.copy()))
                    rows.append(dict(observer=observer_model, target=target_model,
                                     distance_m=distance, sample=sample, bearing_deg=bearing,
                                     in_field=distance <= diagonal, visible=False,
                                     marker='', marker_error_m='', center_proxy_error_m=''))
            print(f'Generated {observer_model} -> {target_model}', flush=True)

    decoded = subprocess.run([str(args.decoder)], input='\n'.join(packets) + '\n',
                             text=True, capture_output=True, check=True).stdout.splitlines()
    if len(decoded) != len(rows):
        raise RuntimeError('Decoder output length mismatch')
    for row, context, line in zip(rows, contexts, decoded):
        if line == 'none':
            continue
        marker, distance, azimuth, elevation = line.split()
        distance, az, el = np.float32(float(distance) * 1000), np.float32(math.radians(float(azimuth))), np.float32(math.radians(float(elevation)))
        # Same float precision/units as RCSSVisionProvider::cameraPoint.
        point = np.array([distance * np.cos(el) * np.cos(az), distance * np.cos(el) * np.sin(az), distance * np.sin(el)], dtype=np.float32)
        camera_position, camera_rotation, markers, target_position = context
        reconstructed = (camera_rotation.astype(np.float32) @ point + (camera_position * 1000).astype(np.float32)) / 1000
        row.update(visible=True, marker=marker,
                   marker_error_m=float(np.linalg.norm(reconstructed[:2] - markers[marker][:2])),
                   center_proxy_error_m=float(np.linalg.norm(reconstructed[:2] - target_position[:2])))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.with_suffix('.csv').open('w') as file:
        writer = csv.DictWriter(file, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    summaries = []
    for distance in distances:
        group = [r for r in rows if r['distance_m'] == distance]
        visible = [r for r in group if r['visible']]
        marker = [r['marker_error_m'] for r in visible]
        center = [r['center_proxy_error_m'] for r in visible]
        summaries.append(dict(distance_m=distance, visible=len(visible), total=len(group),
                              marker_max_m=max(marker), center_p95_m=float(np.percentile(center, 95)),
                              center_max_m=max(center), reliable=max(center) <= args.tolerance))
    revisions = {name: subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()
                 for name, path in (('bhuman', repo), ('server', args.server_repo))}
    summary = dict(seed=args.seed, samples=len(rows), visible=sum(r['visible'] for r in rows),
                   revisions=revisions, mujoco_version=mujoco.__version__, numpy_version=np.__version__,
                   tolerance_m=args.tolerance, field_diagonal_m=diagonal,
                   max_tested_reliable_m=max(s['distance_m'] for s in summaries if s['reliable']),
                   results=summaries,
                   scope='Real server generator and C++ production decoder; known camera pose, upright reference stance with +/-5deg torso tilt. No online localization, tracking, occlusion realism, falls, gait or temporal latency validation.')
    args.output.with_suffix('.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    # The acceptance criterion applies to every visible sample, not just averages.
    if not all(s['reliable'] for s in summaries if s['distance_m'] <= diagonal):
        raise SystemExit('FAIL: in-field measurement exceeds tolerance')


if __name__ == '__main__':
    main()
