"""验证下位机流水线的触发、双架构门禁及隔离约束。"""
from pathlib import Path
import unittest
import yaml

ROOT = Path(__file__).resolve().parents[2]


def merge_job(base, override):
    result = dict(base)
    for key, value in override.items():
        result[key] = merge_job(result[key], value) if isinstance(result.get(key), dict) and isinstance(value, dict) else value
    return result


class LowerCiTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.config = yaml.safe_load((ROOT / ".gitlab/ci/ci.yml").read_text(encoding="utf-8"))

    def job(self, name):
        job = self.config[name]
        return merge_job(self.job(job["extends"]), job) if "extends" in job else job

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
        job = self.job("build-test-x64")
        self.assertEqual(job["tags"], ["lower-ci"])
        self.assertEqual(job["environment"]["name"], "ci-build")
        self.assertEqual(job["artifacts"]["when"], "always")
        self.assertEqual(job["cache"], [])
        self.assertIn("junit", job["artifacts"]["reports"])
        script = (ROOT / "script/gitlab/build_lower.sh").read_text(encoding="utf-8")
        self.assertEqual(self.config["variables"]["CMAKE_BUILD_PARALLEL_LEVEL"], "6")
        self.assertEqual(self.config["variables"]["VCPKG_MAX_CONCURRENCY"], "6")
        self.assertIn("--parallel 6", script)
        self.assertIn("VCPKG_MAX_CONCURRENCY=6", script)
        self.assertIn("--parallel 2", script)
        self.assertIn("--no-tests=error", script)
        self.assertIn("type=docker", script)
        self.assertIn("X_VCPKG_ASSET_SOURCES=", script)
        self.assertIn("x-block-origin", script)
        self.assertIn("{url} {sha512} {dst};x-block-origin", script)
        self.assertNotIn("docker.sock", script)
        self.assertNotIn("check_iec61850_package", script)

    def test_arm_compiler_is_native_x64_with_separate_cross_cache(self):
        job = self.job("build-arm64")
        self.assertEqual(job["image"]["docker"]["platform"], "linux/amd64")
        self.assertEqual(job["image"]["name"], "mskdsp-lower-cross-tools:ubuntu24.04-gcc14-v1")
        self.assertIn('cross-build "${ARCH}"', "\n".join(job["script"]))
        self.assertEqual(job["variables"]["ARCH"], "arm64")
        self.assertEqual(job["variables"]["CACHE_ARCH"], "arm64-cross")
        self.assertEqual(job["cache"], [])
        self.assertIn("build-arm64/", job["artifacts"]["paths"])
        self.assertNotIn("reports", job["artifacts"])
        self.assertNotIn("build-test-arm64", self.config)

    def test_builds_use_persistent_cache_and_architecture_resource_groups(self):
        groups = []
        for name, profile in (("build-test-x64", "x64"), ("build-arm64", "arm64-cross")):
            with self.subTest(job=name):
                job = self.job(name)
                self.assertEqual(job["cache"], [])
                self.assertEqual(job["variables"]["LOWER_CACHE_ROOT"], "/ci-cache/mskdsp/gcc14-v1")
                self.assertEqual(job["variables"]["LOWER_CACHE_MOUNT"], "/ci-cache")
                self.assertEqual(job["variables"]["CACHE_ARCH"], profile)
                self.assertEqual(job["resource_group"], "mskdsp-lower-build-$CACHE_ARCH")
                groups.append(job["resource_group"].replace("$CACHE_ARCH", profile))
                stats = "\n".join(job["after_script"])
                self.assertIn("$LOWER_CACHE_ROOT/ccache/$CACHE_ARCH", stats)
                self.assertNotIn("$CI_PROJECT_DIR/.cache", stats)
        self.assertEqual(len(set(groups)), 2)
        self.assertNotIn("cache", self.config["build-arm64"])

    def test_persistent_cache_stays_outside_artifacts_and_downstream_jobs(self):
        for name in ("build-test-x64", "build-arm64", "test-arm64", "package-arm64", "package-x64"):
            with self.subTest(job=name):
                job = self.job(name)
                paths = job["artifacts"]["paths"]
                self.assertFalse(any("ci-cache" in path or ".cache" in path or "LOWER_CACHE" in path
                                     for path in paths))
                self.assertFalse(any(path in (".", "./", "/") for path in paths))
                if name not in ("build-test-x64", "build-arm64"):
                    self.assertNotIn("LOWER_CACHE_MOUNT", job.get("variables", {}))
                    self.assertNotIn("resource_group", job)
        self.assertIn("build-arm64/", self.job("build-arm64")["artifacts"]["paths"])
        self.assertEqual(self.job("test-arm64")["needs"], [{"job": "build-arm64", "artifacts": True}])

    def test_arm_tests_execute_under_qemu_and_gate_packaging(self):
        job = self.job("test-arm64")
        self.assertEqual(job["image"]["docker"]["platform"], "linux/arm64")
        self.assertEqual(job["image"]["name"], "mskdsp-ci-tools:ubuntu24.04-arm64")
        self.assertIn('test "${ARCH}"', "\n".join(job["script"]))
        self.assertEqual(job["needs"], [{"job": "build-arm64", "artifacts": True}])
        self.assertEqual(job["artifacts"]["reports"]["junit"], "logs/arm64/ctest.xml")
        self.assertEqual(self.job("package-arm64")["needs"], [{"job": "test-arm64", "artifacts": True}])

    def test_arm_build_artifacts_only_exclude_linked_object_files(self):
        # 编译结束后只省略对象文件，保留测试程序、运行依赖、符号和 CTest 目录。
        artifacts = self.job("build-arm64")["artifacts"]
        self.assertEqual(artifacts.get("exclude", []), ["build-arm64/**/*.o"])
        self.assertEqual(artifacts["paths"],
                         ["package/", "release/arm64/", "logs/arm64/", "build-arm64/"])
        self.assertEqual(artifacts["when"], "always")

    def test_arm_test_artifacts_preserve_symbols_archive_and_diagnostics(self):
        # 原始符号只在测试完成并生成符号归档后省略；符号归档、失败日志和报告仍传递。
        artifacts = self.job("test-arm64")["artifacts"]
        self.assertEqual(artifacts.get("exclude", []), ["package/debug/**"])
        self.assertEqual(artifacts["paths"],
                         ["package/", "release/arm64/", "logs/arm64/", "build-arm64/Testing/"])
        self.assertEqual(artifacts["when"], "always")
        self.assertEqual(artifacts["reports"]["junit"], "logs/arm64/ctest.xml")

    def test_artifact_exclusions_do_not_spread_to_other_jobs(self):
        # 精简规则必须局限于两个 ARM64 作业，避免继承影响其他架构或发布包。
        for name in (".build", ".build-test", "build-test-x64", ".package", "package-arm64", "package-x64"):
            with self.subTest(job=name):
                self.assertNotIn("exclude", self.job(name)["artifacts"])


if __name__ == "__main__":
    unittest.main()
