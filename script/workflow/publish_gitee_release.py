#!/usr/bin/env python3
"""将构建产物上传到 Gitee Release。

该脚本只负责创建 Release 和上传附件，服务器同步由工作流随后通过 SSH
触发。大文件采用分块发送，避免一次性读入内存。
"""

from __future__ import annotations

import argparse
import http.client
import json
import mimetypes
import os
import shlex
import subprocess
import tempfile
import time
import uuid
import sys
from pathlib import Path
from typing import NoReturn
from urllib.parse import urlencode, urlsplit
from urllib.request import Request, urlopen


API_BASE = "https://gitee.com/api/v5"
CHUNK_SIZE = 1024 * 1024

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="上传构建产物到 Gitee Release")
    parser.add_argument("--owner", required=True, help="Gitee 用户名或组织名")
    parser.add_argument("--repo", required=True, help="Gitee 仓库名")
    parser.add_argument(
        "--token",
        default=os.environ.get("GITEE_TOKEN", ""),
        help="Gitee API Token，建议通过 GITEE_TOKEN 环境变量传入",
    )
    parser.add_argument("--tag", required=True, help="Release Tag")
    parser.add_argument("--name", required=True, help="Release 名称")
    parser.add_argument("--body", default="MSKDSP 自动构建产物", help="Release 描述")
    parser.add_argument("--target-commitish", default="master", help="目标分支或 commit SHA")
    parser.add_argument("--prerelease", action="store_true", help="标记为预发布")
    parser.add_argument("--asset", action="append", required=True, help="待上传附件，可重复")
    parser.add_argument("--output", default="", help="可选，写入 release_id 和 tag 的 JSON 文件")
    parser.add_argument("--product", choices=("lower", "upper"), help="通知服务器时的产品类型")
    parser.add_argument("--channel", help="通知服务器时的发布通道")
    parser.add_argument("--platform", help="通知服务器时的平台标识")
    parser.add_argument("--remote-root", help="通知服务器时的静态文件根目录")
    parser.add_argument("--ssh-key", default=os.environ.get("STATIC_UPDATE_SSH_KEY", ""), help="SSH 私钥内容")
    parser.add_argument("--ssh-host", default=os.environ.get("STATIC_UPDATE_SSH_HOST", ""), help="SSH 主机")
    parser.add_argument("--ssh-port", default=os.environ.get("STATIC_UPDATE_SSH_PORT", "22"), help="SSH 端口")
    parser.add_argument("--ssh-user", default=os.environ.get("STATIC_UPDATE_SSH_USER", ""), help="SSH 用户")
    parser.add_argument(
        "--remote-script",
        default="/home/daniel/update-server/relay/sync_gitee_release.py",
        help="服务器上的同步脚本路径",
    )
    return parser.parse_args()


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"Gitee 发布失败：{message}")


