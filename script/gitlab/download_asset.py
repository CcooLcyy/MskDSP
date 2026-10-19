#!/usr/bin/env python3
"""通过官方 codeload 获取 GitHub 归档，并严格验证 vcpkg 期望的 SHA512。"""
import hashlib
import os
from pathlib import Path
import re
import sys
import tempfile
import time
from urllib.error import URLError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen


def download_url(value):
    if not isinstance(value, str) or re.search(r'[\s\x00-\x1f\x7f"\\]', value):
        raise ValueError("依赖资产地址不合法")
    try:
        parsed = urlsplit(value)
        port = parsed.port
    except ValueError:
        raise ValueError("依赖资产地址不合法") from None
    if (parsed.scheme != "https" or not parsed.hostname or parsed.username is not None
            or parsed.password is not None or parsed.fragment):
        raise ValueError("依赖资产地址必须为无凭据的 HTTPS URL")
    archive = re.fullmatch(r"/([A-Za-z0-9_.-]+)/([A-Za-z0-9_.-]+)/archive/(.+)\.tar\.gz", parsed.path)
    if parsed.hostname == "github.com" and port in (None, 443) and not parsed.query and archive:
        owner, repository, reference = archive.groups()
        return f"https://codeload.github.com/{owner}/{repository}/tar.gz/{reference}"
    return value


def download(url, expected_sha512, destination):
    resolved = download_url(url)
    if not isinstance(expected_sha512, str) or not re.fullmatch(r"[a-fA-F0-9]{128}", expected_sha512):
        raise ValueError("依赖资产 SHA512 不合法")
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    for attempt in range(3):
        temporary = None
        try:
            # 临时文件与目标位于同一文件系统，摘要确认后才原子替换既有目标。
            with tempfile.NamedTemporaryFile(prefix=".vcpkg-asset-", dir=destination.parent, delete=False) as output:
                temporary = Path(output.name)
                digest = hashlib.sha512()
                request = Request(resolved, headers={"User-Agent": "mskdsp-lower-ci/1"})
                with urlopen(request, timeout=30) as source:
                    for block in iter(lambda: source.read(1024 * 1024), b""):
                        digest.update(block)
                        output.write(block)
                output.flush()
                os.fsync(output.fileno())
            if digest.hexdigest() != expected_sha512.lower():
                raise ValueError("依赖资产 SHA512 校验失败，既有目标保持不变")
            os.replace(temporary, destination)
            print("依赖资产下载完成，SHA512 校验通过")
            return
        except (OSError, URLError):
            if attempt == 2:
                raise ValueError("依赖资产下载失败，已完成有限重试，既有目标保持不变") from None
            time.sleep(attempt + 1)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)


def main(arguments=None):
    arguments = sys.argv[1:] if arguments is None else arguments
    if len(arguments) != 3:
        print("用法：download_asset.py URL SHA512 目标路径", file=sys.stderr)
        return 1
    try:
        download(*arguments)
    except Exception:
        # 不打印 URL、查询参数或底层异常，防止资产地址中的临时令牌进入日志。
        print("依赖资产获取失败，请检查网络及期望 SHA512；底层输出已隐藏", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
