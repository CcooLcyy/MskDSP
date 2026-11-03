"""普通外网事务测试：命令全部在内存模拟，不访问真实网卡。"""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('ordinary_network', Path(__file__).with_name('ordinary_network.py'))
network = importlib.util.module_from_spec(spec)
spec.loader.exec_module(network)


class Commands:
    def __init__(self):
        self.calls = []
        self.rules = []
        self.routes = []
        self.fail = None
        self.unavailable = set()
        self.missing = set()

    def __call__(self, args, missing=False):
        self.calls.append(args)
        args = [arg for arg in args if arg != '-N']
        if self.fail and self.fail in ' '.join(args):
            self.fail = None
            raise network.NetworkError('模拟命令失败')
        if args[:5] == ['ip', '-4', '-j', 'addr', 'show']:
            if args[-1] in self.unavailable:
                raise network.NetworkError('网卡没有可用地址')
            return json.dumps([{'addr_info': [{'family': 'inet', 'local': '192.168.2.219', 'prefixlen': 24}]}])
        if args == ['ip', '-j', 'link', 'show']:
            return json.dumps([{'ifname': f'eth0.10{n}'} for n in range(1, 9) if f'eth0.10{n}' not in self.missing])
        if args[:5] == ['ip', '-4', '-j', 'rule', 'show']:
            return json.dumps(self.rules)
        if args[:5] == ['ip', '-4', '-j', 'route', 'show']:
            return json.dumps(self.routes)
        if args[:3] == ['ip', '-4', 'rule']:
            priority = int(args[args.index('pref') + 1])
            if args[3] == 'del':
                self.rules = [r for r in self.rules if r['priority'] != priority]
            else:
                self.rules.append({'priority': priority, 'table': args[args.index('table') + 1], 'protocol': 242,
                                   **({'suppress_prefixlength': 0} if priority == 20000 else {})})
        if args[:3] == ['ip', '-4', 'route']:
            destination = args[4]
            self.routes = [r for r in self.routes if r['dst'] != destination]
            if args[3] != 'del':
                self.routes.append({'dst': destination, 'protocol': 242})
        if args[:2] == ['resolvectl', 'dns'] and len(args) == 3:
            return 'Link 6 (eth0.102): 114.114.114.114'
        if args[:2] == ['resolvectl', 'domain'] and len(args) == 3:
            return 'Link 6 (eth0.102): example.internal'
        if args[:2] == ['resolvectl', 'default-route'] and len(args) == 3:
            return 'Link 6 (eth0.102): no'
        return ''


class NetworkTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.state = Path(self.directory.name) / 'state.json'
        self.commands = Commands()
        self.config = {'interface_name': 'eth0.102', 'gateway': '192.168.2.1', 'dns_servers': ['192.168.2.1']}

    def apply(self, config=None):
        return network.apply(self.config if config is None else config, self.state, self.commands)

    def test_unmanaged_does_not_touch_network(self):
        self.assertTrue(self.apply({})['applied'])
        self.assertEqual(self.commands.calls, [])

    def test_invalid_configuration_and_off_link_gateway_leave_state_unchanged(self):
        for patch in [{'interface_name': 'eth0;id'}, {'gateway': '999.0.0.1'}, {'dns_servers': ['::1']}, {'gateway': '10.0.0.1'}]:
            with self.subTest(patch=patch), self.assertRaises(network.NetworkError):
                self.apply({**self.config, **patch})
            self.assertFalse(self.state.exists())
            self.assertEqual(self.commands.rules, [])

    def test_managed_policy_preserves_main_routes_and_ppp_default(self):
        result = self.apply()
        self.assertEqual(result['source_address'], '192.168.2.219')
        commands = [' '.join(c) for c in self.commands.calls]
        self.assertTrue(any('suppress_prefixlength 0' in c for c in commands))
        self.assertTrue(any('table 18684' in c and 'via 192.168.2.1' in c for c in commands))
        self.assertFalse(any('flush' in c or 'table main' in c and 'route' in c or 'ppp0' in c for c in commands))

    def test_repeated_apply_is_idempotent_and_restores_original_dns(self):
        self.apply()
        self.apply()
        self.assertEqual(len(self.commands.rules), 2)
        self.apply({})
        self.assertEqual(self.commands.rules, [])
        self.assertEqual(self.commands.routes, [])
        self.assertIn(['resolvectl', 'dns', 'eth0.102', '114.114.114.114'], self.commands.calls)
        self.assertIn(['resolvectl', 'domain', 'eth0.102', 'example.internal'], self.commands.calls)

    def test_command_failure_rolls_back_and_is_reported(self):
        self.commands.fail = 'domain eth0.102 ~.'
        with self.assertRaisesRegex(network.NetworkError, '模拟命令失败'):
            self.apply()
        self.assertEqual(self.commands.rules, [])
        self.assertEqual(self.commands.routes, [])
        self.assertFalse(self.state.exists())

    def test_foreign_policy_is_rejected_without_overwrite(self):
        self.commands.rules = [{'priority': 20010, 'table': '777'}]
        with self.assertRaisesRegex(network.NetworkError, '冲突'):
            self.apply()
        self.assertEqual(self.commands.rules, [{'priority': 20010, 'table': '777'}])

    def test_switch_interface_restores_old_dns_and_tracks_new_interface(self):
        self.apply()
        replacement = {**self.config, 'interface_name': 'eth0.103'}
        self.apply(replacement)
        persisted = json.loads(self.state.read_text())
        self.assertEqual(list(persisted['backups']), ['eth0.103'])
        self.assertIn(['resolvectl', 'dns', 'eth0.102', '114.114.114.114'], self.commands.calls)
        self.assertIn(['resolvectl', 'dns', 'eth0.103', '192.168.2.1'], self.commands.calls)

    def test_failed_switch_preserves_previous_configuration_and_network(self):
        self.apply()
        saved = self.state.read_text()
        self.commands.fail = 'domain eth0.103 ~.'
        with self.assertRaises(network.NetworkError):
            self.apply({**self.config, 'interface_name': 'eth0.103'})
        self.assertEqual(json.loads(self.state.read_text()), json.loads(saved))
        self.assertEqual(len(self.commands.rules), 2)
        self.assertIn(['resolvectl', 'dns', 'eth0.102', '192.168.2.1'], self.commands.calls)

    def test_restart_cleans_pending_transaction_with_no_saved_platform_config(self):
        self.apply()
        persisted = json.loads(self.state.read_text())
        persisted['config'] = network.validate({})
        persisted['pending'] = True
        network.write_state(self.state, persisted)
        self.apply({})
        self.assertFalse(self.state.exists())
        self.assertEqual(self.commands.rules, [])

    def test_foreign_route_in_reserved_table_is_not_deleted(self):
        self.commands.routes = [{'dst': 'default', 'protocol': 'static'}]
        with self.assertRaisesRegex(network.NetworkError, '冲突'):
            self.apply()
        self.assertEqual(self.commands.routes, [{'dst': 'default', 'protocol': 'static'}])

    def test_numeric_main_table_is_recognized_on_reapply(self):
        self.apply()
        for rule in self.commands.rules:
            if rule['table'] == 'main':
                rule['table'] = 254
        self.apply()
        self.assertEqual(len(self.commands.rules), 2)
        self.assertTrue(any('-N' in call for call in self.commands.calls))

    def test_eight_dns_servers_supported_and_invalid_addresses_rejected(self):
        servers = [f'192.168.2.{n}' for n in range(1, 9)]
        self.assertEqual(len(network.validate({**self.config, 'dns_servers': servers})['dns_servers']), 8)
        for config in [{**self.config, 'dns_servers': servers + ['192.168.2.9']},
                       {**self.config, 'interface_name': 'ppp0'},
                       {**self.config, 'dns_servers': ['127.0.0.1']},
                       {**self.config, 'dns_servers': [3232236033]},
                       {**self.config, 'gateway': 3232236033},
                       {**self.config, 'dns_servers': ['240.0.0.1']}]:
            with self.assertRaises(network.NetworkError):
                network.validate(config)

    def test_missing_old_interface_allows_switch_and_skips_its_dns_restore(self):
        self.apply()
        self.commands.unavailable.add('eth0.102')
        self.commands.missing.add('eth0.102')
        self.commands.calls.clear()
        self.apply({**self.config, 'interface_name': 'eth0.103'})
        self.assertEqual(json.loads(self.state.read_text())['config']['interface_name'], 'eth0.103')
        self.assertFalse(any(c[0] == 'resolvectl' and 'eth0.102' in c for c in self.commands.calls))

    def test_old_interface_without_address_allows_unmanage_and_restores_dns(self):
        self.apply()
        self.commands.unavailable.add('eth0.102')
        self.commands.calls.clear()
        self.apply({})
        self.assertFalse(self.state.exists())
        self.assertEqual(self.commands.rules, [])
        self.assertIn(['resolvectl', 'dns', 'eth0.102', '114.114.114.114'], self.commands.calls)

    def test_failed_switch_from_missing_interface_keeps_recovery_record(self):
        self.apply()
        self.commands.unavailable.add('eth0.102')
        self.commands.missing.add('eth0.102')
        self.commands.fail = 'domain eth0.103 ~.'
        with self.assertRaisesRegex(network.NetworkError, '回滚未完成'):
            self.apply({**self.config, 'interface_name': 'eth0.103'})
        self.assertTrue(json.loads(self.state.read_text())['pending'])

    def test_symlink_state_is_rejected(self):
        destination = Path(self.directory.name) / 'other'
        destination.write_text('{}')
        try:
            self.state.symlink_to(destination)
        except OSError:
            self.skipTest('当前系统没有符号链接权限')
        with self.assertRaises(network.NetworkError):
            self.apply()


if __name__ == '__main__':
    unittest.main()
