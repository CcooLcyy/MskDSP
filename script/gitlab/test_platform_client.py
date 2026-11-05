"""下位机平台上传身份、安全边界及真实 gRPC 契约测试。"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import platform_client as client


class IdentityTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.archive = self.root / "mskdsp-test"
        self.archive.write_bytes(b"installer-bytes")
        self.data = dict(product="mskdsp-lower", version="1.0-ci-123", package_version="1.0-ci-123-linux-x64",
                         platform="linux-x64", file_name=self.archive.name, size=self.archive.stat().st_size,
                         sha256=hashlib.sha256(self.archive.read_bytes()).hexdigest(), image_id="sha256:" + "a" * 64,
                         source_repository="https://git.example/mskdsp/mskdsp.git", source_ref="master",
                         source_sha="b" * 40, build_time="2026-10-19T01:02:03Z", channel="ci", pipeline_id="123")
        self.path = self.root / "metadata.json"
        self.env = dict(CI_PIPELINE_SOURCE="push", CI_COMMIT_BRANCH="master", CI_COMMIT_SHA="b" * 40,
                        CI_PIPELINE_ID="123", CI_PROJECT_URL=self.data["source_repository"],
                        CI_SERVER_URL="https://git.example", CI_JOB_TOKEN="not-a-real-secret")

    def write(self):
        self.path.write_text(json.dumps(self.data), encoding="utf-8")

    def test_valid_identity_and_pipeline_id_not_sent_to_proto(self):
        self.write()
        archive, metadata = client.load_metadata(self.path, self.env)
        self.assertEqual(archive, self.archive)
        self.assertNotIn("pipeline_id", metadata)
        self.assertEqual(metadata["platform"], "linux-x64")

    def test_rejects_modified_file_and_traversal(self):
        for field, value in (("sha256", "c" * 64), ("size", 1), ("file_name", "../installer"),
                             ("product", "mskdsp-platform"), ("image_id", "bad"), ("pipeline_id", "122"),
                             ("source_repository", "https://user:secret@git.example/repo"),
                             ("source_sha", "c" * 40), ("build_time", "2026-10-19")):
            with self.subTest(field=field):
                original = self.data[field]
                self.data[field] = value
                self.write()
                with self.assertRaises(client.ClientError):
                    client.load_metadata(self.path, self.env)
                self.data[field] = original

    def test_rejects_mr_beta_and_tag_context(self):
        self.write()
        for change in (dict(CI_PIPELINE_SOURCE="merge_request_event"), dict(CI_COMMIT_BRANCH="beta/1"),
                       dict(CI_COMMIT_TAG="v1"), dict(CI_PIPELINE_SOURCE="schedule")):
            with self.subTest(change=change), self.assertRaises(client.ClientError):
                client.load_metadata(self.path, dict(self.env, **change))

    def test_plaintext_only_explicit_private_ip(self):
        self.assertEqual(client.validate_endpoint("192.168.1.2:50051", plaintext=True), "192.168.1.2:50051")
        for endpoint in ("example.com:443", "8.8.8.8:443", "user:pass@192.168.1.2:443"):
            with self.assertRaises(client.ClientError):
                client.validate_endpoint(endpoint, plaintext=True)

    # 只允许真实 ARM64 Beta，禁止 x64 Beta 或修改通道冒充发布。
    def test_beta_publication_requires_arm64_and_matching_branch(self):
        self.data.update(platform="linux-arm64", channel="beta", source_ref="beta/0.4.0",
                         version="0.4.0-beta-p7-b42-j50-bbbbbbb",
                         package_version="0.4.0-beta-p7-b42-j50-bbbbbbb-linux-arm64")
        env = self.env | {"CI_COMMIT_BRANCH": "beta/0.4.0"}
        self.write()
        _, metadata = client.load_metadata(self.path, env)
        self.assertEqual(metadata["channel"], "beta")
        for changes in ({"platform": "linux-x64"}, {"channel": "ci"},
                        {"version": "0.4.0-ci-p7-b42"}, {"source_ref": "beta/0.4.1"}):
            original = dict(self.data)
            self.data.update(changes)
            self.write()
            with self.subTest(changes=changes), self.assertRaises(client.ClientError):
                client.load_metadata(self.path, env)
            self.data = original

    def test_branch_head_uses_environment_auth_and_rejects_old_sha(self):
        with patch.object(client.subprocess, "run") as run:
            run.return_value.returncode = 0
            run.return_value.stdout = "b" * 40 + "\trefs/heads/master\n"
            client.check_current_head(self.data, self.env)
            arguments, keywords = run.call_args
            self.assertNotIn(self.env["CI_JOB_TOKEN"], " ".join(arguments[0]))
            self.assertEqual(keywords["env"]["GIT_CONFIG_COUNT"], "1")
            run.return_value.stdout = "c" * 40 + "\trefs/heads/master\n"
            with self.assertRaises(client.ClientError):
                client.check_current_head(self.data, self.env)

    # 防过期校验必须查询 Beta 本身，不能错误地比较主分支。
    def test_beta_head_check_uses_frozen_beta_ref(self):
        metadata = self.data | {"source_ref": "beta/0.4.0"}
        with patch.object(client.subprocess, "run") as run:
            run.return_value.returncode = 0
            run.return_value.stdout = "b" * 40 + "\trefs/heads/beta/0.4.0\n"
            client.check_current_head(metadata, self.env)
            self.assertEqual(run.call_args.args[0][-1], "refs/heads/beta/0.4.0")
            run.return_value.stdout = "c" * 40 + "\trefs/heads/beta/0.4.0\n"
            with self.assertRaises(client.ClientError):
                client.check_current_head(metadata, self.env)

    def test_public_manifest_is_unauthenticated_and_validates_identity(self):
        manifest = {name: self.data[name] for name in ("product", "version", "package_version", "platform", "image_id", "channel")}
        manifest.update(schema_version=1, source=dict(repository=self.data["source_repository"],
                                                     ref=self.data["source_ref"], sha=self.data["source_sha"]),
                        asset=dict(name=self.data["file_name"], size=self.data["size"], sha256=self.data["sha256"]))
        with patch.object(client, "urlopen") as opening:
            opening.return_value.__enter__.return_value.read.return_value = json.dumps(manifest).encode()
            client.confirm_manifest("http://192.168.1.2:8082", self.data)
            request = opening.call_args.args[0]
            self.assertEqual(request.full_url, "http://192.168.1.2:8082/mskdsp-lower-x64/ci/latest.json")
            self.assertIsNone(request.get_header("Authorization"))
            manifest["asset"]["sha256"] = "0" * 64
            opening.return_value.__enter__.return_value.read.return_value = json.dumps(manifest).encode()
            with self.assertRaises(client.ClientError):
                client.confirm_manifest("http://192.168.1.2:8082", self.data)

    def test_cli_hides_sensitive_exception(self):
        import io
        with patch.object(client, "upload", side_effect=ValueError("Authorization: Bearer sensitive-token")), \
                patch.object(client.sys, "stderr", new_callable=io.StringIO) as output:
            self.assertEqual(client.main(["--metadata", str(self.path)]), 1)
            self.assertNotIn("sensitive-token", output.getvalue())

    # Beta 公开清单从独立路径读取，并核对冻结来源、大小及摘要。
    def test_beta_public_manifest_uses_beta_path(self):
        self.data.update(platform="linux-arm64", channel="beta", source_ref="beta/0.4.0",
                         version="0.4.0-beta-p7-b42-j50-bbbbbbb")
        manifest = {name: self.data[name] for name in ("product", "version", "package_version", "platform", "image_id", "channel")}
        manifest.update(schema_version=1, source=dict(repository=self.data["source_repository"],
                                                     ref=self.data["source_ref"], sha=self.data["source_sha"]),
                        asset=dict(name=self.data["file_name"], size=self.data["size"], sha256=self.data["sha256"]))
        with patch.object(client, "urlopen") as opening:
            opening.return_value.__enter__.return_value.read.return_value = json.dumps(manifest).encode()
            client.confirm_manifest("http://192.168.1.2:8082", self.data)
            request = opening.call_args.args[0]
            self.assertEqual(request.full_url, "http://192.168.1.2:8082/mskdsp-lower/beta/latest.json")
            self.assertIsNone(request.get_header("Authorization"))


@unittest.skipUnless(importlib.util.find_spec("grpc") and importlib.util.find_spec("grpc_tools"), "需要 Python gRPC 工具镜像")
class GrpcContractTests(unittest.TestCase):
    setUp = IdentityTests.setUp
    write = IdentityTests.write
    def test_real_random_port_resume_publish_and_catalog(self):
        self.exercise("success")

    def test_real_interrupted_stream_resumes_at_persisted_offset(self):
        self.exercise("interrupt")

    def test_real_rejected_integrity_never_publishes(self):
        self.exercise("invalid")

    def test_real_outdated_branch_never_publishes(self):
        self.exercise("stale")

    # Beta 契约复用完整性失败、过期分支与丢失发布响应等原始断言。
    def test_real_beta_channel_contract(self):
        for scenario in ("success", "interrupt", "invalid", "stale"):
            with self.subTest(scenario=scenario):
                self.exercise(scenario, channel="beta")

    def exercise(self, scenario, channel="ci"):
        import grpc
        from concurrent.futures import ThreadPoolExecutor
        if channel == "beta":
            self.data.update(platform="linux-arm64", channel="beta", source_ref="beta/0.4.0",
                             version="0.4.0-beta-p7-b42-j50-bbbbbbb",
                             package_version="0.4.0-beta-p7-b42-j50-bbbbbbb-linux-arm64")
            self.env["CI_COMMIT_BRANCH"] = "beta/0.4.0"
        self.write()
        archive, metadata = client.load_metadata(self.path, self.env)
        protocol = Path(os.environ.get("PLATFORM_PROTO_TEST_ROOT", Path(__file__).resolve().parents[3] /
                                       "MskDSPPlatform/platform-proto"))
        with tempfile.TemporaryDirectory() as generated:
            client.generate_bindings(protocol, generated)
            with client.load_bindings(generated) as (messages, services):
                expected = messages.PackageMetadata(**metadata)
                payload = archive.read_bytes()
                state = {"received": 4, "published": False, "completed": False, "publish_calls": 0, "streams": 0}

                class Service(services.SoftwarePublishServiceServicer, services.SoftwareCatalogServiceServicer):
                    def authorize(self, context):
                        if dict(context.invocation_metadata()).get("authorization") != "Bearer test-token":
                            context.abort(grpc.StatusCode.UNAUTHENTICATED, "测试令牌错误")

                    def upload(self):
                        return messages.Upload(id="upload-1", metadata=expected, received_bytes=state["received"],
                                               state="completed" if state["completed"] else "uploading",
                                               package_id="package-1" if state["completed"] else "")

                    def package(self):
                        return messages.Package(id="package-1", metadata=expected,
                                                channels=[channel] if state["published"] else [],
                                                ever_published=state["published"])

                    def CreateUpload(self, request, context):
                        self.authorize(context)
                        assert request.metadata == expected
                        return messages.CreateUploadResponse(upload=self.upload())

                    def GetUpload(self, request, context):
                        self.authorize(context)
                        return messages.GetUploadResponse(upload=self.upload())

                    def UploadPackage(self, requests, context):
                        self.authorize(context)
                        state["streams"] += 1
                        for request in requests:
                            assert request.offset == state["received"]
                            assert request.data == payload[request.offset:request.offset + len(request.data)]
                            state["received"] += len(request.data)
                            if scenario == "interrupt" and state["streams"] == 1:
                                context.abort(grpc.StatusCode.UNAVAILABLE, "模拟分块落盘后连接中断")
                        return messages.UploadPackageResponse(upload=self.upload())

                    def CompleteUpload(self, request, context):
                        self.authorize(context)
                        assert state["received"] == len(payload)
                        if scenario == "invalid":
                            context.abort(grpc.StatusCode.INVALID_ARGUMENT, "模拟安装包完整性校验失败")
                        state["completed"] = True
                        return messages.CompleteUploadResponse(package=self.package())

                    def PublishRelease(self, request, context):
                        self.authorize(context)
                        assert state["completed"] and request.channel == channel
                        state["published"] = True
                        state["publish_calls"] += 1
                        if state["publish_calls"] == 1:
                            context.abort(grpc.StatusCode.UNAVAILABLE, "模拟发布成功但响应丢失")
                        return messages.PublishReleaseResponse(package=self.package())

                    def GetPackage(self, request, context):
                        assert state["published"]
                        return messages.GetPackageResponse(package=self.package())

                    def ListPackages(self, request, context):
                        assert request.channel == channel and request.platform == metadata["platform"]
                        return messages.ListPackagesResponse(packages=[self.package()] if state["published"] else [])

                server = grpc.server(ThreadPoolExecutor(max_workers=3))
                service = Service()
                services.add_SoftwarePublishServiceServicer_to_server(service, server)
                services.add_SoftwareCatalogServiceServicer_to_server(service, server)
                port = server.add_insecure_port("127.0.0.1:0")
                server.start()
                try:
                    with grpc.insecure_channel(f"127.0.0.1:{port}") as channel:
                        heads = []
                        def before():
                            heads.append(True)
                            if scenario == "stale":
                                raise client.ClientError("远端分支已变化")

                        def transfer():
                            return client.transfer(services.SoftwarePublishServiceStub(channel),
                                                   services.SoftwareCatalogServiceStub(channel), messages,
                                                   archive, metadata, "test-token", before_publish=before,
                                                   retry_delay=0, chunk_size=3)

                        if scenario in ("invalid", "stale"):
                            with self.assertRaises(client.ClientError):
                                transfer()
                            self.assertFalse(state["published"])
                            self.assertEqual(state["publish_calls"], 0)
                        else:
                            result = transfer()
                            self.assertEqual(result["package_id"], "package-1")
                            self.assertEqual(result["state"], "published")
                            self.assertTrue(heads)
                            if scenario == "interrupt":
                                self.assertEqual(state["streams"], 2)
                finally:
                    server.stop(0).wait()


if __name__ == "__main__":
    unittest.main()
