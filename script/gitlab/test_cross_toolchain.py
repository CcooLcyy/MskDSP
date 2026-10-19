"""验证交叉工具链及主机协议生成器的配置边界。"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class CrossToolchainTest(unittest.TestCase):
    def test_target_triplet_pins_gcc14_and_target_binutils(self):
        triplet = (ROOT / "cmake/vcpkg-triplets/arm64-linux-dynamic-cross.cmake").read_text(encoding="utf-8")
        toolchain = (ROOT / "cmake/toolchains/aarch64-linux-gcc14.cmake").read_text(encoding="utf-8")
        self.assertIn("VCPKG_TARGET_ARCHITECTURE arm64", triplet)
        self.assertIn("VCPKG_CHAINLOAD_TOOLCHAIN_FILE", triplet)
        for tool in ("aarch64-linux-gnu-gcc-14", "aarch64-linux-gnu-g++-14", "aarch64-linux-gnu-objdump",
                     "aarch64-linux-gnu-strip", "aarch64-linux-gnu-objcopy"):
            self.assertIn(tool, toolchain)
        self.assertIn("CMAKE_SYSTEM_PROCESSOR aarch64", toolchain)

    def test_cross_protobuf_generation_explicitly_selects_host_protoc(self):
        contents = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("if(CMAKE_CROSSCOMPILING)", contents)
        self.assertIn("IMPORTED_LOCATION_${_mskdsp_protoc_config}", contents)
        self.assertIn("tools/protobuf/protoc", contents)

    @unittest.skipUnless(shutil.which("cmake"), "主机无 CMake 时在 WSL/真实工具镜像验证")
    def test_imported_target_overrides_all_configs_and_rejects_missing_host(self):
        contents = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        fragment = contents.split("find_package(Protobuf CONFIG REQUIRED)", 1)[1].split("find_package(pugixml", 1)[0]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            host = root / "installed/x64-linux-dynamic/tools/protobuf/protoc"
            host.parent.mkdir(parents=True)
            host.write_text("主机工具占位", encoding="utf-8")
            stub = ('cmake_minimum_required(VERSION 3.20)\nproject(HostProtoc NONE)\n'
                    'add_executable(protobuf::protoc IMPORTED GLOBAL)\n'
                    'set_property(TARGET protobuf::protoc PROPERTY IMPORTED_CONFIGURATIONS RELEASE;CUSTOM)\n'
                    'set_property(TARGET protobuf::protoc PROPERTY IMPORTED_LOCATION_RELEASE /target/protoc)\n'
                    'set(CMAKE_CROSSCOMPILING TRUE)\n'
                    'set(VCPKG_HOST_TRIPLET x64-linux-dynamic)\n'
                    'set(VCPKG_INSTALLED_DIR "' + str(root / 'installed').replace('\\', '/') + '")\n' + fragment + '\n'
                    'foreach(config IN ITEMS RELEASE CUSTOM DEBUG RELWITHDEBINFO MINSIZEREL NOCONFIG)\n'
                    'get_target_property(location protobuf::protoc IMPORTED_LOCATION_${config})\n'
                    'if(NOT location STREQUAL "${VCPKG_INSTALLED_DIR}/x64-linux-dynamic/tools/protobuf/protoc")\n'
                    'message(FATAL_ERROR "主机工具配置未覆盖")\nendif()\nendforeach()\n')
            (root / "CMakeLists.txt").write_text(stub, encoding="utf-8")
            configured = subprocess.run(["cmake", "-S", str(root), "-B", str(root / "build")], capture_output=True, text=True, encoding="utf-8")
            self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
            host.unlink()
            failed = subprocess.run(["cmake", "-S", str(root), "-B", str(root / "missing")], capture_output=True, text=True, encoding="utf-8")
            self.assertNotEqual(failed.returncode, 0)


if __name__ == "__main__":
    unittest.main()
