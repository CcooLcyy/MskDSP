"""通过可控命令替身验证真实 Bash 编排、失败门禁及产物传递。"""
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
MOCK = r'''#!/usr/bin/env python3
import hashlib, io, json, os
from pathlib import Path
import sys, tarfile
name = Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ["COMMAND_LOG"], "a") as log:
    log.write(json.dumps([name] + args) + "\n")
if name == "uname":
    print(os.environ.get("TOOL_ARCH", "aarch64" if os.environ["ARCH"] == "arm64" else "x86_64"))
elif name in ("cc", "c++", "gcc-14", "g++-14", "aarch64-linux-gnu-gcc-14", "aarch64-linux-gnu-g++-14"):
    print("aarch64-linux-gnu" if name.startswith("aarch64") and "-dumpmachine" in args else
          "x86_64-linux-gnu" if "-dumpmachine" in args else "14.2.0")
elif name == "git" and "submodule" in args:
    expected_key = "http." + os.environ["CI_SERVER_URL"].rstrip("/") + "/.extraHeader"
    if (os.environ.get("GIT_CONFIG_COUNT") != "1"
            or os.environ.get("GIT_CONFIG_KEY_0") != expected_key
            or not os.environ.get("GIT_CONFIG_VALUE_0", "").startswith("Authorization: Basic ")):
        sys.exit("协议子模块必须直接使用 GitLab 临时认证头，禁止旧地址重写")
    if os.environ.get("FAIL_SUBMODULE_STAGE") in args:
        print(os.environ["CI_JOB_TOKEN"])
        sys.exit(27)
elif name == "git" and any(argument.startswith("--batch-check") for argument in args):
    for expression in sys.stdin.read().splitlines():
        print("a" * 40 + " tree")
elif name == "git" and "--is-shallow-repository" in args:
    print(os.environ.get("SHALLOW_SOURCE", "false"))
elif name == "git" and "cat-file" in args and "-e" in args:
    sys.exit(1 if os.environ.get("MISSING_BASELINE") == "1" else 0)
elif name == "git" and "rev-parse" in args:
    print(json.load(open("vcpkg-configuration.json"))["default-registry"]["baseline"])
elif name == "cmake" and "--install" in args:
    for child in ("module", "lib", "conf"):
        Path("package", child).mkdir(parents=True, exist_ok=True)
    Path("package/MskDSP").write_bytes(b"binary")
    Path("package/temporary_test").write_bytes(b"test")
    if os.environ["ARCH"] == "arm64" and os.environ.get("MISSING_SYMBOLS") != "1":
        Path("package/debug").mkdir()
        Path("package/debug/MskDSP.debug").write_bytes(b"symbols")
elif name == "cmake" and "-B" in args:
    cache_keys = ("VCPKG_ROOT", "VCPKG_DOWNLOADS", "VCPKG_DEFAULT_BINARY_CACHE", "VCPKG_BINARY_SOURCES", "CCACHE_DIR")
    with open(os.environ["COMMAND_LOG"], "a") as log:
        log.write(json.dumps(["cache-environment", {key: os.environ.get(key) for key in cache_keys}]) + "\n")
        log.write(json.dumps(["build-resource-environment", {key: os.environ.get(key) for key in
                              ("CMAKE_BUILD_PARALLEL_LEVEL", "VCPKG_MAX_CONCURRENCY")}]) + "\n")
        log.write(json.dumps(["compiler-cache-environment", {key: os.environ.get(key) for key in
                              ("CCACHE_BASEDIR", "CCACHE_MAXSIZE", "CCACHE_COMPILERCHECK")}]) + "\n")
    if os.environ.get("EXPECT_CACHE_SENTINEL"):
        if Path(os.environ["VCPKG_ROOT"], "source-sentinel").read_text() != os.environ["EXPECT_CACHE_SENTINEL"]:
            sys.exit("未复用持久 vcpkg 源码")
        if Path(os.environ["CCACHE_DIR"], "ccache-sentinel").read_text() != os.environ["EXPECT_CACHE_SENTINEL"]:
            sys.exit("未复用持久 ccache")
    if os.environ.get("GIT_CONFIG_COUNT") != "1" or os.environ.get("GIT_CONFIG_VALUE_0") != "https://github.com/microsoft/vcpkg":
        sys.exit("vcpkg registry 未使用进程级本地重定向")
    if os.environ.get("GIT_CONFIG_KEY_0") != "url.file://" + os.environ["VCPKG_ROOT"] + "/.insteadOf":
        sys.exit("本地 registry 重定向地址不一致")
    if os.environ.get("CHECK_MAKE_ENV") == "1":
        # 对应 baseline vcpkg_configure_make 的规则：已有 CC 环境变量优先于目标工具链。
        compiler = os.environ.get("CC", "aarch64-linux-gnu-gcc-14")
        with open(os.environ["COMMAND_LOG"], "a") as log:
            log.write(json.dumps(["make-target-compiler", compiler]) + "\n")
        if compiler != "aarch64-linux-gnu-gcc-14" or "CXX" in os.environ:
            sys.exit("非 CMake 目标端继承了主机编译器环境")
    directory = Path(args[args.index("-B") + 1])
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "CTestTestfile.cmake").write_text("# 完整测试目录")
elif name == "ctest":
    Path(args[args.index("--output-junit") + 1]).write_text('<testsuites tests="2"/>')
    sys.exit(int(os.environ.get("TEST_FAILURE", "0")))
elif name == "buildctl":
    output = args[args.index("--output") + 1]
    values = dict(part.split("=", 1) for part in output.split(","))
    config = json.dumps({"os": "linux", "architecture": "arm64" if os.environ["ARCH"] == "arm64" else "amd64"}).encode()
    filename = hashlib.sha256(config).hexdigest() + ".json"
    manifest = json.dumps([{"Config": filename, "RepoTags": [values["name"]]}]).encode()
    with tarfile.open(values["dest"], "w") as archive:
        for name, value in ((filename, config), ("manifest.json", manifest)):
            member = tarfile.TarInfo(name)
            member.size = len(value)
            archive.addfile(member, io.BytesIO(value))
'''


