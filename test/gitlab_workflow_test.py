#!/usr/bin/env python3
"""直接验证 GitLab 配置的任务选择和发布边界，不访问外部服务。"""

import copy
import importlib.util
import re
import shlex
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import yaml


ROOT = Path(__file__).resolve().parents[1]
TOKEN = re.compile(
    r'\s*(\$[A-Za-z_][A-Za-z0-9_]*|"(?:[^"\\]|\\.)*"|'
    r"'(?:[^'\\]|\\.)*'|/(?:[^/\\]|\\.)*/[i]?|==|!=|=~|!~|&&|\|\||\(|\))"
)


class RuleExpression:
    """仅解释本项目实际使用的 GitLab rules 表达式子集，未知语法直接失败。"""

    def __init__(self, expression, environment):
        self.environment = environment
        self.tokens = []
        offset = 0
        while offset < len(expression.rstrip()):
            match = TOKEN.match(expression, offset)
            if not match:
                raise ValueError(f"无法验证的规则语法: {expression[offset:]}")
            self.tokens.append(match.group(1))
            offset = match.end()
        self.position = 0

    def peek(self):
        return self.tokens[self.position] if self.position < len(self.tokens) else None

    def pop(self):
        token = self.peek()
        if token is None:
            raise ValueError("规则表达式提前结束")
        self.position += 1
        return token

    def value(self, token):
        if token.startswith("$"):
            return str(self.environment.get(token[1:], ""))
        if token.startswith(('"', "'")):
            return token[1:-1]
        return token

    def atom(self):
        if self.peek() == "(":
            self.pop()
            result = self.logical_or()
            if self.pop() != ")":
                raise ValueError("规则括号不匹配")
            return result
        left = self.value(self.pop())
        operator = self.peek()
        if operator not in ("==", "!=", "=~", "!~"):
            return bool(left)
        self.pop()
        right = self.pop()
        if operator in ("=~", "!~"):
            if not right.startswith("/"):
                raise ValueError("正则规则必须使用 /pattern/ 格式")
            end = right.rfind("/")
            flags = re.IGNORECASE if right[end + 1:] == "i" else 0
            match = re.search(right[1:end].replace(r"\/", "/"), left, flags) is not None
            return match if operator == "=~" else not match
        right = self.value(right)
        return left == right if operator == "==" else left != right

    def logical_and(self):
        result = self.atom()
        while self.peek() == "&&":
            self.pop()
            next_value = self.atom()
            result = result and next_value
        return result

    def logical_or(self):
        result = self.logical_and()
        while self.peek() == "||":
            self.pop()
            next_value = self.logical_and()
            result = result or next_value
        return result

    def evaluate(self):
        result = self.logical_or()
        if self.peek() is not None:
            raise ValueError(f"规则表达式存在剩余 token: {self.peek()}")
        return result


def merge_mapping(base, update):
    result = copy.deepcopy(base)
    for key, value in update.items():
        if isinstance(value, dict) and isinstance(result.get(key), dict):
            result[key] = merge_mapping(result[key], value)
        else:
            result[key] = copy.deepcopy(value)
    return result


def variable_values(variables):
    return {key: value.get("value", "") if isinstance(value, dict) else value
            for key, value in variables.items()}


def expand_variables(value, environment):
    if isinstance(value, str):
        return re.sub(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}|\$([A-Za-z_][A-Za-z0-9_]*)",
                      lambda match: str(environment.get(match.group(1) or match.group(2), "")), value)
    if isinstance(value, list):
        return [expand_variables(item, environment) for item in value]
    if isinstance(value, dict):
        return {key: expand_variables(item, environment) for key, item in value.items()}
    return value


def job_environment(config, job, **context):
    values = variable_values(config.get("variables", {}))
    values.update(context)
    values.update(job.get("variables", {}))
    for _ in range(10):
        expanded = expand_variables(values, values)
        if expanded == values:
            return values
        values = expanded
    raise ValueError("变量展开存在循环或超过验证深度")


def resolve_job(config, name):
    job = config[name]
    result = {}
    parents = job.get("extends", [])
    if isinstance(parents, str):
        parents = [parents]
    for parent in parents:
        result = merge_mapping(result, resolve_job(config, parent))
    return merge_mapping(result, {key: value for key, value in job.items() if key != "extends"})


def first_rule(rules, environment):
    for rule in rules:
        if "if" not in rule or RuleExpression(rule["if"], environment).evaluate():
            return rule
    return {"when": "never"}


