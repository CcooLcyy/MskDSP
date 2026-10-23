"""用独立 CMake 工程验证符号处理等待全部链接，无需下位机依赖。"""

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class StripDebugOrderTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cmake = shutil.which("cmake")
        cls.ninja = shutil.which("ninja")
        if not cls.cmake or not cls.ninja:
            raise unittest.SkipTest("验证符号处理依赖图需要 CMake 和 Ninja")
        cls.temporary = tempfile.TemporaryDirectory(prefix="mskdsp-strip-order-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.source = Path(cls.temporary.name)
        cls.build = cls.source / "build"
        query = cls.build / ".cmake/api/v1/query"
        query.mkdir(parents=True)
        (query / "codemodel-v2").touch()

        # 复用仓库的真实配置块，避免在测试中复制目标收集与依赖逻辑。
        configuration = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        strip_configuration = configuration.split("\nif(MSKDSP_STRIP_DEBUG)\n", 1)[1]
        (cls.source / "strip.cmake").write_text(
            "if(MSKDSP_STRIP_DEBUG)\n" + strip_configuration, encoding="utf-8"
        )
        # 只生成构建图并 dry-run；若意外执行伪编译器，立即报错。
        fake_compiler = cls.source / "fake_compiler.py"
        fake_compiler.write_text("raise SystemExit('禁止执行编译器')\n", encoding="utf-8")
        toolchain = cls.source / "toolchain.cmake"
        toolchain.write_text(
            'set(CMAKE_SYSTEM_NAME Linux)\n'
            f'set(CMAKE_CXX_COMPILER "{Path(sys.executable).as_posix()}")\n'
            f'set(CMAKE_CXX_COMPILER_ARG1 "{fake_compiler.as_posix()}")\n'
            'set(CMAKE_CXX_COMPILER_ID GNU)\n'
            'set(CMAKE_CXX_COMPILER_ID_RUN TRUE)\n'
            'set(CMAKE_CXX_COMPILER_FORCED TRUE)\n'
            'set(CMAKE_CXX_COMPILER_WORKS TRUE)\n'
            'set(CMAKE_STRIP "未执行的strip")\n'
            'set(CMAKE_OBJCOPY "未执行的objcopy")\n',
            encoding="utf-8",
        )
        (cls.source / "sample.cc").write_text("int main() { return 0; }\n", encoding="utf-8")
        (cls.source / "tests").mkdir()
        (cls.source / "tests/CMakeLists.txt").write_text(
            'add_executable(protocol_test "${CMAKE_SOURCE_DIR}/sample.cc")\n'
            'target_link_libraries(protocol_test PRIVATE protocol)\n',
            encoding="utf-8",
        )
        (cls.source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.28)\n'
            'project(StripOrder LANGUAGES CXX)\n'
            'set(MSKDSP_STRIP_DEBUG ON)\n'
            'set(packPath "${CMAKE_BINARY_DIR}/package")\n'
            'add_library(protocol SHARED sample.cc)\n'
            'add_library(plugin MODULE sample.cc)\n'
            'add_executable(application sample.cc)\n'
            'target_link_libraries(application PRIVATE protocol)\n'
            'add_executable(local_test sample.cc)\n'
            'target_link_libraries(local_test PRIVATE protocol)\n'
            'add_subdirectory(tests)\n'
            'include(strip.cmake)\n',
            encoding="utf-8",
        )
        result = subprocess.run(
            [cls.cmake, "-S", str(cls.source), "-B", str(cls.build), "-G", "Ninja",
             f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", f"-DCMAKE_MAKE_PROGRAM={cls.ninja}"],
            text=True, encoding="utf-8", errors="replace", capture_output=True,
        )
        if result.returncode:
            raise AssertionError("最小 CMake 工程配置失败：\n" + result.stdout + result.stderr)
        reply = cls.build / ".cmake/api/v1/reply"
        index = json.loads(next(reply.glob("index-*.json")).read_text(encoding="utf-8"))
        model = json.loads((reply / index["reply"]["codemodel-v2"]["jsonFile"]).read_text(encoding="utf-8"))
        cls.targets = {}
        for target in model["configurations"][0]["targets"]:
            cls.targets[target["name"]] = json.loads(
                (reply / target["jsonFile"]).read_text(encoding="utf-8")
            )

    # 验证真实生成的依赖图包含生产目标、当前目录测试及子目录测试。
    def test_strip_depends_on_every_link_target(self):
        dependencies = {
            dependency["id"]
            for dependency in self.targets["mskdsp_strip_debug"].get("dependencies", [])
        }
        expected = {"protocol", "plugin", "application", "local_test", "protocol_test"}
        missing = sorted(name for name in expected if self.targets[name]["id"] not in dependencies)
        self.assertEqual(missing, [], "符号处理缺少链接前置依赖：" + ", ".join(missing))

    # 验证单独请求符号处理时也会先安排全部测试链接，防止 ALL 掩盖依赖缺失。
    def test_strip_dry_run_links_tests_before_stripping(self):
        result = subprocess.run(
            [self.ninja, "-C", str(self.build), "-n", "mskdsp_strip_debug"],
            text=True, encoding="utf-8", errors="replace", capture_output=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        lines = result.stdout.splitlines()
        strip_index = next(i for i, line in enumerate(lines) if "Stripping binaries" in line)
        for target in ("local_test", "protocol_test"):
            with self.subTest(target=target):
                links = [i for i, line in enumerate(lines)
                         if "Linking CXX executable" in line and target in line]
                self.assertTrue(links, f"符号处理未等待 {target} 链接：\n{result.stdout}")
                self.assertLess(links[0], strip_index, result.stdout)


if __name__ == "__main__":
    unittest.main()
