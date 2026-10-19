"""验证独立安装脚本的策略初始化与缺失运行库拒绝行为。"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("cmake"), "需要 CMake 执行安装脚本契约测试")
class RuntimeDependenciesTest(unittest.TestCase):
    def test_standalone_script_initializes_cmake_policies(self):
        contents = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        template = contents.split("set(MSKDSP_INSTALL_SCRIPT_CONTENT [=[", 1)[1].split("]=])", 1)[0]
        self.assertTrue(template.lstrip().startswith("cmake_minimum_required(VERSION 3.28)"))

    def run_collector(self, unresolved, *, missing_required=False, compiler_runtime=False):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        libraries = root / "vcpkg/lib"
        libraries.mkdir(parents=True)
        for name in ("libcares.so.2", "libsqlite3.so"):
            if not (missing_required and name == "libcares.so.2"):
                (libraries / name).write_bytes("目标库占位".encode("utf-8"))
        contents = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        template = contents.split("set(MSKDSP_INSTALL_SCRIPT_CONTENT [=[", 1)[1].split("]=])", 1)[0]
        # 替换 ELF 扫描输入，其后运行真实安装模板的去重、必需库回退与拒绝规则。
        template = re.sub(r"file\(GET_RUNTIME_DEPENDENCIES\n.*?\n\)",
                          'set(_mskdsp_deps "")\nset(_mskdsp_unresolved "' + unresolved + '")',
                          template, count=1, flags=re.DOTALL)
        if compiler_runtime:
            for name in ("libstdc++.so.6", "libgcc_s.so.1", "libatomic.so.1"):
                (libraries / name).write_bytes(name.encode())
            template = re.sub(r"execute_process\(\n\s+COMMAND \"@CMAKE_CXX_COMPILER@\".*?\n\s*\)",
                              'set(_mskdsp_runtime_path "' + libraries.as_posix() + '/${_lib}")',
                              template, count=1, flags=re.DOTALL)
        replacements = {
            "MSKDSP_INSTALL_EXEC_PATHS": "", "MSKDSP_INSTALL_LIB_PATHS": "",
            "MSKDSP_VCPKG_RUNTIME_DIRS": libraries.as_posix(),
            "MSKDSP_VCPKG_TRIPLET_DIR_REGEX": (root / "vcpkg").as_posix(),
            "CMAKE_CXX_COMPILER": "/missing/compiler", "CMAKE_OBJDUMP": "/missing/objdump",
            "CMAKE_SYSTEM_PROCESSOR": "aarch64",
        }
        for key, value in replacements.items():
            template = template.replace("@" + key + "@", value)
        script = root / "collector.cmake"
        script.write_text('set(CMAKE_INSTALL_PREFIX "' + (root / 'package').as_posix() + '")\n' + template, encoding="utf-8")
        result = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True, encoding="utf-8")
        return result, root

    def test_unknown_library_fails_with_specific_missing_dependency(self):
        result, _ = self.run_collector("libmissing-runtime.so")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("拒绝生成不可运行的交付包", result.stderr)
        self.assertIn("libmissing-runtime.so", result.stderr)
        self.assertNotIn("Unknown arguments", result.stderr)

    def test_missing_required_vcpkg_library_still_fails(self):
        result, _ = self.run_collector("", missing_required=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("libcares.so.2", result.stderr)

    def test_resolved_required_libraries_are_copied(self):
        result, root = self.run_collector("libcares.so.2;libsqlite3.so")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((root / "package/lib/libcares.so.2").is_file())
        self.assertTrue((root / "package/lib/libsqlite3.so").is_file())

    def test_cross_system_libraries_use_base_and_compiler_runtime_is_collected(self):
        actual_unresolved = "ld-linux-aarch64.so.1;libanl.so.1;libc.so.6;libgcc_s.so.1;libm.so.6;libstdc++.so.6"
        result, root = self.run_collector(actual_unresolved, compiler_runtime=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for name in ("libstdc++.so.6", "libgcc_s.so.1", "libatomic.so.1"):
            self.assertTrue((root / "package/lib" / name).is_file())
        for name in ("ld-linux-aarch64.so.1", "libanl.so.1", "libc.so.6", "libm.so.6"):
            self.assertFalse((root / "package/lib" / name).exists())

    def test_missing_compiler_runtime_cannot_be_ignored(self):
        result, _ = self.run_collector("libstdc++.so.6")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("libstdc++.so.6", result.stderr)


if __name__ == "__main__":
    unittest.main()
