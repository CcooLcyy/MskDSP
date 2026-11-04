#!/usr/bin/env python3
"""安装器的宿主机 networkd 兼容准备；不创建任何业务网络配置。"""
from collections import Counter
import json
import os
from pathlib import Path
import platform
import shlex
import stat
import struct
import subprocess
import sys
import tempfile
import time

CONFIG = Path('etc/systemd/networkd.conf.d/60-mskdsp-routing.conf')
MARKER = Path('var/lib/mskdsp/networkd_policy.json')
CONTENT = b'[Network]\nManageForeignRoutingPolicyRules=no\n'
SERVICE = 'systemd-networkd.service'


def command(args, data=None):
    try:
        # ip route restore 会 ftell/fseek 回读快照，不能使用 subprocess input= 的管道。
        with (tempfile.TemporaryFile() if data is not None else open(os.devnull, 'rb')) as source:
            if data is not None:
                source.write(data)
                source.flush()
                source.seek(0)
            result = subprocess.run(args, stdin=source, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    timeout=45, env={'PATH': '/usr/sbin:/usr/bin:/sbin:/bin', 'LC_ALL': 'C'})
    except (OSError, subprocess.TimeoutExpired) as error:
        raise RuntimeError('宿主机网络准备命令不可用或超时：' + args[0]) from error
    if result.returncode:
        raise RuntimeError('宿主机网络准备命令失败：' + ' '.join(args) + '\n' + result.stderr.decode('utf-8', 'replace'))
    return result.stdout


def safe_path(path):
    for entry in (path, *path.parents):
        if entry.is_symlink():
            raise RuntimeError('宿主机准备路径不能包含符号链接：' + str(entry))
    if path.exists() and not path.is_file():
        raise RuntimeError('宿主机准备文件类型不合法：' + str(path))


def atomic_write(path, data, mode=0o600):
    safe_path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix='.networkd-policy-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as output:
            os.chmod(name, mode)
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(name, path)
        if os.name == 'posix':
            directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def process_identity(root, run):
    output = run(['systemctl', 'show', SERVICE, '--property=ActiveState,MainPID,InvocationID']).decode()
    values = dict(line.split('=', 1) for line in output.splitlines() if '=' in line)
    state = values.get('ActiveState')
    if state in ('inactive', 'failed'):
        return None
    if state != 'active' or not values.get('MainPID', '').isdigit() or int(values['MainPID']) <= 0 or not values.get('InvocationID'):
        raise RuntimeError('networkd 正在切换状态或进程身份不可确认，停止准备')
    return {key: values[key] for key in ('MainPID', 'InvocationID')} | {
        'boot_id': (root / 'proc/sys/kernel/random/boot_id').read_text().strip()}


def effective_disabled(run):
    section, value = '', ''
    for line in run(['systemd-analyze', 'cat-config', 'systemd/networkd.conf']).decode().splitlines():
        line = line.strip()
        if not line or line.startswith(('#', ';')):
            continue
        if line.startswith('['):
            section = line
        elif section == '[Network]' and '=' in line:
            key, entry = line.split('=', 1)
            if key.strip() == 'ManageForeignRoutingPolicyRules':
                value = entry.strip().lower()
    return value in ('no', 'false', '0', 'off')


def rule_tokens(raw):
    result = []
    for line in raw.decode().splitlines():
        if not line.strip():
            continue
        priority, separator, tail = line.partition(':')
        tokens = shlex.split(tail)
        if not separator or not priority.strip().isdigit() or not tokens or any('[' in token for token in tokens):
            raise RuntimeError('策略规则快照不能可靠恢复，停止网络准备')
        result.append(tuple(['pref', priority.strip()] + tokens))
    return result


