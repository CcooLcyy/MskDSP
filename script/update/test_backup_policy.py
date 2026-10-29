"""配置来源与可信执行路径的隔离权限、链接和竞态回归。"""
import os
from contextlib import closing
from pathlib import Path
import sqlite3
import stat
import tempfile
import unittest
from unittest.mock import patch

from test_lower_update_task import task, FakeBackend
import hashlib
import uuid


@unittest.skipUnless(os.name == "posix", "生产配置安全遍历使用 Linux 目录描述符")
class BackupPolicyTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.host = self.root / "host"
        self.conf = self.host / "conf"
        self.module = self.host / "module"
        self.conf.mkdir(parents=True)
        self.module.mkdir()
        self.child = self.conf / "configPusher"
        self.child.mkdir(mode=0o777)
        self.child.chmod(0o777)
        (self.child / "sample.json").write_text('{"value": 1}')
        self.destination = self.root / "snapshot"
        # 临时根可能由普通测试账户持有，测试只模拟父链属主，不放宽实际待测写权限规则。
        def trusted(path):
            p = Path(path)
            if not p.is_relative_to(self.host):
                return
            if p.is_symlink() or p.stat().st_mode & 0o022:
                raise ValueError("模拟不可信路径: " + str(p))
        self.trusted = trusted
        self.addCleanup(patch.stopall)
        patch.object(task, "ensure_secure", side_effect=trusted).start()

    # 普通可写配置子目录可以备份，结果为保护目录且保持来源权限。
    def test_writable_configuration_directory_is_data(self):
        before = sorted(str(p) for p in self.host.rglob('*'))
        self.assertEqual(task.scan_business_sources(self.host), [])
        self.assertEqual(sorted(str(p) for p in self.host.rglob('*')), before)
        self.assertFalse(self.destination.exists())
        task.copy_configuration(self.conf, self.destination)
        self.assertEqual((self.destination / "configPusher/sample.json").read_text(), '{"value": 1}')
        self.assertEqual(stat.S_IMODE(self.destination.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE(self.child.stat().st_mode), 0o777)

    # 数据目录放宽不允许配置软链接进入快照。
    def test_configuration_symlink_is_rejected(self):
        (self.child / "escape").symlink_to(self.root)
        with self.assertRaises((ValueError, OSError)):
            task.copy_configuration(self.conf, self.destination)

    # 特殊文件必须在上传前检查阶段被发现。
    def test_scan_aggregates_module_and_configuration_errors(self):
        os.mkfifo(self.child / "fifo")
        bad = self.module / "bad.so"
        bad.write_bytes(b"module")
        bad.chmod(0o666)
        errors = task.scan_business_sources(self.host)
        self.assertTrue(any("fifo" in error for error in errors), errors)
        self.assertTrue(any("bad.so" in error for error in errors), errors)
        self.assertFalse(self.destination.exists())

    # SQLite 的路径重开仍要求父目录独占，不能借数据目录放宽。
    def test_sqlite_in_writable_directory_rejected(self):
        with sqlite3.connect(self.child / "settings.db") as db:
            db.execute("CREATE TABLE item(value)")
        with self.assertRaisesRegex(ValueError, "SQLite"):
            task.copy_configuration(self.conf, self.destination)

    # root 保护父目录中的 SQLite 仍使用在线一致快照。
    def test_sqlite_in_protected_directory_keeps_online_backup(self):
        with sqlite3.connect(self.conf / "settings.db") as db:
            db.execute("PRAGMA journal_mode=WAL")
            db.execute("CREATE TABLE item(value)")
            db.execute("INSERT INTO item VALUES (42)")
            db.commit()
            task.copy_configuration(self.conf, self.destination)
        self.assertFalse((self.destination / "settings.db-wal").exists())
        with closing(sqlite3.connect(self.destination / "settings.db")) as db:
            self.assertEqual(db.execute("SELECT value FROM item").fetchone()[0], 42)

    # 校验后目录被替换时必须拒绝，不能跟随换入的路径读取。
    def test_directory_replacement_is_rejected(self):
        original_open = os.open
        changed = False
        def replace(path, flags, *args, **kwargs):
            nonlocal changed
            if path == "configPusher" and not changed:
                changed = True
                self.child.rename(self.conf / "old")
                self.child.mkdir()
                (self.child / "injected").write_text("bad")
            return original_open(path, flags, *args, **kwargs)
        with patch.object(os, "open", side_effect=replace):
            with self.assertRaisesRegex(ValueError, "变化|替换"):
                task.copy_configuration(self.conf, self.destination)

    # 普通配置文件内容变化必须使快照失败。
    def test_file_change_during_read_is_rejected(self):
        original_read = os.read
        changed = False
        def mutate(fd, size):
            nonlocal changed
            result = original_read(fd, size)
            if result and size == 1024**2 and not changed:
                changed = True
                (self.child / "sample.json").write_text("changed content")
            return result
        with patch.object(os, "read", side_effect=mutate):
            with self.assertRaisesRegex(ValueError, "变化"):
                task.copy_configuration(self.conf, self.destination)

    # 探测前被换入软链接时禁止跟随，不能读取链接目标。
    def test_file_replaced_with_symlink_before_open_is_rejected(self):
        original_open = os.open
        changed = False
        def replace(path, flags, *args, **kwargs):
            nonlocal changed
            if path == 'sample.json' and not changed:
                changed = True
                (self.child / 'sample.json').unlink()
                (self.child / 'sample.json').symlink_to(self.root / 'secret')
            return original_open(path, flags, *args, **kwargs)
        (self.root / 'secret').write_text('secret')
        with patch.object(os, 'open', side_effect=replace):
            with self.assertRaises((ValueError, OSError)):
                task.copy_configuration(self.conf, self.destination)

    # 可写配置目录内不能用硬链接引入其他文件。
    def test_hardlink_is_rejected(self):
        os.link(self.child / 'sample.json', self.child / 'alias')
        with self.assertRaisesRegex(ValueError, '硬链接'):
            task.copy_configuration(self.conf, self.destination)

    # 即使复制开始后文件持续增长，也必须在大小上限停止。
    def test_copy_enforces_running_size_limit(self):
        original_read = os.read
        def grow(fd, size):
            if size == 1024**2:
                return b'x' * 32
            return original_read(fd, size)
        with patch.object(task, 'MAX_PACKAGE', 64), patch.object(os, 'read', side_effect=grow):
            with self.assertRaisesRegex(ValueError, '大小限制'):
                task.copy_configuration(self.conf, self.destination)

    # 扫描预算耗尽后不继续处理其他业务树。
    def test_scan_budget_stops_tree_traversal(self):
        with patch.object(task.time, 'monotonic', side_effect=[0, 11]), patch.object(task, 'copy_configuration') as configuration:
            errors = task.scan_business_sources(self.host)
        self.assertTrue(any('限制' in error for error in errors), errors)
        configuration.assert_not_called()

    # 枚举结束后读取文件头耗尽预算，也必须终止扫描。
    def test_scan_deadline_covers_header_read_after_enumeration(self):
        original_read = os.read
        elapsed = False
        def read_slowly(fd, size):
            nonlocal elapsed
            result = original_read(fd, size)
            elapsed = True
            return result
        with patch.object(task.time, 'monotonic', side_effect=lambda: 11 if elapsed else 0), patch.object(os, 'read', side_effect=read_slowly):
            with self.assertRaises(task.SourceLimit):
                task.copy_configuration(self.conf)

    # module 根本身不能为链接，即使链接指向仍在 host 内。
    def test_module_root_symlink_rejected(self):
        self.module.rmdir()
        self.module.symlink_to(self.conf)
        self.assertTrue(any('module' in error for error in task.scan_business_sources(self.host)))

    # 业务提交 WAL 事务不应被普通文件内容签名误判为路径替换。
    def test_active_wal_commit_is_backed_up_consistently(self):
        with closing(sqlite3.connect(self.conf / 'settings.db')) as db:
            db.execute('PRAGMA journal_mode=WAL')
            db.execute('CREATE TABLE item(value)')
            db.commit()
            original_read = os.read
            changed = False
            def commit(fd, size):
                nonlocal changed
                result = original_read(fd, size)
                if size == 16 and not changed and os.readlink('/proc/self/fd/' + str(fd)).endswith('settings.db-wal'):
                    changed = True
                    db.execute('INSERT INTO item VALUES (99)')
                    db.commit()
                return result
            with patch.object(os, 'read', side_effect=commit):
                task.copy_configuration(self.conf, self.destination)
            self.assertTrue(changed)
            self.assertFalse((self.destination / 'settings.db-wal').exists())
            with closing(sqlite3.connect(self.destination / 'settings.db')) as copied:
                self.assertEqual(copied.execute('SELECT value FROM item').fetchone()[0], 99)

    # 循环模块链接作为问题返回，不能令聚合检查异常退出。
    def test_cyclic_module_link_is_reported(self):
        (self.module / 'cycle').symlink_to('cycle')
        self.assertTrue(any('cycle' in error for error in task.scan_business_sources(self.host)))

    # 生产 secure 分支允许可写普通配置，并在安装失败后用受保护材料恢复。
    def test_secure_task_success_and_rollback_preserve_configuration(self):
        package = self.root / 'package'
        package.write_bytes(b'isolated package')
        for fail_install in (False, True):
            with self.subTest(fail_install=fail_install):
                backend = FakeBackend()
                backend.fail_install = fail_install
                operation = str(uuid.uuid4())
                manager = task.TaskManager(self.root / operation, self.host, backend, secure=True)
                manager.submit(operation, package, hashlib.sha256(package.read_bytes()).hexdigest(), 'sha256:' + 'b' * 64)
                result = manager.run(operation)
                self.assertEqual(result['phase'], 'rolled_back' if fail_install else 'succeeded', result)
                self.assertEqual((self.child / 'sample.json').read_text(), '{"value": 1}')
                copied = manager.root / operation / 'backup/conf/configPusher/sample.json'
                if fail_install:
                    self.assertTrue(backend.restored)
                    self.assertEqual(stat.S_IMODE((self.child / 'sample.json').stat().st_mode), 0o600)
                else:
                    self.assertEqual(copied.read_text(), '{"value": 1}')
                    self.assertEqual(stat.S_IMODE(copied.stat().st_mode), 0o600)


class PreflightAggregationTest(unittest.TestCase):
    # 环境和业务源的独立错误应一次报告，检查不创建任务。
    def test_preflight_aggregates_all_errors(self):
        with patch.object(task, "ensure_secure_existing", side_effect=ValueError("助手路径错误")), patch.object(task.Backend, "environment_preflight", side_effect=ValueError("Docker错误")), patch.object(task, "scan_business_sources", return_value=["业务目录错误"]):
            with self.assertRaises(ValueError) as error:
                task.preflight_environment()
        for expected in ("助手路径错误", "Docker错误", "业务目录错误"):
            self.assertIn(expected, str(error.exception))
