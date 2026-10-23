"""检查 Modbus 模板来源协议及保存顺序，不替代 C++ 运行测试。"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from google.protobuf import descriptor_pb2, descriptor_pool, message_factory

ROOT = Path(__file__).resolve().parents[2]


class ModbusTemplateContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory() as directory:
            descriptor = Path(directory) / "modbus.pb"
            subprocess.run([shutil.which("protoc") or "protoc", "--proto_path=.", "--include_imports",
                            f"--descriptor_set_out={descriptor}", "ModbusRTU.proto", "ModbusTCP.proto"],
                           cwd=ROOT / "protobuf", check=True, capture_output=True)
            files = descriptor_pb2.FileDescriptorSet.FromString(descriptor.read_bytes()).file
        cls.pool = descriptor_pool.DescriptorPool()
        for file in files:
            cls.pool.Add(file)

    def test_compiled_protocol_presence_and_uint64_wire(self):
        for name in ("ModbusRTUProto", "ModbusTCPProto"):
            with self.subTest(name=name):
                request = message_factory.GetMessageClass(self.pool.FindMessageTypeByName(name + ".UpsertPointTableRequest"))
                empty = request(conn_name="test")
                self.assertFalse(empty.HasField("expected_revision"))
                self.assertFalse(empty.HasField("template_bindings_json"))
                self.assertFalse(empty.HasField("expected_link_config"))
                old_wire = empty.SerializeToString()
                self.assertEqual(old_wire, b"\x0a\x04test")
                value = request(conn_name="test", expected_revision=2**64 - 1, template_bindings_json="")
                decoded = request.FromString(value.SerializeToString())
                self.assertEqual(decoded.expected_revision, 2**64 - 1)
                self.assertTrue(decoded.HasField("template_bindings_json"))
                self.assertTrue(decoded.HasField("expected_revision"))
                value.expected_link_config.conn_name = "test"
                self.assertTrue(request.FromString(value.SerializeToString()).HasField("expected_link_config"))
                link_request = message_factory.GetMessageClass(self.pool.FindMessageTypeByName(name + ".UpsertLinkRequest"))
                link = link_request()
                self.assertFalse(link.HasField("expected_config"))
                self.assertFalse(link.HasField("expected_point_table_revision"))
                link.expected_config.conn_name = "test"
                link.expected_point_table_revision = 2**64 - 1
                self.assertEqual(link_request.FromString(link.SerializeToString()).expected_config.conn_name, "test")
                decoded_link = link_request.FromString(link.SerializeToString())
                self.assertTrue(decoded_link.HasField("expected_point_table_revision"))
                self.assertEqual(decoded_link.expected_point_table_revision, 2**64 - 1)
                table = message_factory.GetMessageClass(self.pool.FindMessageTypeByName(name + ".PointTable"))
                decoded_table = table.FromString(table(revision=2**53 + 1, template_bindings_json="{}").SerializeToString())
                self.assertEqual(decoded_table.revision, 2**53 + 1)
                self.assertEqual(decoded_table.template_bindings_json, "{}")

    def test_protocol_fields_match(self):
        for name in ("ModbusRTU", "ModbusTCP"):
            with self.subTest(name=name):
                source = (ROOT / "protobuf" / f"{name}.proto").read_text(encoding="utf-8")
                request = re.search(r"message UpsertPointTableRequest\s*\{(.*?)\}", source, re.S).group(1)
                table = re.search(r"message PointTable\s*\{(.*?)\}", source, re.S).group(1)
                self.assertRegex(request, r"optional uint64 expected_revision\s*=\s*4;")
                self.assertRegex(request, r"optional string template_bindings_json\s*=\s*5;")
                self.assertRegex(request, r"LinkConfig expected_link_config\s*=\s*6;")
                link_request = re.search(r"message UpsertLinkRequest\s*\{(.*?)\}", source, re.S).group(1)
                self.assertRegex(link_request, r"LinkConfig expected_config\s*=\s*3;")
                self.assertRegex(link_request, r"optional uint64 expected_point_table_revision\s*=\s*4;")
                self.assertRegex(table, r"uint64 revision\s*=\s*3;")
                self.assertRegex(table, r"string template_bindings_json\s*=\s*4;")

    def test_atomic_save_contract(self):
        for name in ("ModbusRTU", "ModbusTCP"):
            with self.subTest(name=name):
                source = (ROOT / "src" / name / f"{name}LinkManager.cc").read_text(encoding="utf-8")
                method = source.split("grpc::Status LinkManager::UpsertPointTable(", 1)[1].split("grpc::Status LinkManager::GetPointTable(", 1)[0]
                self.assertIn("ABORTED", method)
                self.assertIn("ValidateTemplateBindings", method)
                self.assertLess(method.index("expected_revision()"), method.index("pointTableStore_.Save("))
                self.assertLess(method.index("pointTableStore_.Save("), method.index("pointTable = std::move(next)"))
                self.assertLess(method.index("pointTableStore_.Save("), method.index("dataCenter_.UpsertConnTags("))
                self.assertIn("has_expected_link_config()", method)
                self.assertNotIn("StartLink(", method)

    def test_import_uses_target_revision(self):
        for name in ("Rtu", "Tcp"):
            with self.subTest(name=name):
                source = (ROOT / "src" / "ConfigPusher" / f"ConfigPusherApplyModbus{name}.cc").read_text(encoding="utf-8")
                self.assertIn("has_template_bindings_json()", source)
                self.assertIn("GetPointTable(", source)
                self.assertIn("set_expected_revision(currentTable.revision())", source)
                self.assertIn("currentTable.revision() == 0", source)

    def test_link_config_save_checks_point_table_revision(self):
        for name in ("ModbusRTU", "ModbusTCP"):
            with self.subTest(name=name):
                source = (ROOT / "src" / name / f"{name}LinkManager.cc").read_text(encoding="utf-8")
                method = source.split("grpc::Status LinkManager::UpsertLink(", 1)[1].split("grpc::Status LinkManager::RenameLink(", 1)[0]
                self.assertIn("has_expected_point_table_revision()", method)
                self.assertIn("request.expected_point_table_revision()", method)
                self.assertIn("ABORTED", method)


if __name__ == "__main__":
    unittest.main()