@unittest.skipUnless(os.name == "posix", "真实Git子模块回归在Linux/WSL执行")
class ProtobufSubmoduleTest(unittest.TestCase):
    def test_unregistered_existing_submodule_refreshes_old_origin_before_fetch(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            origin, parent = base / "protocol-origin", base / "parent"
            environment = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL="/dev/null",
                               GIT_ALLOW_PROTOCOL="file", GIT_TERMINAL_PROMPT="0")

            def git(directory, *arguments, check=True):
                result = subprocess.run(["git", "-C", str(directory), *arguments], env=environment,
                                        capture_output=True, text=True, timeout=20)
                if check:
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                return result

            for directory in (origin, parent):
                directory.mkdir()
                git(directory, "init", "-q")
                git(directory, "config", "user.name", "Submodule fixture")
                git(directory, "config", "user.email", "submodule-test@example.invalid")
            (origin / "schema.proto").write_text("旧协议", encoding="utf-8")
            git(origin, "add", "schema.proto")
            git(origin, "commit", "-qm", "fixture-old")
            git(parent, "submodule", "add", str(origin), "protobuf")
            (origin / "schema.proto").write_text("新增协议", encoding="utf-8")
            git(origin, "commit", "-qam", "fixture-new")
            expected = git(origin, "rev-parse", "HEAD").stdout.strip()
            git(parent, "update-index", "--cacheinfo", "160000," + expected + ",protobuf")
            git(parent, "commit", "-qm", "fixture-pin")
            stale_origin = str(base / "nonexistent-old-origin")
            git(parent / "protobuf", "remote", "set-url", "origin", stale_origin)
            git(parent, "config", "--remove-section", "submodule.protobuf")
            self.assertNotEqual(git(parent / "protobuf", "cat-file", "-e", expected, check=False).returncode, 0)
            self.assertNotEqual(git(parent, "config", "--get", "submodule.protobuf.url", check=False).returncode, 0)
            self.assertEqual(git(parent / "protobuf", "remote", "get-url", "origin").stdout.strip(), stale_origin)
            helper = parent / "script/gitlab/init_platform_proto.py"
            helper.parent.mkdir(parents=True)
            shutil.copyfile(ROOT / "script/gitlab/init_platform_proto.py", helper)
            script = (ROOT / "script/gitlab/build_lower.sh").read_text()
            function = "fetch_protobuf() {" + script.split("fetch_protobuf() {", 1)[1].split("\n}\n", 1)[0] + "\n}\n"
            result = subprocess.run(["bash", "-c", "set -euo pipefail\n" + function + "\nfetch_protobuf\n"],
                                    cwd=parent, env=dict(environment, CI_SERVER_URL="https://gitlab.example",
                                                         CI_JOB_TOKEN="local-test-token"),
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn("local-test-token", result.stdout + result.stderr)
            self.assertEqual(git(parent / "protobuf", "remote", "get-url", "origin").stdout.strip(), str(origin))
            self.assertEqual(git(parent / "protobuf", "rev-parse", "HEAD").stdout.strip(), expected)


@unittest.skipUnless(os.name == "posix", "Bash 编排契约测试在 Linux/WSL 执行")
class BuildLowerTest(unittest.TestCase):
    def prepare(self, arch):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        for filename in ("script/gitlab/build_lower.sh", "script/gitlab/lower_metadata.py",
                         "script/gitlab/download_asset.py", "script/gitlab/init_platform_proto.py", "script/make_exe.sh",
                         "script/workflow/write_lower_update_manifest.py", "Dockerfile", "VERSION", "vcpkg-configuration.json",
                         "cmake/toolchains/aarch64-linux-gcc14.cmake", "cmake/vcpkg-triplets/arm64-linux-dynamic-cross.cmake"):
            destination = root / filename
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / filename, destination)
        tools = root / "bin"
        tools.mkdir()
        for name in ("uname", "git", "cmake", "ctest", "ccache", "buildctl", "cc", "c++", "gcc-14", "g++-14",
                     "aarch64-linux-gnu-gcc-14", "aarch64-linux-gnu-g++-14"):
            executable = tools / name
            executable.write_text(MOCK, encoding="utf-8")
            executable.chmod(0o755)
        for profile in (arch, "arm64-cross"):
            vcpkg = root / ".cache/vcpkg" / profile / "source"
            (vcpkg / ".git").mkdir(parents=True, exist_ok=True)
            (vcpkg / "vcpkg").write_text("#!/bin/sh\nexit 0\n")
            (vcpkg / "vcpkg").chmod(0o755)
            (vcpkg / "versions").mkdir()
            (vcpkg / "versions/baseline.json").write_text('{"default": {}}')
        environment = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"], ARCH=arch,
                           CI_PIPELINE_SOURCE="push", CI_COMMIT_BRANCH="master", CI_COMMIT_SHA="a" * 40,
                           CI_PIPELINE_ID="100", CI_PIPELINE_IID="7", CI_JOB_ID="42",
                           CI_PROJECT_URL="https://gitlab.example/mskdsp/lower", CI_SERVER_URL="https://gitlab.example",
                           CI_JOB_TOKEN="secret-token-value", COMMAND_LOG=str(root / "commands.jsonl"),
                           BUILDKIT_HOST="tcp://buildkit:1234")
        for key in ("BUILDKIT_CA_B64", "BUILDKIT_CERT_B64", "BUILDKIT_KEY_B64"):
            environment[key] = base64.b64encode(b"secret-test-certificate").decode()
        for key in ("LOWER_CACHE_ROOT", "LOWER_CACHE_MOUNT", "EXPECT_CACHE_SENTINEL",
                    "CMAKE_BUILD_PARALLEL_LEVEL", "VCPKG_MAX_CONCURRENCY", "CCACHE_MAXSIZE", "CFLAGS", "CXXFLAGS"):
            environment.pop(key, None)
        return root, environment

    def run_phase(self, root, environment, phase):
        return subprocess.run(["bash", "script/gitlab/build_lower.sh", phase, environment["ARCH"]],
                              cwd=root, env=environment, capture_output=True, text=True, timeout=30)

    def test_build_parallelism_defaults_to_four_and_honors_independent_overrides(self):
        # 默认限制主工程与依赖并发，显式环境变量分别控制两类构建。
        for overrides, expected in (({}, ("4", "4")),
                                    ({"CMAKE_BUILD_PARALLEL_LEVEL": "2", "VCPKG_MAX_CONCURRENCY": "3"}, ("2", "3"))):
            with self.subTest(overrides=overrides):
                root, environment = self.prepare("x64")
                result = self.run_phase(root, environment | overrides, "build")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                commands = self.commands(root)
                build = next(command for command in commands if command[0] == "cmake" and "--build" in command)
                self.assertEqual(build[build.index("--parallel") + 1], expected[0])
                resources = next(command[1] for command in commands if command[0] == "build-resource-environment")
                self.assertEqual(resources, dict(CMAKE_BUILD_PARALLEL_LEVEL=expected[0],
                                                VCPKG_MAX_CONCURRENCY=expected[1]))

    def test_invalid_parallelism_fails_before_dependency_or_project_build(self):
        # 非正整数不得变成无界构建或传入编译器命令。
        for variable in ("CMAKE_BUILD_PARALLEL_LEVEL", "VCPKG_MAX_CONCURRENCY"):
            for value in ("0", "-1", "2.5", "unlimited", "2; echo unexpected"):
                with self.subTest(variable=variable, value=value):
                    root, environment = self.prepare("x64")
                    result = self.run_phase(root, environment | {variable: value}, "build")
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(variable, result.stdout + result.stderr)
                    self.assertFalse(any(command[0] in ("cmake", "git") for command in self.commands(root)))

    def test_ccache_capacity_defaults_to_eight_gigabytes_and_honors_override(self):
        # 持久磁盘上的 ccache 不受内存工作区容量影响，允许按宿主磁盘容量调整上限。
        for override, expected in (({}, "8G"), ({"CCACHE_MAXSIZE": "2G"}, "2G")):
            with self.subTest(override=override):
                root, environment = self.prepare("x64")
                result = self.run_phase(root, environment | override, "build")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                cache = next(command[1] for command in self.commands(root) if command[0] == "compiler-cache-environment")
                self.assertEqual(cache["CCACHE_MAXSIZE"], expected)
                self.assertEqual(cache["CCACHE_BASEDIR"], str(root))
                self.assertEqual(cache["CCACHE_COMPILERCHECK"], "content")

    def test_debug_prefix_is_stable_across_pipeline_roots_without_dropping_user_flags(self):
        # 真实命中另用编译器探针验收，此处验证两种架构的调试路径映射及参数传递。
        for arch, mode in (("x64", "build"), ("arm64", "cross-build")):
            with self.subTest(arch=arch):
                root, environment = self.prepare(arch)
                environment.update(TOOL_ARCH="x86_64", CFLAGS="-Wall", CXXFLAGS="-Wextra")
                result = self.run_phase(root, environment, mode)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                configure = next(command for command in self.commands(root) if command[0] == "cmake" and "-B" in command)
                prefix = "-fdebug-prefix-map=" + str(root) + "=/usr/src/mskdsp -ffile-prefix-map=" + str(root) + "=/usr/src/mskdsp"
                self.assertIn("-DCMAKE_C_FLAGS=-Wall " + prefix, configure)
                self.assertIn("-DCMAKE_CXX_FLAGS=-Wextra " + prefix, configure)

    def external_cache(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        return Path(temporary.name)

    def seed_cache(self, root, cache, profile, sentinel):
        source = cache / "vcpkg" / profile / "source"
        shutil.copytree(root / ".cache/vcpkg" / profile / "source", source)
        (source / "source-sentinel").write_text(sentinel)
        ccache = cache / "ccache" / profile
        ccache.mkdir(parents=True)
        (ccache / "ccache-sentinel").write_text(sentinel)

    def commands(self, root):
        filename = root / "commands.jsonl"
        return [json.loads(line) for line in filename.read_text().splitlines()] if filename.exists() else []

    def assert_cache_environment(self, root, cache, profile):
        environments = [command[1] for command in self.commands(root) if command[0] == "cache-environment"]
        self.assertEqual(len(environments), 1)
        expected = dict(VCPKG_ROOT=str(cache / "vcpkg" / profile / "source"),
                        VCPKG_DOWNLOADS=str(cache / "vcpkg" / profile / "downloads"),
                        VCPKG_DEFAULT_BINARY_CACHE=str(cache / "vcpkg" / profile / "archives"),
                        VCPKG_BINARY_SOURCES="clear;files," + str(cache / "vcpkg" / profile / "archives") + ",readwrite",
                        CCACHE_DIR=str(cache / "ccache" / profile))
        self.assertEqual(environments[0], expected)

    def test_external_cache_reuses_source_and_ccache_across_distinct_workspaces(self):
        cache = self.external_cache()
        first, first_environment = self.prepare("x64")
        self.seed_cache(first, cache, "x64", "两轮作业共用的缓存种子")
        workspaces = [(first, first_environment), self.prepare("x64")]
        self.assertNotEqual(workspaces[0][0], workspaces[1][0])
        for root, environment in workspaces:
            shutil.rmtree(root / ".cache")
            environment.update(LOWER_CACHE_ROOT=str(cache), EXPECT_CACHE_SENTINEL="两轮作业共用的缓存种子")
            result = self.run_phase(root, environment, "build")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assert_cache_environment(root, cache, "x64")
            self.assertFalse((root / ".cache").exists())
            self.assertIn(str(cache), result.stdout)
            self.assertIn("复用", result.stdout)
            self.assertFalse(any(command[0] == "git" and "init" in command and "submodule" not in command
                                 for command in self.commands(root)))
        self.assertEqual((cache / "ccache/x64/ccache-sentinel").read_text(), "两轮作业共用的缓存种子")

    def test_external_cache_keeps_x64_and_arm64_cross_state_separate(self):
        cache = self.external_cache()
        for arch, profile, phase in (("x64", "x64", "build"), ("arm64", "arm64-cross", "cross-build")):
            root, environment = self.prepare(arch)
            self.seed_cache(root, cache, profile, profile)
            shutil.rmtree(root / ".cache")
            environment.update(LOWER_CACHE_ROOT=str(cache), EXPECT_CACHE_SENTINEL=profile, TOOL_ARCH="x86_64")
            result = self.run_phase(root, environment, phase)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assert_cache_environment(root, cache, profile)
        for profile in ("x64", "arm64-cross"):
            self.assertEqual((cache / "vcpkg" / profile / "source/source-sentinel").read_text(), profile)
            self.assertEqual((cache / "ccache" / profile / "ccache-sentinel").read_text(), profile)

    def test_local_build_keeps_project_cache_when_override_is_absent(self):
        root, environment = self.prepare("x64")
        result = self.run_phase(root, environment, "build")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_cache_environment(root, root / ".cache", "x64")

    def test_real_mount_accepts_cache_inside_canonical_mount_boundary(self):
        if not os.path.ismount("/dev/shm"):
            self.skipTest("当前环境没有可验证的 /dev/shm 挂载")
        temporary = tempfile.TemporaryDirectory(dir="/dev/shm")
        self.addCleanup(temporary.cleanup)
        cache = Path(temporary.name)
        root, environment = self.prepare("x64")
        self.seed_cache(root, cache, "x64", "真实挂载缓存种子")
        shutil.rmtree(root / ".cache")
        environment.update(LOWER_CACHE_ROOT=str(cache), LOWER_CACHE_MOUNT="/dev/shm",
                           EXPECT_CACHE_SENTINEL="真实挂载缓存种子")
        result = self.run_phase(root, environment, "build")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_cache_environment(root, cache, "x64")

    def test_arm_package_does_not_require_build_cache_mount(self):
        root, environment = self.prepare("arm64")
        built = self.run_phase(root, environment | {"TOOL_ARCH": "x86_64"}, "cross-build")
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        environment.update(LOWER_CACHE_ROOT="无效的相对缓存", LOWER_CACHE_MOUNT="/不存在的构建挂载")
        packaged = self.run_phase(root, environment, "package")
        self.assertEqual(packaged.returncode, 0, packaged.stdout + packaged.stderr)

    def test_nonmounted_cache_directory_is_rejected_before_cmake(self):
        root, environment = self.prepare("x64")
        cache = self.external_cache()
        self.seed_cache(root, cache, "x64", "无挂载时不能编译")
        environment.update(LOWER_CACHE_ROOT=str(cache), LOWER_CACHE_MOUNT=str(cache))
        result = self.run_phase(root, environment, "build")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("挂载", result.stdout + result.stderr)
        self.assertFalse(any(command[0] == "cmake" for command in self.commands(root)))

    def test_relative_cache_root_is_rejected_before_cmake(self):
        root, environment = self.prepare("x64")
        environment["LOWER_CACHE_ROOT"] = ".cache"
        result = self.run_phase(root, environment, "build")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("绝对路径", result.stdout + result.stderr)
        self.assertFalse(any(command[0] == "cmake" for command in self.commands(root)))

    def test_mounted_cache_cannot_resolve_into_project_workspace(self):
        root, environment = self.prepare("x64")
        environment.update(LOWER_CACHE_ROOT=str(root / ".cache"), LOWER_CACHE_MOUNT="/")
        result = self.run_phase(root, environment, "build")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("工作区", result.stdout + result.stderr)
        self.assertFalse(any(command[0] == "cmake" for command in self.commands(root)))

    def test_cache_symlink_cannot_escape_real_mount(self):
        if not os.path.ismount("/dev/shm"):
            self.skipTest("当前环境没有可验证的 /dev/shm 挂载")
        temporary = tempfile.TemporaryDirectory(dir="/dev/shm")
        self.addCleanup(temporary.cleanup)
        root, environment = self.prepare("x64")
        cache = self.external_cache()
        self.seed_cache(root, cache, "x64", "符号链接逃逸缓存")
        link = Path(temporary.name) / "outside"
        link.symlink_to(cache, target_is_directory=True)
        environment.update(LOWER_CACHE_ROOT=str(link), LOWER_CACHE_MOUNT="/dev/shm")
        result = self.run_phase(root, environment, "build")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("挂载", result.stdout + result.stderr)
        self.assertFalse(any(command[0] == "cmake" for command in self.commands(root)))

    def test_protobuf_auth_uses_gitlab_header_and_hides_job_token(self):
        root, environment = self.prepare("x64")
        result = self.run_phase(root, environment, "build")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn(environment["CI_JOB_TOKEN"], result.stdout + result.stderr)
        commands = (root / "commands.jsonl").read_text()
        submodules = [command for command in map(json.loads, commands.splitlines())
                      if command[0] == "git" and "submodule" in command]
        self.assertEqual(submodules, [
            ["git", "submodule", "init", "--", "protobuf"],
            ["git", "submodule", "sync", "--recursive", "--", "protobuf"],
            ["git", "submodule", "update", "--init", "--recursive", "--", "protobuf"],
        ])

    def test_submodule_failure_identifies_safe_stage_and_blocks_compilation(self):
        for stage, label in (("init", "初始化"), ("sync", "同步地址"), ("update", "更新提交")):
            with self.subTest(stage=stage):
                root, environment = self.prepare("x64")
                environment["FAIL_SUBMODULE_STAGE"] = stage
                result = self.run_phase(root, environment, "build")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("protobuf 子模块" + label + "失败", result.stdout + result.stderr)
                self.assertNotIn(environment["CI_JOB_TOKEN"], result.stdout + result.stderr)
                commands = [json.loads(line) for line in (root / "commands.jsonl").read_text().splitlines()]
                self.assertFalse(any(command[0] == "cmake" for command in commands))

    def test_both_architectures_freeze_packages_and_symbols(self):
        for arch in ("arm64", "x64"):
            with self.subTest(arch=arch):
                root, environment = self.prepare(arch)
                environment["TOOL_ARCH"] = "x86_64"
                built = self.run_phase(root, environment, "cross-build" if arch == "arm64" else "build")
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                if arch == "x64":
                    self.assertTrue((root / "package/temporary_test").is_file())
                    self.assertFalse(any(command[0] == "ctest" for command in self.commands(root)))
                    environment["CI_JOB_ID"] = "49"
                    tested = self.run_phase(root, environment, "test")
                    self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
                self.assertFalse((root / "package/temporary_test").exists())
                environment["CI_JOB_ID"] = "50"
                packaged = self.run_phase(root, environment, "package")
                self.assertEqual(packaged.returncode, 0, packaged.stdout + packaged.stderr)
                release = root / "release" / arch
                metadata = json.loads((release / "metadata.json").read_text())
                self.assertEqual(metadata["build_job_id"], "42")
                self.assertEqual(metadata["package_job_id"], "50")
                self.assertTrue((release / metadata["file_name"]).is_file())
                self.assertEqual(len(list(release.glob("*debugsymbols*.tar.gz"))), 1 if arch == "arm64" else 0)
                checksums = {}
                for line in (release / "SHA256SUMS").read_text().splitlines():
                    digest, filename = line.split(maxsplit=1)
                    checksums[filename] = digest
                    self.assertEqual(hashlib.sha256((release / filename).read_bytes()).hexdigest(), digest)
                self.assertEqual(set(checksums), {metadata["file_name"]} | {
                    item.name for item in release.glob("*debugsymbols*.tar.gz")})
                self.assertNotIn("secret-token-value", built.stdout + built.stderr)
                self.assertNotIn("secret-test-certificate", packaged.stdout + packaged.stderr)
                self.assertEqual((root / "logs" / arch / "ctest.xml").is_file(), arch == "x64")
                commands = (root / "commands.jsonl").read_text()
                self.assertIn('"--parallel", "4"', commands)
                self.assertEqual('"--parallel", "2"' in commands, arch == "x64")
                self.assertIn("-DMSKDSP_BUILD_TESTS=" + ("ON" if arch == "x64" else "OFF"), commands)

    def test_x64_test_failure_preserves_test_files_and_reports(self):
        root, environment = self.prepare("x64")
        built = self.run_phase(root, environment, "build")
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        result = self.run_phase(root, environment | {"TEST_FAILURE": "1"}, "test")
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((root / "package/temporary_test").is_file())
        self.assertTrue((root / "logs/x64/ctest.xml").is_file())
        self.assertFalse((root / "release/x64/metadata.json").exists())
        self.assertFalse(any(command[0] == "buildctl" for command in self.commands(root)))
        self.assertEqual(sum(command[0] == "cmake" and "--install" in command
                             for command in self.commands(root)), 1)

    def test_other_pipeline_cannot_repackage_frozen_build(self):
        root, environment = self.prepare("x64")
        built = self.run_phase(root, environment, "build")
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        result = self.run_phase(root, environment | {"CI_PIPELINE_ID": "101"}, "package")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((root / "release/x64/metadata.json").exists())
        self.assertNotIn('"buildctl"', (root / "commands.jsonl").read_text())

    def test_cross_build_uses_host_tools_and_archives_symbols_without_tests(self):
        root, environment = self.prepare("arm64")
        built = self.run_phase(root, environment | {"TOOL_ARCH": "x86_64"}, "cross-build")
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        commands = [json.loads(line) for line in (root / "commands.jsonl").read_text().splitlines()]
        self.assertFalse(any(command[0] == "ctest" for command in commands))
        configure = next(command for command in commands if command[0] == "cmake" and "-B" in command)
        self.assertIn("-DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++-14", configure)
        self.assertIn("-DVCPKG_HOST_TRIPLET=x64-linux-dynamic", configure)
        self.assertIn("-DVCPKG_TARGET_TRIPLET=arm64-linux-dynamic-cross", configure)
        self.assertIn("-DMSKDSP_BUILD_TESTS=OFF", configure)
        self.assertIn("-DProtobuf_PROTOC_EXECUTABLE=" + str(root / "build-arm64/vcpkg_installed/x64-linux-dynamic/tools/protobuf/protoc"), configure)
        self.assertFalse((root / "package/temporary_test").exists())
        symbols = list((root / "release/arm64").glob("*debugsymbols*.tar.gz"))
        self.assertEqual(len(symbols), 1)
        with tarfile.open(symbols[0], "r:gz") as archive:
            self.assertEqual(archive.extractfile("debug/MskDSP.debug").read(), b"symbols")
        self.assertFalse((root / "logs/arm64/ctest.xml").exists())
        self.assertEqual(list((root / "release/arm64").glob("*context*")), [])
        identity = (root / "release/arm64/build_identity.json").read_bytes()
        tested = self.run_phase(root, environment | {"CI_JOB_ID": "49"}, "test")
        self.assertNotEqual(tested.returncode, 0)
        self.assertIn("仅 x64 执行测试", tested.stdout + tested.stderr)
        self.assertEqual((root / "release/arm64/build_identity.json").read_bytes(), identity)
        all_commands = [json.loads(line) for line in (root / "commands.jsonl").read_text().splitlines()]
        self.assertFalse(any(command[0] == "ctest" for command in all_commands))
        self.assertEqual(sum(command[0] == "cmake" and "--install" in command for command in all_commands), 1)
        self.assertEqual(sum(command[0] == "git" and "submodule" in command and "update" in command for command in all_commands), 1)

    def test_removed_modes_reject_before_running_tools(self):
        for arch, phase in (("arm64", "build"), ("arm64", "test"), ("x64", "cross-build")):
            with self.subTest(arch=arch, phase=phase):
                root, environment = self.prepare(arch)
                result = self.run_phase(root, environment, phase)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("仅 x64 执行测试", result.stdout + result.stderr)
                self.assertEqual(self.commands(root), [])

    def test_x64_build_and_test_are_separate_and_preserve_original_build_identity(self):
        root, environment = self.prepare("x64")
        built = self.run_phase(root, environment, "build")
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        self.assertTrue((root / "package/temporary_test").is_file())
        self.assertFalse((root / "logs/x64/ctest.xml").exists())
        commands = self.commands(root)
        self.assertFalse(any(command[0] == "ctest" for command in commands))
        configure = next(command for command in commands if command[0] == "cmake" and "-B" in command)
        self.assertIn("-DMSKDSP_BUILD_TESTS=ON", configure)
        release = root / "release/x64"
        identity = (release / "build_identity.json").read_bytes()
        context = json.loads((release / "build_context.json").read_text())
        self.assertEqual(context, dict(project_dir=str(root), target_triplet="x64-linux-dynamic",
                                       host_triplet="x64-linux-dynamic"))
        environment.update(CI_JOB_ID="49", LOWER_CACHE_ROOT="无效的相对缓存", LOWER_CACHE_MOUNT="/不存在的构建挂载")
        tested = self.run_phase(root, environment, "test")
        self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
        self.assertEqual((release / "build_identity.json").read_bytes(), identity)
        self.assertFalse((root / "package/temporary_test").exists())
        self.assertTrue((root / "logs/x64/ctest.xml").is_file())
        all_commands = self.commands(root)
        self.assertEqual(sum(command[0] == "ctest" for command in all_commands), 1)
        self.assertEqual(sum(command[0] == "cmake" and "--install" in command for command in all_commands), 1)
        self.assertEqual(sum(command[0] == "git" and "submodule" in command and "update" in command
                             for command in all_commands), 2)

    def test_x64_test_rejects_other_pipeline_or_inconsistent_build_context(self):
        for mismatch in ("pipeline", "project_dir", "target_triplet", "host_triplet"):
            with self.subTest(mismatch=mismatch):
                root, environment = self.prepare("x64")
                built = self.run_phase(root, environment, "build")
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                if mismatch == "pipeline":
                    environment["CI_PIPELINE_ID"] = "101"
                else:
                    filename = root / "release/x64/build_context.json"
                    context = json.loads(filename.read_text())
                    context[mismatch] = "与原构建不一致"
                    filename.write_text(json.dumps(context))
                count = len(self.commands(root))
                tested = self.run_phase(root, environment, "test")
                self.assertNotEqual(tested.returncode, 0)
                self.assertTrue((root / "package/temporary_test").is_file())
                self.assertFalse(any(command[0] == "ctest" for command in self.commands(root)[count:]))

    def test_cross_build_symbol_failure_blocks_success(self):
        for failure in ("missing", "archive"):
            with self.subTest(failure=failure):
                root, environment = self.prepare("arm64")
                environment["TOOL_ARCH"] = "x86_64"
                if failure == "missing":
                    environment["MISSING_SYMBOLS"] = "1"
                else:
                    executable = root / "bin/tar"
                    executable.write_text("#!/bin/sh\nprintf '符号归档失败\\n' >&2\nexit 27\n", encoding="utf-8")
                    executable.chmod(0o755)
                result = self.run_phase(root, environment, "cross-build")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("符号", result.stdout + result.stderr)
                self.assertFalse(any(command[0] in ("ctest", "buildctl") for command in self.commands(root)))
                self.assertEqual(list((root / "release/arm64").glob("*debugsymbols*.tar.gz")), [])
                self.assertFalse((root / "release/arm64/metadata.json").exists())

    def test_cross_make_ports_do_not_inherit_native_compiler_environment(self):
        root, environment = self.prepare("arm64")
        environment.update(TOOL_ARCH="x86_64", CHECK_MAKE_ENV="1", CC="gcc-14", CXX="g++-14")
        result = self.run_phase(root, environment, "cross-build")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        commands = [json.loads(line) for line in (root / "commands.jsonl").read_text().splitlines()]
        self.assertIn(["make-target-compiler", "aarch64-linux-gnu-gcc-14"], commands)

    def test_missing_source_baseline_fetches_complete_history(self):
        root, environment = self.prepare("x64")
        result = self.run_phase(root, environment | {"MISSING_BASELINE": "1"}, "build")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        commands = [json.loads(line) for line in (root / "commands.jsonl").read_text().splitlines()]
        fetches = [command for command in commands if command[0] == "git" and "fetch" in command]
        self.assertEqual(len(fetches), 1)
        self.assertIn("origin", fetches[0])
        self.assertFalse(any(part.startswith("--depth") for part in fetches[0]))

    def test_existing_shallow_source_restores_complete_history(self):
        root, environment = self.prepare("x64")
        result = self.run_phase(root, environment | {"SHALLOW_SOURCE": "true"}, "build")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        commands = [json.loads(line) for line in (root / "commands.jsonl").read_text().splitlines()]
        fetches = [command for command in commands if command[0] == "git" and "fetch" in command]
        self.assertEqual(len(fetches), 1)
        self.assertIn("--unshallow", fetches[0])
        self.assertIn("origin", fetches[0])


@unittest.skipUnless(os.name == "posix", "真实浅仓库验证在 Linux/WSL 执行")
class LocalRegistryTest(unittest.TestCase):
    def git(self, directory, *arguments):
        return subprocess.run(["git", "-C", str(directory), *arguments], check=True,
                              capture_output=True, text=True).stdout.strip()

    def prepare(self, missing_tree=False, shallow=True):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        origin = root / "origin"
        origin.mkdir()
        self.git(origin, "init")
        port = origin / "ports/demo"
        port.mkdir(parents=True)
        (port / "portfile.cmake").write_text("旧树" if missing_tree else "当前树")
        self.git(origin, "add", ".")
        self.git(origin, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                 "-c", "commit.gpgsign=false", "commit", "-m", "fixture")
        tree = self.git(origin, "rev-parse", "HEAD:ports/demo")
        if missing_tree:
            (port / "portfile.cmake").write_text("当前树")
        self.git(origin, "add", "ports")
        index_tree = self.git(origin, "write-tree")
        current_tree = self.git(origin, "rev-parse", index_tree + ":ports/demo")
        versions = origin / "versions"
        (versions / "d-").mkdir(parents=True)
        (versions / "baseline.json").write_text(json.dumps({"default": {"demo": {"baseline": "1.0", "port-version": 1}}}))
        (versions / "d-/demo.json").write_text(json.dumps({"versions": [
            {"version": "1.0", "port-version": 1, "git-tree": current_tree},
            {"version": "1.0", "port-version": 0, "git-tree": tree}]}))
        self.git(origin, "add", ".")
        self.git(origin, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                 "-c", "commit.gpgsign=false", "commit", "-m", "versions")
        source = root / "source"
        subprocess.run(["git", "clone", *(["--depth=1"] if shallow else []), origin.as_uri(), str(source)],
                       check=True, capture_output=True, text=True)
        baseline = self.git(source, "rev-parse", "HEAD")
        self.git(source, "checkout", "--detach", baseline)
        self.assertEqual(self.git(source, "rev-parse", "--is-shallow-repository"), "true" if shallow else "false")
        return root, source, baseline

    def function(self):
        text = (ROOT / "script/gitlab/build_lower.sh").read_text()
        self.assertIn("configure_local_registry() {", text)
        return "configure_local_registry() {" + text.split("configure_local_registry() {", 1)[1].split("\n}\n", 1)[0] + "\n}\n"

    def verify_local_fetch(self, missing_unrelated_tree=False, full_history=False):
        root, source, baseline = self.prepare(missing_tree=missing_unrelated_tree, shallow=not full_history)
        registry = root / "registry"
        registry.mkdir()
        command = self.function() + r'''
set -euo pipefail
log() { printf '%s\n' "$*"; }
configure_local_registry "$PINNED_BASELINE"
git -C "$REGISTRY" init -q
GIT_ALLOW_PROTOCOL=file git -C "$REGISTRY" fetch --update-shallow -- https://github.com/microsoft/vcpkg HEAD
test "$(git -C "$REGISTRY" rev-parse FETCH_HEAD)" = "$PINNED_BASELINE"
git -C "$REGISTRY" cat-file -e "FETCH_HEAD:versions/baseline.json"
git -C "$REGISTRY" cat-file -e "FETCH_HEAD:ports/demo"
if test -n "${HISTORICAL_TREE:-}"; then
  git -C "$REGISTRY" read-tree "$HISTORICAL_TREE"
fi
'''
        record = json.loads((source / "versions/d-/demo.json").read_text())["versions"][-1]
        result = subprocess.run(["bash", "-c", command], env=dict(os.environ, VCPKG_ROOT=str(source),
                                PINNED_BASELINE=baseline, REGISTRY=str(registry),
                                HISTORICAL_TREE=record["git-tree"] if full_history else ""), cwd=root,
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("insteadOf", self.git(source, "config", "--local", "--list"))

    def test_registry_head_fetch_is_local_with_all_network_protocols_disabled(self):
        self.verify_local_fetch()

    def test_unrelated_old_port_tree_does_not_reject_valid_baseline(self):
        self.verify_local_fetch(missing_unrelated_tree=True)

    def test_complete_history_registry_supports_prior_port_revision(self):
        self.verify_local_fetch(missing_unrelated_tree=True, full_history=True)

    def test_missing_required_baseline_directory_is_rejected(self):
        for directory in ("ports", "versions"):
            with self.subTest(directory=directory):
                root, source, _ = self.prepare()
                self.git(source, "rm", "-r", directory)
                self.git(source, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                         "-c", "commit.gpgsign=false", "commit", "-m", "missing required directory")
                baseline = self.git(source, "rev-parse", "HEAD")
                command = self.function() + '\nset -euo pipefail\nlog() { :; }\nconfigure_local_registry "$PINNED_BASELINE"\n'
                result = subprocess.run(["bash", "-c", command], env=dict(os.environ, VCPKG_ROOT=str(source),
                                        PINNED_BASELINE=baseline), cwd=root, capture_output=True, text=True, timeout=30)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("树对象", result.stdout + result.stderr)


@unittest.skipUnless(os.name == "posix", "tmpfs 工作区与真实 ELF 验证在 Linux/WSL 执行")
class TmpfsWorkspaceTest(unittest.TestCase):
    def exercise(self, failure):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            work = root / "build-x64/test_env"
            seed = work / "demo/module/libDummy.so"
            seed.parent.mkdir(parents=True)
            shutil.copyfile("/bin/pwd", seed)
            seed.chmod(0o755)
            (root / "logs/x64").mkdir(parents=True)
            testing = root / "build-x64/Testing/Temporary"
            testing.mkdir(parents=True)
            (testing / "previous.log").write_text("已有测试日志")
            tools = root / "bin"
            tools.mkdir()
            executable = tools / "ctest"
            executable.write_text('''#!/usr/bin/env python3
import json, os, socket, subprocess, sys, tempfile
from pathlib import Path
work = Path(os.environ["ROOT"]) / "build-x64/test_env/demo"
seed = work / "module/libDummy.so"
real_work = work.resolve()
run = subprocess.run([str(seed)], cwd=work, capture_output=True, text=True)
testing = Path(os.environ["ROOT"]) / "build-x64/Testing/Temporary"
testing.mkdir(parents=True, exist_ok=True)
(testing / "LastTest.log").write_text("本次真实日志")
socket_path = tempfile.gettempdir() + "/data_center_grpc_service_test_tmp_1234567890123/./socket/IEC61850.sock"
os.makedirs(os.path.dirname(socket_path))
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as endpoint:
    endpoint.bind(socket_path)
data = dict(arguments=sys.argv[1:], cwd=str(real_work), tmpfs=os.stat(real_work).st_dev == os.stat("/dev/shm").st_dev,
            seed_link=seed.is_symlink(), seed_target=os.readlink(seed) if seed.is_symlink() else "",
            module_directory_link=seed.parent.is_symlink(),
            module_directory_target=os.readlink(seed.parent) if seed.parent.is_symlink() else "",
            directory_entries=[dict(name=entry.name, symlink=entry.is_symlink()) for entry in os.scandir(seed.parent)],
            temporary_dir=tempfile.gettempdir(), temporary_tmpfs=os.stat(tempfile.gettempdir()).st_dev == os.stat("/dev/shm").st_dev,
            testing_tmpfs=os.stat(testing).st_dev == os.stat("/dev/shm").st_dev,
            socket_path=socket_path, socket_length=len(socket_path.encode()),
            executable_status=run.returncode, executable_cwd=run.stdout.strip())
(work / "conf").mkdir()
(work / "conf/soe.sqlite").write_text("临时数据库")
Path(os.environ["PROBE"]).write_text(json.dumps(data))
sys.exit(int(os.environ["TEST_FAILURE"]))
''', encoding="utf-8")
            executable.chmod(0o755)
            script = (ROOT / "script/gitlab/build_lower.sh").read_text()
            function = "run_complete_ctest() {" + script.split("run_complete_ctest() {", 1)[1].split("\n}\n", 1)[0] + "\n}\n"
            command = "set -euo pipefail\nlog() { printf '%s\\n' \"$*\"; }\n" + function + "\nrun_complete_ctest\n"
            result = subprocess.run(["bash", "-c", command], cwd=root, capture_output=True, text=True, timeout=30,
                                    env=dict(os.environ, ROOT=str(root), BUILD_DIR="build-x64", LOG_DIR="logs/x64", ARCH="x64",
                                             PATH=str(tools) + os.pathsep + os.environ["PATH"],
                                             PROBE=str(root / "probe.json"), TEST_FAILURE=str(failure)))
            self.assertEqual(result.returncode, failure, result.stdout + result.stderr)
            data = json.loads((root / "probe.json").read_text())
            self.assertTrue(data["tmpfs"], data)
            self.assertFalse(data["seed_link"], data)
            self.assertTrue(data["module_directory_link"], data)
            self.assertIn("test_env.disk", data["module_directory_target"])
            self.assertEqual(data["directory_entries"], [dict(name="libDummy.so", symlink=False)])
            self.assertTrue(data["temporary_tmpfs"], data)
            self.assertTrue(data["temporary_dir"].startswith("/dev/shm/msk."), data)
            self.assertFalse(Path(data["temporary_dir"]).exists())
            self.assertTrue(data["testing_tmpfs"], data)
            self.assertLess(data["socket_length"], 108, data)
            self.assertTrue(data["socket_path"].endswith("data_center_grpc_service_test_tmp_1234567890123/./socket/IEC61850.sock"))
            self.assertEqual((testing / "LastTest.log").read_text(), "本次真实日志")
            self.assertEqual((testing / "previous.log").read_text(), "已有测试日志")
            self.assertFalse((root / "build-x64/Testing").is_symlink())
            self.assertFalse((root / "build-x64/Testing.disk").exists())
            self.assertEqual(data["executable_status"], 0)
            self.assertEqual(data["executable_cwd"], data["cwd"])
            self.assertTrue(work.is_dir())
            self.assertFalse(work.is_symlink())
            self.assertTrue(seed.is_file())
            self.assertFalse(seed.is_symlink())
            self.assertFalse(work.with_name("test_env.disk").exists())
            self.assertFalse((work / "demo/conf/soe.sqlite").exists())
            self.assertFalse(Path(data["cwd"]).exists())
            self.assertIn("--no-tests=error", data["arguments"])
            self.assertEqual(data["arguments"][data["arguments"].index("--parallel") + 1], "2")

    def test_tmpfs_workspace_preserves_disk_elf_seed_and_restores_after_success(self):
        self.exercise(0)

    def test_tmpfs_workspace_restores_after_failure_and_preserves_exit_code(self):
        self.exercise(27)


if __name__ == "__main__":
    unittest.main()
