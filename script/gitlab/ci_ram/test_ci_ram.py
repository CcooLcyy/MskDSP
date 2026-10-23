"""共享内存配置工具的边界与保密测试。"""
import contextlib
import importlib.util
import io
import os
import pathlib
import shutil
import subprocess
import tempfile
import tomllib
import unittest
from unittest.mock import patch

ROOT = pathlib.Path(__file__).parent
SPEC = importlib.util.spec_from_file_location("ci_ram", ROOT / "ci_ram.py")
ci_ram = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ci_ram)

SOURCE = '''concurrent = 6
check_interval = 0

[[runners]]
  name = "测试 Runner"
  url = "https://private.example/"
  token = "禁止输出的凭据"
  executor = "docker"
  environment = ["OTHER=保留", "GIT_STRATEGY=fetch"]
  pre_build_script = "echo 原有准备"
  [runners.docker]
    image = "测试镜像"
    memory = "16g"
    volumes = ["/cache", "/home/daniel/cache:/ci-cache"]
'''


class PatchTests(unittest.TestCase):
    def test_patch_preserves_credentials_and_existing_settings(self):
        result = ci_ram.patch_config(SOURCE, "lower")
        data = tomllib.loads(result)
        runner = data["runners"][0]
        self.assertEqual(data["concurrent"], 1)
        self.assertEqual(runner["limit"], 1)
        self.assertEqual(runner["request_concurrency"], 1)
        self.assertEqual(runner["token"], "禁止输出的凭据")
        self.assertIn('url = "https://private.example/"', result)
        self.assertIn("OTHER=保留", runner["environment"])
        self.assertIn("GIT_STRATEGY=clone", runner["environment"])
        self.assertIn("GIT_CLONE_PATH=/builds/lower/$CI_PROJECT_PATH", runner["environment"])
        self.assertIn("FF_ENABLE_JOB_CLEANUP=true", runner["environment"])
        self.assertTrue(runner["custom_build_dir"]["enabled"])
        self.assertEqual(runner["pre_build_script"], ". /opt/ci-ram/pre-build.sh\necho 原有准备")
        docker = runner["docker"]
        self.assertEqual((docker["cpus"], docker["memory"], docker["memory_swap"]), ("4", "24g", "24g"))
        self.assertIn("mskdsp-ci-ram:/builds", docker["volumes"])
        self.assertIn("/home/daniel/ci-ram/tools:/opt/ci-ram:ro", docker["volumes"])
        self.assertIn("/home/daniel/cache:/ci-cache", docker["volumes"])
        self.assertEqual(docker["service_memory"], "64m")
        self.assertEqual(docker["service_memory_swap"], "64m")
        self.assertEqual(docker["service_cpus"], "0.1")
        self.assertEqual(docker["wait_for_services_timeout"], -1)
        self.assertEqual(docker["services"], [{"name": "alpine:3.20", "alias": "ci-ram-lease", "entrypoint": ["/bin/sh"], "command": ["-c", "exec sleep 2147483647"]}])

    def test_repeated_patch_is_identical_and_linux_prefix_is_separate(self):
        first = ci_ram.patch_config(SOURCE, "linux")
        self.assertEqual(first, ci_ram.patch_config(first, "linux"))
        self.assertIn("GIT_CLONE_PATH=/builds/linux/$CI_PROJECT_PATH", first)

    def test_multiline_existing_environment_and_script_are_preserved(self):
        source = SOURCE.replace('["OTHER=保留", "GIT_STRATEGY=fetch"]', '[\n "OTHER=保留",\n "GIT_STRATEGY=fetch",\n]')
        source = source.replace('"echo 原有准备"', "'''echo 原有准备\necho 后续步骤'''")
        runner = tomllib.loads(ci_ram.patch_config(source, "lower"))["runners"][0]
        self.assertTrue(runner["pre_build_script"].endswith("echo 后续步骤"))

    def test_cli_keeps_credentials_out_of_output(self):
        with tempfile.TemporaryDirectory() as folder:
            src, out = pathlib.Path(folder) / "source.toml", pathlib.Path(folder) / "result.toml"
            src.write_text(SOURCE, encoding="utf-8")
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                self.assertEqual(ci_ram.main(["patch", "--input", str(src), "--output", str(out), "--kind", "lower"]), 0)
            self.assertNotIn("禁止输出的凭据", stdout.getvalue())
            self.assertNotIn("private.example", stdout.getvalue())
            self.assertEqual(tomllib.loads(out.read_text(encoding="utf-8"))["runners"][0]["token"], "禁止输出的凭据")

    def test_rejects_non_docker_executor(self):
        with self.assertRaises(ValueError):
            ci_ram.patch_config(SOURCE.replace('executor = "docker"', 'executor = "shell"'), "lower")

    def test_rejects_multiple_runners_to_avoid_modifying_unrelated_entries(self):
        source = SOURCE + '\n[[runners]]\n  name = "其他 Runner"\n  executor = "docker"\n'
        with self.assertRaisesRegex(ValueError, "一个"):
            ci_ram.patch_config(source, "lower")

    def test_missing_global_concurrent_is_added_before_session_table(self):
        source = SOURCE.replace('concurrent = 6\n', '[session_server]\n session_timeout = 1800\n')
        parsed = tomllib.loads(ci_ram.patch_config(source, "lower"))
        self.assertEqual(parsed["concurrent"], 1)
        self.assertNotIn("concurrent", parsed["session_server"])

    def test_rejects_nested_array_sections_instead_of_patching_wrong_scope(self):
        source = SOURCE + '\n[[runners.docker.services]]\n name = "示例服务"\n'
        with self.assertRaises(ValueError):
            ci_ram.patch_config(source, "lower")

    def test_managed_service_is_preserved_before_unrelated_docker_children(self):
        service = '\n[[runners.docker.services]]\nname = "alpine:3.20"\nalias = "ci-ram-lease"\nentrypoint = ["/bin/sh"]\ncommand = ["-c", "exec sleep 2147483647"]\n'
        child = '\n[runners.docker.ulimit]\n"nofile" = "65536"\n'
        result = ci_ram.patch_config(SOURCE + service + child, "lower")
        parsed = tomllib.loads(result)["runners"][0]
        self.assertEqual(parsed["docker"]["ulimit"], {"nofile": "65536"})
        self.assertEqual(parsed["docker"]["memory"], "24g")
        self.assertEqual(parsed["docker"]["services"][0]["alias"], "ci-ram-lease")
        self.assertEqual(ci_ram.patch_config(result, "lower"), result)

    def test_empty_inline_services_can_be_replaced(self):
        result = ci_ram.patch_config(SOURCE + '    services = []\n', "linux")
        services = tomllib.loads(result)["runners"][0]["docker"]["services"]
        self.assertEqual(len(services), 1)
        self.assertEqual(services[0]["alias"], "ci-ram-lease")

    def test_modified_or_additional_services_are_rejected(self):
        managed = '\n[[runners.docker.services]]\nname = "alpine:3.20"\nalias = "ci-ram-lease"\nentrypoint = ["/bin/sh"]\ncommand = ["-c", "exec sleep 2147483647"]\n'
        for source in (SOURCE + managed.replace("2147483647", "60"), SOURCE + managed + '\n[[runners.docker.services]]\nname = "其他服务"\n'):
            with self.assertRaises(ValueError):
                ci_ram.patch_config(source, "lower")

    def test_non_service_nested_arrays_are_still_rejected(self):
        with self.assertRaises(ValueError):
            ci_ram.patch_config(SOURCE + '\n[[runners.custom]]\nname = "其他数组"\n', "lower")


