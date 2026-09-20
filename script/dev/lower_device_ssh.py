#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""下位机（真机）免交互 SSH 辅助脚本。

用途：在没有交互式终端的环境里（例如 Windows 侧自动化、Agent 会话），用同一种方式完成
联调排查最常用的三类操作，避免每次重新拼命令：

  1. check  验证 SSH 可达并打印远端基本环境信息
  2. run    在远端执行一行命令
  3. bash   把本地多行脚本以 base64 交给远端 bash 执行（规避 Windows 换行 \\r 污染 fi/done）
  4. sudo   以 root 执行一行命令（sudo 密码经 SSH 通道标准输入发送，不进入命令行参数）

凭据只来自 `--password` 或环境变量 `SSH_DEVICE_PASSWORD`，本脚本不保存任何密码。
"""

from __future__ import annotations

import argparse
import base64
import os
import sys

import paramiko

DEFAULT_HOST = "192.168.1.219"
DEFAULT_PORT = 10022
DEFAULT_USER = "megsky"

# 只读优先：默认不执行任何会改动设备的动作，调用方自己保证远端命令是安全的。
# 注意 docker 需要 root（megsky 不在 docker 组），容器信息用 `sudo run` 单独查。
CHECK_COMMAND = "hostname; uname -m; id -un; id -Gn"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="下位机（真机）免交互 SSH 辅助脚本",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--host", default=os.environ.get("MSKDSP_DEVICE_HOST", DEFAULT_HOST))
    parser.add_argument("--port", type=int, default=int(os.environ.get("MSKDSP_DEVICE_PORT", DEFAULT_PORT)))
    parser.add_argument("--user", default=os.environ.get("MSKDSP_DEVICE_USER", DEFAULT_USER))
    parser.add_argument(
        "--password",
        default=None,
        help="SSH 密码；不传则读取环境变量 SSH_DEVICE_PASSWORD",
    )
    parser.add_argument("--connect-timeout", type=float, default=20.0, help="连接与认证超时（秒）")
    parser.add_argument("--command-timeout", type=float, default=180.0, help="远端命令超时（秒）")

    subparsers = parser.add_subparsers(dest="action", required=True)

    subparsers.add_parser("check", help="验证 SSH 可达并打印远端基本环境信息")

    run_parser = subparsers.add_parser("run", help="在远端执行一行命令")
    run_parser.add_argument("command", help="远端命令（单行）")

    bash_parser = subparsers.add_parser("bash", help="把本地多行脚本交给远端 bash 执行（base64 传输）")
    bash_parser.add_argument("script", help="本地脚本路径；传 - 表示从标准输入读取")

    sudo_parser = subparsers.add_parser("sudo", help="以 root 执行一行命令（密码走 SSH stdin）")
    sudo_parser.add_argument("command", help="需要 root 权限的远端命令（单行）")
    sudo_parser.add_argument(
        "--sudo-password",
        default=None,
        help="sudo 密码；不传则复用 SSH 密码",
    )

    return parser


def resolve_password(explicit: str | None) -> str:
    password = explicit if explicit is not None else os.environ.get("SSH_DEVICE_PASSWORD", "")
    if not password:
        sys.stderr.write(
            "缺少 SSH 密码：请用 --password 传入，或先设置环境变量 SSH_DEVICE_PASSWORD。\n"
            "注意不要把密码拼进会被记录的命令行参数里。\n"
        )
        raise SystemExit(2)
    return password


def read_local_script(path: str) -> str:
    if path == "-":
        raw = sys.stdin.read()
    else:
        with open(path, "r", encoding="utf-8") as handle:
            raw = handle.read()
    # 统一换行：Windows 侧的 \r\n 若原样进 base64，会让远端 bash 再次遇到 \r 污染 fi/done。
    return raw.replace("\r\n", "\n").replace("\r", "\n")


def open_client(args: argparse.Namespace, password: str) -> paramiko.SSHClient:
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    sys.stderr.write(f"连接下位机: {args.user}@{args.host}:{args.port}\n")
    client.connect(
        args.host,
        port=args.port,
        username=args.user,
        password=password,
        timeout=args.connect_timeout,
        banner_timeout=args.connect_timeout,
        auth_timeout=args.connect_timeout,
        look_for_keys=False,
        allow_agent=False,
    )
    return client


def run_remote(client: paramiko.SSHClient, command: str, stdin_data: bytes | None, timeout: float) -> int:
    """执行远端命令；stdin_data 非空时写入该通道的标准输入（用于 base64 脚本或 sudo 密码）。"""
    stdin, stdout, stderr = client.exec_command(command, timeout=timeout)
    if stdin_data is not None:
        stdin.write(stdin_data)
        stdin.flush()
    # 无论是否写入数据，都关闭写端，让远端命令读到 EOF（base64 -d 与 sudo -S 都依赖 EOF 收尾）。
    stdin.channel.shutdown_write()

    out = stdout.read().decode("utf-8", "replace")
    err = stderr.read().decode("utf-8", "replace")
    sys.stdout.write(out)
    if err.strip():
        sys.stdout.write("---- stderr ----\n" + err)
    exit_status = stdout.channel.recv_exit_status()
    return exit_status


def main() -> int:
    args = build_parser().parse_args()
    password = resolve_password(args.password)

    client = open_client(args, password)
    try:
        if args.action == "check":
            return run_remote(client, CHECK_COMMAND, None, args.command_timeout)

        if args.action == "run":
            return run_remote(client, args.command, None, args.command_timeout)

        if args.action == "bash":
            script = read_local_script(args.script)
            # 用 base64 传输：纯 ASCII，规避 Windows 侧 \r 污染远端脚本关键字。
            payload = base64.b64encode(script.encode("utf-8"))
            return run_remote(client, "base64 -d | bash", payload + b"\n", args.command_timeout)

        if args.action == "sudo":
            sudo_password = args.sudo_password if args.sudo_password is not None else password
            if not sudo_password:
                sys.stderr.write("缺少 sudo 密码。\n")
                return 2
            # -S 让 sudo 从标准输入读密码；-p '' 关闭提示符，避免污染输出。
            command = f"sudo -S -p '' {args.command}"
            return run_remote(client, command, (sudo_password + "\n").encode("utf-8"), args.command_timeout)

        sys.stderr.write(f"未知动作: {args.action}\n")
        return 2
    finally:
        client.close()


if __name__ == "__main__":
    # Windows 控制台默认编码可能不是 UTF-8，远端中文日志需要显式按 UTF-8 输出。
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8")
    raise SystemExit(main())
