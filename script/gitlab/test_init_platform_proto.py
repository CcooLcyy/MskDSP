"""固定协议获取过程的令牌隔离及提交校验测试。"""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import init_platform_proto as init


class InitTests(unittest.TestCase):
    def test_fetch_uses_pinned_commit_and_environment_auth(self):
        with tempfile.TemporaryDirectory() as root, patch.object(init.subprocess, "run") as run:
            run.return_value.returncode = 0
            run.return_value.stdout = init.PIN + "\n"
            destination = Path(root) / "proto"
            def initialize_git(*args, **kwargs):
                (destination / ".git").mkdir(exist_ok=True)
                return run.return_value
            run.side_effect = initialize_git
            with patch.object(init, "validate_protocol"):
                init.initialize(destination, {"CI_SERVER_URL": "https://git.example", "CI_JOB_TOKEN": "secret-value"})
            commands = [call.args[0] for call in run.call_args_list]
            self.assertTrue(any(init.PIN in command for command in commands))
            self.assertNotIn("secret-value", str(commands))
            for call in run.call_args_list:
                self.assertEqual(call.kwargs["env"]["GIT_TERMINAL_PROMPT"], "0")
                self.assertIn("GIT_CONFIG_VALUE_0", call.kwargs["env"])

    def test_rejects_credential_url(self):
        for url in ("https://user:pass@git.example", "https://git.example\n", "https://git.example:bad", "https://git.example/path"):
            with self.subTest(url=url), self.assertRaises(init.InitError):
                init.auth_environment({"CI_SERVER_URL": url, "CI_JOB_TOKEN": "secret"})

    def test_auth_does_not_modify_parent_environment_or_keep_old_config(self):
        original = dict(CI_SERVER_URL="https://git.example", CI_JOB_TOKEN="secret",
                        GIT_CONFIG_COUNT="9", GIT_CONFIG_KEY_8="http.extraHeader",
                        GIT_CONFIG_VALUE_8="旧认证", GIT_CONFIG_PARAMETERS="旧配置")
        _, environment = init.auth_environment(original)
        self.assertEqual(original["GIT_CONFIG_COUNT"], "9")
        self.assertNotIn("GIT_CONFIG_KEY_8", environment)
        self.assertNotIn("GIT_CONFIG_PARAMETERS", environment)
        self.assertEqual(environment["GIT_CONFIG_KEY_0"], "http.https://git.example/.extraHeader")

    def test_pin_is_frozen(self):
        self.assertEqual(init.PIN, "a9723c5ebade10a5588da59bdfc0c51123bfdf67")
        self.assertEqual(Path(init.__file__).with_name("platform-proto.ref").read_text().strip(), init.PIN)

    def test_existing_unrelated_repository_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as root, patch.object(init.subprocess, "run") as run:
            destination = Path(root)
            (destination / ".git").mkdir()
            with self.assertRaises(init.InitError):
                init.initialize(destination, {"CI_SERVER_URL": "https://git.example", "CI_JOB_TOKEN": "secret"})
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
