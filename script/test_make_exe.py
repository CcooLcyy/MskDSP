"""在隔离目录验证离线安装器的镜像身份与失败退出行为。"""

from __future__ import annotations

import hashlib
import io
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest


def bash_path(path: Path) -> str:
    value = path.resolve().as_posix()
    if os.name == "nt" and len(value) > 1 and value[1] == ":":
        return "/" + value[0].lower() + value[2:]
    return value


def image_archive(path: Path, *, oci: bool = False, content: bytes | None = None,
                  corrupt_config: bool = False, corrupt_manifest: bool = False,
                  wrong_link: bool = False, multiple: bool = False,
                  multiple_platforms: bool = False, wrong_platform: bool = False,
                  duplicate_config: bool = False, oversized_config: bool = False) -> dict[str, str]:
    config = content or b'{"architecture":"amd64","os":"linux","rootfs":{"type":"layers","diff_ids":[]}}'
    if oversized_config:
        config += b" " * (1024 * 1024)
    config_hash = hashlib.sha256(config).hexdigest()
    config_path = f"blobs/sha256/{config_hash}" if oci else f"{config_hash}.json"
    manifest = [{"Config": config_path, "RepoTags": ["mskdsp:test"], "Layers": []}]
    if multiple:
        manifest.append(dict(manifest[0]))
    files = {config_path: config + (b" " if corrupt_config else b""),
             "manifest.json": json.dumps(manifest).encode()}
    manifest_hash = ""
    if oci:
        descriptor = {"schemaVersion": 2,
                      "mediaType": "application/vnd.oci.image.manifest.v1+json",
                      "config": {"digest": "sha256:" + ("a" * 64 if wrong_link else config_hash)},
                      "layers": []}
        descriptor_data = json.dumps(descriptor).encode()
        manifest_hash = hashlib.sha256(descriptor_data).hexdigest()
        files[f"blobs/sha256/{manifest_hash}"] = descriptor_data + (b" " if corrupt_manifest else b"")
        descriptors = [{
            "mediaType": descriptor["mediaType"], "digest": "sha256:" + manifest_hash,
            "size": len(descriptor_data), "platform": {
                "architecture": "arm64" if wrong_platform else "amd64", "os": "linux"}}]
        if multiple_platforms:
            descriptors.append(dict(descriptors[0]))
        files["index.json"] = json.dumps({"schemaVersion": 2, "manifests": descriptors}).encode()
    with tarfile.open(path, "w") as archive:
        for name, data in files.items():
            member = tarfile.TarInfo(name)
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))
        if duplicate_config:
            member = tarfile.TarInfo(config_path)
            member.size = len(config)
            archive.addfile(member, io.BytesIO(config))
    return {"config_id": "sha256:" + config_hash,
            "manifest_id": "sha256:" + manifest_hash if manifest_hash else ""}


MOCK_DOCKER = r'''
import json
import os
from pathlib import Path
import sys

state_path = Path(os.environ["MOCK_DOCKER_STATE"])
state = json.loads(state_path.read_text(encoding="utf-8"))
args = sys.argv[1:]
state["calls"].append(args)
state_path.write_text(json.dumps(state), encoding="utf-8")
if args[:2] == ["image", "inspect"] or args[:1] == ["images"] and "-q" in args:
    if not state["current_id"]:
        sys.exit(1)
    print(state["current_id"])
elif args[:2] == ["image", "save"] or args[:1] == ["save"]:
    if state.get("save_fail"):
        sys.exit(17)
    sys.stdout.buffer.write(Path(state["installed_archive"]).read_bytes())
elif args[:1] == ["load"]:
    if state.get("load_fail"):
        sys.exit(19)
    state["current_id"] = state["loaded_id"]
    if not state.get("keep_old_on_load"):
        state["installed_archive"] = state["payload_archive"]
elif args[:1] == ["cp"]:
    target = Path(args[-1])
    if target.is_dir() or args[-1].endswith("/"):
        target.mkdir(parents=True, exist_ok=True)
        (target / "default.json").write_text("{}", encoding="utf-8")
    else:
        target.write_text("{}", encoding="utf-8")
elif args[:1] == ["run"]:
    if state.get("run_fail"):
        sys.exit(23)
    state["running"] = not state.get("run_stopped")
    print("mock-container")
elif args[:1] == ["inspect"]:
    print("true" if state.get("running") else "false")
elif args[:1] == ["create"]:
    print("mock-init")
state_path.write_text(json.dumps(state), encoding="utf-8")
'''


class InstallerIdentityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        candidate = os.environ.get("MSKDSP_TEST_BASH")
        if not candidate and os.name == "nt":
            candidate = "C:/software/Git/bin/bash.exe"
        cls.bash = candidate or shutil.which("bash")
        if not cls.bash or not Path(cls.bash).exists():
            raise unittest.SkipTest("未找到可用的 Bash")

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        scripts = self.root / "script"
        scripts.mkdir()
        shutil.copyfile(Path(__file__).with_name("make_exe.sh"), scripts / "make_exe.sh")
        self.payload = self.root / "payload.tar"
        self.installed = self.root / "installed.tar"
        self.state_path = self.root / "state.json"
        self.bin = self.root / "bin"
        self.bin.mkdir()
        mock = self.root / "mock_docker.py"
        mock.write_text(MOCK_DOCKER, encoding="utf-8")
        for name, command in {
            "python3": f"exec {shlex.quote(bash_path(Path(sys.executable)))} \"$@\"\n",
            "docker": f"exec python3 {shlex.quote(bash_path(mock))} \"$@\"\n",
        }.items():
            script = self.bin / name
            script.write_text("#!/usr/bin/env bash\n" + command, encoding="utf-8", newline="\n")
            script.chmod(0o755)

    def run_installer(self, *, oci: bool = False, current_id: str = "", loaded_id: str | None = None,
                      existing_content: bytes | None = None, **options: bool) -> subprocess.CompletedProcess[str]:
        archive_options = {key: options.pop(key) for key in list(options)
                           if key in {"corrupt_config", "corrupt_manifest", "wrong_link", "multiple",
                                      "multiple_platforms", "wrong_platform", "duplicate_config",
                                      "oversized_config"}}
        identity = image_archive(self.payload, oci=oci, **archive_options)
        image_archive(self.installed, content=existing_content)
        state = {"current_id": current_id, "loaded_id": loaded_id or identity["config_id"],
                 "installed_archive": str(self.installed), "payload_archive": str(self.payload),
                 "calls": [], **options}
        self.state_path.write_text(json.dumps(state), encoding="utf-8")
        env = {**os.environ, "PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
               "MOCK_DOCKER_STATE": str(self.state_path), "PYTHONIOENCODING": "utf-8"}
        env.pop("MSYS_NO_PATHCONV", None)
        built = subprocess.run([self.bash, bash_path(self.root / "script" / "make_exe.sh"),
                                bash_path(self.payload)], env=env, capture_output=True, text=True,
                               encoding="utf-8", errors="replace", timeout=30)
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        package = self.root / "images" / "mskdsp-test"
        package.write_bytes(package.read_bytes().replace(b'HOST_DIR="/data/mskdsp"',
                            f"HOST_DIR={shlex.quote(bash_path(self.root / 'runtime'))}".encode()))
        return subprocess.run([self.bash, bash_path(package), "start"], env=env,
                              capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)

    def calls(self, command: str) -> list[list[str]]:
        state = json.loads(self.state_path.read_text(encoding="utf-8"))
        return [args for args in state["calls"] if args[0] == command]

    # 经典归档的 config 身份相同，应安全跳过镜像加载。
    def test_classic_same_config_skips_load(self) -> None:
        identity = image_archive(self.payload)
        result = self.run_installer(current_id=identity["config_id"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls("load"), [])
        self.assertFalse(any(args[:2] == ["image", "save"] for args in self.calls("image")))
        self.assertEqual(self.calls("run")[0][-1], identity["config_id"])
        self.assertTrue(all(args[-1] == identity["config_id"] for args in self.calls("create")))

    # OCI manifest 与 config 已绑定时，containerd 身份应安全跳过加载。
    def test_oci_same_manifest_skips_load(self) -> None:
        identity = image_archive(self.payload, oci=True)
        result = self.run_installer(oci=True, current_id=identity["manifest_id"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls("load"), [])
        self.assertFalse(any(args[:2] == ["image", "save"] for args in self.calls("image")))
        self.assertEqual(self.calls("run")[0][-1], identity["manifest_id"])

    # 不同类型的未知本机身份，必须通过流式导出核对 config 后才能跳过。
    def test_unknown_identity_same_saved_config_skips_load(self) -> None:
        result = self.run_installer(current_id="sha256:" + "b" * 64)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls("load"), [])
        self.assertTrue(any(args[:2] == ["image", "save"] for args in self.calls("image")))
        self.assertIn(["image", "save", "sha256:" + "b" * 64], self.calls("image"))

    # 已有镜像 config 不同，必须加载可信包并核对加载结果。
    def test_different_config_loads_image(self) -> None:
        result = self.run_installer(current_id="sha256:" + "b" * 64,
                                    existing_content=b'{"architecture":"amd64","os":"linux","old":true}')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(self.calls("load")), 1)

    # 路径摘要与 config 实际内容不符，必须在任何加载或容器替换前失败。
    def test_corrupt_config_is_rejected(self) -> None:
        result = self.run_installer(oci=True, corrupt_config=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.calls("load"), [])
        self.assertEqual(self.calls("rm"), [])

    # OCI manifest 内容摘要损坏，不能把其路径摘要当作可信身份。
    def test_corrupt_manifest_is_rejected(self) -> None:
        result = self.run_installer(oci=True, corrupt_manifest=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.calls("load"), [])

    # OCI manifest 指向另一个 config，必须拒绝放宽比较。
    def test_wrong_config_link_is_rejected(self) -> None:
        result = self.run_installer(oci=True, wrong_link=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.calls("load"), [])

    # 多镜像归档有选择歧义，必须在安装前拒绝。
    def test_multiple_images_are_rejected(self) -> None:
        result = self.run_installer(multiple=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.calls("load"), [])

    # OCI 多平台索引不能任意取首个镜像作为本平台的可信身份。
    def test_multiple_platforms_are_rejected(self) -> None:
        result = self.run_installer(oci=True, multiple_platforms=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.calls("load"), [])

    # 索引平台与 config 架构不同，必须拒绝安装。
    def test_wrong_platform_is_rejected(self) -> None:
        result = self.run_installer(oci=True, wrong_platform=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.calls("load"), [])

    # 同一 config 路径出现两次时，必须拒绝归档选择歧义。
    def test_duplicate_config_path_is_rejected(self) -> None:
        result = self.run_installer(duplicate_config=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.calls("load"), [])

    # 超大 config 元数据必须受限，不能无界读入内存。
    def test_oversized_config_is_rejected(self) -> None:
        result = self.run_installer(oversized_config=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.calls("load"), [])

    # 加载后返回不相关的身份且 config 仍错误，不能替换既有容器。
    def test_loaded_wrong_config_is_rejected(self) -> None:
        result = self.run_installer(loaded_id="sha256:" + "b" * 64, keep_old_on_load=True,
                                    existing_content=b'{"architecture":"amd64","os":"linux","old":true}')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.calls("rm"), [])

    # 加载后无法读取本机 config，不能仅凭 load 退出码判定成功。
    def test_loaded_unverifiable_identity_is_rejected(self) -> None:
        result = self.run_installer(loaded_id="sha256:" + "b" * 64, save_fail=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.calls("rm"), [])

    # Docker load 失败，安装器必须返回非零并保留既有业务容器。
    def test_load_failure_is_not_success(self) -> None:
        result = self.run_installer(load_fail=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.calls("rm"), [])

    # Docker run 命令失败，安装器不能返回执行成功。
    def test_run_failure_is_not_success(self) -> None:
        result = self.run_installer(run_fail=True)
        self.assertNotEqual(result.returncode, 0)

    # Docker run 返回容器 ID 但容器已经退出，仍必须返回失败。
    def test_stopped_container_is_not_success(self) -> None:
        result = self.run_installer(run_stopped=True)
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
