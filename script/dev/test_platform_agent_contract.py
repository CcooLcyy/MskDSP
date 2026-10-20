"""验证平台连接组件的协议镜像与构建接入边界，不代替 C++ 功能测试。"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PlatformAgentContractTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("protoc"), "需要已安装的 protoc")
    def test_protocol_compiles_into_descriptor(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "remote.pb"
            result = subprocess.run(["protoc", "--proto_path=.",
                                     "--descriptor_set_out=" + str(output), "--include_imports",
                                     "platform/v1/remote.proto"], cwd=ROOT / "platform-proto",
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertGreater(output.stat().st_size, 0)


if __name__ == "__main__":
    unittest.main()
