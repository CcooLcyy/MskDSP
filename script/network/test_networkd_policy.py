"""宿主机兼容准备的隔离回归，不访问真实网络或 systemd。"""
import importlib.util
import json
from pathlib import Path
import tempfile
import sys
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('networkd_policy', Path(__file__).with_name('networkd_policy.py'))


class FakeHost:
    def __init__(self):
        self.calls = []
        self.active = True
        self.pid = 10
        self.effective = b'[Network]\nManageForeignRoutingPolicyRules=no\n'
        self.rules = b'0: from all lookup local\n20010: from all lookup 18684 proto 242\n'
        self.fail_restart = False
        self.fail_save = False
        self.fail_restore = False
        self.network_list = b'2 eth0.102 vlan routable configured\n3 usb0 ether no-carrier configuring\n'

    def __call__(self, args, data=None):
        self.calls.append((args, data))
        if args[:2] == ['systemctl', 'show']:
            return f'ActiveState={"active" if self.active else "inactive"}\nMainPID={self.pid}\nInvocationID=run-{self.pid}\n'.encode()
        if args[:2] == ['systemd-analyze', 'cat-config']:
            return self.effective
        if args[:2] == ['systemctl', 'restart']:
            self.pid += 1
            self.rules = b'0: from all lookup local\n'
            if self.fail_restart:
                self.fail_restart = False
                raise RuntimeError('模拟重启失败')
            return b''
        if args[:4] == ['ip', '-4', 'rule', 'show']:
            return self.rules
        if args == ['ip', '-4', '-j', 'address', 'show']:
            return b'[{"ifname":"eth0.102","addr_info":[{"family":"inet","local":"192.168.2.219"}]}]'
        if args[:2] == ['networkctl', 'list']:
            return self.network_list
        if 'save' in args and self.fail_save:
            raise RuntimeError('模拟快照失败')
        if 'restore' in args and self.fail_restore:
            raise RuntimeError('模拟恢复失败')
        return b'network-snapshot' if 'save' in args else b''


class NetworkdPolicyTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.host = FakeHost()
        self.root.joinpath('proc/sys/kernel/random').mkdir(parents=True)
        self.root.joinpath('proc/sys/kernel/random/boot_id').write_text('boot-1')
        self.module = importlib.util.module_from_spec(SPEC)
        SPEC.loader.exec_module(self.module)

    # x64 测试环境与未知架构不能读取或修改宿主机网络，即使 networkd 正在运行。
    def test_x64_and_unknown_architectures_skip_network_preparation(self):
        for architecture in ('x64', 'x86_64', 'AMD64', 'riscv64', ''):
            with self.subTest(architecture=architecture):
                self.host.calls.clear()
                self.module.prepare(self.root, self.host, architecture=architecture)
                self.assertEqual(self.host.calls, [])
                self.assertFalse((self.root / 'etc').exists())
                self.assertFalse((self.root / 'var').exists())

    # 生产入口探测的是执行安装器的宿主机架构，不能默认为 ARM64。
    def test_default_architecture_probe_preserves_x64_host_network(self):
        with patch.object(self.module.platform, 'machine', return_value='x86_64'):
            self.module.prepare(self.root, self.host)
        self.assertEqual(self.host.calls, [])
        self.assertFalse((self.root / 'etc').exists())

    # 真机 uname 常见的 aarch64 别名仍须完成宿主机网络准备。
    def test_aarch64_alias_prepares_arm64_host(self):
        self.module.prepare(self.root, self.host, architecture='aarch64')
        self.assertIn(['systemctl', 'restart', 'systemd-networkd.service'], [call[0] for call in self.host.calls])
        self.assertTrue((self.root / self.module.CONFIG).exists())

    # 第一次设置必须重启，恢复额外地址和路由，规则只补缺失条目。
    def test_prepare_restores_runtime_and_second_upgrade_does_not_restart(self):
        self.module.prepare(self.root, self.host, architecture='arm64')
        calls = [c[0] for c in self.host.calls]
        self.assertIn(['systemctl', 'restart', 'systemd-networkd.service'], calls)
        self.assertIn(['ip', '-4', 'address', 'restore'], calls)
        self.assertIn(['ip', '-4', 'route', 'restore'], calls)
        self.assertIn(['ip', '-4', 'rule', 'add', 'pref', '20010', 'from', 'all', 'lookup', '18684', 'proto', '242'], calls)
        self.assertFalse(any(c[:7] == ['ip', '-4', 'rule', 'add', 'pref', '0', 'from'] for c in calls))
        self.assertFalse(any('19900' in c or '18685' in c for c in calls))
        self.host.calls.clear()
        self.module.prepare(self.root, self.host, architecture='arm64')
        self.assertFalse(any('restart' in c[0] for c in self.host.calls))

    # 文件存在不足以证明进程已经加载，进程身份变化也须重新确认。
    def test_file_alone_or_stale_process_marker_does_not_skip(self):
        path = self.root / 'etc/systemd/networkd.conf.d/60-mskdsp-routing.conf'
        path.parent.mkdir(parents=True)
        path.write_text('[Network]\nManageForeignRoutingPolicyRules=no\n')
        self.module.prepare(self.root, self.host, architecture='arm64')
        self.host.pid += 1
        self.host.calls.clear()
        self.module.prepare(self.root, self.host, architecture='arm64')
        self.assertTrue(any('restart' in c[0] for c in self.host.calls))

    # networkd 未运行时不能创建配置或启动服务。
    def test_inactive_is_skipped_without_writes(self):
        self.host.active = False
        self.module.prepare(self.root, self.host, architecture='arm64')
        self.assertFalse((self.root / 'etc').exists())
        self.assertFalse(any('restart' in c[0] for c in self.host.calls))

    # 重启失败须恢复原文件及网络快照，保留备份且不能留下成功标记。
    def test_failed_restart_rolls_back_and_keeps_backup(self):
        path = self.root / 'etc/systemd/networkd.conf.d/60-mskdsp-routing.conf'
        path.parent.mkdir(parents=True)
        path.write_bytes(b'# original\n')
        self.host.fail_restart = True
        with self.assertRaisesRegex(RuntimeError, '中止'):
            self.module.prepare(self.root, self.host, architecture='arm64')
        self.assertEqual(path.read_bytes(), b'# original\n')
        self.assertFalse((self.root / 'var/lib/mskdsp/networkd_policy.json').exists())
        self.assertTrue(list((self.root / 'var/backups').glob('mskdsp-networkd-*')))
        self.assertTrue(any(c[0] == ['ip', '-4', 'address', 'restore'] for c in self.host.calls))

    # 后续全局配置覆盖 no 时不能报告成功，必须回退并终止安装。
    def test_effective_conflict_aborts(self):
        self.host.effective = b'[Network]\nManageForeignRoutingPolicyRules=no\nManageForeignRoutingPolicyRules=yes\n'
        with self.assertRaisesRegex(RuntimeError, '中止'):
            self.module.prepare(self.root, self.host, architecture='arm64')
        self.assertFalse(any('restart' in c[0] for c in self.host.calls))

    # 快照未完整保存时不能写入配置或重启，原现场配置保持原样。
    def test_snapshot_failure_preserves_original_config(self):
        path = self.root / 'etc/systemd/networkd.conf.d/60-mskdsp-routing.conf'
        path.parent.mkdir(parents=True)
        path.write_bytes(b'# original\n')
        self.host.fail_save = True
        with self.assertRaisesRegex(RuntimeError, '快照失败'):
            self.module.prepare(self.root, self.host, architecture='arm64')
        self.assertEqual(path.read_bytes(), b'# original\n')
        self.assertFalse(any('restart' in c[0] for c in self.host.calls))

    # 网络恢复失败须使安装失败并明确报告回退异常，不能留下成功标记。
    def test_runtime_restore_failure_is_not_success(self):
        self.host.fail_restore = True
        with self.assertRaisesRegex(RuntimeError, '回退异常'):
            self.module.prepare(self.root, self.host, architecture='arm64')
        self.assertFalse((self.root / 'var/lib/mskdsp/networkd_policy.json').exists())

    # 只恢复快照已有的纵密规则；健康的同优先级多条规则不重复增加。
    def test_rule_restore_only_adds_missing_snapshot_entries(self):
        backup = self.root / 'backup'
        backup.mkdir()
        for filename in ('addresses.bin', 'routes.bin'):
            (backup / filename).write_bytes(b'snapshot')
        (backup / 'rules.txt').write_bytes(b'19900: from all iif eth0.107 lookup 18685\n20000: from all lookup main suppress_prefixlength 0 proto 242\n')
        self.host.rules = b'19900: from all iif eth0.107 lookup 18685\n'
        self.module.restore_runtime(self.host, backup)
        additions = [c[0] for c in self.host.calls if c[0][:4] == ['ip', '-4', 'rule', 'add']]
        self.assertEqual(additions, [['ip', '-4', 'rule', 'add', 'pref', '20000', 'from', 'all', 'lookup', 'main', 'suppress_prefixlength', '0', 'proto', '242']])

    # 没有原有 IPv4 地址的 USB 配置中状态不能阻塞已就绪有线维护链路。
    def test_wait_only_original_ipv4_links(self):
        self.host.network_list = '● 2 eth0.102 vlan routable configured\n● 3 usb0 ether no-carrier configuring\n'.encode()
        with patch.object(self.module.time, 'monotonic', side_effect=[0, 31]):
            self.module.prepare(self.root, self.host, architecture='arm64')

    # 原来承载 IPv4 的接口仍在配置时须等待，超时明确失败而非盲目恢复。
    def test_original_ipv4_link_setup_timeout_aborts(self):
        self.host.network_list = b'2 eth0.102 vlan routable configuring\n'
        with patch.object(self.module.time, 'monotonic', side_effect=[0, 31, 40, 71]):
            with self.assertRaisesRegex(RuntimeError, '等待超时'):
                self.module.prepare(self.root, self.host, architecture='arm64')

    # 原生 ip route restore 会回读输入；真实子进程必须接收到可 seek 的二进制快照。
    def test_command_snapshot_stdin_is_seekable(self):
        snapshot = b'\x00route-snapshot\xff\n'
        script = 'import sys; s=sys.stdin.buffer; first=s.read(); s.seek(0); assert s.read()==first; sys.stdout.buffer.write(first)'
        self.assertEqual(self.module.command([sys.executable, '-c', script], snapshot), snapshot)


if __name__ == '__main__':
    unittest.main()
