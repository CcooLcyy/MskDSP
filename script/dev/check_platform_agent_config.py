"""使用已安装 clang++ 运行标准库范围的真实 C++ 配置行为检查，不安装依赖。"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    compiler = shutil.which("clang++")
    if not compiler:
        print("缺少现有 clang++，未执行 C++ 配置行为检查", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as temporary:
        executable = Path(temporary) / ("platform-config.exe" if sys.platform == "win32" else "platform-config")
        subprocess.run([compiler, "-std=c++23", "-D_CRT_SECURE_NO_WARNINGS", "-Isrc/PlatformAgent/include",
                        "src/PlatformAgent/PlatformAgentConfig.cc", "test/platform_agent_config_smoke.cc",
                        "-o", str(executable)], cwd=ROOT, check=True)
        return subprocess.run([str(executable)], cwd=ROOT, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
