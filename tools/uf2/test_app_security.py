"""Compile the real application authorization paths with a bounded fake transport."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r"(?m)^(?:static\s+)?[^\n;{}=]*\b" + re.escape(name)
                      + r"\([^;{}]*\)\n\{", source)
    if not match:
        raise ValueError(f"Missing actual function {name}")
    end = source.index("\n}", match.end()) + 2
    return source[match.start():end] + "\n"


class AppSecurityTest(unittest.TestCase):
    def test_real_application_paths(self):
        source = (ROOT / 'src/main.c').read_text()
        names = [
            'connection_get', 'connection_matches', 'peer_revoke',
            'peer_is_authorized', 'authorize_peer', 'pairing_window_set',
            'startup_inject', 'link_ready', 'pending_valid', 'pending_clear',
            'hid_ready_budget_ms', 'clear_dump_secrets', 'start_connecting',
            'wait_for_hid', 'on_subscribed', 'on_host_setup', 'on_output_write',
            'settle_work_handler',
            'sending_clear', 'set_state', 'go_idle', 'stop_pairing_ui',
            'enter_pairing', 'finish_pairing', 'cancel_pairing', 'connected',
            'disconnected', 'recycled', 'pairing_timeout_handler',
            'pairing_accept', 'peer_event_submit', 'reject_peer',
            'discard_completed_bond', 'peer_work_handler', 'security_changed', 'pairing_complete',
            'pairing_finish_work_handler', 'bond_deleted',
            'queue_request', 'classify_pending', 'generate_code', 'slow_typing',
            'totp_boundary_wait_ms', 'send_complete', 'type_now', 'try_send',
            'boot_error_vbus', 'startup_fail', 'bt_ready',
            'button_pressed', 'button_clicked', 'button_long_pressed', 'main',
        ]
        bodies = [function(source, name) for name in names]
        bodies[-1] = bodies[-1].replace('int main(void)', 'int app_main(void)', 1)
        declarations = '\n'.join(body[:body.index('\n{')] + ';' for body in bodies)
        globals_start = source.index('enum app_state {')
        globals_end = source.index('/* Device-wide settings', globals_start)
        pending_start = source.index('enum pending_kind {')
        pending_end = source.index('static struct pending_req pending;', pending_start)
        pending_end += len('static struct pending_req pending;')
        actual = (source[globals_start:globals_end] + '\n' +
                  source[pending_start:pending_end] + '\n')
        harness = (Path(__file__).with_name('app_security_harness.c')).read_text()
        harness = harness.replace('/* ACTUAL_GLOBALS */', actual)
        auth = []
        for prefix in ('static struct bt_conn_auth_info_cb auth_info_cb = {',
                       'static const struct bt_conn_auth_cb auth_cb = {'):
            start = source.index(prefix)
            auth.append(source[start:source.index('\n};', start) + 3])
        harness = harness.replace('/* ACTUAL_DECLARATIONS */', declarations)
        harness = harness.replace('/* ACTUAL_AUTH */', '\n'.join(auth))
        harness = harness.replace('/* ACTUAL_FUNCTIONS */', '\n'.join(bodies))
        with tempfile.TemporaryDirectory(prefix='tapshard-security-') as directory:
            temporary = Path(directory)
            for name, text in {
                'zephyr/bluetooth/addr.h': '#include "security_mocks.h"\n',
                'zephyr/bluetooth/conn.h': '#include "security_mocks.h"\n',
                'zephyr/sys/util.h': '#include "security_mocks.h"\n',
            }.items():
                path = temporary / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            path = temporary / 'application.c'
            path.write_text(harness)
            executable = temporary / 'application'
            flags = [
                'cc', '-std=c99', '-O1', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-unused-variable',
                '-Wno-sign-compare',
                '-fsanitize=undefined', '-fno-sanitize-recover=all',
                '-DCONFIG_TOTP_MAX_BONDS=4', '-DCONFIG_BT_MAX_PAIRED=4',
                '-I', str(temporary), '-I', str(ROOT / 'src'),
                '-I', str(Path(__file__).parent)]
            commands = [flags + ['-Dprofiles_selftest=real_profiles_selftest', '-c',
                                 str(ROOT / 'src/profiles.c'), '-o', str(temporary / 'profiles.o')],
                        flags + [str(path), str(temporary / 'profiles.o'), '-o', str(executable)]]
            for command in commands:
                compiled = subprocess.run(command, timeout=25, capture_output=True, text=True)
                self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(executable)], check=True, timeout=10,
                                    capture_output=True, text=True)
            self.assertIn('application security scenarios passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
