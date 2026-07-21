#!/usr/bin/env python3
"""生成下位机静态更新清单。"""

from __future__ import annotations

import argparse
import json
import re
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import quote


CHANNEL_PATTERN = re.compile(r"^[A-Za-z0-9._-]+$")
PLATFORM_PATTERN = re.compile(r"^[A-Za-z0-9._-]+$")
SHA256_PATTERN = re.compile(r"^[0-9a-fA-F]{64}$")
IMAGE_ID_PATTERN = re.compile(r"^sha256:[0-9a-fA-F]{64}$")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="生成下位机静态更新 latest.json")
    parser.add_argument("--channel", required=True, help="发布通道，例如 stable/beta/nightly/ci")
    parser.add_argument("--platform", default="linux-arm64", help="目标平台，默认 linux-arm64")
    parser.add_argument("--version", required=True, help="完整包版本，通常与镜像 tag 一致")
    parser.add_argument("--display-version", default="", help="界面展示版本，默认从 --version 去掉平台后缀")
    parser.add_argument("--image-id", required=True, help="Docker 镜像 config ID，例如 sha256:<64位十六进制>")
    parser.add_argument("--artifact", required=True, help="自解压安装包路径")
    parser.add_argument("--checksums", default="SHA256SUMS", help="SHA256SUMS 文件路径")
    parser.add_argument("--output", default="latest.json", help="输出 latest.json 路径")
    parser.add_argument("--base-url", required=True, help="静态更新根 URL，例如 https://update.example/mskdsp-lower")
    parser.add_argument("--repository", default="", help="来源仓库")
    parser.add_argument("--source-ref", default="", help="来源 ref/tag/branch")
    parser.add_argument("--source-sha", default="", help="来源 commit sha")
    parser.add_argument("--published-at", default="", help="发布时间 RFC3339，默认当前 UTC 时间")
    return parser.parse_args()


def require_simple_path_part(name: str, value: str, pattern: re.Pattern[str]) -> None:
    if not value or not pattern.fullmatch(value):
        raise SystemExit(f"{name} 只能包含字母、数字、点、下划线和短横线: {value}")


def normalize_image_id(value: str) -> str:
    normalized = value.strip()
    if not IMAGE_ID_PATTERN.fullmatch(normalized):
        raise SystemExit(f"image_id 格式不合法，应为 sha256:<64位十六进制>: {value}")
    return normalized.lower()


def read_sha256(checksums_path: Path, artifact_name: str) -> str:
    if not checksums_path.is_file():
        raise SystemExit(f"未找到校验文件: {checksums_path}")

    for line in checksums_path.read_text(encoding="utf-8").splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        digest = parts[0]
        file_name = parts[-1].lstrip("*")
        if Path(file_name).name == artifact_name:
            if not SHA256_PATTERN.fullmatch(digest):
                raise SystemExit(f"SHA256 格式不合法: {digest}")
            return digest.lower()

    raise SystemExit(f"未在 {checksums_path} 中找到安装包校验值: {artifact_name}")


def display_version_from_package_version(package_version: str, platform: str) -> str:
    suffix = f"-{platform}"
    if package_version.endswith(suffix):
        package_version = package_version[: -len(suffix)]
    if len(package_version) > 1 and package_version[0] == "v" and package_version[1].isdigit():
        return package_version[1:]
    return package_version


def join_static_url(base_url: str, *parts: str) -> str:
    current = base_url.rstrip("/")
    for part in parts:
        current = f"{current}/{quote(part.strip('/'))}"
    return current


def rfc3339_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def main() -> None:
    args = parse_args()
    require_simple_path_part("channel", args.channel, CHANNEL_PATTERN)
    require_simple_path_part("platform", args.platform, PLATFORM_PATTERN)

    artifact_path = Path(args.artifact)
    if not artifact_path.is_file():
        raise SystemExit(f"未找到安装包: {artifact_path}")

    checksums_path = Path(args.checksums)
    output_path = Path(args.output)
    artifact_name = artifact_path.name
    checksums_name = checksums_path.name
    image_id = normalize_image_id(args.image_id)
    sha256 = read_sha256(checksums_path, artifact_name)
    published_at = args.published_at or rfc3339_now()
    display_version = args.display_version or display_version_from_package_version(args.version, args.platform)

    asset_base_url = join_static_url(args.base_url, args.channel, args.platform)
    manifest = {
        "schema_version": 1,
        "product": "mskdsp-lower",
        "channel": args.channel,
        "platform": args.platform,
        "version": display_version,
        "package_version": args.version,
        "image_id": image_id,
        "published_at": published_at,
        "source": {
            "repository": args.repository,
            "ref": args.source_ref,
            "sha": args.source_sha,
        },
        "asset": {
            "name": artifact_name,
            "url": join_static_url(asset_base_url, artifact_name),
            "sha256": sha256,
            "size": artifact_path.stat().st_size,
        },
        "checksum": {
            "name": checksums_name,
            "url": join_static_url(asset_base_url, checksums_name),
        },
    }

    output_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"已生成下位机静态更新清单: {output_path}")
    print(f"安装包 URL: {manifest['asset']['url']}")


if __name__ == "__main__":
    main()