def sanitize_route_snapshot(data):
    """校验原生路由快照，只在恢复副本中清除内核不允许写入的状态位。"""
    error = '路由快照格式不合法或不受支持，停止网络准备'
    if len(data) < 4 or struct.unpack_from('=I', data)[0] != 0x45311224:
        raise RuntimeError(error)
    result = bytearray(data)

    def attributes(start, end, multipath=False):
        while start < end:
            if end - start < 4:
                raise RuntimeError(error)
            length, kind = struct.unpack_from('=HH', result, start)
            aligned = (length + 3) & ~3
            if length < 4 or start + aligned > end:
                raise RuntimeError(error)
            if multipath and (kind & 0x3fff) == 9:  # RTA_MULTIPATH 的下一跳头与属性。
                hop, limit = start + 4, start + length
                if hop == limit:
                    raise RuntimeError(error)
                while hop < limit:
                    if limit - hop < 8:
                        raise RuntimeError(error)
                    hop_length = struct.unpack_from('=H', result, hop)[0]
                    hop_aligned = (hop_length + 3) & ~3
                    if hop_length < 8 or hop + hop_aligned > limit:
                        raise RuntimeError(error)
                    result[hop + 2] &= ~0x11
                    attributes(hop + 8, hop + hop_length)
                    hop += hop_aligned
            start += aligned

    offset = 4
    while offset < len(result):
        if len(result) - offset < 28:
            raise RuntimeError(error)
        length, kind = struct.unpack_from('=IH', result, offset)
        aligned = (length + 3) & ~3
        # nlmsghdr + IPv4 rtmsg；与 iproute2 原生读取器的消息上限一致。
        if length < 28 or length > 16384 or kind != 24 or offset + aligned > len(result) or result[offset + 16] != 2:
            raise RuntimeError(error)
        flags = struct.unpack_from('=I', result, offset + 24)[0]
        struct.pack_into('=I', result, offset + 24, flags & ~0x11)
        attributes(offset + 28, offset + length, multipath=True)
        offset += aligned
    return bytes(result)


def runtime_entries(data, kind):
    """提取配置语义；不把链路状态、生命周期倒计时当作配置差异。"""
    label = '地址' if kind == 'address' else '路由'
    try:
        entries = json.loads(data)
    except ValueError as error:
        raise RuntimeError('运行时 IPv4 ' + label + '快照格式不合法') from error
    if not isinstance(entries, list) or any(not isinstance(entry, dict) for entry in entries):
        raise RuntimeError('运行时 IPv4 ' + label + '快照格式不合法')

    def route(value, network_object=False):
        if isinstance(value, dict):
            normalized = {}
            for key, item in value.items():
                if network_object and key in ('expires', 'cache'):
                    continue
                if network_object and key == 'flags':
                    if not isinstance(item, list) or any(not isinstance(flag, str) for flag in item):
                        raise RuntimeError('运行时 IPv4 路由标志格式不合法')
                    item = sorted(flag for flag in item if flag not in
                                  ('dead', 'linkdown', 'offload', 'trap', 'offload_failed'))
                    if not item:
                        continue
                if network_object and key == 'nexthops':
                    if not isinstance(item, list) or any(not isinstance(hop, dict) for hop in item):
                        raise RuntimeError('运行时 IPv4 多路径路由格式不合法')
                    normalized[key] = sorted((route(hop, True) for hop in item), key=encode)
                else:
                    normalized[key] = route(item)
            return normalized
        if isinstance(value, list):
            return [route(item) for item in value]
        return value

    def encode(value):
        return json.dumps(value, sort_keys=True, separators=(',', ':'))

    if kind == 'route':
        if any(not isinstance(entry.get('dst'), str) for entry in entries):
            raise RuntimeError('运行时 IPv4 路由目标格式不合法')
        return Counter(encode(route(entry, True)) for entry in entries)
    result = Counter()
    for entry in entries:
        if not isinstance(entry.get('ifname'), str) or not isinstance(entry.get('addr_info'), list):
            raise RuntimeError('运行时 IPv4 地址快照格式不合法')
        for address in entry['addr_info']:
            if not isinstance(address, dict) or address.get('family') != 'inet' or not address.get('local'):
                raise RuntimeError('运行时 IPv4 地址条目格式不合法')
            stable = {key: value for key, value in address.items() if key not in
                      ('valid_life_time', 'preferred_life_time', 'secondary', 'dynamic', 'deprecated', 'tentative', 'dadfailed')}
            result[(entry['ifname'], encode(stable))] += 1
    return result


