"""验证 CI 构建身份与安装包冻结规则。"""
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parent))
import lower_metadata as target


class MetadataTest(unittest.TestCase):
    def setUp(self):
        self.environment = {
            "CI_PIPELINE_SOURCE": "push", "CI_COMMIT_BRANCH": "master",
            "CI_COMMIT_SHA": "a" * 40, "CI_PIPELINE_ID": "100", "CI_PIPELINE_IID": "7",
            "CI_JOB_ID": "42", "CI_PROJECT_URL": "https://gitlab.example/mskdsp/lower",
        }

    def test_identity_rejects_non_main_sources_and_missing_ids(self):
        for values in ({"CI_COMMIT_BRANCH": "beta/1"}, {"CI_PIPELINE_SOURCE": "merge_request_event"},
                       {"CI_COMMIT_TAG": "v1"}, {"CI_JOB_ID": ""}, {"CI_COMMIT_SHA": "bad"}):
            with self.subTest(values=values), self.assertRaises(ValueError):
                target.create_identity("1.0.0", "arm64", self.environment | values)

    def test_retries_and_architectures_have_distinct_versions(self):
        first = target.create_identity("1.0.0\n", "arm64", self.environment)
        retry = target.create_identity("1.0.0", "arm64", self.environment | {"CI_JOB_ID": "43"})
        self.assertNotEqual(first["version"], retry["version"])
        self.assertIn("p7-b42", first["version"])
        packaged = target.package_identity(first, self.environment | {"CI_JOB_ID": "50"})
        self.assertIn("-j50", packaged["version"])
        self.assertTrue(packaged["package_version"].endswith("-linux-arm64"))
        with self.assertRaises(ValueError):
            target.package_identity(first, self.environment | {"CI_PIPELINE_ID": "101"})

    # Beta 必须从同版本分支构建，架构产物保留真实来源且不可篡改通道。
    def test_beta_identity_freezes_branch_version_and_channel(self):
        env = self.environment | {"CI_COMMIT_BRANCH": "beta/0.4.0"}
        for arch in ("arm64", "x64"):
            identity = target.create_identity("0.4.0", arch, env)
            self.assertEqual(identity["channel"], "beta")
            self.assertEqual(identity["source_ref"], "beta/0.4.0")
            self.assertTrue(identity["version"].startswith("0.4.0-beta-p7-b42"))
            packaged = target.package_identity(identity, env | {"CI_JOB_ID": "50"})
            self.assertEqual(packaged["channel"], "beta")
            self.assertIn("-j50-", packaged["version"])
            with self.assertRaises(ValueError):
                target.package_identity(identity | {"channel": "ci"}, env)
        for branch in ("beta/1", "beta/0.4.1", "beta/0.4.0/extra", "beta/00.4.0"):
            with self.subTest(branch=branch), self.assertRaises(ValueError):
                target.create_identity("0.4.0", "arm64", env | {"CI_COMMIT_BRANCH": branch})

    def test_freeze_validates_image_platform_and_actual_package_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            identity = target.package_identity(target.create_identity("1.0.0", "arm64", self.environment),
                                               self.environment | {"CI_JOB_ID": "50"})
            archive = root / "image.tar"
            config = json.dumps({"os": "linux", "architecture": "arm64"}).encode()
            config_name = hashlib.sha256(config).hexdigest() + ".json"
            with tarfile.open(archive, "w") as stream:
                for name, data in ((config_name, config), ("manifest.json", json.dumps([
                    {"Config": config_name, "RepoTags": ["mskdsp:" + identity["package_version"]]}]).encode())):
                    member = tarfile.TarInfo(name)
                    member.size = len(data)
                    stream.addfile(member, io.BytesIO(data))
            artifact = root / ("mskdsp-" + identity["package_version"])
            artifact.write_bytes(b"self-extracting-package")
            result = target.freeze_metadata(identity, artifact, archive)
            self.assertEqual(result["sha256"], hashlib.sha256(artifact.read_bytes()).hexdigest())
            self.assertEqual(result["size"], artifact.stat().st_size)
            self.assertEqual(result["image_id"], "sha256:" + config_name[:-5])
            with self.assertRaises(SystemExit):
                target.freeze_metadata(identity | {"platform": "linux-x64"}, artifact, archive)
            with self.assertRaises(ValueError):
                target.freeze_metadata(identity, artifact.with_name("missing"), archive)


if __name__ == "__main__":
    unittest.main()
