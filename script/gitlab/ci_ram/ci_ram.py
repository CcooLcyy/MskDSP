#!/usr/bin/env python3
"""生成 Linux Runner 共享内存配置，并检查或创建 Docker 临时卷。"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tomllib

VOLUME = "mskdsp-ci-ram"
OPTIONS = {"type": "tmpfs", "device": "tmpfs", "o": "size=24g,noswap,mode=1777"}
PRE_BUILD = ". /opt/ci-ram/pre-build.sh"
LEASE_SERVICE = {"name": "alpine:3.20", "alias": "ci-ram-lease", "entrypoint": ["/bin/sh"], "command": ["-c", "exec sleep 2147483647"]}
SERVICE_HEADER = "[[runners.docker.services]]"


def literal(value):
    """JSON 基本类型也是合法的 TOML 基本类型。"""
    return json.dumps(value, ensure_ascii=False)


def set_value(section, key, value, remove=False):
    """只替换指定配置项，不重写凭据及其他未知配置。"""
    lines = section.splitlines(keepends=True)
    pattern = re.compile(r"^(\s*)" + re.escape(key) + r"\s*=")
    for start, line in enumerate(lines):
        match = pattern.match(line)
        if not match:
            continue
        for end in range(start + 1, len(lines) + 1):
            fragment = "".join(lines[start:end])
            try:
                tomllib.loads(fragment)
            except tomllib.TOMLDecodeError:
                continue
            lines[start:end] = [] if remove else [f"{match.group(1)}{key} = {literal(value)}\n"]
            return "".join(lines)
        raise ValueError("无法安全定位配置项，已停止生成配置")
    if remove:
        return section
    if section and not section.endswith("\n"):
        section += "\n"
    return section + f"  {key} = {literal(value)}\n"


def patch_runner(block, runner, kind):
    if runner.get("executor") != "docker":
        raise ValueError("配置包含非 Docker Runner，拒绝修改")
    nested = re.findall(r"(?m)^\s*(\[\[runners\.[^\]\n]+\]\])", block)
    if any(header != SERVICE_HEADER for header in nested):
        raise ValueError("Runner 含其他嵌套数组配置，需要人工核对后再接入")
    services = runner.get("docker", {}).get("services", [])
    if services and services != [LEASE_SERVICE]:
        raise ValueError("Runner 已有其他服务配置，拒绝更改服务及健康等待策略")
    # 按表头定位范围，保留原始配置和注释。
    headers = list(re.finditer(r"(?m)^\s*(\[\[runners\]\]|\[\[runners\.docker\.services\]\]|\[runners\.[^\]\n]+\])[^\n]*\n?", block))
    sections = {}
    order = []
    for index, header in enumerate(headers):
        key = header.group(1)
        end = headers[index + 1].start() if index + 1 < len(headers) else len(block)
        if key in sections:
            raise ValueError("Runner 配置表重复，拒绝修改")
        sections[key] = block[header.start():end]
        order.append(key)
    top_key = "[[runners]]"
    top = sections[top_key]
    env = list(runner.get("environment", []))
    wanted = {"GIT_STRATEGY": "clone", "FF_ENABLE_JOB_CLEANUP": "true", "GIT_CLONE_PATH": f"/builds/{kind}/$CI_PROJECT_PATH"}
    env = [item for item in env if item.split("=", 1)[0] not in wanted]
    env.extend(f"{key}={value}" for key, value in wanted.items())
    script = runner.get("pre_build_script", "")
    if script != PRE_BUILD and not script.startswith(PRE_BUILD + "\n"):
        script = PRE_BUILD + ("\n" + script if script else "")
    for key, value in {"limit": 1, "request_concurrency": 1, "environment": env, "pre_build_script": script}.items():
        top = set_value(top, key, value)
    sections[top_key] = top
    custom = "[runners.custom_build_dir]"
    sections[custom] = set_value(sections.get(custom, custom + "\n"), "enabled", True)
    if custom not in order:
        order.append(custom)
    docker_key = "[runners.docker]"
    docker_section = sections.get(docker_key, docker_key + "\n")
    # 既有空数组或完整受管内联数组统一转为独立数组表，避免同名字段重复。
    docker_section = set_value(docker_section, "services", None, remove=True)
    volumes = runner.get("docker", {}).get("volumes", [])
    filtered = []
    for volume in volumes:
        parts = volume.split(":")
        destination = parts[1] if len(parts) > 1 else parts[0]
        if destination not in ("/builds", "/opt/ci-ram"):
            filtered.append(volume)
    filtered.extend([VOLUME + ":/builds", "/home/daniel/ci-ram/tools:/opt/ci-ram:ro"])
    for key, value in {"cpus": "4", "memory": "24g", "memory_swap": "24g", "volumes": filtered,
                       "service_memory": "64m", "service_memory_swap": "64m", "service_cpus": "0.1",
                       "wait_for_services_timeout": -1}.items():
        docker_section = set_value(docker_section, key, value)
    sections[docker_key] = docker_section
    if docker_key not in order:
        order.append(docker_key)
    # 该服务仅在作业期间保持共享卷挂载，覆盖源码 helper、主容器和归档 helper 的切换间隙。
    if SERVICE_HEADER not in sections:
        sections[SERVICE_HEADER] = SERVICE_HEADER + "\n"
        order.append(SERVICE_HEADER)
    for key, value in LEASE_SERVICE.items():
        sections[SERVICE_HEADER] = set_value(sections[SERVICE_HEADER], key, value)
    return "".join(sections[key] for key in order)


def patch_config(source, kind):
    if kind not in ("lower", "linux"):
        raise ValueError("Runner 类型必须为 lower 或 linux")
    try:
        parsed = tomllib.loads(source)
    except tomllib.TOMLDecodeError:
        raise ValueError("输入 TOML 配置无效，已隐藏原始内容") from None
    runners = parsed.get("runners", [])
    if len(runners) != 1:
        raise ValueError("配置必须仅包含一个指定 Runner，拒绝修改其他条目")
    starts = list(re.finditer(r"(?m)^\s*\[\[runners\]\][^\n]*\n?", source))
    if not runners or len(starts) != len(runners):
        raise ValueError("未找到可安全修改的 Runner 配置")
    prefix = source[:starts[0].start()]
    first_table = re.search(r"(?m)^\s*\[", prefix)
    global_end = first_table.start() if first_table else len(prefix)
    prefix = set_value(prefix[:global_end], "concurrent", 1) + prefix[global_end:]
    chunks = [prefix]
    for index, start in enumerate(starts):
        end = starts[index + 1].start() if index + 1 < len(starts) else len(source)
        chunks.append(patch_runner(source[start.start():end], runners[index], kind))
    result = "".join(chunks)
    tomllib.loads(result)
    return result


def docker(*args, check=True):
    result = subprocess.run(["docker", *args], capture_output=True, text=True, check=False)
    if check and result.returncode:
        raise ValueError("Docker 命令执行失败，请检查守护进程及权限")
    return result


def inspect_volume():
    result = docker("volume", "inspect", VOLUME, check=False)
    if result.returncode:
        # 只有明确不存在才允许创建，不能把权限或连接错误当作卷不存在。
        if "no such volume" in result.stderr.lower():
            return None
        raise ValueError("无法检查共享卷，请检查 Docker 连接及权限")
    try:
        values = json.loads(result.stdout)
        return values[0]
    except (ValueError, IndexError, TypeError):
        raise ValueError("Docker 返回的卷信息无效") from None


def volume_matches(volume):
    if not volume or volume.get("Driver") != "local":
        return False
    opts = volume.get("Options") or {}
    return (opts.get("type") == "tmpfs" and opts.get("device") == "tmpfs"
            and set(opts.get("o", "").split(",")) == set(OPTIONS["o"].split(","))
            and set(opts) == set(OPTIONS))


def ensure_volume():
    existing = inspect_volume()
    if existing is not None:
        if not volume_matches(existing):
            raise ValueError("同名卷配置与 24 GiB 无交换内存卷不一致，拒绝覆盖")
        print("共享内存卷配置正确，无需变更")
        return
    docker("volume", "create", "--driver", "local", "--opt", "type=tmpfs", "--opt", "device=tmpfs", "--opt", "o=" + OPTIONS["o"], VOLUME)
    if not volume_matches(inspect_volume()):
        raise ValueError("共享内存卷创建后校验失败")
    print("已创建 24 GiB 共享内存卷，按需占用且禁止交换")


def write_private(path, content):
    """输出与备份均只允许当前用户读写，避免凭据泄露。"""
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(content)
    os.chmod(path, 0o600)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Linux CI 共享内存配置管理")
    commands = parser.add_subparsers(dest="command", required=True)
    patcher = commands.add_parser("patch", help="生成配置并保留凭据原文")
    patcher.add_argument("--input", required=True)
    patcher.add_argument("--output", required=True)
    patcher.add_argument("--kind", choices=("lower", "linux"), required=True)
    patcher.add_argument("--backup", help="可选的原配置备份文件，权限为 600")
    commands.add_parser("inspect-volume", help="显示共享卷状态与容器消费者")
    commands.add_parser("ensure-volume", help="幂等创建共享内存卷")
    args = parser.parse_args(argv)
    try:
        if args.command == "patch":
            src, dest = Path(args.input), Path(args.output)
            if src.resolve() == dest.resolve():
                raise ValueError("输出必须与输入分离，审核后再替换线上配置")
            original = src.read_text(encoding="utf-8")
            result = patch_config(original, args.kind)
            if args.backup:
                write_private(Path(args.backup), original)
            write_private(dest, result)
            print("已生成共享内存 Runner 配置，未输出凭据；原文件未修改")
        elif args.command == "ensure-volume":
            ensure_volume()
        else:
            value = inspect_volume()
            print("共享卷：" + ("配置正确" if volume_matches(value) else "不存在或配置不一致"))
            if value:
                consumers = docker("ps", "-a", "--filter", "volume=" + VOLUME, "--format", "{{.ID}}\t{{.Status}}")
                print("关联共享卷的容器（运行中的消费者保持挂载，停止即释放）：")
                print(consumers.stdout.strip() or "无")
        return 0
    except (ValueError, OSError):
        # 不打印异常对象，避免第三方错误信息携带配置或凭据。
        print("操作失败：请检查输入配置、输出是否已存在、卷参数及 Docker 权限；未覆盖原配置", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
