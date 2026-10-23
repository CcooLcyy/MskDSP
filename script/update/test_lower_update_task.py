"""独立更新任务隔离测试：所有文件和 Docker/systemd 替身均位于临时目录。"""
import hashlib
import io
from contextlib import closing
import importlib.util
import json
import os
import sqlite3
import gzip
from pathlib import Path
import tempfile
import tarfile
import sys
import unittest
from unittest.mock import MagicMock, patch
import uuid

SPEC = importlib.util.spec_from_file_location("lower_update_task", Path(__file__).with_name("lower_update_task.py"))
task = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(task)


class FakeBackend:
    def __init__(self):
        self.started = []
        self.active = False
        self.fail_start = False
        self.fail_install = False
        self.fail_health = False
        self.fail_restore = False
        self.restored = False
        self.previous_deleted = []
        self.image = "sha256:" + "a" * 64

    def schedule(self, operation, helper):
        self.started.append(operation)
        if self.fail_start:
            raise RuntimeError("模拟 systemd 受理失败")
        self.active = True

    def scheduled(self, operation):
        return self.active and operation in self.started

    def preflight(self, package, expected):
        return {"Image": self.image, "modules": ["DataCenter"]}

    def pin_image(self, image, tag):
        pass

    def install(self, package, log):
        self.image = "sha256:" + "b" * 64
        if self.fail_install:
            raise RuntimeError("模拟安装失败")

    def health(self, image, modules):
        if self.fail_health and not self.restored:
            raise RuntimeError("模拟本地健康失败")
        return self.image

    def restore(self, snapshot, log):
        self.restored = True
        if self.fail_restore:
            raise RuntimeError("模拟恢复失败")
        self.image = snapshot["Image"]

    def remove_pin(self, tag):
        self.previous_deleted.append(tag)


class UpdateTaskTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.host = self.root / "host"
        for name in ("module", "conf", "log"):
            (self.host / name).mkdir(parents=True)
            (self.host / name / "original").write_text(name)
        self.package = self.root / "upload"
        self.package.write_bytes("隔离安装包".encode())
        self.sha = hashlib.sha256(self.package.read_bytes()).hexdigest()
        self.backend = FakeBackend()
        self.manager = task.TaskManager(self.root / "tasks", self.host, self.backend, secure=False)
        self.op = str(uuid.uuid4())
        self.expected = "sha256:" + "b" * 64

    def submit(self, operation=None, sha=None):
        return self.manager.submit(operation or self.op, self.package, sha or self.sha, self.expected)

    # 相同任务重复提交必须返回原记录，不重复启动独立任务。
    def test_idempotent_submit(self):
        first = self.submit()
        self.assertEqual(first["phase"], "accepted")
        self.assertEqual(self.submit()["operation_id"], self.op)
        self.assertEqual(self.backend.started, [self.op])

    # 相同编号不同摘要必须拒绝，不可替换已经受理的包。
    def test_conflicting_content_rejected(self):
        self.submit()
        with self.assertRaisesRegex(ValueError, "内容"):
            self.submit(sha="f" * 64)

    # 上传校验失败必须在受理和调度前退出。
    def test_bad_digest(self):
        with self.assertRaisesRegex(ValueError, "摘要"):
            self.submit(sha="f" * 64)
        self.assertFalse(self.backend.started)

    # 已写 accepted 但调度失败的重试必须重新调度同一个编号。
    def test_accepted_reconciles_schedule_failure(self):
        self.backend.fail_start = True
        with self.assertRaises(RuntimeError):
            self.submit()
        self.backend.fail_start = False
        result = self.submit()
        self.assertEqual(result["phase"], "accepted")
        self.assertEqual(self.backend.started, [self.op, self.op])

    # 全局互斥覆盖已经受理但尚未运行的窗口。
    def test_second_operation_rejected(self):
        self.submit()
        with self.assertRaisesRegex(ValueError, "任务"):
            self.submit(str(uuid.uuid4()))

    # 新版本成功后保留旧镜像和模块/配置，日志独立保留。
    def test_success_preserves_one_previous(self):
        self.submit()
        result = self.manager.run(self.op)
        self.assertEqual(result["phase"], "succeeded")
        previous = json.loads((self.root / "tasks" / "previous.json").read_text())
        self.assertEqual(previous["operation_id"], self.op)
        self.assertTrue((self.root / "tasks" / self.op / "backup" / "conf" / "original").exists())
        self.assertEqual((self.host / "log" / "original").read_text(), "log")
        self.assertIn("默认容器部署校验通过", result["log_tail"])

    # 健康失败必须自动恢复模块、配置和旧镜像，不能报告升级成功。
    def test_health_failure_restores(self):
        self.submit()
        self.backend.fail_health = True
        result = self.manager.run(self.op)
        self.assertEqual(result["phase"], "rolled_back")
        self.assertTrue(self.backend.restored)
        self.assertEqual(result["actual_image_id"], "sha256:" + "a" * 64)
        self.assertFalse((self.root / "tasks" / self.op / "backup").exists())
        self.assertFalse((self.root / "tasks" / self.op / "package").exists())

    # 安装命令失败也必须自动恢复；恢复失败需要明确终态并保留材料。
    def test_rollback_failure_preserves_backup(self):
        self.submit()
        self.backend.fail_install = True
        self.backend.fail_restore = True
        result = self.manager.run(self.op)
        self.assertEqual(result["phase"], "rollback_failed")
        self.assertTrue((self.root / "tasks" / self.op / "backup").exists())
        with self.assertRaisesRegex(ValueError, "任务"):
            self.submit(str(uuid.uuid4()))

    # 两次成功更新只保留最近一次恢复材料，成功前不删除旧材料。
    def test_rotation_after_success_only(self):
        self.submit()
        self.manager.run(self.op)
        old_backup = self.root / "tasks" / self.op / "backup"
        second = str(uuid.uuid4())
        self.submit(second)
        self.assertTrue(old_backup.exists())
        self.manager.run(second)
        self.assertFalse(old_backup.exists())
        self.assertTrue((self.root / "tasks" / second / "backup").exists())

    # 符号链接上传文件不能成为 root 执行来源。
    @unittest.skipUnless(hasattr(Path, "symlink_to"), "系统不支持符号链接")
    def test_upload_symlink_rejected(self):
        link = self.root / "link"
        try:
            link.symlink_to(self.package)
        except OSError:
            self.skipTest("当前账户不能创建符号链接")
        with self.assertRaises((ValueError, OSError)):
            self.manager.submit(self.op, link, self.sha, self.expected)

    # task UUID 不允许目录穿越。
    def test_invalid_operation(self):
        with self.assertRaises(ValueError):
            self.submit("../escape")

    # accepted 的查询能调和调度回执丢失，仍使用原编号和 root helper。
    def test_query_reconciles_accepted(self):
        self.submit()
        self.backend.active = False
        self.manager.reconcile(self.op)
        self.assertEqual(self.backend.started, [self.op, self.op])

    # 安装过程被外部中断不能重新安装，需明确恢复材料仍保留。
    def test_interrupted_install_is_not_reinstalled(self):
        self.submit()
        state = self.manager.state(self.op)
        self.manager.save(state, "installing", "模拟已开始安装")
        self.backend.active = False
        self.assertEqual(self.manager.reconcile(self.op)["phase"], "rollback_failed")
        self.assertEqual(self.backend.started, [self.op])

    # 标准 module 中的相对 so 链接需保留，越界链接必须拒绝。
    def test_module_relative_link_preserved(self):
        library = self.host / "module" / "libX.so.1"
        library.write_text("模块")
        link = self.host / "module" / "libX.so"
        try:
            link.symlink_to(library.name)
        except OSError:
            self.skipTest("当前账户不能创建符号链接")
        self.submit()
        self.assertEqual(self.manager.run(self.op)["phase"], "succeeded")
        copied = self.root / "tasks" / self.op / "backup" / "module" / "libX.so"
        self.assertTrue(copied.is_symlink())
        self.assertEqual(copied.read_text(), "模块")

    # module 内链接不能越界读取配置或系统文件。
    def test_module_escaping_link_rejected(self):
        try:
            (self.host / "module" / "escape").symlink_to("../conf/original")
        except OSError:
            self.skipTest("当前账户不能创建符号链接")
        self.submit()
        self.assertEqual(self.manager.run(self.op)["phase"], "failed")

    # 成功提交之后清理历史目录失败只能记录警告，不能触发回退。
    def test_success_cleanup_failure_is_warning(self):
        self.submit()
        with patch.object(self.manager, "cleanup_terminal", side_effect=OSError("模拟清理失败")):
            result = self.manager.run(self.op)
        self.assertEqual(result["phase"], "succeeded")
        self.assertFalse(self.backend.restored)
        self.assertIn("清理失败", result["log_tail"])

    # 已恢复业务后清理失败不得误报恢复失败。
    def test_rollback_cleanup_failure_is_warning(self):
        self.submit()
        self.backend.fail_health = True
        with patch.object(self.manager, "cleanup_terminal", side_effect=OSError("模拟清理失败")):
            result = self.manager.run(self.op)
        self.assertEqual(result["phase"], "rolled_back")
        self.assertTrue(self.backend.restored)

    # 活跃 WAL 数据库必须在线备份，快照包含已提交事务且不混入旧 WAL/SHM。
    def test_sqlite_wal_snapshot_is_consistent(self):
        database = self.host / "conf" / "config.db"
        with closing(sqlite3.connect(database)) as connection:
            connection.execute("PRAGMA journal_mode=WAL")
            connection.execute("CREATE TABLE settings(value TEXT)")
            connection.execute("INSERT INTO settings VALUES ('已提交')")
            connection.commit()
            self.submit()
            self.assertEqual(self.manager.run(self.op)["phase"], "succeeded")
            backup = self.root / "tasks" / self.op / "backup" / "conf"
            self.assertFalse((backup / "config.db-wal").exists())
            self.assertFalse((backup / "config.db-shm").exists())
            with closing(sqlite3.connect(backup / "config.db")) as copied:
                self.assertEqual(copied.execute("SELECT value FROM settings").fetchone()[0], "已提交")

    # SQLite 一致快照失败时必须在替换业务之前终止。
    def test_sqlite_snapshot_failure_is_preflight_failure(self):
        with closing(sqlite3.connect(self.host / "conf" / "config.db")) as connection:
            connection.execute("CREATE TABLE test(value TEXT)")
        self.submit()
        with patch.object(task, "copy_database", side_effect=TimeoutError("快照超时")):
            result = self.manager.run(self.op)
        self.assertEqual(result["phase"], "failed")
        self.assertFalse(self.backend.restored)
        self.assertEqual(self.backend.image, "sha256:" + "a" * 64)

    # 回退候选若正好是原先前版本，清理必须保护 previous 引用的镜像。
    def test_rollback_cleanup_protects_previous_image(self):
        self.submit()
        self.backend.fail_health = True
        self.manager.run(self.op)
        previous_image = "sha256:" + "c" * 64
        task.atomic_json(self.root / "tasks" / "previous.json", {"operation_id": str(uuid.uuid4()), "image_id": previous_image})
        backend = task.Backend()
        self.manager.backend = backend
        data = self.manager.state(self.op)
        snapshot = {"Image": "sha256:" + "a" * 64, "update_candidate_tag": "mskdsp:previous"}
        with patch.object(backend, "remove_pin"), patch.object(backend, "command", return_value=json.dumps([{"Id": previous_image}])), patch.object(backend, "cleanup_image") as cleanup:
            self.manager.cleanup_terminal(data, snapshot, None)
        self.assertIn(previous_image, cleanup.call_args.args[1])

    # 文件锁实际互斥，不能只靠进程内布尔状态保护安装。
    def test_execution_lock_is_exclusive(self):
        with task.exclusive(self.root / "tasks" / "real.lock"):
            with self.assertRaises(OSError):
                with task.exclusive(self.root / "tasks" / "real.lock"):
                    self.fail("同一设备锁不得被第二个执行者获得")