def verify_runtime(run, backup):
    errors = []
    for kind, filename, label in (('address', 'addresses.json', '地址'), ('route', 'routes.json', '路由')):
        args = ['ip', '-4', '-j', kind, 'show'] + (['table', 'all'] if kind == 'route' else [])
        original = runtime_entries((backup / filename).read_bytes(), kind)
        missing = original - runtime_entries(run(args), kind)
        if missing:
            errors.append(label + '缺失：' + str(dict(missing)))
    original = Counter(rule_tokens((backup / 'rules.txt').read_bytes()))
    missing = original - Counter(rule_tokens(run(['ip', '-4', 'rule', 'show'])))
    if missing:
        errors.append('策略规则缺失：' + str(dict(missing)))
    if errors:
        raise RuntimeError('恢复后运行时 IPv4 网络核验失败：' + '；'.join(errors))
    print('恢复后运行时 IPv4 地址、路由和策略规则核验通过')


def restore_runtime(run, backup):
    errors = []
    original_routes = (backup / 'routes.bin').read_bytes()
    restored_routes = sanitize_route_snapshot(original_routes)
    if restored_routes != original_routes:
        print('路由恢复副本已清除 DEAD/LINKDOWN 状态位，原始备份保持不变')
    # 原生 restore 会保留现有条目，仅补回 networkd 重启清理的运行时地址和路由。
    for kind, filename in (('address', 'addresses.bin'), ('route', 'routes.bin')):
        try:
            data = restored_routes if kind == 'route' else (backup / filename).read_bytes()
            run(['ip', '-4', kind, 'restore'], data)
        except Exception as error:
            errors.append(str(error))
    try:
        current = Counter(rule_tokens(run(['ip', '-4', 'rule', 'show'])))
        original = Counter(rule_tokens((backup / 'rules.txt').read_bytes()))
        for rule, count in original.items():
            if any(other[1] == rule[1] and other not in original for other in current):
                raise RuntimeError('恢复策略规则时发现优先级冲突：' + rule[1])
            for _ in range(max(0, count - current[rule])):
                run(['ip', '-4', 'rule', 'add', *rule])
    except Exception as error:
        errors.append(str(error))
    try:
        verify_runtime(run, backup)
    except Exception as error:
        errors.append(str(error))
    if errors:
        raise RuntimeError('恢复宿主机运行时网络失败：' + '；'.join(errors))


def restart_networkd(run, original_links):
    run(['systemctl', 'restart', SERVICE])
    # Type=notify 只保证进程就绪；等接口配置结束后再补回运行时地址，防止被异步清理。
    deadline = time.monotonic() + 30
    while True:
        text = run(['networkctl', 'list', '--no-legend', '--no-pager']).decode()
        states = {}
        for line in text.splitlines():
            words = line.split()
            for name in original_links:
                if name in words:
                    states[name] = words[-1]
        # 无地址的蜂窝 USB/DHCP 接口可长期处于 configuring，不能阻塞有线链路恢复。
        if all(states.get(name) in ('configured', 'unmanaged') for name in original_links):
            return
        if time.monotonic() >= deadline:
            raise RuntimeError('networkd 接口配置等待超时')
        time.sleep(0.5)


