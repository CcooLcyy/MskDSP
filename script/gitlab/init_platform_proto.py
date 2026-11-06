#!/usr/bin/env python3
"""通过仅存于子进程环境的 CI 认证获取固定平台协议，不修改项目子模块。"""
import argparse
import base64
import os
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import urlsplit

PIN = "a9723c5ebade10a5588da59bdfc0c51123bfdf67"


class InitError(ValueError):
    """可安全输出的协议初始化错误。"""


def auth_environment(environ):
    server = environ.get("CI_SERVER_URL", "").rstrip("/")
    try:
        parsed = urlsplit(server)
        valid_port = parsed.port is None or 0 < parsed.port <= 65535
    except ValueError:
        raise InitError("GitLab 地址不合法") from None
    token = environ.get("CI_JOB_TOKEN", "")
    if (parsed.scheme not in ("http", "https") or not parsed.hostname or not valid_port or parsed.username
            or parsed.password or parsed.query or parsed.fragment or parsed.path not in ("", "/")
            or "\\" in server or any(char.isspace() or ord(char) < 32 for char in server)
            or not token or not token.isascii() or any(ord(char) < 33 or ord(char) > 126 for char in token)):
        raise InitError("GitLab 地址或作业认证环境不合法")
    environment = dict(environ)
    environment.pop("GIT_CONFIG_PARAMETERS", None)
    for name in list(environment):
        if name.startswith("GIT_CONFIG_KEY_") or name.startswith("GIT_CONFIG_VALUE_"):
            environment.pop(name)
    encoded = base64.b64encode(("gitlab-ci-token:" + token).encode()).decode()
    environment.update(GIT_TERMINAL_PROMPT="0", GIT_CONFIG_COUNT="1",
                       GIT_CONFIG_KEY_0=f"http.{server}/.extraHeader",
                       GIT_CONFIG_VALUE_0="Authorization: Basic " + encoded)
    return server, environment


def validate_protocol(destination):
    if not (destination / "platform/v1/software.proto").is_file():
        raise InitError("固定协议提交缺少软件仓库协议")


def initialize(destination, environ=None):
    server, environment = auth_environment(os.environ if environ is None else environ)
    pin_file = Path(__file__).with_name("platform-proto.ref")
    if pin_file.read_text(encoding="utf-8").strip() != PIN or not re.fullmatch(r"[a-f0-9]{40}", PIN):
        raise InitError("平台协议固定提交不一致")
    destination = Path(destination).absolute()
    marker = destination / ".git/mskdsp-lower-protocol-cache"
    if destination.is_symlink() or destination.exists() and not destination.is_dir():
        raise InitError("协议目标必须为独立临时目录")
    if destination.exists() and any(destination.iterdir()):
        if not marker.is_file() or marker.read_text(encoding="utf-8").strip() != PIN:
            raise InitError("拒绝覆盖未由本脚本创建的已有目录或仓库")
    destination.mkdir(parents=True, exist_ok=True)

    def run(arguments):
        result = subprocess.run(["git", *arguments], cwd=destination, env=environment,
                                text=True, capture_output=True, timeout=180, check=False)
        if result.returncode:
            raise InitError("获取平台固定协议失败，请检查网络、作业令牌及 allowlist")
        return result.stdout.strip()

    if not (destination / ".git").exists():
        run(["init"])
        marker.write_text(PIN + "\n", encoding="utf-8")
    repository = server + "/mskdsp/mskdspplatformproto.git"
    run(["fetch", "--no-tags", "--depth=1", repository, PIN])
    run(["checkout", "--detach", "--force", PIN])
    if run(["rev-parse", "HEAD"]) != PIN:
        raise InitError("平台协议检出提交与固定提交不一致")
    validate_protocol(destination)
    return destination


class SafeArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        raise SystemExit("协议初始化参数不合法；认证仅从 CI 环境读取")


def main(argv=None):
    parser = SafeArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, default=Path(".ci/platform-proto"))
    args = parser.parse_args(argv)
    try:
        initialize(args.destination)
    except Exception:
        print("平台协议初始化失败，请检查固定提交、网络及 CI 作业访问授权；底层输出已隐藏", file=sys.stderr)
        return 1
    print("平台固定协议已准备完成：" + PIN)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
