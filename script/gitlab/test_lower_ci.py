"""验证下位机流水线的触发、双架构门禁及隔离约束。"""
from pathlib import Path
import unittest
import tomllib
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
            self.assertEqual(needs, {"package-arm64": arch == "arm64", "package-x64": arch == "x64",
                                     "test-publish-contract": False})
            self.assertEqual(job["resource_group"], "mskdsp-lower-ci-" + arch)
            self.assertEqual(job["environment"]["name"], "ci")
        scripts = "\n".join(self.config[".publish"]["script"])
        self.assertIn("init_platform_proto.py", scripts)
        self.assertIn("platform_client.py", scripts)
        self.assertNotIn("build_lower.sh", scripts)

    # Beta 只能发布 ARM64，仍等待双架构打包和发布契约测试。
    def test_beta_branch_has_isolated_arm64_publication_and_full_gates(self):
        self.assertIn("beta", self.config["workflow"]["rules"][0]["if"])
        beta = self.config["publish-beta-arm64"]
        self.assertEqual(beta["variables"]["ARCH"], "arm64")
        self.assertEqual(beta["resource_group"], "mskdsp-lower-beta-arm64")
        self.assertEqual(beta["environment"]["name"], "ci")
        self.assertIn("beta", beta["rules"][0]["if"])
        self.assertEqual({item["job"] for item in beta["needs"]},
                         {"package-arm64", "package-x64", "test-publish-contract"})
        for arch in ("arm64", "x64"):
            self.assertIn('"master"', self.config["publish-" + arch]["rules"][0]["if"])
            self.assertNotIn("beta", self.config["publish-" + arch]["rules"][0]["if"])
        self.assertIn("test-x64", {item["job"] for item in self.config["package-arm64"]["needs"]})
        contract = self.job("test-publish-contract")
        self.assertIn("test_platform_client.py", "\n".join(contract["script"]))

    def test_build_cache_architectures_and_reports_are_isolated(self):
        job = self.job("build-x64")
        self.assertEqual(job["tags"], ["lower-ci"])
        self.assertEqual(job["environment"]["name"], "ci-build")
        self.assertEqual(job["artifacts"]["when"], "always")
        self.assertEqual(job["cache"], [])
        self.assertNotIn("reports", job["artifacts"])
        self.assertIn("junit", self.job("test-x64")["artifacts"]["reports"])
        script = (ROOT / "script/gitlab/build_lower.sh").read_text(encoding="utf-8")
        self.assertEqual(self.config["variables"]["CMAKE_BUILD_PARALLEL_LEVEL"], "4")
        self.assertEqual(self.config["variables"]["VCPKG_MAX_CONCURRENCY"], "4")
        self.assertEqual(self.config["variables"]["CCACHE_MAXSIZE"], "8G")
        self.assertIn("--parallel 2", script)
        self.assertIn("--no-tests=error", script)
        self.assertIn("type=docker", script)
        self.assertIn("X_VCPKG_ASSET_SOURCES=", script)
        self.assertIn("x-block-origin", script)
        self.assertIn("{url} {sha512} {dst};x-block-origin", script)
        self.assertNotIn("docker.sock", script)
        self.assertNotIn("check_iec61850_package", script)

    # Stable 仅保护标签可触发 ARM64 发布，完整测试与双架构门禁保留。
    def test_stable_tag_job_is_protected_and_waits_for_all_gates(self):
        rule = self.config["workflow"]["rules"][1]["if"]
        self.assertIn('CI_COMMIT_REF_PROTECTED == "true"', rule)
        self.assertIn("CI_COMMIT_TAG =~", rule)
        self.assertIn('"push"', rule)
        self.assertIn('"web"', rule)
        stable = self.config["publish-stable-arm64"]
        self.assertEqual(stable["variables"]["ARCH"], "arm64")
        self.assertEqual(stable["environment"]["name"], "ci")
        self.assertEqual(stable["resource_group"], "mskdsp-lower-stable-arm64")
        self.assertIn('CI_COMMIT_REF_PROTECTED == "true"', stable["rules"][0]["if"])
        self.assertEqual({item["job"] for item in stable["needs"]},
                         {"package-arm64", "package-x64", "test-publish-contract"})
        self.assertNotIn("publish-stable-x64", self.config)

    def test_shared_ram_workspace_preserves_cross_job_path_and_cleanup_after_artifacts(self):
        # 同流水线同架构路径一致，不同流水线及架构隔离，清理由 Runner 在上传后执行。
        variables = self.config["variables"]
        self.assertEqual(variables["GIT_CLONE_PATH"], "/builds/lower/$CI_PROJECT_PATH/$CI_PIPELINE_ID/$ARCH")
        self.assertEqual(variables["GIT_STRATEGY"], "clone")
        self.assertEqual(variables["FF_ENABLE_JOB_CLEANUP"], "true")
        for name in ("build-x64", "build-arm64", "test-x64"):
            scripts = "\n".join(self.job(name).get("after_script", []))
            self.assertNotIn("rm -rf", scripts)
        for name in ("build-x64", "build-arm64", "test-x64", "package-x64", "package-arm64", "publish-x64", "publish-arm64"):
            self.assertIn(self.job(name)["variables"]["ARCH"], ("x64", "arm64"))
        self.assertEqual(self.job("build-x64")["variables"]["ARCH"], self.job("test-x64")["variables"]["ARCH"])

    def test_runner_mounts_shared_volume_and_persistent_cache_with_bounded_resources(self):
        # 作业和 helper 共用命名卷，持久缓存留在磁盘且下位机允许两个隔离作业。
        config = tomllib.loads((ROOT / "docker/gitlab-runner/config.template.toml").read_text(encoding="utf-8"))
        self.assertEqual(config["concurrent"], 2)
        runner = config["runners"][0]
        self.assertEqual(runner["limit"], 2)
        self.assertEqual(runner["request_concurrency"], 2)
        self.assertTrue(runner["custom_build_dir"]["enabled"])
        self.assertEqual(runner["pre_build_script"], ". /opt/ci-ram/pre-build.sh")
        environment = dict(item.split("=", 1) for item in runner["environment"])
        self.assertEqual(environment["GIT_CLONE_PATH"], "/builds/lower/$CI_PROJECT_PATH/$CI_PIPELINE_ID/$ARCH")
        self.assertEqual(environment["GIT_STRATEGY"], "clone")
        self.assertEqual(environment["FF_ENABLE_JOB_CLEANUP"], "true")
        docker = runner["docker"]
        self.assertEqual(docker["cpus"], "4")
        self.assertEqual(docker["memory"], "24g")
        self.assertEqual(docker["memory_swap"], "24g")
        self.assertFalse(docker["privileged"])
        self.assertIn("mskdsp-ci-ram:/builds", docker["volumes"])
        self.assertIn("/home/daniel/ci-ram/tools:/opt/ci-ram:ro", docker["volumes"])
        self.assertIn("/home/daniel/mskdsp-lower-ci/cache-direct:/ci-cache", docker["volumes"])

    def test_runner_lease_keeps_ram_mounted_between_helper_and_build_stages(self):
        # 服务跨越检出、主脚本及归档阶段持有共享卷，避免阶段容器停止后丢失源码。
        config = tomllib.loads((ROOT / "docker/gitlab-runner/config.template.toml").read_text(encoding="utf-8"))
        docker = config["runners"][0]["docker"]
        self.assertEqual(docker["services"], [{"name": "alpine:3.20", "alias": "ci-ram-lease",
                                             "entrypoint": ["/bin/sh"],
                                             "command": ["-c", "exec sleep 2147483647"]}])
        self.assertEqual(docker["service_memory"], "64m")
        self.assertEqual(docker["service_memory_swap"], "64m")
        self.assertEqual(docker["service_cpus"], "0.1")
        self.assertEqual(docker["wait_for_services_timeout"], -1)

    def test_arm_compiler_is_native_x64_with_separate_cross_cache(self):
        job = self.job("build-arm64")
        self.assertEqual(job["image"]["docker"]["platform"], "linux/amd64")
        self.assertEqual(job["image"]["name"], "mskdsp-lower-cross-tools:ubuntu24.04-gcc14-v1")
        self.assertIn('cross-build "${ARCH}"', "\n".join(job["script"]))
        self.assertEqual(job["variables"]["ARCH"], "arm64")
        self.assertEqual(job["variables"]["CACHE_ARCH"], "arm64-cross")
        self.assertEqual(job["cache"], [])
        self.assertNotIn("build-arm64/", job["artifacts"]["paths"])
        self.assertNotIn("reports", job["artifacts"])
        self.assertNotIn("build-test-arm64", self.config)

    def test_builds_use_persistent_cache_and_architecture_resource_groups(self):
        groups = []
        for name, profile in (("build-x64", "x64"), ("build-arm64", "arm64-cross")):
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
        for name in ("build-x64", "build-arm64", "test-x64", "package-arm64", "package-x64"):
            with self.subTest(job=name):
                job = self.job(name)
                paths = job["artifacts"]["paths"]
                self.assertFalse(any("ci-cache" in path or ".cache" in path or "LOWER_CACHE" in path
                                     for path in paths))
                self.assertFalse(any(path in (".", "./", "/") for path in paths))
                if name not in ("build-x64", "build-arm64"):
                    self.assertNotIn("LOWER_CACHE_MOUNT", job.get("variables", {}))
                    self.assertNotIn("resource_group", job)
        self.assertNotIn("build-arm64/", self.job("build-arm64")["artifacts"]["paths"])

    def test_only_x64_tests_gate_both_architecture_packages(self):
        self.assertEqual(self.config["stages"], ["build", "test", "package", "publish"])
        self.assertNotIn("build-test-x64", self.config)
        self.assertNotIn("test-arm64", self.config)
        self.assertNotIn("test-arm64", self.config["stages"])
        self.assertEqual(self.job("package-arm64")["needs"], [
            {"job": "test-network-runtime", "artifacts": False},
            {"job": "build-arm64", "artifacts": True},
            {"job": "test-x64", "artifacts": False},
        ])
        self.assertEqual(self.job("package-x64")["needs"], [
            {"job": "test-network-runtime", "artifacts": False},
            {"job": "test-x64", "artifacts": True},
        ])
        for name in ("build-x64", "build-arm64", "test-x64", "package-arm64", "package-x64"):
            with self.subTest(job=name):
                job = self.job(name)
                self.assertFalse(job.get("allow_failure", False))
                self.assertEqual(job.get("when", "on_success"), "on_success")
                for need in job.get("needs", []):
                    self.assertFalse(need.get("optional", False))

    def test_x64_tests_run_in_separate_native_job_with_build_artifacts(self):
        build = self.job("build-x64")
        tested = self.job("test-x64")
        self.assertEqual(build["stage"], "build")
        self.assertIn('build "$ARCH"', "\n".join(build["script"]))
        self.assertIn("build-x64/", build["artifacts"]["paths"])
        self.assertEqual(build["artifacts"]["exclude"], ["build-x64/**/*.o", "package/debug/**"])
        self.assertEqual(tested["stage"], "test")
        self.assertEqual(tested["image"]["docker"]["platform"], "linux/amd64")
        self.assertEqual(tested["image"]["name"], "mskdsp-ci-tools:ubuntu24.04-x64")
        self.assertIn('test "${ARCH}"', "\n".join(tested["script"]))
        self.assertEqual(tested["needs"], [{"job": "build-x64", "artifacts": True}])
        self.assertEqual(tested["artifacts"]["paths"],
                         ["package/", "release/x64/", "logs/x64/", "build-x64/Testing/"])
        self.assertEqual(tested["artifacts"]["reports"]["junit"], "logs/x64/ctest.xml")
        self.assertEqual(tested["artifacts"]["when"], "always")
        self.assertEqual(tested["artifacts"]["exclude"], ["package/debug/**"])

    def test_arm_build_artifacts_keep_runtime_symbols_and_diagnostics(self):
        # 交叉编译直接交付运行文件和符号归档，不再传递测试目录及重复符号。
        artifacts = self.job("build-arm64")["artifacts"]
        self.assertEqual(artifacts.get("exclude", []), ["package/debug/**"])
        self.assertEqual(artifacts["paths"],
                         ["package/", "release/arm64/", "symbols/arm64/", "logs/arm64/"])
        self.assertEqual(artifacts["when"], "always")
        self.assertNotIn("reports", artifacts)

    def test_debug_artifact_exclusions_do_not_spread_to_package_jobs(self):
        # 原始符号只在构建及测试交接排除，不影响安装包或诊断日志。
        for name in (".build", ".package", "package-arm64", "package-x64"):
            with self.subTest(job=name):
                self.assertNotIn("exclude", self.job(name)["artifacts"])

    def test_symbols_remain_in_build_artifacts_for_thirty_days_without_test_duplicates(self):
        # 独立符号保留在原构建归档，不再重复经过测试与打包归档传递。
        for arch in ("arm64", "x64"):
            with self.subTest(arch=arch):
                artifacts = self.job("build-" + arch)["artifacts"]
                self.assertEqual(artifacts["expire_in"], "30 days")
                self.assertIn("symbols/" + arch + "/", artifacts["paths"])
                self.assertIn("package/debug/**", artifacts["exclude"])
                self.assertEqual(artifacts["when"], "always")
        for name in ("test-x64", "package-arm64", "package-x64"):
            with self.subTest(job=name):
                self.assertFalse(any(path.startswith("symbols/") for path in self.job(name)["artifacts"]["paths"]))

    def test_package_artifacts_only_transfer_installer_frozen_metadata_and_current_log(self):
        # 发布只下载必需安装包与冻结校验信息，失败诊断保留本次打包日志。
        expected = ["release/$ARCH/mskdsp-*", "release/$ARCH/metadata.json",
                    "release/$ARCH/SHA256SUMS", "logs/$ARCH/package.log"]
        for arch in ("arm64", "x64"):
            with self.subTest(arch=arch):
                artifacts = self.job("package-" + arch)["artifacts"]
                self.assertEqual(artifacts["paths"], expected)
                self.assertEqual(artifacts["when"], "always")
                self.assertEqual(artifacts["expire_in"], "30 days")

    def test_shallow_clone_is_limited_to_publish_and_preserves_build_history(self):
        # 仅发布浅检出，构建历史与 vcpkg 基线验证维持完整历史契约。
        self.assertEqual(self.config["variables"]["GIT_DEPTH"], "0")
        for arch in ("arm64", "x64"):
            self.assertEqual(self.job("publish-" + arch)["variables"]["GIT_DEPTH"], "1")
        for name in (".build", "build-arm64", "build-x64", "test-x64", ".package", "package-arm64", "package-x64"):
            with self.subTest(job=name):
                self.assertNotIn("GIT_DEPTH", self.job(name).get("variables", {}))


if __name__ == "__main__":
    unittest.main()