def selected_jobs(config, **context):
    environment = variable_values(config.get("variables", {}))
    environment.update({"CI_PIPELINE_SOURCE": "push", "CI_COMMIT_BRANCH": "", "CI_COMMIT_TAG": ""})
    environment.update(context)
    workflow = first_rule(config["workflow"]["rules"], environment)
    if workflow.get("when") == "never":
        return set()
    environment.update(workflow.get("variables", {}))
    jobs = set()
    for name, job in config.items():
        if name.startswith(".") or not isinstance(job, dict) or name in ("workflow", "variables", "default"):
            continue
        effective = resolve_job(config, name)
        job_environment = dict(environment)
        job_environment.update(effective.get("variables", {}))
        rule = first_rule(effective.get("rules", [{}]), job_environment)
        if rule.get("when") != "never":
            jobs.add(name)
    return jobs


def channel_jobs(channel, architectures, phases):
    return {f"{channel}-{arch}-{phase}" for arch in architectures for phase in phases}


class GitLabWorkflowTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with (ROOT / ".gitlab-ci.yml").open(encoding="utf-8") as source:
            cls.config = yaml.safe_load(source)

    # 验证 MR 只进行两个架构的编译和测试，不产生安装包或发布。
    def test_merge_request_only_builds(self):
        self.assertEqual(selected_jobs(self.config, CI_PIPELINE_SOURCE="merge_request_event"),
                         channel_jobs("ci", ["x64", "arm64"], ["build"]))

    # 验证主分支 push 创建两个独立的完整 CI 产物链。
    def test_mainline_push_packages_both_architectures(self):
        for branch in ("master", "main"):
            with self.subTest(branch=branch):
                self.assertEqual(selected_jobs(self.config, CI_COMMIT_BRANCH=branch),
                                 channel_jobs("ci", ["x64", "arm64"], ["build", "package", "publish"]))

    # 验证 Beta push 保留 CI 编译，但仅创建 ARM64 Beta 安装包。
    def test_beta_push_retains_ci_and_beta_builds(self):
        expected = channel_jobs("ci", ["x64", "arm64"], ["build"])
        expected |= channel_jobs("beta", ["arm64"], ["build", "package", "publish"])
        for branch in ("beta/0.1", "beta/0.1/hotfix"):
            with self.subTest(branch=branch):
                self.assertEqual(selected_jobs(self.config, CI_COMMIT_BRANCH=branch), expected)

    # 验证正式 tag 只选择 ARM64 Stable 流程。
    def test_version_tag_only_stable(self):
        self.assertEqual(selected_jobs(self.config, CI_COMMIT_TAG="v0.1.1"),
                         channel_jobs("stable", ["arm64"], ["build", "package", "publish"]))

    # 验证三个手动入口互斥，手动 CI 不出包。
    def test_manual_modes(self):
        modes = {
            "ci": channel_jobs("ci", ["x64", "arm64"], ["build"]),
            "beta": channel_jobs("beta", ["arm64"], ["build", "package", "publish"]),
            "stable": channel_jobs("stable", ["arm64"], ["build", "package", "publish"]),
        }
        for mode, expected in modes.items():
            with self.subTest(mode=mode):
                self.assertEqual(selected_jobs(self.config, CI_PIPELINE_SOURCE="web",
                                               CI_COMMIT_BRANCH="master", MSKDSP_PIPELINE_MODE=mode,
                                               BETA_REF="beta/0.1", RELEASE_TAG="v0.1.1"), expected)

    # 验证不会误触发普通分支、非版本 tag 和未开放的流水线来源。
    def test_unsupported_sources_and_refs_are_rejected(self):
        contexts = [
            {"CI_COMMIT_BRANCH": "feature/gitlab"}, {"CI_COMMIT_TAG": "beta-0.1-old"},
            {"CI_PIPELINE_SOURCE": "schedule", "CI_COMMIT_BRANCH": "master"},
            {"CI_PIPELINE_SOURCE": "api", "CI_COMMIT_BRANCH": "master"},
            {"CI_PIPELINE_SOURCE": "web", "MSKDSP_PIPELINE_MODE": "unknown"},
        ]
        for context in contexts:
            with self.subTest(context=context):
                self.assertEqual(selected_jobs(self.config, **context), set())

    # 验证新增配置首次运行不会自动写入发布端。
    def test_publishing_is_disabled_by_default(self):
        self.assertEqual(variable_values(self.config["variables"])["MSKDSP_PUBLISH_ENABLED"], "false")

    # 验证每个打包/发布任务只下载同渠道同架构产物，不混用安装树。
    def test_artifact_dependencies_pair_channel_and_architecture(self):
        for channel, architectures in (("ci", ["x64", "arm64"]), ("beta", ["arm64"]), ("stable", ["arm64"])):
            for architecture in architectures:
                for phase, upstream in (("package", "build"), ("publish", "package")):
                    name = f"{channel}-{architecture}-{phase}"
                    with self.subTest(job=name):
                        needs = resolve_job(self.config, name)["needs"]
                        self.assertEqual(needs, [{"job": f"{channel}-{architecture}-{upstream}", "artifacts": True}])

    # 验证构建可并行，选择正确架构 Runner；Docker 权限限定到打包任务。
    def test_runner_and_parallelism_boundaries(self):
        for architecture, tags in (("x64", ["linux", "amd64", "docker", "ci"]),
                                   ("arm64", ["linux", "arm64-qemu", "docker", "ci"])):
            job = resolve_job(self.config, f"ci-{architecture}-build")
            self.assertEqual(job["tags"], tags)
            self.assertFalse(job.get("needs"))
        package_tags = ["linux", "amd64", "docker-package", "mskdsp"]
        for name in channel_jobs("ci", ["x64", "arm64"], ["package"]) | channel_jobs("beta", ["arm64"], ["package"]) | channel_jobs("stable", ["arm64"], ["package"]):
            self.assertEqual(resolve_job(self.config, name)["tags"], package_tags)

    # 验证正式版只读取依赖缓存，两个架构的缓存键不共用。
    def test_dependency_cache_isolation_and_stable_read_only(self):
        keys = []
        for architecture in ("x64", "arm64"):
            job = resolve_job(self.config, f"ci-{architecture}-build")
            cache = expand_variables(job["cache"], job_environment(self.config, job))
            caches = cache if isinstance(cache, list) else [cache]
            keys.append(str(caches))
        self.assertNotEqual(keys[0], keys[1])
        job = resolve_job(self.config, "stable-arm64-build")
        cache = expand_variables(job["cache"], job_environment(self.config, job))
        caches = cache if isinstance(cache, list) else [cache]
        dependency_caches = [entry for entry in caches if "vcpkg" in str(entry.get("paths", []))]
        self.assertTrue(dependency_caches)
        self.assertTrue(all(entry.get("policy") == "pull" for entry in dependency_caches))

    # 验证同一主分支页面手动 Beta 不回写 CI 缓存，Stable 读取 CI 主线缓存。
    def test_manual_beta_cache_does_not_overwrite_mainline(self):
        prefixes = {}
        for name in ("ci-arm64-build", "beta-arm64-build", "stable-arm64-build"):
            job = resolve_job(self.config, name)
            values = job_environment(self.config, job, CI_COMMIT_REF_SLUG="master", BETA_REF="beta/0.1")
            caches = expand_variables(job["cache"], values)
            prefixes[name] = caches[0]["key"]["prefix"]
        self.assertNotEqual(prefixes["beta-arm64-build"], prefixes["ci-arm64-build"])
        self.assertEqual(prefixes["stable-arm64-build"], prefixes["ci-arm64-build"])

    # 验证 CI 的安装树开关和打包任务选取规则一致，MR/Beta CI/手动 CI 不安装。
    def test_install_tree_flag_matches_ci_package_rules(self):
        job = resolve_job(self.config, "ci-x64-build")
        contexts = [
            ({"CI_PIPELINE_SOURCE": "push", "CI_COMMIT_BRANCH": "master"}, "true"),
            ({"CI_PIPELINE_SOURCE": "push", "CI_COMMIT_BRANCH": "main"}, "true"),
            ({"CI_PIPELINE_SOURCE": "push", "CI_COMMIT_BRANCH": "beta/0.1"}, "false"),
            ({"CI_PIPELINE_SOURCE": "merge_request_event"}, "false"),
            ({"CI_PIPELINE_SOURCE": "web", "MSKDSP_PIPELINE_MODE": "ci"}, "false"),
        ]
        for context, expected in contexts:
            with self.subTest(context=context):
                rule = first_rule(job["rules"], context)
                self.assertEqual(rule["variables"]["MAKE_PACKAGE"], expected)

    # 验证 CTest 契约保持并发 2、失败输出及零测试即失败，并对齐 JUnit 产物路径。
    def test_ctest_execution_and_report_contract(self):
        source = (ROOT / "script/workflow/gitlab_lower_build.sh").read_text(encoding="utf-8")
        commands = source.replace("\\\n", " ").splitlines()
        tests = [shlex.split(line.strip()) for line in commands if line.strip().startswith("ctest ")]
        self.assertEqual(len(tests), 1)
        arguments = tests[0]
        self.assertEqual(arguments[arguments.index("--parallel") + 1], "2")
        self.assertIn("--output-on-failure", arguments)
        self.assertIn("--no-tests=error", arguments)
        filename = arguments[arguments.index("--output-junit") + 1]
        for architecture in ("x64", "arm64"):
            job = resolve_job(self.config, f"ci-{architecture}-build")
            artifact = expand_variables(job["artifacts"]["reports"]["junit"], job["variables"])
            self.assertEqual(artifact, f"build-{architecture}/{filename}")
            self.assertEqual(job["artifacts"]["when"], "always")


class GitLabPublicationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("gitlab_lower_publish", ROOT / "script/workflow/gitlab_lower_publish.py")
        cls.publisher = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.publisher)

    def environment(self, channel="beta"):
        return {
            "MSKDSP_PUBLISH_ENABLED": "true", "CHANNEL": channel,
            "R2_ACCOUNT_ID": "test-account", "R2_ACCESS_KEY_ID": "test-id",
            "R2_SECRET_ACCESS_KEY": "test-secret", "PLATFORM": "linux-arm64",
            "SOURCE_SHA": "a" * 40, "TARGET_REF": "beta/0.1", "BETA_LINE": "0.1",
            "BETA_TAG_PREFIX": "beta-0.1-", "BETA_TAG": "beta-0.1-new",
            "IMAGE_VERSION": "0.1-beta-0.1-new-linux-arm64",
        }

    def patches(self):
        stack = __import__("contextlib").ExitStack()
        api = stack.enter_context(mock.patch.object(self.publisher, "GitLabAPI"))
        r2 = stack.enter_context(mock.patch.object(self.publisher, "upload_r2"))
        stack.enter_context(mock.patch.object(self.publisher, "artifact_files", return_value=[Path("images/package")]))
        stack.enter_context(mock.patch.object(self.publisher.shutil, "which", return_value="工具"))
        stack.enter_context(mock.patch.object(self.publisher, "upload_packages", return_value=[{"name": "package", "url": "https://example.invalid/package"}]))
        stack.enter_context(mock.patch.object(self.publisher, "release_notes", return_value="测试发布说明"))
        api.return_value.list_releases.return_value = []
        api.return_value.project_request.return_value = None
        return stack, api.return_value, r2

    # 验证默认关闭发布时没有 HTTP、Git 或上传子进程，且不要求凭据和产物。
    def test_default_disabled_has_no_external_side_effects(self):
        with mock.patch.object(self.publisher, "GitLabAPI") as api, mock.patch.object(self.publisher.subprocess, "run") as run, mock.patch.object(self.publisher.urllib.request, "build_opener") as network:
            self.publisher.publish({})
            self.publisher.publish({"MSKDSP_PUBLISH_ENABLED": "false", "CHANNEL": "beta"})
        api.assert_not_called()
        run.assert_not_called()
        network.assert_not_called()

    # 验证关闭发布时真实 CLI 无凭据也能成功退出，不需外部程序。
    def test_cli_disabled_returns_success(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.dict(self.publisher.os.environ, {"MSKDSP_PUBLISH_ENABLED": "false"}, clear=True), mock.patch.object(self.publisher.subprocess, "run") as run:
            result = self.publisher.main(["--env-file", str(Path(directory) / "missing.env")])
        self.assertEqual(result, 0)
        run.assert_not_called()

    # 验证 dotenv 不能注入发布凭据或把默认禁用状态改成启用。
    def test_metadata_cannot_enable_publication_or_inject_credentials(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.dict(self.publisher.os.environ, {}, clear=True):
            metadata = Path(directory) / "build.env"
            metadata.write_text("CHANNEL=beta\nSOURCE_SHA=" + "a" * 40 + "\nMSKDSP_PUBLISH_ENABLED=true\nGITLAB_RELEASE_TOKEN=artifact-secret\nR2_SECRET_ACCESS_KEY=artifact-secret\n", encoding="utf-8")
            environment = self.publisher.load_environment(metadata)
        self.assertEqual(environment["CHANNEL"], "beta")
        self.assertNotIn("MSKDSP_PUBLISH_ENABLED", environment)
        self.assertNotIn("GITLAB_RELEASE_TOKEN", environment)
        self.assertNotIn("R2_SECRET_ACCESS_KEY", environment)

    # 验证 CI 发布只有 R2，不创建 Package Registry 附件、标签或 Release。
    def test_ci_never_creates_release(self):
        stack, api, r2 = self.patches()
        with stack:
            self.publisher.publish(self.environment("ci"))
        r2.assert_called_once()
        api.project_request.assert_not_called()
        api.check_permissions.assert_not_called()

    # 验证清理范围限定当前 Beta 线及 Beta 标题，保留当前与正式发布。
    def test_beta_cleanup_selection(self):
        releases = [
            {"tag_name": "beta-0.1-old", "name": "Beta 0.1 旧版本"},
            {"tag_name": "beta-0.1-new", "name": "Beta 0.1 新版本"},
            {"tag_name": "beta-0.2-old", "name": "Beta 0.2 旧版本"},
            {"tag_name": "v0.1", "name": "正式版"},
            {"tag_name": "beta-0.1-kept", "name": "保留发布"},
        ]
        self.assertEqual(self.publisher.old_beta_releases(releases, "beta-0.1-", "beta-0.1-new"), [releases[0]])

    # 验证只有新 Release 成功后才删除同线旧页面和标签。
    def test_beta_creates_new_release_before_cleanup(self):
        stack, api, r2 = self.patches()
        with stack:
            api.list_releases.return_value = [{"tag_name": "beta-0.1-old", "name": "Beta 0.1 旧版本"}]
            self.publisher.publish(self.environment())
        calls = api.project_request.call_args_list
        creation = next(index for index, call in enumerate(calls) if call.args == ("POST", "/releases"))
        deletions = [(index, call.args[1]) for index, call in enumerate(calls) if call.args[0] == "DELETE"]
        self.assertEqual([path for _, path in deletions], ["/releases/beta-0.1-old", "/repository/tags/beta-0.1-old"])
        self.assertTrue(all(index > creation for index, _ in deletions))
        r2.assert_called_once()

    # 验证旧 Beta 标签受保护时在写 R2 前失败，避免半发布。
    def test_protected_beta_tag_rejected_before_upload(self):
        stack, api, r2 = self.patches()
        with stack:
            api.list_releases.return_value = [{"tag_name": "beta-0.1-old", "name": "Beta 0.1 旧版本"}]
            api.project_request.return_value = {"protected": True}
            with self.assertRaisesRegex(ValueError, "受保护"):
                self.publisher.publish(self.environment())
        r2.assert_not_called()

    # 验证创建新 Release 失败后不会删除旧 Beta 发布。
    def test_failed_beta_creation_keeps_old_release(self):
        stack, api, _ = self.patches()
        def request(method, path, **kwargs):
            if method == "POST" and path == "/releases":
                raise RuntimeError("测试发布失败")
            return None
        with stack:
            api.list_releases.return_value = [{"tag_name": "beta-0.1-old", "name": "Beta 0.1 旧版本"}]
            api.project_request.side_effect = request
            with self.assertRaisesRegex(RuntimeError, "测试发布失败"):
                self.publisher.publish(self.environment())
        self.assertFalse(any(call.args[0] == "DELETE" for call in api.project_request.call_args_list))

    # 验证正式 tag 与构建 commit 不一致时阻止上传。
    def test_stable_target_sha_mismatch_rejected(self):
        stack, api, r2 = self.patches()
        with stack:
            api.project_request.return_value = {"commit": {"id": "b" * 40}}
            environment = self.environment("stable")
            environment["TARGET_REF"] = "v0.1.1"
            with self.assertRaisesRegex(ValueError, "提交不一致"):
                self.publisher.publish(environment)
        r2.assert_not_called()

    # 验证重复正式发布只更新附件，保留标签和现有发布说明。
    def test_stable_existing_release_only_updates_assets(self):
        stack, api, r2 = self.patches()
        def request(method, path, **kwargs):
            if path == "/repository/tags/v0.1.1":
                return {"commit": {"id": "a" * 40}}
            if path == "/releases/v0.1.1":
                return {"description": "保持现有说明"}
            if path.endswith("/assets/links") and method == "GET":
                return [{"id": 7, "name": "package"}]
            return None
        with stack:
            api.project_request.side_effect = request
            environment = self.environment("stable")
            environment["TARGET_REF"] = "v0.1.1"
            self.publisher.publish(environment)
        writes = [call.args for call in api.project_request.call_args_list if call.args[0] != "GET"]
        self.assertEqual(writes, [("PUT", "/releases/v0.1.1/assets/links/7")])
        r2.assert_called_once()


if __name__ == "__main__":
    unittest.main()
