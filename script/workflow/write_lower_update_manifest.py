#!/usr/bin/env python3
"""生成下位机静态更新清单。"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import tarfile
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import quote


CHANNEL_PATTERN = re.compile(r"^[A-Za-z0-9._-]+$")
PLATFORM_PATTERN = re.compile(r"^[A-Za-z0-9._-]+$")
SHA256_PATTERN = re.compile(r"^[0-9a-fA-F]{64}$")
IMAGE_ID_PATTERN = re.compile(r"^sha256:[0-9a-fA-F]{64}$")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="生成下位机静态更新 latest.json")
    parser.add_argument("--channel", required=True, help="发布通道，例如 stable/beta/ci")
    parser.add_argument("--platform", default="linux-arm64", help="目标平台，默认 linux-arm64")
    parser.add_argument("--version", required=True, help="完整包版本，通常与镜像 tag 一致")
    parser.add_argument("--display-version", default="", help="界面展示版本，默认从 --version 去掉平台后缀")
    identity = parser.add_mutually_exclusive_group(required=True)
    identity.add_argument("--image-id", help="显式 config 摘要，兼容既有调用，不接受 manifest/index 摘要")
    identity.add_argument("--image-tar", help="从镜像归档验证并计算 config 摘要，发布流程优先使用")
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


def read_image_config_id(image_tar: Path, platform: str) -> str:
    """仅读取归档元数据，按原始配置内容计算平台一致的构建身份。"""
    def read_metadata(archive: tarfile.TarFile, name: str) -> bytes:
        members = [member for member in archive.getmembers() if member.name == name]
        if len(members) != 1:
            raise ValueError(f"镜像元数据缺失或路径重复: {name}")
        member = members[0]
        if not member.isfile() or member.size > 1024 * 1024:
            raise ValueError(f"镜像元数据必须为不超过 1 MiB 的普通文件: {name}")
        stream = archive.extractfile(member)
        if stream is None:
            raise ValueError(f"镜像元数据无法读取: {name}")
        return stream.read()

    try:
        with tarfile.open(image_tar, "r:*") as archive:
            manifest = json.loads(read_metadata(archive, "manifest.json"))
            if not isinstance(manifest, list) or len(manifest) != 1:
                raise ValueError("发布归档必须明确包含单个镜像")
            name = manifest[0].get("Config", "")
            if not isinstance(name, str) or not re.fullmatch(
                r"(?:[0-9a-fA-F]{64}\.json|blobs/sha256/[0-9a-fA-F]{64})", name
            ):
                raise ValueError("镜像配置摘要路径不合法")
            raw = read_metadata(archive, name)
            digest = hashlib.sha256(raw).hexdigest()
            path_digest = name.rsplit("/", 1)[-1].removesuffix(".json").lower()
            if digest != path_digest:
                raise ValueError("镜像配置内容与路径声明的摘要不一致")
            config = json.loads(raw)
            architecture = {"linux-arm64": "arm64", "linux-x64": "amd64"}.get(platform)
            if architecture is None or config.get("os") != "linux" or config.get("architecture") != architecture:
                raise ValueError(f"镜像配置平台与发布平台不一致: {platform}")
            if any(member.name == "index.json" for member in archive.getmembers()):
                index = json.loads(read_metadata(archive, "index.json"))
                descriptors = index.get("manifests")
                if index.get("schemaVersion") != 2 or not isinstance(descriptors, list) or len(descriptors) != 1:
                    raise ValueError("OCI 发布归档必须明确关联单个镜像")
                descriptor = descriptors[0]
                manifest_id = normalize_image_id(descriptor.get("digest", ""))
                manifest_raw = read_metadata(archive, "blobs/sha256/" + manifest_id[7:])
                if "sha256:" + hashlib.sha256(manifest_raw).hexdigest() != manifest_id:
                    raise ValueError("OCI manifest 内容摘要与索引关联不一致")
                image_manifest = json.loads(manifest_raw)
                if image_manifest.get("schemaVersion") != 2 or image_manifest.get("config", {}).get("digest") != "sha256:" + digest:
                    raise ValueError("OCI manifest 与镜像配置关联不一致")
                indexed_platform = descriptor.get("platform") or {}
                for field in ("os", "architecture", "variant"):
                    if field in indexed_platform and indexed_platform[field] != config.get(field):
                        raise ValueError("OCI 索引平台与镜像配置不一致")
            return "sha256:" + digest
    except (OSError, tarfile.TarError, KeyError, ValueError, TypeError, AttributeError) as error:
        raise SystemExit(f"读取镜像归档配置摘要失败: {error}") from error


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
    image_id = (read_image_config_id(Path(args.image_tar), args.platform)
                if args.image_tar else normalize_image_id(args.image_id))
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
    print(f"镜像配置摘要: {image_id}")
    print(f"安装包 URL: {manifest['asset']['url']}")


if __name__ == "__main__":
    main()