def request_json(url: str, method: str, form: dict[str, str], token: str) -> dict:
    payload = dict(form)
    payload["access_token"] = token
    request = Request(
        url,
        data=urlencode(payload).encode("utf-8"),
        method=method,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    try:
        with urlopen(request, timeout=60) as response:
            raw = response.read()
    except Exception as exc:  # pragma: no cover - 网络错误由运行环境决定
        fail(f"请求 {url} 失败：{exc}")
    try:
        value = json.loads(raw.decode("utf-8"))
    except json.JSONDecodeError as exc:
        fail(f"Gitee 返回的不是合法 JSON：{exc}")
    if not isinstance(value, dict):
        fail("Gitee 返回格式不是对象")
    return value


def get_existing_release(url: str, token: str) -> dict | None:
    request = Request(url, headers={"Accept": "application/json"})
    try:
        with urlopen(request, timeout=60) as response:
            value = json.loads(response.read().decode("utf-8"))
    except Exception as exc:
        if getattr(exc, "code", None) == 404:
            return None
        fail(f"查询已有 Release 失败：{exc}")
    if value is None:
        return None
    if not isinstance(value, dict):
        fail("查询已有 Release 返回格式错误")
    return value


def list_attachments(url: str) -> list[dict]:
    request = Request(url, headers={"Accept": "application/json"})
    try:
        with urlopen(request, timeout=60) as response:
            value = json.loads(response.read().decode("utf-8"))
    except Exception as exc:
        fail(f"查询 Release 附件失败：{exc}")
    if not isinstance(value, list) or not all(isinstance(item, dict) for item in value):
        fail("Gitee 附件列表返回格式错误")
    return value


def delete_attachment(url: str, token: str) -> None:
    request = Request(
        f"{url}?{urlencode({'access_token': token})}",
        method="DELETE",
        headers={"Accept": "application/json"},
    )
    try:
        with urlopen(request, timeout=60) as response:
            response.read()
    except Exception as exc:
        fail(f"删除旧附件失败：{exc}")


def notify_server(args: argparse.Namespace) -> None:
    fields = (args.product, args.channel, args.platform, args.remote_root)
    if not any(fields):
        return
    if not all(fields):
        fail("--product、--channel、--platform、--remote-root 必须同时提供")
    if not args.ssh_key or not args.ssh_host or not args.ssh_user:
        fail("通知服务器需要 SSH 私钥、主机和用户")

    key_path = Path(tempfile.gettempdir()) / f"mskdsp-static-update-key-{os.getpid()}"
    normalized_key = args.ssh_key.replace("\r\n", "\n")
    if not normalized_key.endswith("\n"):
        normalized_key += "\n"
    key_path.write_text(normalized_key, encoding="ascii")
    try:
        key_path.chmod(0o600)
        remote_parts = [
            "python3",
            args.remote_script,
            "--owner",
            args.owner,
            "--repo",
            args.repo,
            "--tag",
            args.tag,
            "--product",
            args.product,
            "--channel",
            args.channel,
            "--platform",
            args.platform,
            "--remote-root",
            args.remote_root,
        ]
        remote_command = " ".join(shlex.quote(part) for part in remote_parts)
        command = [
            "ssh",
            "-i",
            str(key_path),
            "-p",
            str(args.ssh_port),
            "-o",
            "StrictHostKeyChecking=accept-new",
            "-o",
            "IdentitiesOnly=yes",
            "-o",
            "ConnectTimeout=15",
            "-o",
            "ServerAliveInterval=15",
            "-o",
            "ServerAliveCountMax=3",
            f"{args.ssh_user}@{args.ssh_host}",
            remote_command,
        ]
        print(f"Gitee Release 上传完成，通知服务器立即同步：{args.tag}", flush=True)
        subprocess.run(command, check=True)
    finally:
        key_path.unlink(missing_ok=True)


def upload_file(url: str, token: str, file_path: Path) -> dict:
    parsed = urlsplit(url)
    if parsed.scheme != "https" or not parsed.hostname:
        fail(f"上传地址不合法：{url}")

    boundary = f"----mskdsp-{uuid.uuid4().hex}"
    boundary_bytes = boundary.encode("ascii")
    filename = file_path.name
    content_type = mimetypes.guess_type(filename)[0] or "application/octet-stream"
    prefix = (
        b"--"
        + boundary_bytes
        + b"\r\nContent-Disposition: form-data; name=\"access_token\"\r\n\r\n"
        + token.encode("utf-8")
        + b"\r\n--"
        + boundary_bytes
        + b"\r\nContent-Disposition: form-data; name=\"file\"; filename=\""
        + filename.encode("utf-8")
        + b"\"\r\nContent-Type: "
        + content_type.encode("ascii")
        + b"\r\n\r\n"
    )
    suffix = b"\r\n--" + boundary_bytes + b"--\r\n"
    content_length = len(prefix) + file_path.stat().st_size + len(suffix)
    path = parsed.path or "/"
    if parsed.query:
        path = f"{path}?{parsed.query}"

    connection = http.client.HTTPSConnection(parsed.hostname, parsed.port or 443, timeout=120)
    started = time.monotonic()
    sent = 0
    try:
        connection.putrequest("POST", path)
        connection.putheader("Content-Type", f"multipart/form-data; boundary={boundary}")
        connection.putheader("Content-Length", str(content_length))
        connection.endheaders()
        connection.send(prefix)
        with file_path.open("rb") as source:
            while True:
                chunk = source.read(CHUNK_SIZE)
                if not chunk:
                    break
                connection.send(chunk)
                sent += len(chunk)
                if sent == len(chunk) or sent == file_path.stat().st_size or sent % (16 * CHUNK_SIZE) == 0:
                    elapsed = max(time.monotonic() - started, 0.001)
                    speed = sent / elapsed / 1024 / 1024
                    print(f"上传 {filename}：{sent / 1024 / 1024:.2f} MiB，{speed:.2f} MiB/s", flush=True)
        connection.send(suffix)
        response = connection.getresponse()
        raw = response.read()
    except Exception as exc:  # pragma: no cover - 网络错误由运行环境决定
        fail(f"上传附件 {filename} 失败：{exc}")
    finally:
        connection.close()

    if response.status < 200 or response.status >= 300:
        fail(f"上传附件 {filename} 返回 HTTP {response.status}：{raw[:500].decode('utf-8', 'replace')}")
    try:
        value = json.loads(raw.decode("utf-8"))
    except json.JSONDecodeError as exc:
        fail(f"上传附件 {filename} 返回的不是合法 JSON：{exc}")
    if not isinstance(value, dict):
        fail(f"上传附件 {filename} 返回格式错误")
    return value


def main() -> None:
    args = parse_args()
    if not args.token.strip():
        fail("Token 为空")

    assets: list[Path] = []
    for raw_path in args.asset:
        path = Path(raw_path)
        if not path.is_file():
            fail(f"附件不存在：{path}")
        assets.append(path)

    # latest.json 必须最后上传，服务器以此判断本次附件已经完整。
    assets.sort(key=lambda path: path.name == "latest.json")
    release_url = f"{API_BASE}/repos/{args.owner}/{args.repo}/releases"
    print(f"正在创建 Gitee Release：{args.tag}")
    release = get_existing_release(f"{release_url}/tags/{args.tag}", args.token)
    if release is None:
        release = request_json(
            release_url,
            "POST",
            {
                "tag_name": args.tag,
                "name": args.name,
                "body": args.body,
                "target_commitish": args.target_commitish,
                "prerelease": "true" if args.prerelease else "false",
            },
            args.token,
        )
    else:
        print(f"Gitee Release 已存在，继续使用：{args.tag}")
    release_id = release.get("id")
    if not isinstance(release_id, int):
        fail(f"创建 Release 返回中缺少 id：{release}")

    upload_url = f"{release_url}/{release_id}/attach_files"
    existing_attachments = list_attachments(upload_url)
    existing_by_name = {
        item.get("name"): item
        for item in existing_attachments
        if isinstance(item.get("name"), str)
    }
    for asset in assets:
        existing = existing_by_name.get(asset.name)
        if existing is not None and isinstance(existing.get("id"), int):
            delete_url = f"{upload_url}/{existing['id']}"
            print(f"删除 Gitee Release 旧附件：{asset.name}")
            delete_attachment(delete_url, args.token)
        upload_file(upload_url, args.token, asset)
        print(f"已上传 Gitee 附件：{asset.name}")

    result = {"release_id": release_id, "tag": args.tag}
    if args.output:
        output_path = Path(args.output)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(json.dumps(result, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Gitee Release 上传完成：{args.tag}（ID: {release_id}）")
    notify_server(args)


if __name__ == "__main__":
    main()
