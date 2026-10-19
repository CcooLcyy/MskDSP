"""验证下位机流水线的触发、双架构门禁及隔离约束。"""
from pathlib import Path
import unittest
import yaml

ROOT = Path(__file__).resolve().parents[2]


class LowerCiTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.config = yaml.safe_load((ROOT / ".gitlab/ci/ci.yml").read_text(encoding="utf-8"))

    def test_only_main_push_or_web_is_admitted(self):
        rules = self.config["workflow"]["rules"]
        self.assertEqual(rules[-1], {"when": "never"})
        condition = rules[0]["if"]
        for token in ("CI_COMMIT_TAG", "master", "main", "push", "web"):
            self.assertIn(token, condition)
        self.assertNotIn("merge_request_event", condition)

    def test_publish_waits_for_both_packages_without_rebuilding(self):
        for arch in ("arm64", "x64"):
            job = self.config["publish-" + arch]
            needs = {item["job"]: item["artifacts"] for item in job["needs"]}
            self.assertEqual(needs, {"package-arm64": arch == "arm64", "package-x64": arch == "x64"})
            self.assertEqual(job["resource_group"], "mskdsp-lower-ci-" + arch)
            self.assertEqual(job["environment"]["name"], "ci")
        scripts = "\n".join(self.config[".publish"]["script"])
        self.assertIn("init_platform_proto.py", scripts)
        self.assertIn("platform_client.py", scripts)
        self.assertNotIn("build_lower.sh", scripts)

    def test_build_cache_architectures_and_reports_are_isolated(self):
        job = self.config[".build-test"]
        self.assertEqual(job["tags"], ["lower-ci"])
        self.assertEqual(job["environment"]["name"], "ci-build")
        self.assertEqual(job["artifacts"]["when"], "always")
        self.assertEqual(job["cache"]["when"], "always")
        self.assertIn("$ARCH", job["cache"]["key"]["prefix"])
        self.assertIn("junit", job["artifacts"]["reports"])
        self.assertNotEqual(self.config["build-test-arm64"]["image"], self.config["build-test-x64"]["image"])
        script = (ROOT / "script/gitlab/build_lower.sh").read_text(encoding="utf-8")
        self.assertIn("--parallel 8", script)
        self.assertIn("--parallel 2", script)
        self.assertIn("--no-tests=error", script)
        self.assertIn("type=docker", script)
        self.assertIn("X_VCPKG_ASSET_SOURCES=", script)
        self.assertIn("x-block-origin", script)
        self.assertIn("{url} {sha512} {dst};x-block-origin", script)
        self.assertNotIn("docker.sock", script)
        self.assertNotIn("check_iec61850_package", script)


if __name__ == "__main__":
    unittest.main()
