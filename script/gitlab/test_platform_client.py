"""下位机平台上传身份、安全边界及真实 gRPC 契约测试。"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import platform_client as client
import lower_metadata
from test_github_environment import github_environment


class IdentityTests(unittest.TestCase):
    # GitHub 上传只能消费本次运行、重跑、架构和真实仓库的冻结产物。
    def test_github_publish_identity_and_public_source_fetch(self):
        env = github_environment(LOWER_JOB_ARCH="x64")
        identity = lower_metadata.create_identity("1.0.0", "x64", env)
        identity = lower_metadata.package_identity(identity, env | {"LOWER_JOB_PHASE": "package"})
        self.archive = self.root / ("mskdsp-" + identity["package_version"])
        self.archive.write_bytes(b"installer-bytes")
        self.data.update(identity, file_name=self.archive.name, size=self.archive.stat().st_size,
                         sha256=hashlib.sha256(self.archive.read_bytes()).hexdigest())
        self.write()
        publish = env | {"LOWER_JOB_PHASE": "publish"}
        _, metadata = client.load_metadata(self.path, publish)
        self.assertNotIn("github_run_attempt", metadata)
        expected = metadata["source_sha"] + "\trefs/heads/master\n"
        with patch.object(client.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, expected)) as run:
            client.check_current_head(metadata, publish)
            self.assertNotIn("CI_JOB_TOKEN", run.call_args.kwargs["env"])
        for changes in ({"GITHUB_RUN_ATTEMPT": "2"}, {"GITHUB_RUN_ID": "101"},
                        {"LOWER_JOB_ARCH": "arm64"}, {"GITHUB_REPOSITORY": "other/repo"}):
            with self.subTest(changes=changes), self.assertRaises(client.ClientError):
                client.load_metadata(self.path, publish | changes)

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

    def test_public_plaintext_requires_opt_in_and_a_valid_endpoint(self):
        endpoint = "8.8.8.8:50000"
        self.assertEqual(client.validate_endpoint(endpoint, plaintext=True, allow_public_plaintext=True), endpoint)
        with self.assertRaises(client.ClientError):
            client.validate_endpoint("user:secret@8.8.8.8:50000", plaintext=True, allow_public_plaintext=True)

    # 退役 Beta 在元数据入口被拒绝，不能更换通道冒充 CI 或 Stable。
    def test_beta_publication_is_rejected_in_all_contexts(self):
        historical = self.data | dict(platform="linux-arm64", channel="beta", source_ref="beta/0.4.0",
                                      version="0.4.0-beta-p7-b42-j50-bbbbbbb",
                                      package_version="0.4.0-beta-p7-b42-j50-bbbbbbb-linux-arm64")
        for env in (self.env, self.env | {"CI_COMMIT_BRANCH": "beta/0.4.0"},
                    self.env | {"CI_COMMIT_BRANCH": "", "CI_COMMIT_TAG": "v0.4.0",
                                "CI_COMMIT_REF_PROTECTED": "true"}):
            for channel in ("beta", "ci", "stable"):
                self.data = historical | {"channel": channel}
                self.write()
                with self.subTest(env=env, channel=channel), self.assertRaises(client.ClientError):
                    client.load_metadata(self.path, env)

    # 绕过元数据加载直接调用上传函数时，Beta 也必须在接触平台 RPC 前拒绝。
    def test_direct_beta_transfer_never_calls_platform(self):
        from unittest.mock import Mock
        publisher, catalog = Mock(), Mock()
        metadata = self.data | {"channel": "beta", "platform": "linux-arm64", "source_ref": "beta/0.4.0"}
        with self.assertRaisesRegex(client.ClientError, "仅允许 CI/Stable"):
            client.transfer(publisher, catalog, None, self.archive, metadata, "test-token", before_publish=lambda: None)
        self.assertEqual(publisher.mock_calls, [])
        self.assertEqual(catalog.mock_calls, [])
        with patch.object(client.subprocess, "run") as run:
            with self.assertRaises(client.ClientError):
                client.check_current_head(metadata, self.env)
            run.assert_not_called()

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

    # 防过期校验必须查询冻结的实际主干，不能固定使用 master。
    def test_main_head_check_uses_frozen_source_ref(self):
        metadata = self.data | {"source_ref": "main"}
        with patch.object(client.subprocess, "run") as run:
            run.return_value.returncode = 0
            run.return_value.stdout = "b" * 40 + "\trefs/heads/main\n"
            client.check_current_head(metadata, self.env)
            self.assertEqual(run.call_args.args[0][-1], "refs/heads/main")
            run.return_value.stdout = "c" * 40 + "\trefs/heads/main\n"
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

    # Stable 冻结纯版本与受保护标签，禁止 x64 和跨通道冒充。
    def test_stable_identity_requires_protected_arm64_tag(self):
        self.data.update(platform="linux-arm64", channel="stable", source_ref="v0.4.0",
                         version="0.4.0", package_version="0.4.0-linux-arm64")
        env = self.env | {"CI_COMMIT_BRANCH": "", "CI_COMMIT_TAG": "v0.4.0", "CI_COMMIT_REF_PROTECTED": "true"}
        self.write()
        _, metadata = client.load_metadata(self.path, env)
        self.assertEqual(metadata["channel"], "stable")
        for changes in ({"platform": "linux-x64"}, {"channel": "beta"},
                        {"version": "0.4.0-beta-p7-b42"}, {"package_version": "0.4.0-ci-linux-arm64"}):
            original = dict(self.data)
            self.data.update(changes)
            self.write()
            with self.subTest(changes=changes), self.assertRaises(client.ClientError):
                client.load_metadata(self.path, env)
            self.data = original
        self.write()
        with self.assertRaises(client.ClientError):
            client.load_metadata(self.path, env | {"CI_COMMIT_REF_PROTECTED": "false"})

    # 使用真实 Git 图验证历史祖先关系，避免用 HEAD 相等误拒维护分支前进。
    def test_stable_source_ancestry_tag_integrity_and_version_series(self):
        repository = self.root / "source.git"
        repository.mkdir()

        def git(*arguments):
            return subprocess.run(["git", *arguments], cwd=repository, check=True,
                                  capture_output=True, text=True).stdout.strip()

        git("init", "--initial-branch=unrelated")
        git("config", "user.name", "发布门禁测试")
        git("config", "user.email", "release-test@example.invalid")
        git("commit", "--allow-empty", "-m", "维护修复")
        sha = git("rev-parse", "HEAD")
        metadata = self.data | {"channel": "stable", "source_ref": "v0.8.1", "version": "0.8.1",
                               "source_sha": sha, "source_repository": str(repository)}
        env = self.env | {"CI_COMMIT_REF_PROTECTED": "true"}
        git("tag", "v0.8.1")
        git("branch", "beta/0.8.1")
        git("branch", "release/0.9")
        # 只有 Beta 或错误版本维护线可达时拒绝，即使标签精确匹配。
        with self.assertRaises(client.ClientError):
            client.check_current_head(metadata, env)
        git("branch", "release/0.8")
        client.check_current_head(metadata, env)
        git("checkout", "release/0.8")
        git("commit", "--allow-empty", "-m", "维护线继续前进")
        client.check_current_head(metadata, env)
        # 附注标签同样接受 peeled commit，标签移动或删除必须拒绝。
        git("tag", "-d", "v0.8.1")
        git("tag", "-a", "v0.8.1", sha, "-m", "正式版本")
        client.check_current_head(metadata, env)
        git("tag", "-f", "v0.8.1", "HEAD")
        with self.assertRaises(client.ClientError):
            client.check_current_head(metadata, env)
        git("tag", "-d", "v0.8.1")
        with self.assertRaises(client.ClientError):
            client.check_current_head(metadata, env)
        git("tag", "v0.8.1", sha)
        git("branch", "-D", "beta/0.8.1", "release/0.9")
        git("checkout", "unrelated")
        git("branch", "-D", "release/0.8")
        for branch in ("master", "main"):
            git("branch", branch, sha)
            client.check_current_head(metadata, env)
            git("branch", "-D", branch)
        # 允许分支存在但历史不包含标签提交时拒绝。
        git("checkout", "--orphan", "master")
        git("commit", "--allow-empty", "-m", "不相关主干")
        with self.assertRaises(client.ClientError):
            client.check_current_head(metadata, env)

    # 来源查询失败不可继续上传，不传播底层认证错误。
    def test_stable_source_query_failure_is_closed(self):
        metadata = self.data | {"channel": "stable", "source_ref": "v0.8.1", "version": "0.8.1"}
        with patch.object(client.subprocess, "run") as run:
            run.return_value.returncode = 1
            run.return_value.stdout = ""
            with self.assertRaises(client.ClientError):
                client.check_current_head(metadata, self.env | {"CI_COMMIT_REF_PROTECTED": "true"})

    def test_cli_hides_sensitive_exception(self):
        import io
        with patch.object(client, "upload", side_effect=ValueError("Authorization: Bearer sensitive-token")), \
                patch.object(client.sys, "stderr", new_callable=io.StringIO) as output:
            self.assertEqual(client.main(["--metadata", str(self.path)]), 1)
            self.assertNotIn("sensitive-token", output.getvalue())

    # Stable 公开清单走正式路径并核对冻结的纯版本、标签和完整性字段。
    def test_stable_manifest_uses_stable_path(self):
        self.data.update(platform="linux-arm64", channel="stable", source_ref="v0.4.0",
                         version="0.4.0", package_version="0.4.0-linux-arm64")
        manifest = {name: self.data[name] for name in ("product", "version", "package_version", "platform", "image_id", "channel")}
        manifest.update(schema_version=1, source=dict(repository=self.data["source_repository"],
                                                     ref=self.data["source_ref"], sha=self.data["source_sha"]),
                        asset=dict(name=self.data["file_name"], size=self.data["size"], sha256=self.data["sha256"]))
        with patch.object(client, "urlopen") as opening:
            opening.return_value.__enter__.return_value.read.return_value = json.dumps(manifest).encode()
            client.confirm_manifest("http://192.168.1.2:8082", self.data)
            request = opening.call_args.args[0]
            self.assertEqual(request.full_url, "http://192.168.1.2:8082/mskdsp-lower/stable/latest.json")
            self.assertIsNone(request.get_header("Authorization"))
            manifest["asset"]["size"] += 1
            opening.return_value.__enter__.return_value.read.return_value = json.dumps(manifest).encode()
            with self.assertRaises(client.ClientError):
                client.confirm_manifest("http://192.168.1.2:8082", self.data)

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

    # 仅凭发布响应完成，不访问目录。响应丢失后必须收到重试的成功响应。
    def test_publish_response_only_retries_until_acknowledged(self):
        self.exercise("success", response_only=True)

    # 即使服务端已发布，所有响应均丢失也不能判为成功。
    def test_publish_response_only_reports_unknown_after_all_responses_lost(self):
        self.exercise("lost", response_only=True)

    # 原 Beta 成功发布场景迁至 ARM64 CI，保留原始完整性、过期和响应丢失断言。
    def test_real_arm64_ci_channel_contract(self):
        for scenario in ("success", "interrupt", "invalid", "stale"):
            with self.subTest(scenario=scenario):
                self.exercise(scenario, platform="linux-arm64")

    # Stable 保留原成功、续传、完整性失败、过期拒绝及响应丢失断言。
    def test_real_stable_channel_contract(self):
        for scenario in ("success", "interrupt", "invalid", "stale"):
            with self.subTest(scenario=scenario):
                self.exercise(scenario, channel="stable")

    def exercise(self, scenario, channel="ci", platform="linux-x64", *, response_only=False):
        import grpc
        from concurrent.futures import ThreadPoolExecutor
        if platform == "linux-arm64":
            self.data.update(platform=platform, package_version=self.data["version"] + "-" + platform)
        if channel == "stable":
            self.data.update(platform="linux-arm64", channel="stable", source_ref="v0.4.0",
                             version="0.4.0", package_version="0.4.0-linux-arm64")
            self.env.update(CI_COMMIT_BRANCH="", CI_COMMIT_TAG="v0.4.0", CI_COMMIT_REF_PROTECTED="true")
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
                        if state["publish_calls"] == 1 or scenario == "lost":
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
                    with grpc.insecure_channel(f"127.0.0.1:{port}") as rpc_channel:
                        heads = []
                        def before():
                            heads.append(True)
                            if scenario == "stale":
                                raise client.ClientError("远端分支已变化")

                        def transfer():
                            catalog = None if response_only else services.SoftwareCatalogServiceStub(rpc_channel)
                            return client.transfer(services.SoftwarePublishServiceStub(rpc_channel), catalog, messages,
                                                   archive, metadata, "test-token", before_publish=before,
                                                   retry_delay=0, chunk_size=3)

                        if scenario in ("invalid", "stale"):
                            with self.assertRaises(client.ClientError):
                                transfer()
                            self.assertFalse(state["published"])
                            self.assertEqual(state["publish_calls"], 0)
                        elif scenario == "lost":
                            with self.assertRaisesRegex(client.ClientError, "无法确认"):
                                transfer()
                            self.assertTrue(state["published"])
                            self.assertEqual(state["publish_calls"], 4)
                        else:
                            result = transfer()
                            self.assertEqual(result["package_id"], "package-1")
                            self.assertEqual(result["state"], "published")
                            self.assertTrue(heads)
                            if response_only:
                                self.assertEqual(state["publish_calls"], 2)
                            if scenario == "interrupt":
                                self.assertEqual(state["streams"], 2)
                finally:
                    server.stop(0).wait()


if __name__ == "__main__":
    unittest.main()
