#!/usr/bin/env python3
"""将下位机构建发布到 R2，并使用 GitLab 永久包附件维护发布页面。"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request


METADATA_KEYS = {
    "CHANNEL", "ARCH_SLUG", "PLATFORM", "ARCHIVE_NAME", "DEBUG_ARCHIVE_NAME",
    "IMAGE_VERSION", "TARGET_REF", "SOURCE_SHA", "R2_PREFIX", "BETA_LINE",
    "BETA_TAG", "BETA_TAG_PREFIX", "NOTES_START_TAG", "RELEASE_TITLE",
}


def log(message):
    print(f"GitLab 发布: {message}", flush=True)


def require(env, key):
    value = env.get(key, "").strip()
    if not value:
        raise ValueError(f"缺少发布变量: {key}")
    return value


def load_environment(path):
    """只读取构建元数据，不执行文件，也不允许文件注入发布凭据。"""
    values = {}
    if path.is_file():
        for line in path.read_text(encoding="utf-8").splitlines():
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            key, separator, value = line.partition("=")
            if not separator:
                raise ValueError("构建元数据存在缺少等号的行")
            if key in METADATA_KEYS:
                values[key] = value
    values.update(os.environ)
    return values


class NoRedirect(urllib.request.HTTPRedirectHandler):
    """禁止认证请求重定向，避免将令牌传到其他主机。"""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class GitLabAPI:
    def __init__(self, env):
        self.base = require(env, "CI_API_V4_URL").rstrip("/")
        parsed = urllib.parse.urlsplit(self.base)
        if parsed.scheme not in ("http", "https") or not parsed.netloc or parsed.username:
            raise ValueError("CI_API_V4_URL 必须为不含凭据的 HTTP 或 HTTPS 地址")
        self.project = "/projects/" + urllib.parse.quote(require(env, "CI_PROJECT_ID"), safe="")
        self.token = require(env, "GITLAB_RELEASE_TOKEN")
        self.opener = urllib.request.build_opener(NoRedirect())

    def request(self, method, path, payload=None, file=None, allow_missing=False):
        headers = {"PRIVATE-TOKEN": self.token}
        data = None
        if payload is not None:
            data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if file is not None:
            # urllib/http.client 会分块读取文件，无需将大型安装包放入内存。
            data = file.open("rb")
            headers["Content-Type"] = "application/octet-stream"
            headers["Content-Length"] = str(file.stat().st_size)
        try:
            request = urllib.request.Request(self.base + path, data=data, headers=headers, method=method)
            with self.opener.open(request, timeout=300) as response:
                body = response.read()
            return json.loads(body) if body else None
        except urllib.error.HTTPError as error:
            if error.code == 404 and allow_missing:
                return None
            # 不回显服务端响应或请求头，以免错误内容带入敏感信息。
            raise RuntimeError(f"GitLab API {method} 请求失败，HTTP {error.code}") from None
        except urllib.error.URLError:
            raise RuntimeError("GitLab API 连接失败，请检查地址和网络") from None
        finally:
            if file is not None:
                data.close()

    def project_request(self, method, path, **kwargs):
        return self.request(method, self.project + path, **kwargs)

    def list_releases(self):
        releases = []
        page = 1
        while True:
            batch = self.project_request("GET", f"/releases?per_page=100&page={page}")
            releases.extend(batch)
            if len(batch) < 100:
                return releases
            page += 1

    def check_permissions(self, channel):
        token = self.request("GET", "/personal_access_tokens/self")
        if not token.get("active") or token.get("revoked") or "api" not in token.get("scopes", []):
            raise ValueError("GITLAB_RELEASE_TOKEN 必须有效且具有 api 权限")
        project = self.project_request("GET", "")
        permissions = project.get("permissions", {})
        level = max((permissions.get(kind) or {}).get("access_level", 0)
                    for kind in ("project_access", "group_access"))
        minimum = 40 if channel == "beta" else 30
        if level < minimum:
            role = "Maintainer" if channel == "beta" else "Developer"
            raise ValueError(f"GITLAB_RELEASE_TOKEN 的项目权限不足，{channel} 发布至少需要 {role}")
        if project.get("packages_enabled") is False or project.get("package_registry_access_level") == "disabled":
            raise ValueError("项目 Package Registry 已禁用，请先启用再发布")
        log("已核验发布令牌权限和项目 Package Registry 配置")


def old_beta_releases(releases, prefix, current_tag):
    """GitLab 没有预发布标志，仅清理本版本线、带 Beta 标题的发布。"""
    return [release for release in releases
            if release.get("tag_name", "").startswith(prefix)
            and release["tag_name"] != current_tag
            and release.get("name", "").startswith("Beta ")]


def git_output(*args, optional=False):
    result = subprocess.run(["git", *args], capture_output=True, text=True, encoding="utf-8")
    if result.returncode:
        if optional:
            return ""
        raise RuntimeError("无法读取发布目标的 Git 记录，请确认完整历史和目标提交均已拉取")
    return result.stdout.strip()


def release_notes(env, sha):
    channel = env["CHANNEL"]
    baseline = env.get("NOTES_START_TAG", "")
    if not baseline:
        target = sha if channel == "beta" else sha + "^"
        baseline = git_output("describe", "--tags", "--match", "v*", "--abbrev=0", target, optional=True)
    revision = f"{baseline}..{sha}" if baseline else sha
    if baseline:
        result = subprocess.run(["git", "merge-base", "--is-ancestor", baseline, sha], capture_output=True)
        if result.returncode:
            raise ValueError("发布说明基线不属于目标提交的历史")
    changes = git_output("-c", "log.showSignature=false", "log", "--format=- %h %s", revision)
    if channel == "beta":
        intro = (f"Beta 候选版本线：{env['BETA_LINE']}\n\n"
                 f"目标分支：`{env['TARGET_REF']}`\n\n"
                 "此页面为 Beta 预发布，候选包通过 beta 更新通道交付。\n\n")
    else:
        intro = f"正式版本：`{env['TARGET_REF']}`\n\n"
    return (intro + f"基线正式版本：`{baseline}`\n\n" if baseline else intro + "当前没有可用的正式版本基线。\n\n") + \
        "以下为上述基线以来的累计提交变更：\n\n" + (changes or "没有新增提交。") + f"\n\n源码提交：`{sha}`\n"


def artifact_files(env):
    archive = require(env, "ARCHIVE_NAME")
    if Path(archive).name != archive or "\\" in archive:
        raise ValueError("ARCHIVE_NAME 必须为文件名")
    assets = [Path("images") / archive, Path("SHA256SUMS"), Path("latest.json")]
    debug = env.get("DEBUG_ARCHIVE_NAME", "")
    if debug:
        if Path(debug).name != debug or "\\" in debug:
            raise ValueError("DEBUG_ARCHIVE_NAME 必须为文件名")
        debug_file = Path(debug + ".tar.gz")
        if debug_file.is_file():
            assets.append(debug_file)
    for asset in assets:
        if not asset.is_file():
            raise ValueError(f"未找到发布文件: {asset}")
    return assets


def upload_r2(env, assets):
    command = ["bash", "script/workflow/upload_r2.sh",
               "--endpoint", f"https://{require(env, 'R2_ACCOUNT_ID')}.r2.cloudflarestorage.com",
               "--bucket", env.get("R2_BUCKET") or "mskdsp-update",
               "--prefix", env.get("R2_PREFIX") or f"mskdsp-lower/{env['CHANNEL']}",
               "--public-base-url", env.get("R2_PUBLIC_BASE_URL") or "https://update.clsclear.top",
               "--platform", require(env, "PLATFORM"), "--package-file", str(assets[0]),
               "--checksum-file", "SHA256SUMS", "--manifest-file", "latest.json"]
    if len(assets) > 3:
        command.extend(["--debug-file", str(assets[3])])
    child_env = dict(env)
    child_env["AWS_ACCESS_KEY_ID"] = require(env, "R2_ACCESS_KEY_ID")
    child_env["AWS_SECRET_ACCESS_KEY"] = require(env, "R2_SECRET_ACCESS_KEY")
    log(f"发布 R2 通道 {env['CHANNEL']} / {env['PLATFORM']}")
    subprocess.run(command, check=True, env=child_env)


def upload_packages(api, env, assets):
    package_name = "mskdsp-lower-" + require(env, "PLATFORM")
    version = require(env, "IMAGE_VERSION")
    if not re.fullmatch(r"[A-Za-z0-9._+-]+", version):
        raise ValueError("IMAGE_VERSION 含 Package Registry 不支持的字符")
    links = []
    for asset in assets:
        path = "/packages/generic/" + "/".join(urllib.parse.quote(part, safe="")
                                                for part in (package_name, version, asset.name))
        log(f"上传永久发布附件: {asset.name}")
        api.project_request("PUT", path, file=asset)
        links.append({"name": asset.name, "url": api.base + api.project + path,
                      "link_type": "package", "direct_asset_path": "/" + asset.name})
    return links


def publish(env):
    if env.get("MSKDSP_PUBLISH_ENABLED", "false").lower() != "true":
        log("MSKDSP_PUBLISH_ENABLED 未启用，跳过全部 R2、Package Registry 和 Release 发布")
        return
    channel = require(env, "CHANNEL")
    if channel not in ("ci", "beta", "stable"):
        raise ValueError("CHANNEL 只能为 ci、beta 或 stable")
    for key in ("R2_ACCOUNT_ID", "R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY", "PLATFORM"):
        require(env, key)
    for executable in ("bash", "aws"):
        if not shutil.which(executable):
            raise ValueError(f"发布环境缺少命令: {executable}")
    assets = artifact_files(env)
    if channel == "ci":
        upload_r2(env, assets)
        log("CI 通道发布完成，不创建 Release")
        return

    # 必须先检查权限、目标和说明，避免 R2 已发布后才发现缺少 GitLab 凭据。
    api = GitLabAPI(env)
    api.check_permissions(channel)
    sha = env.get("SOURCE_SHA") or git_output("rev-parse", "HEAD")
    if not re.fullmatch(r"[0-9a-fA-F]{40,64}", sha):
        raise ValueError("SOURCE_SHA 必须为完整的 Git 提交哈希")
    target = require(env, "TARGET_REF")
    if channel == "beta":
        if not target.startswith("beta/"):
            raise ValueError("Beta 发布目标必须为 beta/ 分支")
        line = require(env, "BETA_LINE")
        prefix = require(env, "BETA_TAG_PREFIX")
        tag = require(env, "BETA_TAG")
        if line != target[5:] or prefix != f"beta-{line}-" or not tag.startswith(prefix):
            raise ValueError("Beta 版本线、标签前缀和发布目标不一致")
        previous = old_beta_releases(api.list_releases(), prefix, tag)
        for release in previous:
            old_tag = api.project_request("GET", "/repository/tags/" + urllib.parse.quote(release["tag_name"], safe=""), allow_missing=True)
            if old_tag and old_tag.get("protected"):
                raise ValueError("同版本线存在受保护的旧 Beta 标签，无法按既定流程清理")
    else:
        if not target.startswith("v"):
            raise ValueError("Stable 发布目标必须为 v 开头的已有正式标签")
        tag = target
        previous = []
    tag_path = "/repository/tags/" + urllib.parse.quote(tag, safe="")
    tag_info = api.project_request("GET", tag_path, allow_missing=channel == "beta")
    if tag_info and tag_info.get("commit", {}).get("id") != sha:
        raise ValueError("GitLab 发布标签指向的提交与构建提交不一致")
    release_path = "/releases/" + urllib.parse.quote(tag, safe="")
    existing = api.project_request("GET", release_path, allow_missing=True)
    notes = release_notes(env, sha) if not existing else ""
    # 在 R2 上传之前核验包版本命名，避免出现半发布状态。
    if not re.fullmatch(r"[A-Za-z0-9._+-]+", require(env, "IMAGE_VERSION")):
        raise ValueError("IMAGE_VERSION 含 Package Registry 不支持的字符")

    upload_r2(env, assets)
    links = upload_packages(api, env, assets)
    if not tag_info:
        log(f"创建 Beta 标签: {tag}")
        api.project_request("POST", "/repository/tags", payload={"tag_name": tag, "ref": sha})
    if existing:
        # 保留正式发布已有说明，使用链接 API 更新附件，等价于附件覆盖。
        log(f"更新已有发布附件: {tag}")
        existing_links = api.project_request("GET", release_path + "/assets/links")
        for link in links:
            match = next((item for item in existing_links if item["name"] == link["name"]), None)
            if match:
                api.project_request("PUT", release_path + f"/assets/links/{match['id']}", payload=link)
            else:
                api.project_request("POST", release_path + "/assets/links", payload=link)
    else:
        title = env.get("RELEASE_TITLE") or (f"Beta {env['BETA_LINE']} {tag}" if channel == "beta" else tag)
        log(f"创建 GitLab 发布页面: {tag}")
        api.project_request("POST", "/releases", payload={"tag_name": tag, "name": title,
                                                         "description": notes, "assets": {"links": links}})
    # 新发布成功后再清理旧页面及标签，只处理当前 Beta 版本线。
    for release in previous:
        old = urllib.parse.quote(release["tag_name"], safe="")
        log(f"清理同版本线旧 Beta 发布与标签: {release['tag_name']}")
        api.project_request("DELETE", "/releases/" + old, allow_missing=True)
        api.project_request("DELETE", "/repository/tags/" + old, allow_missing=True)
    log(f"{channel} 发布完成: {tag}，永久附件保存在项目 Package Registry")


def main(argv=None):
    parser = argparse.ArgumentParser(description="发布下位机 GitLab 构建与更新通道")
    parser.add_argument("--env-file", type=Path, default=Path("build.env"), help="构建元数据文件")
    args = parser.parse_args(argv)
    try:
        publish(load_environment(args.env_file))
        return 0
    except (ValueError, RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print(f"GitLab 发布失败: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
