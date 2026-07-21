#!/usr/bin/env python3
"""验证下位机静态更新清单的 Docker image_id 字段。"""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("write_lower_update_manifest.py")


class WriteLowerUpdateManifestTest(unittest.TestCase):
    def make_inputs(self, root: Path) -> tuple[Path, Path, str]:
        artifact = root / "mskdsp-test-linux-arm64"
        artifact.write_bytes(b"test lower update package")
        digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
        checksums = root / "SHA256SUMS"
        checksums.write_text(f"{digest}  {artifact.name}\n", encoding="utf-8")
        return artifact, checksums, digest

    def run_generator(self, root: Path, image_id: str) -> subprocess.CompletedProcess[str]:
        artifact, checksums, _ = self.make_inputs(root)
        output = root / "latest.json"
        return subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--channel",
                "ci",
                "--platform",
                "linux-arm64",
                "--version",
                "0.2.4-ci-test-linux-arm64",
                "--image-id",
                image_id,
                "--artifact",
                str(artifact),
                "--checksums",
                str(checksums),
                "--output",
                str(output),
                "--base-url",
                "https://update.example/mskdsp-lower",
            ],
            cwd=SCRIPT.parent.parent.parent,
            capture_output=True,
            text=True,
            check=False,
        )

    def test_manifest_contains_normalized_image_id(self) -> None:
        image_id = "sha256:" + "A" * 64
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            result = self.run_generator(root, image_id)
            self.assertEqual(result.returncode, 0, result.stderr)
            manifest = json.loads((root / "latest.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["image_id"], "sha256:" + "a" * 64)

    def test_generator_rejects_invalid_image_id(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            result = self.run_generator(Path(temp_dir), "sha256:not-a-docker-image-id")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("image_id", result.stderr)


if __name__ == "__main__":
    unittest.main()
