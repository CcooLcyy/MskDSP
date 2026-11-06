"""检查托管双架构流水线的并行、失败门禁和发布权限边界。"""
from pathlib import Path
import unittest

import yaml


ROOT = Path(__file__).resolve().parents[2]


class HostedActionsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workflow = yaml.safe_load((ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8"))
        cls.jobs = cls.workflow["jobs"]

    # 两个原生架构独立调度，一边失败不得取消另一边的验证。
    def test_architectures_build_and_test_independently(self):
        build = self.jobs["build"]
        self.assertNotIn("needs", build)
        self.assertIs(build["strategy"]["fail-fast"], False)
        self.assertNotEqual(build["strategy"].get("max-parallel"), 1)
        runners = {row["arch"]: row["runner"] for row in build["strategy"]["matrix"]["include"]}
        self.assertEqual(runners, {"x64": "ubuntu-24.04", "arm64": "ubuntu-24.04-arm"})
        scripts = "\n".join(step.get("run", "") for step in build["steps"])
        self.assertIn('build_lower.sh build "${ARCH}"', scripts)
        self.assertIn('build_lower.sh test "${ARCH}"', scripts)
        self.assertNotIn("cross-build", scripts)
        self.assertIn("tar ", scripts)

    # 只有两架构编译测试和独立检查全部成功后才打包，发布同时依赖双架构包。
    def test_package_and_publish_keep_all_success_gates(self):
        package = self.jobs["package"]
        self.assertEqual(set(package["needs"]), {"build", "checks"})
        self.assertEqual(package["env"]["LOWER_PACKAGE_BACKEND"], "local-docker")
        for name in ("publish-ci", "publish-stable"):
            job = self.jobs[name]
            self.assertEqual(set(job["needs"]), {"package", "checks"})
            self.assertNotIn("always()", str(job.get("if", "")))
            self.assertIs(job["concurrency"]["cancel-in-progress"], False)
            self.assertEqual(job["environment"], "ci")

    # 正式包仅保护标签发布 ARM64，维护线和历史 Beta 不覆盖主干 CI。
    def test_release_routes_and_secret_boundaries(self):
        events = self.workflow.get("on", self.workflow.get(True))
        self.assertEqual(set(events), {"push", "workflow_dispatch"})
        self.assertNotIn("beta/**", events["push"]["branches"])
        ci = self.jobs["publish-ci"]
        self.assertIn("github.ref_type == 'branch'", ci["if"])
        self.assertIn("github.ref_name == 'master'", ci["if"])
        self.assertIn("github.ref_name == 'main'", ci["if"])
        self.assertNotIn("release/", ci["if"])
        stable = self.jobs["publish-stable"]
        self.assertIn("github.ref_type == 'tag'", stable["if"])
        self.assertIn("github.ref_protected", stable["if"])
        self.assertEqual(stable["env"]["LOWER_JOB_ARCH"], "arm64")
        self.assertEqual(self.workflow["permissions"], {"contents": "read"})
        self.assertNotIn("concurrency", self.workflow)
        for name in ("build", "checks", "package"):
            self.assertNotIn("PLATFORM_UPLOAD_TOKEN", str(self.jobs[name]))

    # 日志即使失败也保留，不能用继续执行标志把测试失败改判成功。
    def test_failure_evidence_is_retained_without_masking_failures(self):
        for name in ("build", "checks", "package"):
            job = self.jobs[name]
            self.assertNotIn("continue-on-error", job)
            self.assertTrue(any(step.get("if") == "always()" and "upload-artifact" in step.get("uses", "")
                                for step in job["steps"]))
            for step in job["steps"]:
                self.assertNotIn("continue-on-error", step)


if __name__ == "__main__":
    unittest.main()
