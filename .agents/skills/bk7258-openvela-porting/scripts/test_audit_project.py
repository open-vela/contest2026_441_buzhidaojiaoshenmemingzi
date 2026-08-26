"""Host-only audit tests; temporary fixtures are not device evidence."""

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name('audit_project.py')
spec = importlib.util.spec_from_file_location('audit_project', SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class AuditTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='bk7258-audit-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'repo with spaces'
        self.source = self.root / 'app/vision_badge/src'
        self.source.mkdir(parents=True)
        (self.root / 'board').mkdir()
        for name in ('vision', 'camera', 'audio'):
            self.write(f'app/vision_badge/src/{name}_service.c', 'int probe(void) { return 0; }\n')

    def write(self, name, text):
        (self.root / name).write_text(text, encoding='utf-8')

    def test_enabled_defconfig_is_found(self):
        self.write('board/defconfig', 'CONFIG_LCD=y\n')
        result = module.audit(self.root)['capabilities']['graphics']
        self.assertEqual(result['status'], 'signal-needs-review')
        self.assertEqual(result['evidence'], [{'path': 'board/defconfig', 'line': 1}])

    def test_disabled_config_and_comments_are_not_implementation(self):
        self.write('board/defconfig', '# CONFIG_LCD is not set\nCONFIG_LVGL=n\n')
        self.write('app/vision_badge/src/view.c', '// lv_init();\n/* lcd_initialize(); */\nchar *s = "nx_open()";\n')
        self.assertEqual(module.audit(self.root)['capabilities']['graphics']['status'], 'not-evidenced')

    def test_api_line_is_preserved(self):
        self.write('app/vision_badge/src/view.c', '/* first\n second */\nvoid f(void) { lv_init(); }\n')
        result = module.audit(self.root)['capabilities']['graphics']
        self.assertEqual(result['evidence'][0]['line'], 3)

    def test_multiline_placeholder(self):
        self.write('app/vision_badge/src/vision_service.c', 'int f(void) {\n return\n (-ENOSYS);\n}\n')
        result = module.audit(self.root)['capabilities']['ai']
        self.assertEqual(result['status'], 'placeholder-signal')
        self.assertEqual(result['evidence'][0]['line'], 3)

    def test_placeholder_in_comment_or_string_is_ignored(self):
        self.write('app/vision_badge/src/vision_service.c', '/* return -ENOSYS; */\nchar *s = "ENOSYS";\n')
        self.assertEqual(module.audit(self.root)['capabilities']['ai']['status'], 'needs-runtime-evidence')

    def test_one_media_backend_does_not_hide_the_other(self):
        self.write('app/vision_badge/src/audio_service.c', 'int f(void) { return -ENOSYS; }\n')
        result = module.audit(self.root)['capabilities']['multimedia']
        self.assertEqual(result['status'], 'placeholder-signal')
        self.assertEqual(len(result['evidence']), 1)

    def test_missing_service_fails_closed(self):
        (self.source / 'camera_service.c').unlink()
        with self.assertRaises(OSError):
            module.audit(self.root)

    def test_missing_board_fails_closed(self):
        (self.root / 'board').rmdir()
        with self.assertRaises(ValueError):
            module.audit(self.root)

    def test_invalid_encoding_is_an_error(self):
        (self.source / 'audio_service.c').write_bytes(b'\xff')
        with self.assertRaises(UnicodeError):
            module.audit(self.root)

    def test_cli_and_paths_with_spaces(self):
        run = subprocess.run([sys.executable, '-X', 'utf8', '-B', str(SCRIPT), str(self.root), '--json'],
                             capture_output=True, encoding='utf-8')
        self.assertEqual(run.returncode, 0, run.stderr)
        result = json.loads(run.stdout)
        self.assertEqual(result['scope'], 'static-source-signals-only')
        self.assertEqual(result['capabilities']['multimedia']['status'], 'needs-runtime-evidence')

    def test_invalid_root_returns_two_without_success_output(self):
        run = subprocess.run([sys.executable, '-X', 'utf8', '-B', str(SCRIPT), str(self.root / 'absent'), '--json'],
                             capture_output=True, encoding='utf-8')
        self.assertEqual(run.returncode, 2)
        self.assertEqual(run.stdout, '')


if __name__ == '__main__':
    unittest.main()
