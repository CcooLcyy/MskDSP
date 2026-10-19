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


if __name__ == "__main__":
    unittest.main()
