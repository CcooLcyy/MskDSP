#!/usr/bin/env python3
"""普通 IPv4 出口事务；只维护本功能的路由规则和逐接口 DNS。"""
import argparse
import contextlib
import ipaddress
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile

TABLE = '18684'
PROTOCOL = '242'
PRIORITIES = (20000, 20010)


class NetworkError(RuntimeError):
    pass


def validate(config):
    if not isinstance(config, dict) or set(config) - {'interface_name', 'gateway', 'dns_servers'}:
        raise NetworkError('普通外网配置字段不合法')
    interface = config.get('interface_name', '')
    gateway = config.get('gateway', '')
    dns = config.get('dns_servers', [])
    if not isinstance(interface, str) or not isinstance(gateway, str) or not isinstance(dns, list):
        raise NetworkError('普通外网配置类型不合法')
    if not interface:
        if gateway or dns:
            raise NetworkError('使用系统网络时不能指定网关或 DNS')
        return {'interface_name': '', 'gateway': '', 'dns_servers': []}
    if not re.fullmatch(r'eth0\.10[1-8]', interface):
        raise NetworkError('普通外网网卡必须是 eth0.101 至 eth0.108')
    def address(value):
        try:
            if not isinstance(value, str):
                raise ValueError()
            result = ipaddress.IPv4Address(value)
            if result.is_unspecified or result.is_loopback or int(result) >= int(ipaddress.IPv4Address('224.0.0.0')):
                raise ValueError()
            return str(result)
        except (ValueError, TypeError, ipaddress.AddressValueError):
            raise NetworkError('网关和 DNS 必须是有效 IPv4 单播地址') from None
    if len(dns) > 8 or len(set(map(str, dns))) != len(dns):
        raise NetworkError('DNS 最多配置八个且不能重复')
    return {'interface_name': interface, 'gateway': address(gateway), 'dns_servers': [address(x) for x in dns]}


def command(args, missing=False):
    try:
        result = subprocess.run(args, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                                timeout=3, check=False, env={'PATH': '/usr/sbin:/usr/bin:/sbin:/bin', 'LC_ALL': 'C'})
    except (OSError, subprocess.TimeoutExpired):
        raise NetworkError('网络命令不可用或执行超时，请检查权限、工具和宿主机系统总线') from None
    if result.returncode:
        if missing and ('No such process' in result.stderr or 'FIB table does not exist' in result.stderr):
            return '[]'
        raise NetworkError('网络命令执行失败，请检查网卡、权限及宿主机 resolved 服务')
    return result.stdout


def check_file(path):
    try:
        info = path.lstat()
    except FileNotFoundError:
        return
    if not stat.S_ISREG(info.st_mode) or info.st_size > 65536:
        raise NetworkError('网络状态文件类型或大小不合法')
    if os.name == 'posix' and (info.st_mode & 0o077 or info.st_uid != os.geteuid()):
        raise NetworkError('网络状态文件属主或权限不合法')


def read_state(path):
    check_file(path)
    if not path.exists():
        return None
    try:
        descriptor = os.open(path, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
        with os.fdopen(descriptor, encoding='utf-8') as handle:
            state = json.load(handle)
        if state.get('version') != 1 or not isinstance(state.get('backups'), dict):
            raise ValueError()
        state['config'] = validate(state['config'])
        for interface, backup in state['backups'].items():
            if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.:-]{0,14}', interface):
                raise ValueError()
            if not isinstance(backup.get('dns'), list) or not isinstance(backup.get('domains'), list) or backup.get('default_route') not in ['yes', 'no']:
                raise ValueError()
            if any(not isinstance(v, str) or '\x00' in v or len(v) > 253 for v in backup['dns'] + backup['domains']):
                raise ValueError()
        return state
    except (ValueError, KeyError, TypeError):
        raise NetworkError('网络状态文件损坏，未修改网络') from None