def prepare(root=Path('/'), run=command, *, architecture=None):
    architecture = (platform.machine() if architecture is None else architecture).strip().lower()
    if architecture not in ('aarch64', 'arm64'):
        print('宿主机架构为 ' + (architecture or '未知') + '，沿用系统网络，跳过 networkd 兼容准备')
        return
    root = Path(root)
    identity = process_identity(root, run)
    if identity is None:
        print('宿主机 networkd 未运行，跳过兼容准备，不启动网络管理服务')
        return
    config, marker = root / CONFIG, root / MARKER
    safe_path(config)
    safe_path(marker)
    try:
        recorded = json.loads(marker.read_text()) if marker.exists() else None
    except (ValueError, OSError):
        recorded = None
    if config.exists() and config.read_bytes() == CONTENT and recorded == identity and effective_disabled(run):
        print('宿主机 networkd 兼容配置及运行进程已确认，跳过重复重启')
        return
    backup_root = root / 'var/backups'
    safe_path(backup_root / '.mskdsp-networkd-path-check')
    backup_root.mkdir(parents=True, exist_ok=True)
    backup = Path(tempfile.mkdtemp(prefix='mskdsp-networkd-', dir=backup_root))
    os.chmod(backup, 0o700)
    original = config.read_bytes() if config.exists() else None
    mode = stat.S_IMODE(config.stat().st_mode) if original is not None else 0o644
    (backup / 'config.json').write_text(json.dumps({'existed': original is not None, 'mode': mode}), encoding='utf-8')
    if original is not None:
        (backup / '60-mskdsp-routing.conf').write_bytes(original)
    for kind, filename in (('address', 'addresses.bin'), ('route', 'routes.bin')):
        args = ['ip', '-4', kind, 'save'] + (['table', 'all'] if kind == 'route' else [])
        (backup / filename).write_bytes(run(args))
    # 在改变配置和重启服务前拒绝不能安全恢复的快照；原始证据不改写。
    sanitize_route_snapshot((backup / 'routes.bin').read_bytes())
    rules = run(['ip', '-4', 'rule', 'show'])
    rule_tokens(rules)
    (backup / 'rules.txt').write_bytes(rules)
    for kind, filename in (('address', 'addresses.json'), ('route', 'routes.json')):
        args = ['ip', '-4', '-j', kind, 'show'] + (['table', 'all'] if kind == 'route' else [])
        data = run(args)
        (backup / filename).write_bytes(data)
        runtime_entries(data, kind)
    addresses = json.loads((backup / 'addresses.json').read_bytes())
    original_links = {entry['ifname'] for entry in addresses if entry.get('ifname') != 'lo'
                      and any(address.get('family') == 'inet' for address in entry.get('addr_info', []))}
    (backup / 'ipv4-links.json').write_text(json.dumps(sorted(original_links)), encoding='utf-8')
    # 旧成功记录先失效，防止第一次写文件后失败而被下一次升级误判为已生效。
    marker.unlink(missing_ok=True)
    print('准备宿主机 networkd 策略兼容，运行时 IPv4 网络备份：' + str(backup))
    restart_attempted = False
    try:
        atomic_write(config, CONTENT, 0o644)
        if not effective_disabled(run):
            raise RuntimeError('networkd 全局有效配置被其他文件覆盖')
        restart_attempted = True
        restart_networkd(run, original_links)
        restore_runtime(run, backup)
        current = process_identity(root, run)
        if current is None or not effective_disabled(run):
            raise RuntimeError('networkd 重启后的运行状态或有效配置不正确')
        atomic_write(marker, json.dumps(current).encode())
    except Exception as failure:
        recovery = []
        try:
            if original is None:
                config.unlink(missing_ok=True)
            else:
                atomic_write(config, original, mode)
        except Exception as error:
            recovery.append(str(error))
        if restart_attempted:
            try:
                restart_networkd(run, original_links)
            except Exception as error:
                recovery.append(str(error))
        try:
            restore_runtime(run, backup)
        except Exception as error:
            recovery.append(str(error))
        raise RuntimeError('宿主机网络准备失败，中止安装并保留旧业务容器；备份：' + str(backup) + '；原因：' + str(failure)
                           + ('；回退异常：' + '；'.join(recovery) if recovery else '；已恢复原配置和运行时网络')) from failure
    print('宿主机 networkd 策略兼容已准备，原运行时 IPv4 地址、路由和规则已恢复')


def main():
    try:
        # 安装器并发与同一配置写入串行，锁不放在业务 conf 目录。
        lock = Path('/run/lock/mskdsp-networkd-policy.lock')
        descriptor = os.open(lock, os.O_CREAT | os.O_RDWR | getattr(os, 'O_NOFOLLOW', 0), 0o600)
        with os.fdopen(descriptor, 'a+b') as stream:
            import fcntl
            info = os.fstat(stream.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid():
                raise RuntimeError('宿主机网络准备锁文件类型或属主不安全')
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
            prepare()
        return 0
    except Exception as error:
        print('宿主机网络准备中止：' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
