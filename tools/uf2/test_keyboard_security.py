"""Exercise the real sender's authorization, destination and connection lifetime."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_app_security import ROOT, function


class KeyboardSecurityTest(unittest.TestCase):
    def test_real_sender(self):
        source = (ROOT / 'src/keyboard.c').read_text()
        names = ['kbd_init', 'kbd_is_subscribed', 'kbd_set_authorized_cb',
                 'read_input_report', 'char_to_usage', 'sender_clear', 'sender_finish',
                 'transient_notify_error', 'sender_send_failed', 'character_delay_ms',
                 'kbd_send_duration_ms', 'kbd_selftest', 'report_complete',
                 'sender_work_handler', 'kbd_send_code', 'kbd_cancel_send']
        bodies = [function(source, name) for name in names]
        declarations = '\n'.join(body[:body.index('\n{')] + ';' for body in bodies)
        start = source.index('static volatile int cccd_notify;')
        end = source.index('} sender;', start) + len('} sender;')
        constants = '\n'.join(re.findall(r'(?m)^#define KBD_[^\n]+', source))
        harness = Path(__file__).with_name('keyboard_security_harness.c').read_text()
        harness = harness.replace('/* ACTUAL_GLOBALS */', constants + '\n' + source[start:end])
        harness = harness.replace('/* ACTUAL_FUNCTIONS */', declarations + '\n' + '\n'.join(bodies))
        with tempfile.TemporaryDirectory(prefix='tapshard-keyboard-') as directory:
            path = Path(directory) / 'sender.c'
            path.write_text(harness)
            executable = Path(directory) / 'sender'
            result = subprocess.run([
                'cc', '-std=c99', '-O1', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-unused-variable',
                '-fsanitize=undefined', '-fno-sanitize-recover=all',
                '-I', str(ROOT / 'src'), '-I', str(Path(__file__).parent),
                str(path), '-o', str(executable),
            ], timeout=25, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], check=True, timeout=10,
                                    capture_output=True, text=True)
            self.assertIn('keyboard security scenarios passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
