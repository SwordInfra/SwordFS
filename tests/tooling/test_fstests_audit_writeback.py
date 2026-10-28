#!/usr/bin/env python3
"""Validate log isolation and deliberate inode-policy error distinction."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('audit_writeback', ROOT / 'scripts/conformance/fstests/audit_writeback.py')
assert SPEC and SPEC.loader
AUDIT = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = AUDIT
SPEC.loader.exec_module(AUDIT)


class AuditWritebackTest(unittest.TestCase):
    def test_watches_only_bytes_written_during_selected_case(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'swordfs-scratch.log'
            path.write_text('FileReadWriter::Flush chunk FAILED: earlier testcase\n')
            offset = path.stat().st_size
            path.write_text(path.read_text() + 'FileReadWriter::Flush chunk FAILED: later testcase\n')
            errors = AUDIT.audit('generic/346', {'scratch': (path, offset)})
            self.assertEqual(len(errors), 1)
            self.assertIn('later testcase', errors[0])
            self.assertNotIn('earlier testcase', errors[0])

    def test_generic079_can_reject_inode_policy_but_generic346_cannot(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'scratch.log'
            path.write_text('FileReadWriter::Flush chunk FAILED: finalize chunk write rejected by inode policy\n'
                            'local RELEASE failed: finalize chunk write rejected by inode policy\n')
            self.assertEqual([], AUDIT.audit('generic/079', {'scratch': (path, 0)}))
            self.assertEqual(2, len(AUDIT.audit('generic/346', {'scratch': (path, 0)})))

    def test_unrelated_error_and_missing_logs_are_not_silently_ignored(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'scratch.log'
            path.write_text('FileReadWriter::Flush chunk FAILED: Slice publication lost too many EOF/attachment races\n')
            self.assertEqual(1, len(AUDIT.audit('generic/079', {'scratch': (path, 0)})))
            with self.assertRaises(OSError):
                AUDIT.audit('generic/346', {'scratch': (Path(tmp) / 'missing', 0)})
            with self.assertRaises(ValueError):
                AUDIT.audit('generic/346', {'scratch': (path, 999999)})


if __name__ == '__main__':
    unittest.main()
