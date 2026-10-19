"""通过可控命令替身验证真实 Bash 编排、失败门禁及产物传递。"""
import base64
import json
import os
from pathlib import Path
import shutil
import subprocess
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
    if os.environ["ARCH"] == "arm64":
        Path("package/debug").mkdir()
        Path("package/debug/MskDSP.debug").write_bytes(b"symbols")
elif name == "cmake" and "-B" in args:
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


@unittest.skipUnless(os.name == "posix", "Bash 编排契约测试在 Linux/WSL 执行")
class BuildLowerTest(unittest.TestCase):
    def prepare(self, arch):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        for filename in ("script/gitlab/build_lower.sh", "script/gitlab/lower_metadata.py",
                         "script/gitlab/download_asset.py", "script/make_exe.sh",
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
        return root, environment

    def run_phase(self, root, environment, phase):
        return subprocess.run(["bash", "script/gitlab/build_lower.sh", phase, environment["ARCH"]],
                              cwd=root, env=environment, capture_output=True, text=True, timeout=30)

    def test_both_architectures_freeze_packages_and_symbols(self):
        for arch in ("arm64", "x64"):
            with self.subTest(arch=arch):
                root, environment = self.prepare(arch)
                built = self.run_phase(root, environment, "build")
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
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
                self.assertNotIn("secret-token-value", built.stdout + built.stderr)
                self.assertNotIn("secret-test-certificate", packaged.stdout + packaged.stderr)
                self.assertTrue((root / "logs" / arch / "ctest.xml").is_file())
                commands = (root / "commands.jsonl").read_text()
                self.assertIn('"--parallel", "8"', commands)
                self.assertIn('"--parallel", "2"', commands)

    def test_test_failure_blocks_install_and_package(self):
        root, environment = self.prepare("arm64")
        result = self.run_phase(root, environment | {"TEST_FAILURE": "1"}, "build")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((root / "package/MskDSP").exists())
        self.assertTrue((root / "logs/arm64/ctest.xml").is_file())
        self.assertNotIn('"--install"', (root / "commands.jsonl").read_text())

    def test_other_pipeline_cannot_repackage_frozen_build(self):
        root, environment = self.prepare("x64")
        built = self.run_phase(root, environment, "build")
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        result = self.run_phase(root, environment | {"CI_PIPELINE_ID": "101"}, "package")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((root / "release/x64/metadata.json").exists())
        self.assertNotIn('"buildctl"', (root / "commands.jsonl").read_text())

    def test_cross_build_uses_host_tools_and_leaves_tests_for_arm_job(self):
        root, environment = self.prepare("arm64")
        built = self.run_phase(root, environment | {"TOOL_ARCH": "x86_64"}, "cross-build")
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        commands = [json.loads(line) for line in (root / "commands.jsonl").read_text().splitlines()]
        self.assertFalse(any(command[0] == "ctest" for command in commands))
        configure = next(command for command in commands if command[0] == "cmake" and "-B" in command)
        self.assertIn("-DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++-14", configure)
        self.assertIn("-DVCPKG_HOST_TRIPLET=x64-linux-dynamic", configure)
        self.assertIn("-DVCPKG_TARGET_TRIPLET=arm64-linux-dynamic-cross", configure)
        self.assertIn("-DProtobuf_PROTOC_EXECUTABLE=" + str(root / "build-arm64/vcpkg_installed/x64-linux-dynamic/tools/protobuf/protoc"), configure)
        self.assertTrue((root / "package/temporary_test").is_file())
        self.assertEqual(list((root / "release/arm64").glob("*debugsymbols*")), [])
        identity = (root / "release/arm64/build_identity.json").read_bytes()
        tested = self.run_phase(root, environment | {"CI_JOB_ID": "49"}, "test")
        self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
        self.assertFalse((root / "package/temporary_test").exists())
        self.assertEqual((root / "release/arm64/build_identity.json").read_bytes(), identity)
        self.assertEqual(len(list((root / "release/arm64").glob("*debugsymbols*"))), 1)
        all_commands = [json.loads(line) for line in (root / "commands.jsonl").read_text().splitlines()]
        self.assertEqual(sum(command[0] == "cmake" and "--install" in command for command in all_commands), 1)
        self.assertEqual(sum(command[0] == "git" and "submodule" in command and "update" in command for command in all_commands), 2)

    def test_arm_test_rejects_wrong_pipeline_and_preserves_tests_on_failure(self):
        root, environment = self.prepare("arm64")
        built = self.run_phase(root, environment | {"TOOL_ARCH": "x86_64"}, "cross-build")
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        wrong = self.run_phase(root, environment | {"CI_PIPELINE_ID": "101"}, "test")
        self.assertNotEqual(wrong.returncode, 0)
        failed = self.run_phase(root, environment | {"TEST_FAILURE": "1"}, "test")
        self.assertNotEqual(failed.returncode, 0)
        self.assertTrue((root / "package/temporary_test").is_file())
        self.assertTrue((root / "logs/arm64/ctest.xml").is_file())
        self.assertEqual(list((root / "release/arm64").glob("*debugsymbols*")), [])

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
            tools = root / "bin"
            tools.mkdir()
            executable = tools / "ctest"
            executable.write_text('''#!/usr/bin/env python3
import json, os, subprocess, sys, tempfile
from pathlib import Path
work = Path(os.environ["ROOT"]) / "build-x64/test_env/demo"
seed = work / "module/libDummy.so"
real_work = work.resolve()
run = subprocess.run([str(seed)], cwd=work, capture_output=True, text=True)
data = dict(arguments=sys.argv[1:], cwd=str(real_work), tmpfs=os.stat(real_work).st_dev == os.stat("/dev/shm").st_dev,
            seed_link=seed.is_symlink(), seed_target=os.readlink(seed) if seed.is_symlink() else "",
            module_directory_link=seed.parent.is_symlink(),
            module_directory_target=os.readlink(seed.parent) if seed.parent.is_symlink() else "",
            directory_entries=[dict(name=entry.name, symlink=entry.is_symlink()) for entry in os.scandir(seed.parent)],
            temporary_dir=tempfile.gettempdir(), temporary_tmpfs=os.stat(tempfile.gettempdir()).st_dev == os.stat("/dev/shm").st_dev,
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
            self.assertTrue(data["temporary_dir"].startswith("/dev/shm/mskdsp-ctest-x64."), data)
            self.assertFalse(Path(data["temporary_dir"]).exists())
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
