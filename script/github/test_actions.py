"""检查托管双架构流水线的并行、失败门禁和发布权限边界。"""
import os
from pathlib import Path
import subprocess
import tempfile
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

    # 单架构补发优先使用本次归档，缺失时只使用同运行、同架构的历史归档。
    def test_ci_publish_selects_frozen_package_on_job_rerun(self):
        job = self.jobs["publish-ci"]
        select_package = next(step for step in job["steps"] if step.get("id") == "package")
        download = next(step for step in job["steps"] if "download-artifact" in step.get("uses", ""))
        publish = next(step for step in job["steps"] if step.get("run") == "bash script/github/publish.sh")
        self.assertEqual(download["with"]["name"], "${{ steps.package.outputs.name }}")
        self.assertEqual(download["with"]["run-id"], "${{ github.run_id }}")
        self.assertEqual(publish["env"]["LOWER_PACKAGE_RUN_ATTEMPT"], "${{ steps.package.outputs.attempt }}")
        self.assertEqual(publish["env"]["PLATFORM_STREAM_UPLOAD_TIMEOUT"], "300")
        with tempfile.TemporaryDirectory() as directory:
            gh = Path(directory) / "gh"
            gh.write_text('#!/usr/bin/env bash\nprintf "%s\\n" "$TEST_ARTIFACT_NAMES"\n', encoding="utf-8")
            gh.chmod(0o755)
            output = Path(directory) / "output"
            other_packages = ["lower-package-arm64-100-2", "lower-package-x64-99-2", "lower-package-x64-100-3"]
            for available, expected in (([1, 2], 2), ([1], 1), ([], None)):
                with self.subTest(available=available):
                    output.write_text("", encoding="utf-8")
                    names = other_packages + [f"lower-package-x64-100-{attempt}" for attempt in available]
                    environment = os.environ | {
                        "PATH": directory + os.pathsep + os.environ["PATH"], "GITHUB_REPOSITORY": "owner/repo",
                        "GITHUB_RUN_ID": "100", "GITHUB_RUN_ATTEMPT": "2", "LOWER_JOB_ARCH": "x64",
                        "GITHUB_OUTPUT": str(output), "TEST_ARTIFACT_NAMES": "\n".join(names),
                    }
                    result = subprocess.run(["bash", "-c", select_package["run"]], env=environment,
                                            capture_output=True, text=True, timeout=5)
                    if expected is None:
                        self.assertNotEqual(result.returncode, 0)
                        self.assertEqual(output.read_text(encoding="utf-8"), "")
                    else:
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertEqual(output.read_text(encoding="utf-8"),
                                         f"name=lower-package-x64-100-{expected}\nattempt={expected}\n")

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
