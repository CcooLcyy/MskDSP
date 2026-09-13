#!/usr/bin/env python3
"""Gitee 中转同步逻辑的最小确定性测试。"""

from __future__ import annotations

import hashlib
import importlib.util
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("sync_gitee_release.py")
SPEC = importlib.util.spec_from_file_location("sync_gitee_release", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class GiteeRelayTest(unittest.TestCase):
    # 验证 SHA256SUMS 正确时允许发布。
    def test_verify_checksums_accepts_matching_asset(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            asset = root / "package.bin"
            asset.write_bytes(b"mskdsp")
            digest = hashlib.sha256(asset.read_bytes()).hexdigest()
            checksum = root / "SHA256SUMS"
            checksum.write_text(f"{digest}  package.bin\n", encoding="utf-8")
            MODULE.verify_checksums(root, checksum)

    # 验证路径穿越文件名会被拒绝。
    def test_verify_checksums_rejects_unsafe_name(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            checksum = root / "SHA256SUMS"
            checksum.write_text("0" * 64 + "  ../package.bin\n", encoding="utf-8")
            with self.assertRaises(SystemExit):
                MODULE.verify_checksums(root, checksum)


if __name__ == "__main__":
    unittest.main()
