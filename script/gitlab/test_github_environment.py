"""验证 GitHub 原生来源、重跑身份和阶段架构隔离。"""
import unittest

from github_environment import normalize_environment


def github_environment(**changes):
    """构造不含 GitLab 凭据的原生 GitHub 作业环境。"""
    return dict(GITHUB_ACTIONS="true", GITHUB_SERVER_URL="https://github.com",
                GITHUB_REPOSITORY="CcooLcyy/MskDSP", GITHUB_EVENT_NAME="push",
                GITHUB_REF_TYPE="branch", GITHUB_REF="refs/heads/master", GITHUB_REF_NAME="master",
                GITHUB_REF_PROTECTED="false", GITHUB_SHA="a" * 40,
                GITHUB_RUN_ID="100", GITHUB_RUN_NUMBER="7", GITHUB_RUN_ATTEMPT="1",
                LOWER_JOB_PHASE="build", LOWER_JOB_ARCH="x64") | changes


class GitHubEnvironmentTest(unittest.TestCase):
    # 原生来源必须覆盖继承的旧 CI 字段，不能把其他仓库或提交混入本次身份。
    def test_native_source_replaces_inherited_gitlab_fields(self):
        source = github_environment(CI_PROJECT_URL="https://git.example/other", CI_COMMIT_SHA="b" * 40,
                                    CI_COMMIT_TAG="v9.9.9", CI_JOB_TOKEN="old-secret")
        environment = normalize_environment(source)
        self.assertEqual(environment["CI_PROJECT_URL"], "https://github.com/CcooLcyy/MskDSP")
        self.assertEqual(environment["CI_COMMIT_SHA"], "a" * 40)
        self.assertEqual(environment["CI_COMMIT_BRANCH"], "master")
        self.assertNotIn("CI_COMMIT_TAG", environment)
        self.assertNotIn("CI_JOB_TOKEN", environment)
        self.assertEqual(source["CI_COMMIT_TAG"], "v9.9.9")

    # 两架构、阶段和重跑必须产生不同的整数作业身份。
    def test_job_identity_has_no_stage_architecture_or_attempt_collision(self):
        ids = {normalize_environment(github_environment(LOWER_JOB_ARCH=arch, LOWER_JOB_PHASE=phase,
                                                       GITHUB_RUN_ATTEMPT=str(attempt),
                                                       GITHUB_RUN_ID=str(run)))["CI_JOB_ID"]
               for arch in ("x64", "arm64") for phase in ("build", "test", "package", "publish")
               for attempt in (1, 2, 3) for run in (100, 101)}
        self.assertEqual(len(ids), 48)
        self.assertTrue(all(value.isdecimal() and int(value) > 0 for value in ids))

    # 标签保护状态只能来自运行时真实值，不能把分支附加到标签环境。
    def test_tag_protection_and_ref_are_not_invented(self):
        env = normalize_environment(github_environment(GITHUB_REF_TYPE="tag", GITHUB_REF="refs/tags/v1.0.0",
                                                       GITHUB_REF_NAME="v1.0.0"))
        self.assertEqual(env["CI_COMMIT_TAG"], "v1.0.0")
        self.assertNotIn("CI_COMMIT_BRANCH", env)
        self.assertEqual(env["CI_COMMIT_REF_PROTECTED"], "false")

    # 非受控事件、伪造引用、非法运行编号和非官方来源均应拒绝。
    def test_invalid_runtime_is_rejected(self):
        for changes in ({"GITHUB_EVENT_NAME": "pull_request"}, {"GITHUB_EVENT_NAME": "schedule"},
                        {"GITHUB_REF": "refs/heads/other"}, {"GITHUB_RUN_ATTEMPT": "0"},
                        {"GITHUB_RUN_ID": "bad"}, {"GITHUB_REF_PROTECTED": ""},
                        {"LOWER_JOB_ARCH": "other"}, {"LOWER_JOB_PHASE": "unknown"},
                        {"GITHUB_SERVER_URL": "https://user:secret@github.com"},
                        {"GITHUB_REPOSITORY": "owner/repo/extra"}, {"GITHUB_SHA": "bad"}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                normalize_environment(github_environment(**changes))

    # 既有 GitLab 调用保留原始身份，不修改父环境。
    def test_gitlab_environment_remains_unchanged(self):
        original = {"CI_PIPELINE_ID": "9"}
        actual = normalize_environment(original)
        self.assertEqual(actual, original)
        self.assertIsNot(actual, original)


if __name__ == "__main__":
    unittest.main()
