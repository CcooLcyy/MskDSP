"""宿主机兼容准备的隔离回归，不访问真实网络或 systemd。"""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import sys
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('networkd_policy', Path(__file__).with_name('networkd_policy.py'))


def route_snapshot(flags=0, nexthop_flags=None, extra_attribute=b'', nexthop_attributes=None):
    attributes = b''
    if nexthop_flags is not None:
        hops = b''
        for index, entry in enumerate(nexthop_flags):
            nested = nexthop_attributes[index] if nexthop_attributes is not None else b''
            hops += struct.pack('=HBBi', 8 + len(nested), entry, index, index + 2) + nested
        attributes = struct.pack('=HH', len(hops) + 4, 9) + hops
    payload = struct.pack('=BBBBBBBBI', 2, 24, 0, 0, 254, 4, 0, 1, flags) + attributes + extra_attribute
    return struct.pack('=I', 0x45311224) + struct.pack('=IHHII', len(payload) + 16, 24, 0x601, 1, 0) + payload


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
        self.fail_route_restore_once = False
        self.ignore_rule_add = False
        self.drop_addresses = False
        self.drop_routes = False
        self.addresses = [{'ifname': 'eth0.102', 'addr_info': [
            {'family': 'inet', 'local': '192.168.2.219', 'prefixlen': 24, 'scope': 'global',
             'valid_life_time': 4294967295, 'preferred_life_time': 4294967295}]}]
        self.routes = [{'dst': 'default', 'gateway': '192.168.2.1', 'dev': 'eth0.102',
                        'table': 18684, 'protocol': 'static', 'flags': ['linkdown']}]
        self.saved_routes = route_snapshot(flags=0x11)
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
        if args[:4] == ['ip', '-4', 'rule', 'add']:
            if not self.ignore_rule_add:
                self.rules += (args[5] + ': ' + ' '.join(args[6:]) + '\n').encode()
            return b''
        if args == ['ip', '-4', '-j', 'address', 'show']:
            return json.dumps([] if self.drop_addresses and self.pid > 10 else self.addresses).encode()
        if args == ['ip', '-4', '-j', 'route', 'show', 'table', 'all']:
            return json.dumps([] if self.drop_routes and self.pid > 10 else self.routes).encode()
        if args[:2] == ['networkctl', 'list']:
            return self.network_list
        if 'save' in args and self.fail_save:
            raise RuntimeError('模拟快照失败')
        if 'restore' in args and self.fail_restore:
            raise RuntimeError('模拟恢复失败')
        if args == ['ip', '-4', 'route', 'save', 'table', 'all']:
            return self.saved_routes
        if args == ['ip', '-4', 'route', 'restore']:
            if self.fail_route_restore_once:
                self.fail_route_restore_once = False
                raise RuntimeError('模拟第一次路由恢复失败')
            if struct.unpack_from('=I', data, 28)[0] & 0x11:
                raise RuntimeError('Error: Invalid rtm_flags - can not contain DEAD or LINKDOWN.')
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

    def make_backup(self):
        backup = self.root / 'backup'
        backup.mkdir()
        (backup / 'addresses.bin').write_bytes(b'address-snapshot')
        (backup / 'routes.bin').write_bytes(self.host.saved_routes)
        (backup / 'addresses.json').write_text(json.dumps(self.host.addresses), encoding='utf-8')
        (backup / 'routes.json').write_text(json.dumps(self.host.routes), encoding='utf-8')
        (backup / 'rules.txt').write_bytes(self.host.rules)
        return backup

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
        backup = self.make_backup()
        (backup / 'rules.txt').write_bytes(b'19900: from all iif eth0.107 lookup 18685\n20000: from all lookup main suppress_prefixlength 0 proto 242\n')
        self.host.rules = b'19900: from all iif eth0.107 lookup 18685\n'
        self.module.restore_runtime(self.host, backup)
        additions = [c[0] for c in self.host.calls if c[0][:4] == ['ip', '-4', 'rule', 'add']]
        self.assertEqual(additions, [['ip', '-4', 'rule', 'add', 'pref', '20000', 'from', 'all', 'lookup', 'main', 'suppress_prefixlength', '0', 'proto', '242']])

    # 原始备份必须保留故障现场；恢复输入仅清除内核拒绝重放的 DEAD/LINKDOWN。
    def test_prepare_sanitizes_restore_input_and_preserves_original_backup(self):
        self.module.prepare(self.root, self.host, architecture='arm64')
        backup, = (self.root / 'var/backups').glob('mskdsp-networkd-*')
        self.assertEqual((backup / 'routes.bin').read_bytes(), self.host.saved_routes)
        restored = [data for args, data in self.host.calls if args == ['ip', '-4', 'route', 'restore']]
        self.assertEqual(restored, [route_snapshot()])
        self.assertEqual(json.loads((backup / 'routes.json').read_text()), self.host.routes)
        self.assertEqual(json.loads((backup / 'addresses.json').read_text()), self.host.addresses)

    # 第一次恢复失败而回退成功时，两次均须净化输入，且备份现场不可改写。
    def test_rollback_reuses_sanitized_input_without_changing_backup(self):
        self.host.fail_route_restore_once = True
        self.host.saved_routes = route_snapshot(0x15, [0x11, 0x15])
        with self.assertRaisesRegex(RuntimeError, '已恢复原配置和运行时网络'):
            self.module.prepare(self.root, self.host, architecture='arm64')
        backup, = (self.root / 'var/backups').glob('mskdsp-networkd-*')
        self.assertEqual((backup / 'routes.bin').read_bytes(), self.host.saved_routes)
        restored = [data for args, data in self.host.calls if args == ['ip', '-4', 'route', 'restore']]
        self.assertEqual(restored, [route_snapshot(0x04, [0, 0x04])] * 2)
        self.assertFalse((self.root / self.module.MARKER).exists())

    # rtmsg 的其他语义位与后续消息、属性原样保留，只清除两类瞬时状态位。
    def test_sanitize_preserves_other_flags_and_multiple_route_messages(self):
        attribute = struct.pack('=HHI', 8, 6, 100)
        original = route_snapshot(0x211, extra_attribute=attribute) + route_snapshot(0x14)[4:]
        expected = route_snapshot(0x200, extra_attribute=attribute) + route_snapshot(0x04)[4:]
        self.assertEqual(self.module.sanitize_route_snapshot(original), expected)
        self.assertEqual(self.module.sanitize_route_snapshot(expected), expected)

    # ECMP 每个 nexthop 的运行状态也须清理，ONLINK 及权重、接口保持不变。
    def test_sanitize_multipath_nexthop_flags(self):
        original = route_snapshot(0x11, [0x15, 0x05, 0x10])
        self.assertEqual(self.module.sanitize_route_snapshot(original), route_snapshot(0, [0x04, 0x04, 0]))

    # 多路径网关属性属于转发语义；净化不能丢失各下一跳网关、权重或接口。
    def test_sanitize_preserves_nested_multipath_gateways(self):
        gateways = [struct.pack('=HH', 8, 5) + bytes([192, 168, 2, 1]),
                    struct.pack('=HH', 8, 5) + bytes([192, 168, 3, 1])]
        original = route_snapshot(0x11, [0x11, 0x15], nexthop_attributes=gateways)
        expected = route_snapshot(0, [0, 0x04], nexthop_attributes=gateways)
        self.assertEqual(self.module.sanitize_route_snapshot(original), expected)

    # 下一跳内的属性长度也须验证，不能仅检查外层多路径和下一跳长度。
    def test_sanitize_rejects_malformed_nested_gateway_attribute(self):
        nested = struct.pack('=HH', 12, 5) + bytes([192, 168, 2, 1])
        snapshot = route_snapshot(0x11, [0x11], nexthop_attributes=[nested])
        with self.assertRaises(RuntimeError):
            self.module.sanitize_route_snapshot(snapshot)

    # 路由属性的对齐填充不属于有效载荷，清理时不能丢失或改写。
    def test_sanitize_preserves_attribute_padding(self):
        attribute = struct.pack('=HH', 5, 99) + b'x\x00\x00\x00'
        self.assertEqual(self.module.sanitize_route_snapshot(route_snapshot(0x11, extra_attribute=attribute)),
                         route_snapshot(0, extra_attribute=attribute))

    # magic、消息、属性和多路径边界损坏必须拒绝，不能把坏快照交给内核。
    def test_sanitize_rejects_malformed_snapshots(self):
        valid = route_snapshot()
        bad_header = bytearray(valid)
        struct.pack_into('=I', bad_header, 4, 12)
        bad_type = bytearray(valid)
        struct.pack_into('=H', bad_type, 8, 20)
        bad_hop = struct.pack('=HH', 12, 9) + struct.pack('=HBBi', 0, 0, 0, 2)
        snapshots = [b'', b'bad!', valid[:-1], bytes(bad_header), bytes(bad_type),
                     valid + b'\x00', route_snapshot(extra_attribute=struct.pack('=HH', 3, 9)),
                     route_snapshot(extra_attribute=struct.pack('=HH', 20, 9)),
                     route_snapshot(extra_attribute=bad_hop)]
        for snapshot in snapshots:
            with self.subTest(snapshot=snapshot):
                with self.assertRaises(RuntimeError):
                    self.module.sanitize_route_snapshot(snapshot)

    # 空路由表仍包含合法 magic，属于可恢复的空快照。
    def test_sanitize_accepts_empty_route_table(self):
        snapshot = struct.pack('=I', 0x45311224)
        self.assertEqual(self.module.sanitize_route_snapshot(snapshot), snapshot)

    # 快照格式错误必须在改配置及重启 networkd 前被发现。
    def test_invalid_route_snapshot_aborts_before_network_changes(self):
        path = self.root / self.module.CONFIG
        path.parent.mkdir(parents=True)
        path.write_bytes(b'# original\n')
        self.host.saved_routes = b'invalid-route-snapshot'
        with self.assertRaises(RuntimeError):
            self.module.prepare(self.root, self.host, architecture='arm64')
        self.assertEqual(path.read_bytes(), b'# original\n')
        self.assertFalse(any('restart' in args or 'restore' in args for args, _ in self.host.calls))

    # restore 退出成功不代表状态完整；原始地址或路由缺失时必须中止且不留成功标记。
    def test_missing_runtime_entries_abort_even_when_restore_succeeds(self):
        for missing in ('drop_addresses', 'drop_routes', 'ignore_rule_add'):
            with self.subTest(missing=missing):
                self.host = FakeHost()
                setattr(self.host, missing, True)
                with self.assertRaisesRegex(RuntimeError, '恢复|缺失'):
                    self.module.prepare(self.root, self.host, architecture='arm64')
                self.assertFalse((self.root / self.module.MARKER).exists())

    # 核验只要求原快照条目存在；动态生命周期和新增运行时资源不能造成误报。
    def test_verify_allows_additions_and_transient_changes(self):
        backup = self.make_backup()
        self.host.addresses[0]['addr_info'][0]['valid_life_time'] = 120
        self.host.addresses[0]['addr_info'][0]['preferred_life_time'] = 60
        self.host.addresses[0]['addr_info'].append({'family': 'inet', 'local': '192.168.2.220', 'prefixlen': 24})
        self.host.routes[0]['flags'] = ['offload']
        self.host.routes[0]['expires'] = 30
        self.host.routes.append({'dst': '10.0.0.0/8', 'dev': 'eth0.102'})
        self.host.rules += b'21000: from all lookup 18686\n'
        self.module.verify_runtime(self.host, backup)

    # ONLINK 是持久路由语义，不能与 LINKDOWN 一起忽略。
    def test_verify_rejects_changed_onlink_route_semantics(self):
        self.host.routes[0]['flags'] = ['onlink', 'linkdown']
        backup = self.make_backup()
        self.host.routes[0]['flags'] = []
        with self.assertRaises(RuntimeError):
            self.module.verify_runtime(self.host, backup)

    # ECMP 下一跳集合的输出顺序及瞬时链路状态变化不改变原始转发语义。
    def test_verify_allows_multipath_order_and_link_state_changes(self):
        self.host.routes = [{'dst': 'default', 'table': 18684, 'protocol': 'static', 'nexthops': [
            {'gateway': '192.168.2.1', 'dev': 'eth0.102', 'weight': 1, 'flags': ['linkdown', 'onlink']},
            {'gateway': '192.168.3.1', 'dev': 'eth0.103', 'weight': 2, 'flags': []}]}]
        backup = self.make_backup()
        self.host.routes[0]['nexthops'].reverse()
        self.host.routes[0]['nexthops'][0]['flags'] = ['dead', 'linkdown']
        self.host.routes[0]['nexthops'][1]['flags'] = ['onlink']
        self.module.verify_runtime(self.host, backup)

    # ECMP 的网关、权重和路由表均影响实际转发，任一偏离都必须报告恢复不完整。
    def test_verify_rejects_multipath_gateway_weight_and_table_changes(self):
        self.host.routes = [{'dst': 'default', 'table': 18684, 'protocol': 'static', 'nexthops': [
            {'gateway': '192.168.2.1', 'dev': 'eth0.102', 'weight': 1, 'flags': []},
            {'gateway': '192.168.3.1', 'dev': 'eth0.103', 'weight': 2, 'flags': []}]}]
        backup = self.make_backup()
        for field, changed in (('gateway', '192.168.2.254'), ('weight', 3), ('table', 18685)):
            with self.subTest(field=field):
                self.host.routes = json.loads((backup / 'routes.json').read_text())
                target = self.host.routes[0] if field == 'table' else self.host.routes[0]['nexthops'][0]
                target[field] = changed
                with self.assertRaises(RuntimeError):
                    self.module.verify_runtime(self.host, backup)

    # 封装段列表有顺序语义，不能因统一排序 JSON 数组而漏报转发路径变化。
    def test_verify_rejects_changed_encapsulation_segment_order(self):
        self.host.routes[0]['encap'] = {'type': 'seg6', 'mode': 'encap',
                                       'segments': ['2001:db8:1::1', '2001:db8:2::1']}
        backup = self.make_backup()
        self.host.routes[0]['encap']['segments'].reverse()
        with self.assertRaises(RuntimeError):
            self.module.verify_runtime(self.host, backup)

    # JSON 快照语法或顶层类型不合法时，必须在修改配置和重启 networkd 前拒绝。
    def test_invalid_json_snapshots_abort_before_network_changes(self):
        path = self.root / self.module.CONFIG
        path.parent.mkdir(parents=True)
        path.write_bytes(b'# original\n')
        for kind in ('address', 'route'):
            for invalid in (b'{', b'{}'):
                with self.subTest(kind=kind, invalid=invalid):
                    self.host.calls.clear()

                    def corrupt_snapshot(args, data=None):
                        if args[:4] == ['ip', '-4', '-j', kind]:
                            self.host.calls.append((args, data))
                            return invalid
                        return self.host(args, data)

                    with self.assertRaises(RuntimeError):
                        self.module.prepare(self.root, corrupt_snapshot, architecture='arm64')
                    self.assertEqual(path.read_bytes(), b'# original\n')
                    self.assertFalse(any('restart' in args or 'restore' in args for args, _ in self.host.calls))

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