class VolumeTests(unittest.TestCase):
    def test_volume_requires_exact_shared_capacity_and_noswap(self):
        good = {"Driver": "local", "Options": {"type": "tmpfs", "device": "tmpfs", "o": "size=24g,noswap,mode=1777"}}
        self.assertTrue(ci_ram.volume_matches(good))
        for option in ("size=24g,mode=1777", "size=12g,noswap,mode=1777"):
            self.assertFalse(ci_ram.volume_matches(dict(good, Options=dict(good["Options"], o=option))))
        self.assertFalse(ci_ram.volume_matches(dict(good, Driver="other")))

    def test_existing_wrong_volume_is_never_recreated(self):
        with patch.object(ci_ram, "inspect_volume", return_value={"Driver": "local", "Options": {}}), patch.object(ci_ram, "docker") as command:
            with self.assertRaises(ValueError):
                ci_ram.ensure_volume()
            command.assert_not_called()

    def test_existing_correct_volume_does_not_create_again(self):
        value = {"Driver": "local", "Options": ci_ram.OPTIONS}
        with patch.object(ci_ram, "inspect_volume", return_value=value), patch.object(ci_ram, "docker") as command:
            with contextlib.redirect_stdout(io.StringIO()):
                ci_ram.ensure_volume()
            command.assert_not_called()

    def test_prebuild_guards_and_lock_are_present(self):
        source = (ROOT / "pre-build.sh").read_text(encoding="utf-8")
        for text in ("stat -f -c %T", "/builds/lower/*", "/builds/linux/*", "exec 9>", "flock 9", "256", "GOTMPDIR", "MemAvailable"):
            self.assertIn(text, source)


@unittest.skipUnless(os.name == "posix" and shutil.which("flock"), "需要 POSIX shell 与 flock")
class PrepareShellTests(unittest.TestCase):
    def test_lock_lives_until_job_shell_exits_and_tmp_is_exported(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            project = root / "lower/team/project"
            project.mkdir(parents=True)
            script = root / "pre-build.sh"
            script.write_text((ROOT / "pre-build.sh").read_text(encoding="utf-8").replace("/builds", str(root)), encoding="utf-8")
            command = '''stat() { printf 'tmpfs\\n'; }
df() { printf '文件系统 1024块 已用 可用 使用率 挂载点\\n共享盘 25165824 0 25165824 0%% /builds\\n'; }
. "$CI_TEST_PREPARE"
test "$TMPDIR" = "$CI_PROJECT_DIR/.ci-tmp" || exit 41
test "$GOTMPDIR" = "$TMPDIR" || exit 42
if flock -n "$CI_TEST_LOCK" true; then exit 43; fi
'''
            env = dict(os.environ, CI_PROJECT_DIR=str(project), CI_TEST_PREPARE=str(script), CI_TEST_LOCK=str(root / ".compile.lock"))
            result = subprocess.run(["sh", "-c", command], env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue((project / ".ci-tmp").is_dir())
            self.assertEqual(subprocess.run(["flock", "-n", str(root / ".compile.lock"), "true"]).returncode, 0)

    def test_disk_workspace_and_escaped_path_fail_before_preparation(self):
        source = ROOT / "pre-build.sh"
        for directory in ("/tmp/其他目录", "/builds/lower/../../tmp"):
            result = subprocess.run(["sh", "-c", 'stat() { echo ext4; }; . "$CI_TEST_PREPARE"; exit 42'],
                                    env=dict(os.environ, CI_PROJECT_DIR=directory, CI_TEST_PREPARE=str(source)), capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotEqual(result.returncode, 42)


if __name__ == "__main__":
    unittest.main()
