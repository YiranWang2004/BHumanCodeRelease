"""Launcher readiness and argument validation; no match processes are started."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import socket
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('apollo_launcher', Path(__file__).resolve().parents[1] / 'start_bhuman_vs_apollo.py')
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


class ApolloLauncherTest(unittest.TestCase):
    def test_trace_readiness_requires_a_complete_sensor_frame(self):
        with tempfile.TemporaryDirectory() as temporary:
            trace = Path(temporary) / 'player.csv'
            self.assertIsNone(launcher.trace_time(trace))
            trace.write_bytes(b'time,mode\n12.3,BeforeKickOff')
            self.assertIsNone(launcher.trace_time(trace))
            trace.write_bytes(b'time,mode\n12.3,BeforeKickOff\n12.32,PlayOn')
            self.assertEqual(launcher.trace_time(trace), 12.3)
            with trace.open('ab') as output:
                output.write(b'\n')
            self.assertEqual(launcher.trace_time(trace), 12.32)

    def test_socket_readiness_requires_an_established_socket_owned_by_the_process(self):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen()
            port = listener.getsockname()[1]
            # A listening port alone does not make the client ready.
            self.assertFalse(launcher.apollo_connected(os.getpid(), port))
            with socket.create_connection(('127.0.0.1', port)):
                self.assertTrue(launcher.apollo_connected(os.getpid(), port))
            self.assertFalse(launcher.apollo_connected(os.getpid(), port))

    def test_default_models_teams_and_privileged_localization(self):
        args = launcher.parse_args([])
        self.assertEqual((args.robot, args.team_bhuman, args.team_apollo, args.players), ('K1', 'BHumanK1', 'Apollo3D', 7))
        self.assertEqual(args.localization, 'truth')
        self.assertFalse(args.start_server)
        self.assertEqual(launcher.parse_args(['--robot', 'T1']).team_bhuman, 'BHumanT1')

    def test_invalid_names_ports_and_nonfinite_durations_are_rejected(self):
        for arguments in [['--team-bhuman', 'bad name'], ['--port', '60001'], ['--team-id', '0'],
                          ['--sim-speed', 'nan'], ['--duration', 'inf'], ['--ready-timeout', '-1']]:
            with self.subTest(arguments=arguments), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                launcher.parse_args(arguments)


if __name__ == '__main__':
    unittest.main()
