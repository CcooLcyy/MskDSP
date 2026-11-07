#!/usr/bin/env python3
"""验证下位机静态更新清单的 Docker image_id 字段。"""

from __future__ import annotations

import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import tarfile
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

    def run_generator(self, root: Path, image_id: str, channel: str = "ci") -> subprocess.CompletedProcess[str]:
        artifact, checksums, _ = self.make_inputs(root)
        output = root / "latest.json"
        return subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--channel",
                channel,
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
            encoding="utf-8",
            env={**os.environ, "PYTHONIOENCODING": "utf-8"},
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

    # 共享镜像校验仍可使用，清单生成入口不能创建退役 Beta 新清单。
    def test_generator_rejects_retired_beta_channel(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            result = self.run_generator(root, "sha256:" + "a" * 64, channel="beta")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Beta", result.stderr)
            self.assertFalse((root / "latest.json").exists())

    def test_generator_rejects_invalid_image_id(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            result = self.run_generator(Path(temp_dir), "sha256:not-a-docker-image-id")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("image_id", result.stderr)

    def make_image_tar(self, root: Path, *, oci: bool = False, corrupt: bool = False,
                       multiple: bool = False, architecture: str = "arm64") -> tuple[Path, str]:
        config = json.dumps({"architecture": architecture, "os": "linux",
                             "config": {}, "rootfs": {"type": "layers", "diff_ids": []}}).encode()
        digest = hashlib.sha256(config).hexdigest()
        name = f"blobs/sha256/{digest}" if oci else f"{digest}.json"
        manifest = [{"Config": name, "RepoTags": ["mskdsp:test"], "Layers": []}]
        if multiple:
            manifest.append(dict(manifest[0]))
        path = root / "image.tar"
        with tarfile.open(path, "w") as archive:
            for member_name, content in [(name, config + b" " if corrupt else config),
                                         ("manifest.json", json.dumps(manifest).encode())]:
                member = tarfile.TarInfo(member_name)
                member.size = len(content)
                archive.addfile(member, io.BytesIO(content))
        return path, "sha256:" + digest

    def run_tar_generator(self, root: Path, image_tar: Path) -> subprocess.CompletedProcess[str]:
        artifact, checksums, _ = self.make_inputs(root)
        return subprocess.run([
            sys.executable, str(SCRIPT), "--channel", "ci", "--platform", "linux-arm64",
            "--version", "test-linux-arm64", "--image-tar", str(image_tar),
            "--artifact", str(artifact), "--checksums", str(checksums),
            "--output", str(root / "latest.json"), "--base-url", "https://update.example/lower",
        ], capture_output=True, text=True, encoding="utf-8",
           env={**os.environ, "PYTHONIOENCODING": "utf-8"}, check=False)

    # 验证经典与 OCI 路径都按配置原始内容生成身份，不依赖本机 Docker 的 ID 含义。
    def test_tar_generator_uses_config_content_digest_for_both_layouts(self) -> None:
        for oci in (False, True):
            with self.subTest(oci=oci), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir)
                image_tar, expected = self.make_image_tar(root, oci=oci)
                result = self.run_tar_generator(root, image_tar)
                self.assertEqual(result.returncode, 0, result.stderr)
                manifest = json.loads((root / "latest.json").read_text(encoding="utf-8"))
                self.assertEqual(manifest["image_id"], expected)

    # 验证配置文件内容与其摘要路径不一致时拒绝发布清单。
    def test_tar_generator_rejects_corrupt_config(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            image_tar, _ = self.make_image_tar(root, oci=True, corrupt=True)
            result = self.run_tar_generator(root, image_tar)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("摘要", result.stderr)
            self.assertFalse((root / "latest.json").exists())

    # 验证多镜像归档不能随意选择第一条配置发布。
    def test_tar_generator_rejects_ambiguous_images(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            image_tar, _ = self.make_image_tar(root, multiple=True)
            result = self.run_tar_generator(root, image_tar)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("单个镜像", result.stderr)

    # 验证归档镜像架构必须与发布清单平台一致。
    def test_tar_generator_rejects_wrong_platform(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            image_tar, _ = self.make_image_tar(root, architecture="amd64")
            result = self.run_tar_generator(root, image_tar)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("平台", result.stderr)

    # 验证 GitLab 保留原始镜像归档到安装包及冻结元数据完成，共享归档校验仍被使用。
    def test_gitlab_keeps_image_tar_until_identity_freeze(self) -> None:
        root = SCRIPT.parents[2]
        source = (root / "script/gitlab/build_lower.sh").read_text(encoding="utf-8")
        packaged = source.index('bash script/make_exe.sh "${image_tar}"')
        frozen = source.index("python3 script/gitlab/lower_metadata.py freeze", packaged)
        removed = source.index('rm -f "${image_tar}"', packaged)
        self.assertLess(frozen, removed)
        self.assertIn('--image-tar "${image_tar}"', source[frozen:removed])
        metadata = (root / "script/gitlab/lower_metadata.py").read_text(encoding="utf-8")
        self.assertIn("from write_lower_update_manifest import read_image_config_id", metadata)
        self.assertIn('read_image_config_id(image_tar, identity["platform"])', metadata)

    # GitHub Actions 与 GitLab 使用平台软件库发布，旧 R2 和静态发布入口继续退役。
    def test_ci_uses_platform_without_legacy_static_publishers(self) -> None:
        root = SCRIPT.parents[2]
        self.assertTrue((root / ".gitlab-ci.yml").is_file())
        workflow_dir = root / ".github/workflows"
        workflows = list(workflow_dir.glob("*.yml")) + list(workflow_dir.glob("*.yaml"))
        self.assertEqual(sorted(path.name for path in workflows), ["ci.yml"])
        self.assertIn("bash script/github/publish.sh", (workflow_dir / "ci.yml").read_text(encoding="utf-8"))
        publish_script = (root / "script/github/publish.sh").read_text(encoding="utf-8")
        self.assertIn("python script/gitlab/platform_client.py", publish_script)
        for name in ("upload_r2.sh", "sync_static_lower_update.sh", "sync_static_lower_update_if_configured.sh"):
            with self.subTest(script=name):
                self.assertFalse(SCRIPT.with_name(name).exists())
        client = (root / "script/gitlab/platform_client.py").read_text(encoding="utf-8")
        self.assertIn("publisher.CompleteUpload", client)
        self.assertIn("publisher.PublishRelease", client)

    # 验证 OCI manifest 不得指向 Docker 清单之外的另一份配置。
    def test_tar_generator_rejects_unrelated_oci_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            image_tar, _ = self.make_image_tar(root, oci=True)
            manifest_raw = json.dumps({"schemaVersion": 2, "config": {
                "digest": "sha256:" + "f" * 64}, "layers": []}).encode()
            manifest_digest = hashlib.sha256(manifest_raw).hexdigest()
            index = json.dumps({"schemaVersion": 2, "manifests": [
                {"digest": "sha256:" + manifest_digest}]}).encode()
            with tarfile.open(image_tar, "a") as archive:
                for name, raw in [("blobs/sha256/" + manifest_digest, manifest_raw), ("index.json", index)]:
                    member = tarfile.TarInfo(name)
                    member.size = len(raw)
                    archive.addfile(member, io.BytesIO(raw))
            result = self.run_tar_generator(root, image_tar)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("关联", result.stderr)


if __name__ == "__main__":
    unittest.main()
