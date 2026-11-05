#!/usr/bin/env python3
"""将冻结的下位机安装包续传入库并发布来源通道，不执行设备安装。"""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import importlib
import importlib.util
import ipaddress
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

from init_platform_proto import auth_environment
from lower_metadata import release_environment


class ClientError(ValueError):
    """不包含底层异常和认证信息的可安全诊断。"""


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def safe_component(value):
    return (isinstance(value, str) and len(value) <= 180 and value not in (".", "..")
            and not value.endswith(".") and bool(re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]*", value)))


def repository_url(value):
    parsed = urlsplit(value)
    return (parsed.scheme in ("http", "https") and bool(parsed.hostname) and not parsed.username
            and not parsed.password and not parsed.query and not parsed.fragment
            and not any(char.isspace() or ord(char) < 32 for char in value) and "\\" not in value)


def load_metadata(metadata_path, environ=None):
    environment = os.environ if environ is None else environ
    path = Path(metadata_path).absolute()
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 64 * 1024:
        raise ClientError("缺少合法的冻结元数据文件")
    data = json.loads(path.read_text(encoding="utf-8"))
    fields = ("product", "version", "package_version", "platform", "file_name", "size", "sha256", "image_id",
              "source_repository", "source_ref", "source_sha", "build_time", "channel", "pipeline_id")
    if not isinstance(data, dict) or any(name not in data for name in fields):
        raise ClientError("冻结元数据字段不完整")
    if (data["product"] != "mskdsp-lower" or data["channel"] not in ("ci", "beta", "stable")
            or data["platform"] not in ("linux-arm64", "linux-x64")
            or any(not safe_component(data[name]) for name in ("version", "package_version", "file_name"))
            or data["file_name"] == "SHA256SUMS"
            or not isinstance(data["size"], int) or isinstance(data["size"], bool)
            or not 0 < data["size"] <= 8 * 1024 ** 3
            or not re.fullmatch(r"[a-f0-9]{64}", str(data["sha256"]))
            or not re.fullmatch(r"sha256:[a-f0-9]{64}", str(data["image_id"]))
            or not re.fullmatch(r"[a-f0-9]{40}", str(data["source_sha"]))
            or not repository_url(str(data["source_repository"]))):
        raise ClientError("下位机冻结身份、文件或摘要不合法")
    try:
        channel, branch_version, source_ref = release_environment(environment)
    except ValueError:
        raise ClientError("发布来源分支不合法") from None
    if (data["channel"] != channel
            or channel in ("beta", "stable") and data["platform"] != "linux-arm64"
            or channel == "stable" and (data["version"] != branch_version
                or data["package_version"] != branch_version + "-linux-arm64")
            or channel == "beta" and (data["platform"] != "linux-arm64" or not re.fullmatch(
                re.escape(branch_version) + r"-beta-p[1-9][0-9]*-b[1-9][0-9]*-j[1-9][0-9]*-" + data["source_sha"][:7],
                data["version"]))
            or environment.get("CI_PIPELINE_SOURCE") not in ("push", "web")
            or data["source_ref"] != source_ref
            or data["source_sha"] != environment.get("CI_COMMIT_SHA")
            or str(data["pipeline_id"]) != environment.get("CI_PIPELINE_ID")
            or not re.fullmatch(r"[1-9][0-9]*", str(data["pipeline_id"]))
            or data["source_repository"].rstrip("/") != environment.get("CI_PROJECT_URL", "").rstrip("/")):
        raise ClientError("仅允许受控引用发布自身冻结产物，Beta/Stable 仅支持 ARM64")
    try:
        moment = datetime.fromisoformat(data["build_time"].replace("Z", "+00:00"))
        if moment.tzinfo is None or moment.utcoffset() != timezone.utc.utcoffset(moment):
            raise ValueError()
    except (TypeError, ValueError, AttributeError):
        raise ClientError("构建时间必须为带 UTC 时区的 RFC3339 时间") from None
    archive = path.parent / data["file_name"]
    if (archive.is_symlink() or not archive.is_file() or archive.stat().st_size != data["size"]
            or sha256_file(archive) != data["sha256"]):
        raise ClientError("安装包文件、大小或 SHA256 与冻结元数据不一致")
    return archive, {name: data[name] for name in fields if name != "pipeline_id"}


def check_current_head(metadata, environ=None):
    environment = os.environ if environ is None else environ
    _, git_environment = auth_environment(environment)
    repository = metadata["source_repository"].rstrip("/")
    if not repository.endswith(".git"):
        repository += ".git"
    if metadata["channel"] == "stable":
        if (environment.get("CI_COMMIT_REF_PROTECTED") != "true"
                or metadata["source_ref"] != "v" + metadata["version"]
                or not re.fullmatch(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", metadata["version"])):
            raise ClientError("正式发布来源标签或保护状态不合法")
        tag = "refs/tags/" + metadata["source_ref"]
        series = ".".join(metadata["version"].split(".")[:2])
        branches = ("refs/heads/master", "refs/heads/main", "refs/heads/release/" + series)
        allowed = (tag, tag + "^{}", *branches)
        result = subprocess.run(["git", "ls-remote", "--exit-code", repository, *allowed],
                                env=git_environment, capture_output=True, text=True, timeout=60, check=False)
        refs = {}
        for line in result.stdout.splitlines():
            parts = line.split("\t")
            if (len(parts) != 2 or parts[1] not in allowed or parts[1] in refs
                    or not re.fullmatch(r"[a-f0-9]{40}", parts[0])):
                raise ClientError("远端标签及来源分支响应不合法")
            refs[parts[1]] = parts[0]
        commit = refs.get(tag + "^{}", refs.get(tag))
        if result.returncode or tag not in refs or commit != metadata["source_sha"]:
            raise ClientError("远端正式标签与冻结提交不一致，拒绝发布")
        # 发布作业浅克隆，使用独立临时仓库获取完整远端历史，不改源码工作区。
        with tempfile.TemporaryDirectory(prefix="lower-release-source-") as temporary:
            def git(*arguments):
                return subprocess.run(["git", *arguments], cwd=temporary, env=git_environment,
                                      capture_output=True, text=True, timeout=180, check=False)

            if git("init", "--bare").returncode:
                raise ClientError("无法初始化正式来源校验仓库")
            for branch in branches:
                if branch not in refs:
                    continue
                if git("fetch", "--no-tags", repository, branch).returncode:
                    raise ClientError("无法获取正式来源分支历史，拒绝发布")
                head = git("rev-parse", "FETCH_HEAD")
                if head.returncode or head.stdout.strip() != refs[branch]:
                    raise ClientError("来源分支在查询期间变化，拒绝本次发布")
                # 不可达提交可能未被 fetch；不存在即不属于此分支历史。
                if git("cat-file", "-e", commit + "^{commit}").returncode:
                    continue
                ancestor = git("merge-base", "--is-ancestor", commit, "FETCH_HEAD")
                if ancestor.returncode == 0:
                    # 获取完整历史可能耗时，再次核对标签防止查询期间被移动。
                    current = subprocess.run(["git", "ls-remote", "--exit-code", repository, tag, tag + "^{}"],
                                             env=git_environment, capture_output=True, text=True,
                                             timeout=60, check=False)
                    expected_tag = {key: value for key, value in refs.items() if key in (tag, tag + "^{}")}
                    expected_lines = {value + "\t" + key for key, value in expected_tag.items()}
                    if current.returncode or set(current.stdout.splitlines()) != expected_lines:
                        raise ClientError("正式标签在历史校验期间变化，拒绝发布")
                    print("正式来源历史校验通过：" + branch)
                    return
                if ancestor.returncode != 1:
                    raise ClientError("无法确认正式提交的历史归属，拒绝发布")
        raise ClientError("正式提交不属于主干或同版本维护分支历史，拒绝发布")
    result = subprocess.run(["git", "ls-remote", "--exit-code", repository,
                             "refs/heads/" + metadata["source_ref"]], env=git_environment,
                            capture_output=True, text=True, timeout=60, check=False)
    expected = metadata["source_sha"] + "\trefs/heads/" + metadata["source_ref"]
    if result.returncode or result.stdout.strip() != expected:
        raise ClientError("远端来源分支已变化或无法确认，拒绝发布旧构建")


def validate_endpoint(endpoint, *, plaintext=False):
    try:
        parsed = urlsplit("//" + endpoint)
        valid_port = parsed.port and 0 < parsed.port <= 65535
    except ValueError:
        raise ClientError("gRPC 地址必须为无凭据的 host:port") from None
    if (not parsed.hostname or not valid_port or parsed.username is not None or parsed.password is not None
            or parsed.path or parsed.query or parsed.fragment or "\\" in endpoint
            or any(char.isspace() or ord(char) < 32 for char in endpoint)):
        raise ClientError("gRPC 地址必须为无凭据的 host:port")
    if plaintext:
        try:
            address = ipaddress.ip_address(parsed.hostname)
            networks = ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "127.0.0.0/8", "::1/128", "fc00::/7")
            if not any(address in ipaddress.ip_network(network) for network in networks):
                raise ValueError()
        except ValueError:
            raise ClientError("明文上传仅允许显式内网或回环 IP") from None
    return endpoint


def generate_bindings(proto_root, output_dir):
    if not all(importlib.util.find_spec(name) for name in ("grpc", "grpc_tools")):
        raise ClientError("缺少 Python gRPC 依赖，请使用 CI 工具镜像")
    protocol = Path(proto_root).resolve() / "platform/v1/software.proto"
    if not protocol.is_file():
        raise ClientError("缺少固定的软件仓库协议")
    output = Path(output_dir).resolve()
    output.mkdir(parents=True, exist_ok=True)
    result = subprocess.run([sys.executable, "-m", "grpc_tools.protoc", f"-I{protocol.parent}",
                             f"--python_out={output}", f"--grpc_python_out={output}", protocol.name],
                            cwd=protocol.parent, capture_output=True, timeout=60, check=False)
    if result.returncode or not all((output / name).is_file() for name in ("software_pb2.py", "software_pb2_grpc.py")):
        raise ClientError("生成软件仓库 gRPC 绑定失败")
    return output


@contextmanager
def load_bindings(directory):
    names = ("software_pb2", "software_pb2_grpc")
    previous = {name: sys.modules.pop(name, None) for name in names}
    sys.path.insert(0, str(directory))
    importlib.invalidate_caches()
    try:
        yield tuple(importlib.import_module(name) for name in names)
    finally:
        sys.path.remove(str(directory))
        for name in names:
            sys.modules.pop(name, None)
            if previous[name] is not None:
                sys.modules[name] = previous[name]


def confirm_manifest(base_url, metadata, *, timeout=30):
    """通过公开清单核对发布身份；HTTP 请求不携带上传令牌。"""
    if not repository_url(base_url):
        raise ClientError("平台公开地址必须为无凭据 HTTP(S) 地址")
    product_path = "mskdsp-lower" if metadata["platform"] == "linux-arm64" else "mskdsp-lower-x64"
    route = f"/{product_path}/{metadata['channel']}/latest.json"
    request = Request(base_url.rstrip("/") + route, headers={"Cache-Control": "no-cache"})
    with urlopen(request, timeout=timeout) as response:
        body = response.read(1024 * 1024 + 1)
    if len(body) > 1024 * 1024:
        raise ClientError("公开清单大小不合法")
    manifest = json.loads(body)
    expected = {name: metadata[name] for name in ("product", "version", "package_version", "platform", "image_id", "channel")}
    if (manifest.get("schema_version") != 1 or any(manifest.get(key) != value for key, value in expected.items())
            or manifest.get("source") != dict(repository=metadata["source_repository"], ref=metadata["source_ref"], sha=metadata["source_sha"])
            or any(manifest.get("asset", {}).get(key) != metadata[field]
                   for key, field in (("name", "file_name"), ("size", "size"), ("sha256", "sha256")))):
        raise ClientError("公开清单身份与本次安装包不一致")


def transfer(publisher, catalog, messages, archive, metadata, token, *, before_publish,
             attempts=4, retry_delay=1, timeout=60, chunk_size=1024 * 1024, after_publish=None):
    import grpc
    channel = metadata["channel"]
    if channel not in ("ci", "beta", "stable") or channel in ("beta", "stable") and metadata["platform"] != "linux-arm64":
        raise ClientError("发布通道不合法，Beta/Stable 仅支持 ARM64")
    if (not 1 <= attempts <= 10 or not 0 < timeout <= 1800 or not 1 <= chunk_size <= 1024 * 1024
            or not token or not token.isascii() or any(ord(char) < 33 or ord(char) > 126 for char in token)):
        raise ClientError("上传配置或发布令牌不合法")
    authorization = (("authorization", "Bearer " + token),)
    expected = messages.PackageMetadata(**metadata)
    retryable = (grpc.StatusCode.UNAVAILABLE, grpc.StatusCode.DEADLINE_EXCEEDED, grpc.StatusCode.CANCELLED)

    def call(method, request, *, authenticated=True):
        for attempt in range(attempts):
            try:
                return method(request, metadata=authorization if authenticated else (), timeout=timeout)
            except grpc.RpcError as error:
                if error.code() not in retryable or attempt == attempts - 1:
                    raise ClientError("平台 RPC 失败：" + error.code().name) from None
                time.sleep(retry_delay)

    created = call(publisher.CreateUpload, messages.CreateUploadRequest(metadata=expected)).upload
    if not created.id:
        raise ClientError("平台未返回上传任务身份")
    upload_id = created.id

    def state():
        current = call(publisher.GetUpload, messages.GetUploadRequest(upload_id=upload_id)).upload
        if (current.id != upload_id or current.metadata != expected or current.received_bytes > metadata["size"]
                or current.state not in ("uploading", "completed")
                or current.state == "completed" and (not current.package_id or current.received_bytes != metadata["size"])):
            raise ClientError("平台上传状态与冻结身份不一致")
        return current

    def chunks(offset):
        with Path(archive).open("rb") as source:
            source.seek(offset)
            while block := source.read(chunk_size):
                yield messages.UploadPackageRequest(upload_id=upload_id, offset=offset, data=block)
                offset += len(block)

    package_id = ""
    for attempt in range(attempts):
        current = state()
        if current.state == "completed":
            package_id = current.package_id
            break
        try:
            if current.received_bytes < metadata["size"]:
                publisher.UploadPackage(chunks(current.received_bytes), metadata=authorization, timeout=timeout)
            current = state()
            if current.received_bytes != metadata["size"]:
                raise ClientError("上传结束后实际长度仍不完整")
            package = publisher.CompleteUpload(messages.CompleteUploadRequest(upload_id=upload_id),
                                               metadata=authorization, timeout=timeout).package
            current = state()
            if (not package.id or package.metadata != expected or current.state != "completed"
                    or current.package_id != package.id):
                raise ClientError("完成入库响应与冻结身份不一致")
            package_id = package.id
            break
        except grpc.RpcError as error:
            current = state()
            if current.state == "completed":
                package_id = current.package_id
                break
            if error.code() not in retryable or attempt == attempts - 1:
                raise ClientError("平台上传失败：" + error.code().name) from None
            time.sleep(retry_delay)
    if not package_id:
        raise ClientError("平台未确认完成入库")
    print("下位机安装包已完成校验入库，准备发布通道：" + channel)

    def confirm_public():
        package = call(catalog.GetPackage, messages.GetPackageRequest(package_id=package_id), authenticated=False).package
        packages = call(catalog.ListPackages, messages.ListPackagesRequest(product="mskdsp-lower", channel=channel,
                                                                          platform=metadata["platform"]), authenticated=False).packages
        if (package.id != package_id or package.metadata != expected or channel not in package.channels
                or not package.ever_published or not any(item.id == package_id and item.metadata == expected
                                                         and channel in item.channels for item in packages)):
            raise ClientError("公开目录来源通道与本次安装包不一致")

    for attempt in range(attempts):
        before_publish()
        try:
            package = publisher.PublishRelease(messages.PublishReleaseRequest(package_id=package_id, channel=channel),
                                               metadata=authorization, timeout=timeout).package
            if package.id != package_id or package.metadata != expected or channel not in package.channels:
                raise ClientError("平台发布响应与安装包身份不一致")
            confirm_public()
            if after_publish:
                after_publish()
            return dict(state="published", package_id=package_id, upload_id=upload_id)
        except grpc.RpcError as error:
            if error.code() not in retryable:
                raise ClientError("平台发布失败：" + error.code().name) from None
            try:
                confirm_public()
                if after_publish:
                    after_publish()
                return dict(state="published", package_id=package_id, upload_id=upload_id)
            except ClientError:
                if attempt == attempts - 1:
                    raise ClientError("无法确认平台来源通道发布结果") from None
            time.sleep(retry_delay)
    raise ClientError("平台来源通道发布未完成")


def upload(metadata, proto_root=Path(".ci/platform-proto"), timeout=60):
    archive, identity = load_metadata(metadata)
    check_current_head(identity)
    plaintext = os.environ.get("PLATFORM_GRPC_PLAINTEXT", "0") == "1"
    endpoint = validate_endpoint(os.environ.get("PLATFORM_GRPC_ENDPOINT", ""), plaintext=plaintext)
    ca_file = os.environ.get("PLATFORM_GRPC_CA_FILE")
    if plaintext and ca_file:
        raise ClientError("明文连接不能配置 TLS CA 文件")
    public_base = os.environ.get("PLATFORM_PUBLIC_BASE_URL", "")
    if public_base and not repository_url(public_base):
        raise ClientError("平台公开地址必须为无凭据 HTTP(S) 地址")
    with tempfile.TemporaryDirectory(prefix="lower-platform-grpc-") as temporary:
        generate_bindings(proto_root, temporary)
        import grpc
        with load_bindings(temporary) as (messages, services):
            if plaintext:
                channel = grpc.insecure_channel(endpoint)
            else:
                roots = Path(ca_file).read_bytes() if ca_file else None
                channel = grpc.secure_channel(endpoint, grpc.ssl_channel_credentials(roots))
            with channel:
                return transfer(services.SoftwarePublishServiceStub(channel), services.SoftwareCatalogServiceStub(channel),
                                messages, archive, identity, os.environ.get("PLATFORM_UPLOAD_TOKEN", ""),
                                before_publish=lambda: check_current_head(identity), timeout=timeout,
                                after_publish=(lambda: confirm_manifest(public_base, identity, timeout=timeout)) if public_base else None)


class SafeArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        raise SystemExit("上传参数不合法；令牌仅从 CI 环境读取")


def main(argv=None):
    parser = SafeArgumentParser(description=__doc__)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--proto-root", type=Path, default=Path(".ci/platform-proto"))
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args(argv)
    try:
        result = upload(**vars(args))
    except Exception:
        print("下位机平台上传发布失败，请检查冻结身份、来源分支、权限、网络及协议；底层输出已隐藏", file=sys.stderr)
        return 1
    print("下位机来源通道发布完成：package_id=" + result["package_id"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
