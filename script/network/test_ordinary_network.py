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
        self.dns = {}

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
                route = {'dst': destination, 'protocol': 242}
                for option, field in [('dev', 'dev'), ('src', 'prefsrc'), ('via', 'gateway'), ('scope', 'scope')]:
                    if option in args:
                        route[field] = args[args.index(option) + 1]
                self.routes.append(route)
        if args[0] == 'resolvectl':
            kind, interface = args[1:3]
            value = self.dns.setdefault((kind, interface), {'dns': ['114.114.114.114'], 'domain': ['example.internal'], 'default-route': ['no']}[kind])
            if len(args) == 3:
                return f'Link 6 ({interface}): ' + ' '.join(value)
            self.dns[kind, interface] = [v for v in args[3:] if v]
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

    def test_real_iproute2_suppress_prefixlen_json_is_supported(self):
        self.apply()
        self.commands.rules = json.loads('[{"priority":0,"src":"all","table":"255","protocol":"2"},'
            '{"priority":20000,"src":"all","table":"254","suppress_prefixlen":0,"protocol":"242"},'
            '{"priority":20010,"src":"all","table":"18684","protocol":"242"},'
            '{"priority":32766,"src":"all","table":"254","protocol":"2"},'
            '{"priority":32767,"src":"all","table":"253","protocol":"2"}]')
        self.commands.calls.clear()
        self.assertTrue(network.check(self.config, self.state, self.commands)['applied'])
        self.apply()
        self.assertEqual(self.mutations(), [])
        next(r for r in self.commands.rules if r['priority'] == 20000)['suppress_prefixlength'] = 0
        self.assertTrue(network.check(self.config, self.state, self.commands)['applied'])

    def test_real_numeric_route_json_scope_and_type_are_supported_without_writes(self):
        self.apply()
        sample = json.loads('[{"dst":"default","gateway":"192.168.2.1","dev":"eth0.102","protocol":"242","prefsrc":"192.168.2.219","flags":[]},'
            '{"dst":"192.168.2.1","dev":"eth0.102","protocol":"242","scope":"253","prefsrc":"192.168.2.219","flags":[]}]')
        for scope in ['253', 253, 'link']:
            for route_type in ['unicast', 1, '1']:
                with self.subTest(scope=scope, route_type=route_type):
                    self.commands.routes = json.loads(json.dumps(sample))
                    self.commands.routes[1]['scope'] = scope
                    for route in self.commands.routes:
                        route['type'] = route_type
                    self.commands.calls.clear()
                    self.assertTrue(network.check(self.config, self.state, self.commands)['applied'])
                    self.apply()
                    self.assertEqual(self.mutations(), [])
    def test_inconsistent_suppress_aliases_and_suppressed_exit_are_rejected(self):
        self.apply()
        baseline = json.loads(json.dumps(self.commands.rules))
        variants = [[{**baseline[0], 'suppress_prefixlen': 1}, baseline[1]],
                    [baseline[0], {**baseline[1], 'suppress_prefixlen': 0}],
                    [baseline[0], {**baseline[1], 'suppress_prefixlength': 0}]]
        for rules in variants:
            with self.subTest(rules=rules):
                self.commands.rules = rules
                self.commands.calls.clear()
                with self.assertRaisesRegex(network.NetworkError, '冲突'):
                    self.apply()
                self.assertEqual(self.mutations(), [])

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

    def mutations(self):
        return [c for c in self.commands.calls if (c[:3] in [['ip', '-4', 'rule'], ['ip', '-4', 'route']]
                and c[3] in ['add', 'del', 'replace']) or c[0] == 'resolvectl' and len(c) > 3]

    def test_readonly_check_detects_missing_rules_without_changing_journal(self):
        self.apply()
        saved = self.state.read_bytes()
        self.commands.rules.clear()
        self.commands.calls.clear()
        self.assertFalse(network.check(self.config, self.state, self.commands)['applied'])
        self.assertEqual(self.mutations(), [])
        self.assertEqual(self.state.read_bytes(), saved)

    def test_healthy_reapply_does_not_write_network_or_journal(self):
        self.apply()
        saved = self.state.stat().st_mtime_ns
        self.commands.calls.clear()
        self.apply()
        self.assertEqual(self.mutations(), [])
        self.assertEqual(self.state.stat().st_mtime_ns, saved)
        self.assertTrue(network.check(self.config, self.state, self.commands)['applied'])

    def test_same_config_repairs_only_missing_policy_and_preserves_longitudinal(self):
        self.apply()
        longitudinal = {'priority': 19900, 'table': 18685, 'iif': 'eth0.107'}
        self.commands.rules = [r for r in self.commands.rules if r['priority'] != 20010] + [longitudinal]
        self.commands.calls.clear()
        self.apply()
        self.assertEqual(self.mutations(), [['ip', '-4', 'rule', 'add', 'pref', '20010', 'table', '18684', 'protocol', '242']])
        self.assertIn(longitudinal, self.commands.rules)

    def test_missing_route_is_repaired_without_deleting_healthy_policy(self):
        self.apply()
        self.commands.routes = [r for r in self.commands.routes if r['dst'] != 'default']
        self.assertFalse(network.check(self.config, self.state, self.commands)['applied'])
        self.commands.calls.clear()
        self.apply()
        self.assertEqual(len(self.mutations()), 1)
        self.assertEqual(self.mutations()[0][:5], ['ip', '-4', 'route', 'replace', 'default'])

    def test_dns_drift_repaired_without_route_writes_and_preserves_backup(self):
        self.apply()
        backup = self.state.read_bytes()
        self.commands.dns['dns', 'eth0.102'] = ['8.8.8.8']
        self.assertFalse(network.check(self.config, self.state, self.commands)['applied'])
        self.commands.calls.clear()
        self.apply()
        self.assertEqual(self.mutations(), [['resolvectl', 'dns', 'eth0.102', '192.168.2.1']])
        self.assertEqual(self.state.read_bytes(), backup)

    def test_route_wrong_gateway_source_or_interface_is_not_healthy(self):
        for field, value in [('gateway', '192.168.2.2'), ('prefsrc', '192.168.2.220'), ('dev', 'eth0.103')]:
            with self.subTest(field=field):
                self.apply()
                next(r for r in self.commands.routes if r['dst'] == 'default')[field] = value
                self.assertFalse(network.check(self.config, self.state, self.commands)['applied'])
                self.commands.calls.clear()
                self.apply()
                self.assertFalse(any('del' in c for c in self.mutations()))
                self.assertTrue(network.check(self.config, self.state, self.commands)['applied'])

    def test_pending_or_mismatched_journal_is_not_healthy(self):
        self.apply()
        state = json.loads(self.state.read_text())
        state['pending'] = True
        network.write_state(self.state, state)
        self.assertFalse(network.check(self.config, self.state, self.commands)['applied'])
        state['pending'] = False
        state['config']['gateway'] = '192.168.2.2'
        network.write_state(self.state, state)
        self.assertFalse(network.check(self.config, self.state, self.commands)['applied'])

    def test_empty_dns_is_not_checked_and_external_policy_conflict_is_not_overwritten(self):
        config = {**self.config, 'dns_servers': []}
        self.apply(config)
        self.commands.calls.clear()
        self.assertTrue(network.check(config, self.state, self.commands)['applied'])
        self.assertFalse(any(c[0] == 'resolvectl' for c in self.commands.calls))
        self.commands.rules = [{'priority': 20010, 'table': 777}]
        self.commands.calls.clear()
        with self.assertRaisesRegex(network.NetworkError, '冲突'):
            self.apply(config)
        self.assertEqual(self.mutations(), [])

    def test_duplicate_or_conditional_owned_policy_is_rejected_without_writes(self):
        self.apply()
        baseline = json.loads(json.dumps(self.commands.rules))
        variants = [baseline + [dict(baseline[0])], [{**baseline[0], 'goto': 32766}, baseline[1]],
                    [baseline[0], {**baseline[1], 'suppress_prefixlength': 1}]]
        for rules in variants:
            with self.subTest(rules=rules):
                self.commands.rules = rules
                self.commands.calls.clear()
                with self.assertRaises(network.NetworkError):
                    self.apply()
                self.assertEqual(self.mutations(), [])

    def test_incremental_repair_failure_keeps_healthy_resources_for_retry(self):
        self.apply()
        self.commands.rules = [r for r in self.commands.rules if r['priority'] == 20000]
        self.commands.fail = 'rule add pref 20010'
        saved = self.state.read_bytes()
        self.commands.calls.clear()
        with self.assertRaises(network.NetworkError):
            self.apply()
        self.assertEqual(self.state.read_bytes(), saved)
        self.assertEqual(len(self.commands.routes), 2)
        self.assertEqual(len(self.commands.rules), 1)
        self.assertFalse(any('del' in c for c in self.mutations()))
        self.apply()
        self.assertTrue(network.check(self.config, self.state, self.commands)['applied'])

    def test_extra_route_and_missing_dns_backup_are_not_silently_accepted(self):
        self.apply()
        self.commands.routes.append({'dst': '8.8.8.8/32', 'protocol': 242})
        self.commands.calls.clear()
        with self.assertRaises(network.NetworkError):
            self.apply()
        self.assertEqual(self.mutations(), [])
        self.commands.routes.pop()
        state = json.loads(self.state.read_text())
        state['backups'] = {}
        network.write_state(self.state, state)
        self.assertFalse(network.check(self.config, self.state, self.commands)['applied'])
        with self.assertRaisesRegex(network.NetworkError, '恢复记录'):
            self.apply()

    def test_unmanaged_check_creates_no_lock_or_state_file(self):
        self.assertTrue(network.check({}, self.state, self.commands)['applied'])
        self.assertEqual(list(self.state.parent.iterdir()), [])
        self.assertEqual(self.commands.calls, [])

    def test_nonstandard_owned_route_is_a_conflict_not_a_replace(self):
        for field, value in [('metric', 200), ('tos', 16), ('nhid', 3), ('mtu', 1400),
                             ('encap', 'unexpected'), ('type', 'blackhole'),
                             ('multipath', [{'gateway': '192.168.2.2'}])]:
            with self.subTest(field=field):
                self.apply()
                route = next(r for r in self.commands.routes if r['dst'] == 'default')
                route[field] = value
                self.commands.calls.clear()
                with self.assertRaisesRegex(network.NetworkError, '冲突'):
                    self.apply()
                self.assertEqual(self.mutations(), [])
                route.pop(field)

    def test_successful_command_without_kernel_effect_is_not_successful_repair(self):
        self.apply()
        self.commands.rules = [r for r in self.commands.rules if r['priority'] == 20000]
        def ineffective_rule_add(args, missing=False):
            if args[:4] == ['ip', '-4', 'rule', 'add']:
                return ''
            return self.commands(args, missing=missing)
        with self.assertRaisesRegex(network.NetworkError, '恢复'):
            network.apply(self.config, self.state, ineffective_rule_add)
        self.assertFalse(network.check(self.config, self.state, self.commands)['applied'])

    @unittest.skipUnless(__import__('os').name == 'posix', '文件锁仅在 POSIX 系统生效')
    def test_check_during_apply_lock_fails_without_commands(self):
        self.apply()
        self.commands.calls.clear()
        with network.state_lock(self.state):
            with self.assertRaisesRegex(network.NetworkError, '正在应用'):
                network.check(self.config, self.state, self.commands)
        self.assertEqual(self.commands.calls, [])


if __name__ == '__main__':
    unittest.main()