def write_state(path, state):
    check_file(path)
    if state is None:
        path.unlink(missing_ok=True)
        return
    fd, temporary = tempfile.mkstemp(prefix='.ordinary-network-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as handle:
            os.chmod(temporary, 0o600)
            json.dump(state, handle, ensure_ascii=False)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
        if os.name == 'posix':
            directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


@contextlib.contextmanager
def state_lock(path):
    # 同一持久目录锁定事务，禁止多个调用交错写路由和 DNS。
    flags = os.O_CREAT | os.O_RDWR | getattr(os, 'O_NOFOLLOW', 0)
    fd = os.open(str(path) + '.lock', flags, 0o600)
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            raise NetworkError('网络事务锁类型不合法')
        if os.name == 'posix':
            import fcntl
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield
    except BlockingIOError:
        raise NetworkError('已有普通外网配置事务正在执行') from None
    finally:
        os.close(fd)


def resources(run):
    rules = json.loads(run(['ip', '-N', '-4', '-j', 'rule', 'show']))
    routes = json.loads(run(['ip', '-N', '-4', '-j', 'route', 'show', 'table', TABLE], missing=True) or '[]')
    ours = []
    for rule in rules:
        if rule.get('priority') not in PRIORITIES:
            continue
        first = rule['priority'] == PRIORITIES[0]
        if (str(rule.get('protocol')) != PROTOCOL or str(rule.get('table')) not in (['main', '254'] if first else [TABLE])
                or first and rule.get('suppress_prefixlength') != 0
                or rule.get('src', 'all') != 'all' or rule.get('dst', 'all') != 'all'
                or any(k in rule for k in ['iif', 'oif', 'fwmark', 'uidrange', 'not'])):
            raise NetworkError('普通外网策略优先级与现有规则冲突，未覆盖规则')
        ours.append(rule)
    if any(str(route.get('protocol')) != PROTOCOL for route in routes):
        raise NetworkError('普通外网专用路由表与现有配置冲突，未覆盖路由')
    return ours, routes


def remove_routes(run):
    rules, routes = resources(run)
    for rule in reversed(rules):
        args = ['ip', '-4', 'rule', 'del', 'pref', str(rule['priority']), 'table', str(rule['table'])]
        if rule['priority'] == PRIORITIES[0]:
            args += ['suppress_prefixlength', '0']
        run(args + ['protocol', PROTOCOL])
    for route in routes:
        run(['ip', '-4', 'route', 'del', route.get('dst', 'default'), 'table', TABLE, 'proto', PROTOCOL], missing=True)


def restore_dns(run, backups):
    if not backups:
        return
    # 内核已删除的接口不再有逐接口 DNS，不能让旧接口阻塞切换或撤销。
    links = json.loads(run(['ip', '-j', 'link', 'show']))
    available = {link['ifname'] for link in links}
    for interface, old in backups.items():
        if interface not in available:
            continue
        run(['resolvectl', 'dns', interface] + (old['dns'] or ['']))
        run(['resolvectl', 'domain', interface] + (old['domains'] or ['']))
        run(['resolvectl', 'default-route', interface, old['default_route']])


def snapshot_dns(run, interface):
    def values(kind):
        text = run(['resolvectl', kind, interface]).strip()
        if not re.match(r'^Link \d+ \([^)]+\):', text):
            raise NetworkError('无法读取接口原有 DNS 状态')
        return text.split('):', 1)[1].strip().split()
    dns, domains, default = values('dns'), values('domain'), values('default-route')
    if default not in [['yes'], ['no']]:
        raise NetworkError('无法读取接口原有 DNS 默认路由状态')
    return {'dns': dns, 'domains': domains, 'default_route': default[0]}


def source_address(run, config):
    interface, gateway = config['interface_name'], ipaddress.IPv4Address(config['gateway'])
    entries = json.loads(run(['ip', '-4', '-j', 'addr', 'show', 'dev', interface]))
    for entry in entries:
        for info in entry.get('addr_info', []):
            if info.get('family') != 'inet':
                continue
            address = ipaddress.IPv4Interface(f"{info['local']}/{info['prefixlen']}")
            unusable = [address.ip]
            if address.network.prefixlen < 31:
                unusable += [address.network.network_address, address.network.broadcast_address]
            if gateway in address.network and gateway not in unusable:
                return str(address.ip)
    raise NetworkError('所选网卡没有与网关同网段的 IPv4 地址')


def install(run, config, source):
    if not config['interface_name']:
        return
    interface, gateway = config['interface_name'], config['gateway']
    run(['ip', '-4', 'route', 'replace', gateway + '/32', 'dev', interface, 'scope', 'link', 'src', source, 'table', TABLE, 'proto', PROTOCOL])
    run(['ip', '-4', 'route', 'replace', 'default', 'via', gateway, 'dev', interface, 'src', source, 'table', TABLE, 'proto', PROTOCOL])
    run(['ip', '-4', 'rule', 'add', 'pref', '20000', 'table', 'main', 'suppress_prefixlength', '0', 'protocol', PROTOCOL])
    run(['ip', '-4', 'rule', 'add', 'pref', '20010', 'table', TABLE, 'protocol', PROTOCOL])
    if config['dns_servers']:
        run(['resolvectl', 'dns', interface] + config['dns_servers'])
        run(['resolvectl', 'domain', interface, '~.'])
        run(['resolvectl', 'default-route', interface, 'yes'])


def apply(config, state_path=Path('./conf/ordinary_network_state.json'), run=command):
    config = validate(config)
    path = Path(state_path)
    if not path.parent.is_dir() or path.parent.is_symlink():
        raise NetworkError('网络状态目录不存在或不安全')
    with state_lock(path):
        old = read_state(path)
        if old is None and not config['interface_name']:
            return {'applied': True, 'error': '', 'source_address': ''}
        resources(run)
        source = source_address(run, config) if config['interface_name'] else ''
        old_config = old['config'] if old else validate({})
        old_source = ''
        if old_config['interface_name']:
            try:
                old_source = source_address(run, old_config)
            except NetworkError:
                # 当前配置必须有效；旧接口不可用不能阻塞用户指定一个新的可用出口。
                # 若后续失败，则保留 journal，明确报告无法完整恢复旧出口。
                pass
        backups = dict(old['backups']) if old else {}
        interface = config['interface_name']
        if interface and config['dns_servers'] and interface not in backups:
            backups[interface] = snapshot_dns(run, interface)
        journal = {'version': 1, 'config': old_config, 'backups': backups, 'pending': True}
        write_state(path, journal)
        try:
            remove_routes(run)
            restore_dns(run, backups)
            install(run, config, source)
            if interface:
                retained = {interface: backups[interface]} if config['dns_servers'] else {}
                write_state(path, {'version': 1, 'config': config, 'backups': retained, 'pending': False})
            else:
                write_state(path, None)
        except Exception as failure:
            try:
                remove_routes(run)
                restore_dns(run, backups)
                if old_config['interface_name'] and not old_source:
                    raise NetworkError('旧出口网卡或地址不可用，无法完整恢复旧出口')
                install(run, old_config, old_source)
                write_state(path, old)
            except Exception:
                raise NetworkError('网络应用失败且回滚未完成，保留恢复记录，请重新读取并重试') from failure
            if isinstance(failure, NetworkError):
                raise
            raise NetworkError('网络应用失败，已恢复原配置') from failure
        return {'applied': True, 'error': '', 'source_address': source}


def main():
    parser = argparse.ArgumentParser(description='应用普通外网出口配置')
    parser.add_argument('--config', required=True)
    args = parser.parse_args()
    try:
        result = apply(json.loads(args.config))
    except Exception as error:
        message = str(error) if isinstance(error, NetworkError) else '网络配置执行失败，请检查运行环境和状态文件'
        result = {'applied': False, 'error': message, 'source_address': ''}
    print(json.dumps(result, ensure_ascii=False))
    return 0 if result['applied'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
