#!/usr/bin/env python3
"""从公开 Gitee Release 拉取更新包并原子发布到静态目录。"""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
import re
import shutil
import tempfile
import time
from pathlib import Path
from typing import NoReturn
from urllib.parse import quote
from urllib.request import Request, urlopen


API_BASE = "https://gitee.com/api/v5"
CHUNK_SIZE = 1024 * 1024
SAFE_NAME = re.compile(r"^[A-Za-z0-9._-]+$")


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"Gitee 同步失败：{message}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="从 Gitee Release 拉取并发布更新包")
    parser.add_argument("--owner", required=True, help="Gitee 用户名或组织名")
    parser.add_argument("--repo", required=True, help="Gitee 仓库名")
    parser.add_argument("--tag", required=True, help="Release Tag")
    parser.add_argument("--product", choices=("lower", "upper"), required=True, help="产品类型")
    parser.add_argument("--channel", required=True, help="发布通道")
    parser.add_argument("--platform", required=True, help="平台标识")
    parser.add_argument("--remote-root", required=True, help="静态文件根目录")
    parser.add_argument("--state-root", default="", help="状态文件目录，默认放在 update-server 根目录")
    parser.add_argument("--force", action="store_true", help="即使已同步过也重新下载")
    return parser.parse_args()


def fetch_json(url: str) -> object:
    request = Request(url, headers={"Accept": "application/json", "User-Agent": "mskdsp-update-relay/1"})
    try:
        with urlopen(request, timeout=60) as response:
            return json.loads(response.read().decode("utf-8"))
    except Exception as exc:  # pragma: no cover - 网络错误由运行环境决定
        fail(f"请求 {url} 失败：{exc}")


def download(url: str, destination: Path) -> int:
    request = Request(url, headers={"User-Agent": "mskdsp-update-relay/1"})
    started = time.monotonic()
    total = 0
    try:
        with urlopen(request, timeout=120) as response, destination.open("wb") as output:
            while True:
                chunk = response.read(CHUNK_SIZE)
                if not chunk:
                    break
                output.write(chunk)
                total += len(chunk)
                if total == len(chunk) or total % (16 * CHUNK_SIZE) == 0:
                    elapsed = max(time.monotonic() - started, 0.001)
                    speed = total / elapsed / 1024 / 1024
                    print(f"下载 {destination.name}：{total / 1024 / 1024:.2f} MiB，{speed:.2f} MiB/s", flush=True)
    except Exception as exc:  # pragma: no cover - 网络错误由运行环境决定
        fail(f"下载 {url} 失败：{exc}")
    return total


def verify_checksums(staging: Path, checksum_path: Path) -> None:
    lines = checksum_path.read_text(encoding="utf-8").splitlines()
    checked = 0
    for line in lines:
        parts = line.split()
        if len(parts) < 2:
            continue
        digest, raw_name = parts[0].lower(), parts[-1].lstrip("*")
        if not re.fullmatch(r"[0-9a-f]{64}", digest):
            continue
        if not SAFE_NAME.fullmatch(raw_name):
            fail(f"校验文件包含不安全的文件名：{raw_name}")
        target = staging / raw_name
        if not target.is_file():
            fail(f"校验文件引用的附件未下载：{raw_name}")
        digest_hash = hashlib.sha256()
        with target.open("rb") as source:
            for chunk in iter(lambda: source.read(CHUNK_SIZE), b""):
                digest_hash.update(chunk)
        actual = digest_hash.hexdigest()
        if actual != digest:
            fail(f"SHA256 校验失败：{raw_name}")
        checked += 1
    if checked == 0:
        fail(f"校验文件没有有效条目：{checksum_path.name}")
    print(f"已完成 SHA256 校验：{checked} 个文件")


def main() -> None:
    args = parse_args()
    if not SAFE_NAME.fullmatch(args.tag) or not SAFE_NAME.fullmatch(args.channel) or not SAFE_NAME.fullmatch(args.platform):
        fail("tag、channel、platform 只能包含字母、数字、点、下划线和短横线")

    root = Path(args.remote_root).expanduser().resolve()
    channel_dir = root / args.channel
    asset_dir = channel_dir / args.platform
    state_dir = Path(args.state_root).expanduser().resolve() if args.state_root else root.parent.parent / ".gitee-relay-state"
    state_dir.mkdir(parents=True, exist_ok=True)
    state_path = state_dir / f"{args.product}-{args.channel}-{args.platform}.tag"
    lock_path = state_dir / ".sync.lock"

    with lock_path.open("w", encoding="utf-8") as lock_file:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
        if not args.force and state_path.is_file() and state_path.read_text(encoding="utf-8").strip() == args.tag:
            print(f"Gitee Release 已同步，跳过：{args.tag}")
            return

        release_url = f"{API_BASE}/repos/{quote(args.owner)}/{quote(args.repo)}/releases/tags/{quote(args.tag)}"
        release = fetch_json(release_url)
        if not isinstance(release, dict) or not isinstance(release.get("id"), int):
            fail(f"Release 响应格式错误：{release}")
        release_id = release["id"]
        attachments_url = f"{API_BASE}/repos/{quote(args.owner)}/{quote(args.repo)}/releases/{release_id}/attach_files?per_page=100"
        attachments = fetch_json(attachments_url)
        if not isinstance(attachments, list) or not attachments:
            fail("Release 没有附件")

        staging = Path(tempfile.mkdtemp(prefix=f".gitee-{args.tag}-", dir=str(root)))
        try:
            for attachment in attachments:
                if not isinstance(attachment, dict):
                    fail(f"附件响应格式错误：{attachment}")
                name = attachment.get("name")
                download_url = attachment.get("browser_download_url")
                if not isinstance(name, str) or not SAFE_NAME.fullmatch(name):
                    fail(f"附件文件名不安全：{name}")
                if not isinstance(download_url, str) or not download_url.startswith("https://"):
                    fail(f"附件下载地址不安全：{name}")
                print(f"开始下载 Gitee 附件：{name}")
                download(download_url, staging / name)

            manifest = staging / "latest.json"
            if not manifest.is_file():
                fail("Release 缺少 latest.json")
            try:
                json.loads(manifest.read_text(encoding="utf-8"))
            except json.JSONDecodeError as exc:
                fail(f"latest.json 不是合法 JSON：{exc}")

            checksum_files = sorted(staging.glob("*SHA256SUMS*"))
            if not checksum_files:
                fail("Release 缺少 SHA256SUMS 文件")
            verify_checksums(staging, checksum_files[0])

            asset_dir.mkdir(parents=True, exist_ok=True)
            channel_dir.mkdir(parents=True, exist_ok=True)
            for source in staging.iterdir():
                if source.name == "latest.json":
                    continue
                target = asset_dir / source.name
                os.replace(source, target)
            os.replace(manifest, channel_dir / "latest.json")
            state_path.write_text(args.tag + "\n", encoding="utf-8")
            print(f"Gitee Release 已发布到静态目录：{args.product}/{args.channel}/{args.platform}（{args.tag}）")
        finally:
            shutil.rmtree(staging, ignore_errors=True)


if __name__ == "__main__":
    main()