class BackendContractTest(unittest.TestCase):
    def default_snapshot(self):
        return {"Image": "old", "State": {"Running": True, "Pid": 123},
                "Config": {"Entrypoint": ["./MskDSP"], "Cmd": None, "WorkingDir": "/opt/mskdsp", "User": "", "Env": []},
                "HostConfig": {"Binds": ["/dev:/dev", "/data/mskdsp/conf:/opt/mskdsp/conf", "/data/mskdsp/module:/opt/mskdsp/module", "/data/mskdsp/log:/opt/mskdsp/log"],
                               "NetworkMode": "host", "Privileged": True, "RestartPolicy": {"Name": "unless-stopped"}, "LogConfig": {"Type": "none"}}}

    # systemd 独立进程不能等待 SSH、分配 PTY 或关联调用端生存期。
    def test_systemd_argv(self):
        backend = task.Backend()
        with patch.object(backend, "command") as command:
            backend.schedule("abc", Path("/var/lib/mskdsp-update/abc/helper.py"))
        args = command.call_args.args
        self.assertEqual(args[0], "systemd-run")
        self.assertIn("--collect", args)
        self.assertNotIn("--wait", args)
        self.assertNotIn("--pty", args)
        self.assertEqual(args[-3:], ("run", "--operation-id", "abc"))

    # 恢复命令必须恢复默认部署的设备、三项业务挂载、网络和重启策略。
    def test_restore_argv(self):
        backend = task.Backend()
        with patch.object(backend, "command") as command:
            backend.restore({"Image": "sha256:" + "a" * 64}, Path("unused"))
        args = command.call_args.args
        self.assertEqual(args[:3], ("docker", "run", "-d"))
        self.assertIn("/dev:/dev", args)
        self.assertIn("/data/mskdsp/module:/opt/mskdsp/module", args)
        self.assertIn("/data/mskdsp/conf:/opt/mskdsp/conf", args)
        self.assertIn("/data/mskdsp/log:/opt/mskdsp/log", args)
        self.assertIn("unless-stopped", args)
        self.assertIn("host", args)

    # 自定义命令、环境变量或挂载必须在更改部署前拒绝。
    def test_custom_deployment_rejected(self):
        backend = task.Backend()
        data = {"Config": {"Entrypoint": ["custom"]}, "HostConfig": {}}
        with self.assertRaisesRegex(ValueError, "默认部署"):
            backend.validate_deployment(data)

    # 默认安装器部署可恢复；自定义 IPC、sysctl、入口环境必须拒绝。
    def test_default_deployment_and_custom_options(self):
        backend = task.Backend()
        for key, value in [(None, None), ("Sysctls", {"net.ipv4.ip_forward": "1"}), ("IpcMode", "host"), ("Runtime", "custom")]:
            data = self.default_snapshot()
            if key:
                data["HostConfig"][key] = value
            with patch.object(backend, "command", return_value=json.dumps([{"Config": {"Env": []}}])):
                if key:
                    with self.assertRaises(ValueError):
                        backend.validate_deployment(data)
                else:
                    backend.validate_deployment(data)

    # 特权容器的默认 SELinux 标记与 host 网络默认主机名应组合通过预检。
    def test_docker_normalized_defaults_accepted(self):
        backend = task.Backend()
        container_id = "418aed6321b7" + "a" * 52
        for options in (None, [], ["label=disable"]):
            for hostname in (None, "", container_id[:12], "daniel-H410M-K"):
                with self.subTest(options=options, hostname=hostname):
                    data = self.default_snapshot()
                    data["Id"] = container_id
                    data["HostConfig"]["SecurityOpt"] = options
                    data["Config"]["Hostname"] = hostname
                    before = json.dumps(data, sort_keys=True)
                    with patch.object(task.socket, "gethostname", return_value="daniel-H410M-K"), patch.object(backend, "command", return_value=json.dumps([{"Config": {"Env": []}}])):
                        backend.validate_deployment(data)
                    self.assertEqual(json.dumps(data, sort_keys=True), before)

    # 只兼容单项默认标记，不能放行显式定制、混合列表或错误类型。
    def test_custom_security_options_rejected(self):
        backend = task.Backend()
        for options in (["seccomp=unconfined"], ["apparmor=unconfined"], ["no-new-privileges"],
                        ["label=type:custom_t"], ["label=disable", "seccomp=unconfined"],
                        ["label=disable", "label=disable"], "label=disable", "", {}):
            with self.subTest(options=options):
                data = self.default_snapshot()
                data["HostConfig"]["SecurityOpt"] = options
                with patch.object(backend, "command", return_value=json.dumps([{"Config": {"Env": []}}])):
                    with self.assertRaisesRegex(ValueError, "SecurityOpt"):
                        backend.validate_deployment(data)

    # 默认安全标记不能绕过自定义主机名或原有默认部署约束。
    def test_normalized_defaults_keep_custom_guards(self):
        backend = task.Backend()
        cases = [("Config", "Hostname", "custom-host", "自定义主机名"),
                 ("HostConfig", "Privileged", False, "默认部署"),
                 ("HostConfig", "NetworkMode", "bridge", "默认部署"),
                 ("HostConfig", "CapDrop", ["ALL"], "CapDrop"),
                 ("Config", "Env", ["CUSTOM=1"], "自定义环境变量")]
        for section, key, value, error in cases:
            with self.subTest(key=key):
                data = self.default_snapshot()
                data["HostConfig"]["SecurityOpt"] = ["label=disable"]
                data["Config"]["Hostname"] = "daniel-H410M-K"
                data[section][key] = value
                with patch.object(task.socket, "gethostname", return_value="daniel-H410M-K"), patch.object(backend, "command", return_value=json.dumps([{"Config": {"Env": []}}])):
                    with self.assertRaisesRegex(ValueError, error):
                        backend.validate_deployment(data)

    # 安装与回退均依赖默认特权和 host 网络，不应传入主机名或安全选项覆盖。
    def test_install_and_restore_keep_docker_defaults(self):
        backend = task.Backend()
        process = MagicMock()
        process.wait.return_value = 0
        with tempfile.TemporaryDirectory() as temporary, patch.object(task.subprocess, "Popen", return_value=process) as popen:
            backend.install(Path("package"), Path(temporary) / "task.log")
        self.assertEqual(popen.call_args.args[0], ["/bin/bash", "package", "start"])
        self.assertEqual(popen.call_args.kwargs["env"]["MSKDSP_MANAGED_UPDATE"], "1")
        data = self.default_snapshot()
        data["HostConfig"]["SecurityOpt"] = ["label=disable"]
        data["Config"]["Hostname"] = "daniel-H410M-K"
        with patch.object(backend, "command") as command:
            backend.restore(data, Path("unused"))
        args = command.call_args.args
        self.assertIn("--privileged", args)
        self.assertEqual(args[args.index("--network") + 1], "host")
        self.assertNotIn("--security-opt", args)
        self.assertNotIn("--hostname", args)

    # 真实包解析在调用安装器前验证配置摘要、主机架构和受管理更新标记。
    def test_preflight_archive_identity_architecture_and_marker(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary) / "package"
            config = b'{"architecture":"amd64","os":"linux"}'
            expected = "sha256:" + hashlib.sha256(config).hexdigest()
            content = io.BytesIO()
            with tarfile.open(fileobj=content, mode="w") as archive:
                files = {expected[7:] + ".json": config, "manifest.json": json.dumps([{"Config": expected[7:] + ".json", "RepoTags": ["mskdsp:new"]}]).encode(),
                         "blobs/sha256/" + "c" * 64: b"x" * (2 * 1024**2)}
                for name, value in files.items():
                    member = tarfile.TarInfo(name)
                    member.size = len(value)
                    archive.addfile(member, io.BytesIO(value))
            package.write_bytes(b"#!/bin/bash\nMSKDSP_MANAGED_UPDATE_V1=1\n__ARCHIVE_BELOW__\n" + gzip.compress(content.getvalue()))
            backend = task.Backend()
            snapshot = self.default_snapshot()
            def command(*args, **kwargs):
                if args[:2] == ("docker", "info"):
                    return temporary
                if args[:3] == ("docker", "image", "inspect"):
                    return json.dumps([{"Config": {"Env": []}}])
                return ""
            with patch.object(backend, "command", side_effect=command), patch.object(backend, "inspect", return_value=snapshot), patch.object(backend, "module_sockets", return_value=["DataCenter"]), patch.object(backend, "health", return_value="old"), patch.object(task.shutil, "which", return_value="fake"), patch.object(task.platform, "machine", return_value="x86_64"):
                result = backend.preflight(package, expected)
                self.assertEqual(result["update_candidate_tag"], "mskdsp:new")
                with self.assertRaisesRegex(ValueError, "镜像身份"):
                    backend.preflight(package, "sha256:" + "f" * 64)
                with patch.object(task.platform, "machine", return_value="aarch64"):
                    with self.assertRaisesRegex(ValueError, "架构"):
                        backend.preflight(package, expected)
                package.write_bytes(package.read_bytes().replace(task.MANAGED_MARKER, b"OLD_PACKAGE=1"))
                with self.assertRaisesRegex(ValueError, "受管理"):
                    backend.preflight(package, expected)

    # 缺失任务有稳定错误标记；CLI 只在 stdout 返回一个 JSON。
    def test_cli_json_and_missing_operation_marker(self):
        with tempfile.TemporaryDirectory() as temporary:
            manager = task.TaskManager(Path(temporary), Path(temporary), FakeBackend(), secure=False)
            operation = str(uuid.uuid4())
            with self.assertRaisesRegex(ValueError, "LOWER_UPDATE_TASK_NOT_FOUND"):
                manager.query(operation)
            data = {"operation_id": operation, "phase": "succeeded", "message": "完成"}
            with patch.object(task, "ReadOnlyTaskManager", return_value=manager), patch.object(manager, "query", return_value=data), patch.object(manager, "reconcile") as reconcile, patch.object(task.os, "geteuid", return_value=0, create=True), patch.object(task, "ensure_secure"), patch.object(sys, "argv", ["helper", "query", "--operation-id", operation]), patch.object(sys, "stdout", new_callable=io.StringIO) as stdout:
                self.assertEqual(task.main(), 0)
                self.assertEqual(json.loads(stdout.getvalue()), data)
                reconcile.assert_not_called()

    # 平台连接状态不参与健康检查；检查仅使用本地管理端口及原模块。
    def test_health_uses_local_endpoints(self):
        backend = task.Backend()
        snapshot = {"Image": "old", "State": {"Running": True, "Pid": 123}}
        with patch.object(backend, "inspect", return_value=snapshot), patch.object(task.socket, "create_connection") as connection, patch.object(backend, "connect_unix", return_value=True) as unix:
            self.assertEqual(backend.health("old", ["DataCenter"], timeout=1), "old")
        self.assertEqual(connection.call_args.args[0], ("127.0.0.1", 17000))
        self.assertEqual(str(unix.call_args.args[0]).replace("\\", "/"), "/proc/123/root/opt/mskdsp/socket/DataCenter.sock")

    # 管理端口不通，不能因为容器运行就误报成功。
    def test_missing_manager_port_fails_health(self):
        backend = task.Backend()
        snapshot = {"Image": "old", "State": {"Running": True, "Pid": 123}}
        with patch.object(backend, "inspect", return_value=snapshot), patch.object(task.socket, "create_connection", side_effect=OSError("拒绝连接")), patch.object(task.time, "monotonic", side_effect=[0, 0, 2]), patch.object(task.time, "sleep"):
            with self.assertRaisesRegex(RuntimeError, "健康检查超时"):
                backend.health("old", [], timeout=1)

    # containerd 返回 manifest ID 时，契约实际身份必须返回已核验的 config ID。
    def test_verified_manifest_returns_config_identity(self):
        backend = task.Backend()
        snapshot = {"Image": "manifest-id", "State": {"Running": True, "Pid": 123}}
        with patch.object(backend, "inspect", return_value=snapshot), patch.object(backend, "image_matches", return_value=True), patch.object(task.socket, "create_connection"):
            self.assertEqual(backend.health("config-id", [], timeout=1), "config-id")
            self.assertEqual(backend.last_runtime_image, "manifest-id")

    # manifest/config 不同时，真实流式镜像校验需读取正确配置内容摘要。
    def test_streamed_image_identity(self):
        backend = task.Backend()
        config = b'{"architecture":"amd64","os":"linux"}'
        expected = "sha256:" + hashlib.sha256(config).hexdigest()
        archive = io.BytesIO()
        with tarfile.open(fileobj=archive, mode="w") as writer:
            member = tarfile.TarInfo("blobs/sha256/" + expected[7:])
            member.size = len(config)
            writer.addfile(member, io.BytesIO(config))
        process = MagicMock()
        process.stdout = io.BytesIO(archive.getvalue())
        process.wait.return_value = 0
        process.poll.return_value = 0
        with patch.object(task.subprocess, "Popen", return_value=process) as popen, patch.object(task.threading, "Timer"):
            self.assertTrue(backend.image_matches("manifest-id", expected))
        self.assertEqual(popen.call_args.args[0], ["docker", "image", "save", "manifest-id"])

    # 安装超时必须结束整个安装进程组，再交给状态机执行恢复。
    def test_install_timeout_terminates_group(self):
        backend = task.Backend()
        process = MagicMock()
        process.pid = 123
        process.wait.side_effect = [task.subprocess.TimeoutExpired("installer", 1800), 0]
        with tempfile.TemporaryDirectory() as temporary, patch.object(task.subprocess, "Popen", return_value=process) as popen, patch.object(task.os, "killpg", create=True) as killpg:
            with self.assertRaisesRegex(RuntimeError, "进程组已终止"):
                backend.install(Path("fake-installer"), Path(temporary) / "task.log")
        self.assertTrue(popen.call_args.kwargs["start_new_session"])
        killpg.assert_called_once_with(123, task.signal.SIGTERM)


class ReadOnlyRecoveryTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / "tasks"
        self.op = str(uuid.uuid4())
        self.backend = MagicMock(unsafe=True)
        self.backend.scheduled.return_value = False

    def manager(self):
        return task.ReadOnlyTaskManager(self.root, backend=self.backend, secure=False)

    # 目录不存在时只能报告不存在，不创建目录或调度任务。
    def test_missing_root_query_does_not_create_files(self):
        with self.assertRaisesRegex(ValueError, "LOWER_UPDATE_TASK_NOT_FOUND:"):
            self.manager().query(self.op)
        self.assertFalse(self.root.exists())
        self.backend.assert_idle.assert_called_once()
        self.backend.schedule.assert_not_called()

    def write_state(self, phase="accepted", operation=None):
        operation = operation or self.op
        folder = self.root / operation
        folder.mkdir(parents=True, exist_ok=True)
        data = {"operation_id": operation, "phase": phase, "message": "已受理", "expected_image_id": "sha256:" + "a" * 64}
        (folder / "state.json").write_text(json.dumps(data), encoding="utf-8")
        return folder / "state.json"

    # 已受理但调度中断的查询只给出说明，不改状态或重发旧安装。
    def test_accepted_query_never_reschedules_or_writes(self):
        state = self.write_state()
        before = state.read_bytes()
        result = self.manager().query(self.op)
        self.assertEqual(result["phase"], "accepted")
        self.assertIn("未自动", result["message"])
        self.assertEqual(state.read_bytes(), before)
        self.backend.schedule.assert_not_called()

    # 其他活动或回退失败任务不能被任务不存在掩盖。
    def test_other_active_or_rollback_failed_blocks_not_found(self):
        for phase in ("accepted", "installing", "rolling_back", "rollback_failed"):
            with self.subTest(phase=phase):
                state = self.write_state(phase, str(uuid.uuid4()))
                with self.assertRaisesRegex(ValueError, "不能确认原任务不存在"):
                    self.manager().query(self.op)
                state.unlink()
                state.parent.rmdir()

    # 损坏的状态文件必须保留未知语义。
    def test_corrupt_state_does_not_become_not_found(self):
        state = self.write_state("succeeded", str(uuid.uuid4()))
        state.write_text("broken", encoding="utf-8")
        with self.assertRaises(Exception) as caught:
            self.manager().query(self.op)
        self.assertNotIn("LOWER_UPDATE_TASK_NOT_FOUND:", str(caught.exception))

    # 孤立任务目录代表受理未完成，不能直接结束本地记录。
    def test_incomplete_original_directory_is_not_not_found(self):
        (self.root / self.op).mkdir(parents=True)
        with self.assertRaisesRegex(ValueError, "不完整"):
            self.manager().query(self.op)

    # 安装进程存在或观察失败时不能报告任务不存在。
    def test_running_installer_blocks_not_found(self):
        self.backend.assert_idle.side_effect = RuntimeError("安装仍在执行")
        with self.assertRaisesRegex(RuntimeError, "安装仍在执行"):
            self.manager().query(self.op)

    # 预检检查已有祖先并保持缺失目录原样。
    def test_preflight_uses_readonly_existing_ancestor_validation(self):
        with patch.object(task, "ensure_secure_existing") as secure, patch.object(task.Backend, "environment_preflight") as environment:
            task.preflight_environment()
        environment.assert_called_once()
        self.assertTrue(any(call.args[0] == Path("/usr/local/libexec/mskdsp-update") for call in secure.call_args_list))
        self.assertTrue(any(call.args[0] == Path("/var/lib/mskdsp-update") for call in secure.call_args_list))

    # 目录检查覆盖根目录，拒绝任一已有祖先组可写。
    def test_existing_ancestor_group_write_rejected(self):
        import stat
        info = MagicMock(st_uid=0, st_mode=stat.S_IFDIR | 0o755)
        unsafe = MagicMock(st_uid=0, st_mode=stat.S_IFDIR | 0o775)
        def lookup(path):
            return unsafe if path == Path("/usr").absolute() else info
        with patch.object(Path, "lstat", lookup):
            with self.assertRaises(ValueError):
                task.ensure_secure_existing(Path("/usr/local/libexec/mskdsp-update"))

    # 显式关闭持久记录后，迟到的旧安装提交必须在受理前拒绝。
    def test_close_prevents_late_submission(self):
        self.backend.inspect.return_value = {'State': {'Running': True}, 'Image': 'sha256:' + 'a' * 64}
        manager = self.manager()
        manager.close(self.op, 'a' * 64, 'sha256:' + 'b' * 64)
        self.assertTrue((self.root / self.op / 'state.json').is_file())
        with self.assertRaisesRegex(ValueError, 'LOWER_UPDATE_TASK_REJECTED:'):
            task.TaskManager(self.root, backend=self.backend, secure=False).submit(self.op, Path('absent'), 'a' * 64, 'sha256:' + 'b' * 64)
        self.backend.schedule.assert_not_called()
        # 模拟旧版 submit 的完整兼容分支：身份相同的 failed 状态绝不调度。
        legacy = task.TaskManager(self.root, backend=self.backend, secure=False)
        data = legacy.state(self.op)
        self.assertEqual(data['package_sha256'], 'a' * 64)
        self.assertEqual(data['expected_image_id'], 'sha256:' + 'b' * 64)
        self.assertIn(data['phase'], task.TERMINAL)
        manager.close(self.op, 'a' * 64, 'sha256:' + 'b' * 64)
        with self.assertRaisesRegex(ValueError, 'LOWER_UPDATE_TASK_NOT_FOUND:'):
            manager.query(self.op)

    # 关闭记录写入失败撤销本次空目录，用户可重试，既有目录不得清理。
    def test_close_write_failure_remains_blocked(self):
        self.backend.inspect.return_value = {'State': {'Running': True}, 'Image': 'sha256:' + 'a' * 64}
        with patch.object(task, 'atomic_json', side_effect=OSError('模拟落盘失败')):
            with self.assertRaisesRegex(OSError, '落盘失败'):
                self.manager().close(self.op, 'a' * 64, 'sha256:' + 'b' * 64)
        with self.assertRaisesRegex(ValueError, 'LOWER_UPDATE_TASK_NOT_FOUND:'):
            self.manager().query(self.op)
        self.manager().close(self.op, 'a' * 64, 'sha256:' + 'b' * 64)
        self.assertTrue(self.manager().state(self.op)['closed'])

    # 已有提交锁被占用时只读查询不得宣称任务不存在。
    @unittest.skipIf(os.name == 'nt', '生产使用 Linux flock，共享锁验证在 Linux 执行')
    def test_busy_admission_lock_refuses_not_found(self):
        self.root.mkdir()
        with task.exclusive(self.root / 'admission.lock'):
            with self.assertRaisesRegex(ValueError, '锁正在占用'):
                self.manager().query(self.op)

    # 无任务时仍须检查 systemd 更新服务，不能只读空目录就结束。
    def test_active_systemd_unit_blocks_absence(self):
        backend = task.Backend()
        with patch.object(backend, 'command', return_value='mskdsp-update-old.service loaded active running'):
            with self.assertRaisesRegex(ValueError, '更新服务'):
                backend.assert_idle()

    # 完整祖先中的符号链接即使 root 所有也必须拒绝。
    def test_secure_ancestor_symlink_rejected(self):
        import stat
        info = MagicMock(st_uid=0, st_mode=stat.S_IFDIR | 0o755)
        link = MagicMock(st_uid=0, st_mode=stat.S_IFLNK | 0o755)
        with patch.object(Path, 'lstat', lambda path: link if path == Path('/usr').absolute() else info):
            with self.assertRaises(ValueError):
                task.ensure_secure_existing(Path('/usr/local/libexec/mskdsp-update'))

    # 原任务已存在、容器停止或镜像不合法时禁止结束。
    def test_close_refuses_existing_task_and_unhealthy_container(self):
        self.write_state('succeeded')
        with self.assertRaisesRegex(ValueError, '已存在'):
            self.manager().close(self.op, 'a' * 64, 'sha256:' + 'b' * 64)
        state = self.root / self.op / 'state.json'
        state.unlink()
        state.parent.rmdir()
        for snapshot in ({'State': {'Running': False}, 'Image': 'sha256:' + 'a' * 64}, {'State': {'Running': True}, 'Image': 'bad'}):
            self.backend.inspect.return_value = snapshot
            with self.assertRaisesRegex(ValueError, '容器'):
                self.manager().close(self.op, 'a' * 64, 'sha256:' + 'b' * 64)
        self.assertFalse((self.root / self.op / 'state.json').exists())


if __name__ == "__main__":
    unittest.main()
