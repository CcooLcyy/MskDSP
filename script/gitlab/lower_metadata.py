#!/usr/bin/env python3
"""冻结下位机 CI 构建来源和安装包身份。"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import sys
from urllib.parse import urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "workflow"))
from write_lower_update_manifest import read_image_config_id


def require(condition, message):
    if not condition:
        raise ValueError(message)


def release_context(branch):
    """由主干或维护分支推导 CI 通道，保留历史 Beta 身份解析。"""
    if branch in ("master", "main"):
        return "ci", None
    if re.fullmatch(r"release/(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", branch or ""):
        return "ci", None
    match = re.fullmatch(r"beta/((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))", branch or "")
    require(match is not None, "仅允许主干、release/主版本.次版本或历史 Beta 引用")
    return "beta", match.group(1)


def validate_environment(environment):
    release_environment(environment)
    require(environment.get("CI_PIPELINE_SOURCE") in ("push", "web"), "仅允许受控引用 push 或手动流水线")
    for field in ("CI_PIPELINE_ID", "CI_PIPELINE_IID", "CI_JOB_ID"):
        require(re.fullmatch(r"[1-9][0-9]*", environment.get(field, "")), "流水线或作业编号不合法")
    require(re.fullmatch(r"[a-f0-9]{40}", environment.get("CI_COMMIT_SHA", "")), "来源提交不合法")
    url = urlsplit(environment.get("CI_PROJECT_URL", ""))
    require(url.scheme in ("http", "https") and url.hostname and not url.username and not url.password
            and not url.query and not url.fragment, "来源仓库地址不合法")


def release_environment(environment):
    """正式版只允许无分支名的受保护标签，返回通道、版本和真实来源引用。"""
    tag = environment.get("CI_COMMIT_TAG")
    if tag:
        require(not environment.get("CI_COMMIT_BRANCH") and environment.get("CI_COMMIT_REF_PROTECTED") == "true",
                "正式发布必须来自受保护标签")
        match = re.fullmatch(r"v((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))", tag)
        require(match is not None, "正式标签必须为 v主版本.次版本.修订版本")
        return "stable", match.group(1), tag
    channel, version = release_context(environment.get("CI_COMMIT_BRANCH"))
    return channel, version, environment["CI_COMMIT_BRANCH"]


def validate_maintenance_version(source_ref, version):
    """维护线只允许同一主次版本，防止把新开发线产物混入旧维护线。"""
    if source_ref.startswith("release/"):
        series = source_ref.removeprefix("release/")
        require(re.fullmatch(re.escape(series) + r"\.(?:0|[1-9][0-9]*)", version) is not None,
                "维护分支主次版本与 VERSION 不一致")


def create_identity(base_version, arch, environment):
    validate_environment(environment)
    require(arch in ("arm64", "x64"), "构建架构不合法")
    base_version = base_version.strip()
    require(re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,60}", base_version), "基础版本不合法")
    channel, branch_version, source_ref = release_environment(environment)
    validate_maintenance_version(source_ref, base_version)
    require(branch_version is None or branch_version == base_version, "发布引用版本与 VERSION 不一致")
    return {
        "product": "mskdsp-lower", "channel": channel, "platform": "linux-" + arch,
        "version": base_version if channel == "stable" else f"{base_version}-{channel}-p{environment['CI_PIPELINE_IID']}-b{environment['CI_JOB_ID']}",
        "source_repository": environment["CI_PROJECT_URL"].rstrip("/"),
        "source_ref": source_ref, "source_sha": environment["CI_COMMIT_SHA"],
        "pipeline_id": environment["CI_PIPELINE_ID"], "pipeline_iid": environment["CI_PIPELINE_IID"],
        "build_job_id": environment["CI_JOB_ID"],
        "build_time": datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
    }


def package_identity(identity, environment):
    validate_environment(environment)
    channel, branch_version, source_ref = release_environment(environment)
    for key, variable in (("pipeline_id", "CI_PIPELINE_ID"), ("pipeline_iid", "CI_PIPELINE_IID"),
                          ("source_sha", "CI_COMMIT_SHA"),
                          ("source_repository", "CI_PROJECT_URL")):
        require(str(identity[key]) == environment[variable].rstrip("/"), "打包作业与构建来源不一致")
    require(identity.get("source_ref") == source_ref, "打包作业与构建来源引用不一致")
    require(identity.get("product") == "mskdsp-lower" and identity.get("channel") == channel
            and identity.get("platform") in ("linux-arm64", "linux-x64")
            and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]+", identity.get("version", "")), "构建身份不合法")
    if source_ref.startswith("release/"):
        require(re.fullmatch(re.escape(source_ref.removeprefix("release/"))
                             + r"\.(?:0|[1-9][0-9]*)-ci-p[1-9][0-9]*-b[1-9][0-9]*",
                             identity["version"]) is not None, "维护线打包身份与版本不一致")
    require(channel != "beta" or re.fullmatch(
        re.escape(branch_version) + r"-beta-p[1-9][0-9]*-b[1-9][0-9]*", identity["version"]), "Beta 构建版本与来源不一致")
    require(channel != "stable" or identity["version"] == branch_version, "正式版本与标签不一致")
    version = identity["version"] if channel == "stable" else f"{identity['version']}-j{environment['CI_JOB_ID']}-{identity['source_sha'][:7]}"
    return identity | {"version": version, "package_version": version + "-" + identity["platform"],
                       "package_job_id": environment["CI_JOB_ID"]}


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def freeze_metadata(identity, artifact, image_tar):
    require(artifact.is_file() and not artifact.is_symlink() and artifact.stat().st_size > 0,
            "安装包不存在、为空或为符号链接")
    require(artifact.name == "mskdsp-" + identity["package_version"], "安装包名称与构建身份不一致")
    return identity | {"file_name": artifact.name, "size": artifact.stat().st_size,
                       "sha256": sha256_file(artifact),
                       "image_id": read_image_config_id(image_tar, identity["platform"])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("build", "package", "freeze"))
    parser.add_argument("--arch", choices=("arm64", "x64"))
    parser.add_argument("--identity", type=Path)
    parser.add_argument("--artifact", type=Path)
    parser.add_argument("--image-tar", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.mode == "build":
        data = create_identity(Path("VERSION").read_text(encoding="utf-8"), args.arch, os.environ)
    else:
        identity = json.loads(args.identity.read_text(encoding="utf-8"))
        data = (package_identity(identity, os.environ) if args.mode == "package"
                else freeze_metadata(identity, args.artifact, args.image_tar))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"已冻结下位机 {args.mode} 身份：{args.output}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError) as error:
        raise SystemExit(f"生成下位机元数据失败：{error}") from None
