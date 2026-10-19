"""验证交叉镜像保留原生工具并提供真实 GCC14 ARM64 工具链。"""
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "docker/gitlab-ci-cross-tools"
IMAGE = "mskdsp-lower-cross-tools:ubuntu24.04-gcc14-v1"


class CrossToolsImageTests(unittest.TestCase):
    def test_official_versioned_cross_packages_and_compiler_targets(self):
        dockerfile = (TOOLS / "Dockerfile").read_text(encoding="utf-8")
        self.assertIn("FROM mskdsp-ci-tools:ubuntu24.04-x64", dockerfile)
        for package in ("gcc-14-aarch64-linux-gnu", "g++-14-aarch64-linux-gnu", "binutils-aarch64-linux-gnu"):
            self.assertIn(package, dockerfile)
        for command in ("gcc-14 -dumpmachine", "g++-14 -dumpmachine", "aarch64-linux-gnu-gcc-14 -dumpmachine",
                        "aarch64-linux-gnu-g++-14 -dumpmachine", "aarch64-linux-gnu-readelf", "x86_64-linux-gnu"):
            self.assertIn(command, dockerfile)
        self.assertNotIn("update-alternatives", dockerfile)
        self.assertNotIn("add-apt-repository", dockerfile)

    def test_build_uses_new_tag_and_host_network_without_running_qemu(self):
        script = (TOOLS / "build.sh").read_text(encoding="utf-8")
        self.assertIn(IMAGE, script)
        self.assertIn("--network=host", script)
        self.assertIn("--platform=linux/amd64", script)
        self.assertIn("--entrypoint bash", script)
        self.assertNotIn("--privileged", script)

    @unittest.skipUnless(os.environ.get("CROSS_TOOLS_DOCKER_TEST") == "1", "真实镜像烟测由显式 Docker 环境运行")
    def test_installed_compilers_produce_native_and_arm64_elf(self):
        command = """set -euo pipefail
test "$(gcc-14 -dumpmachine)" = x86_64-linux-gnu
test "$(g++-14 -dumpmachine)" = x86_64-linux-gnu
test "$(aarch64-linux-gnu-gcc-14 -dumpmachine)" = aarch64-linux-gnu
test "$(aarch64-linux-gnu-g++-14 -dumpmachine)" = aarch64-linux-gnu
printf '#include <iostream>\nint main(){std::cout << "ok";return 0;}\n' >/tmp/cross-smoke.cc
g++-14 /tmp/cross-smoke.cc -o /tmp/native-smoke
aarch64-linux-gnu-g++-14 /tmp/cross-smoke.cc -o /tmp/arm64-smoke
test "$(/tmp/native-smoke)" = ok
readelf -h /tmp/native-smoke | grep -q 'Advanced Micro Devices X86-64'
aarch64-linux-gnu-readelf -h /tmp/arm64-smoke | grep -q 'AArch64'
"""
        result = subprocess.run(["docker", "run", "--rm", "--network=none", "--entrypoint", "bash", IMAGE, "-lc", command],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
