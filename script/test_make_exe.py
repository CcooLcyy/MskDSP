"""在隔离目录验证离线安装器的镜像身份与失败退出行为。"""

from __future__ import annotations

import hashlib
import gzip
import bz2
import io
import json
import lzma
import os
from pathlib import Path
import shlex
import shutil
import stat
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
import gzip
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
    input_path = Path(args[args.index("-i") + 1])
    assert sorted(path.name for path in input_path.parent.iterdir()) == ["image.tar.gz"], "安装器只能落盘压缩镜像临时文件"
    compressed = input_path.read_bytes()
    assert compressed[:2] == b"\x1f\x8b", "Docker 必须收到 gzip 压缩载荷"
    assert gzip.decompress(compressed) == Path(state["payload_archive"]).read_bytes(), "Docker 输入必须完整还原原始镜像 tar"
    state["loaded_gzip"] = True
    state["load_input_name"] = Path(args[args.index("-i") + 1]).name
    state_path.write_text(json.dumps(state), encoding="utf-8")
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
        # 安装器内嵌准备步骤也只运行隔离桩，严禁测试接触真实宿主机网络。
        (scripts / 'network').mkdir()
        (scripts / 'network/networkd_policy.py').write_text('''import json, os, sys
from pathlib import Path
p = Path(os.environ["MOCK_DOCKER_STATE"])
s = json.loads(p.read_text())
s["calls"].append(["host-network-prepare"])
p.write_text(json.dumps(s))
sys.exit(31 if s.get("host_prepare_fail") else 0)
''', encoding='utf-8')
        self.payload = self.root / "payload.tar"
        self.installed = self.root / "installed.tar"
        self.state_path = self.root / "state.json"
        self.bin = self.root / "bin"
        self.bin.mkdir()
        mock = self.root / "mock_docker.py"
        mock.write_text(MOCK_DOCKER, encoding="utf-8")
        # Git Bash 调用 Windows Python 时保留设备挂载参数，不转换为 Windows 路径。
        for name, command in {
            "python3": f"exec {shlex.quote(bash_path(Path(sys.executable)))} \"$@\"\n",
            "docker": f"MSYS2_ARG_CONV_EXCL='/dev:/dev;/run/dbus/system_bus_socket:/run/dbus/system_bus_socket:ro' exec python3 {shlex.quote(bash_path(mock))} \"$@\"\n",
        }.items():
            script = self.bin / name
            script.write_text("#!/usr/bin/env bash\n" + command, encoding="utf-8", newline="\n")
            script.chmod(0o755)

    def run_installer(self, *, oci: bool = False, current_id: str = "", loaded_id: str | None = None,
                      existing_content: bytes | None = None, payload_fault: str | None = None,
                      payload_content: bytes | None = None,
                      host_os: str = "Linux", host_arch: str = "x86_64", host_probe_fail: bool = False,
                      archive_limit: int | None = None,
                      compressed_input: str | None = None,
                      build_archive_limit: int | None = None,
                      managed_update: bool = False,
                      system_bus_socket: bool | None = None,
                      resolved_stub: bool | None = None,
                      **options: bool) -> subprocess.CompletedProcess[str]:
        archive_options = {key: options.pop(key) for key in list(options)
                           if key in {"corrupt_config", "corrupt_manifest", "wrong_link", "multiple",
                                      "multiple_platforms", "wrong_platform", "duplicate_config",
                                      "oversized_config"}}
        identity = image_archive(self.payload, oci=oci, content=payload_content, **archive_options)
        build_input = self.payload
        if compressed_input:
            build_input = self.payload.with_suffix(".tar." + compressed_input)
            compressor = {"gz": gzip.compress, "bz2": bz2.compress, "xz": lzma.compress}[compressed_input]
            build_input.write_bytes(compressor(self.payload.read_bytes()))
        image_archive(self.installed, content=existing_content)
        state = {"current_id": current_id, "loaded_id": loaded_id or identity["config_id"],
                 "installed_archive": str(self.installed), "payload_archive": str(self.payload),
                 "calls": [], **options}
        self.state_path.write_text(json.dumps(state), encoding="utf-8")
        env = {**os.environ, "PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
               "MOCK_DOCKER_STATE": str(self.state_path), "PYTHONIOENCODING": "utf-8"}
        self.payload_temp = self.root / "payload-temp"
        self.payload_temp.mkdir(exist_ok=True)
        env["TMPDIR"] = bash_path(self.payload_temp)
        env.pop("MSYS_NO_PATHCONV", None)
        if managed_update:
            env["MSKDSP_MANAGED_UPDATE"] = "1"
        if build_archive_limit is not None:
            build_script = self.root / "script" / "make_exe.sh"
            build_script.write_bytes(Path(__file__).with_name("make_exe.sh").read_bytes().replace(
                b"MAX_ARCHIVE_BYTES = 4 * 1024 * 1024 * 1024",
                f"MAX_ARCHIVE_BYTES = {build_archive_limit}".encode()))
        built = subprocess.run([self.bash, bash_path(self.root / "script" / "make_exe.sh"),
                                bash_path(build_input)], env=env, capture_output=True, text=True,
                               encoding="utf-8", errors="replace", timeout=30)
        if compressed_input or build_archive_limit is not None:
            return built
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        package = self.root / "images" / "mskdsp-test"
        package.write_bytes(package.read_bytes().replace(b'HOST_DIR="/data/mskdsp"',
                            f"HOST_DIR={shlex.quote(bash_path(self.root / 'runtime'))}".encode()))
        # 仅在隔离安装器中替换探测函数，保留真实调用语句以覆盖变量遮蔽等问题。
        host_system = repr(host_os) if not host_probe_fail else '(_ for _ in ()).throw(OSError("模拟探测失败"))'
        patched = package.read_bytes()
        if system_bus_socket is not None:
            patched = patched.replace(b"[[ -S /run/dbus/system_bus_socket ]]", b"true" if system_bus_socket else b"false")
        if resolved_stub is not None:
            patched = patched.replace(b"[[ -f /run/systemd/resolve/stub-resolv.conf ]]", b"true" if resolved_stub else b"false")
        for module_name, import_line in (("platform", "import platform\n"),
                                         ("host_platform", "import platform as host_platform\n")):
            replacement = (import_line + f"{module_name}.system = lambda: {host_system}\n"
                           + f"{module_name}.machine = lambda: {host_arch!r}\n")
            patched = patched.replace(import_line.encode(), replacement.encode())
        package.write_bytes(patched)
        self.package = package
        if archive_limit is not None:
            package.write_bytes(package.read_bytes().replace(
                b"MAX_ARCHIVE_BYTES = 4 * 1024 * 1024 * 1024",
                f"MAX_ARCHIVE_BYTES = {archive_limit}".encode()))
        if payload_fault:
            header, payload = package.read_bytes().split(b"\n__ARCHIVE_BELOW__\n", 1)
            if payload_fault == "raw":
                payload = self.payload.read_bytes()
            else:
                payload = bytearray(gzip.compress(self.payload.read_bytes(), compresslevel=6, mtime=0))
                if payload_fault == "crc":
                    payload[-8] ^= 1
                elif payload_fault == "truncated":
                    del payload[-8:]
                elif payload_fault == "multiple_members":
                    payload.extend(gzip.compress(b"extra-member", mtime=0))
                elif payload_fault == "trailing_data":
                    payload.extend(b"trailing")
                elif payload_fault == "trailing_zero":
                    payload.extend(b"\0")
                else:
                    raise ValueError(payload_fault)
            package.write_bytes(header + b"\n__ARCHIVE_BELOW__\n" + payload)
            for directory in ("module", "conf"):
                preserved = self.root / "runtime" / directory
                preserved.mkdir(parents=True, exist_ok=True)
                (preserved / "preserve.txt").write_text("现场内容", encoding="utf-8")
        return subprocess.run([self.bash, bash_path(package), "start"], env=env,
                              capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)

    def calls(self, command: str) -> list[list[str]]:
        state = json.loads(self.state_path.read_text(encoding="utf-8"))
        return [args for args in state["calls"] if args[0] == command]

    # 宿主机准备失败必须发生在替换旧业务容器与现场模块之前。
    def test_host_prepare_failure_preserves_old_container(self):
        result = self.run_installer(host_prepare_fail=True)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.calls('host-network-prepare'), [['host-network-prepare']])
        self.assertEqual(self.calls('rm'), [])
        self.assertEqual(self.calls('run'), [])

    # 完整镜像身份校验后先准备宿主机，再进入任何现场目录或容器替换。
    def test_host_prepare_runs_after_identity_before_container_replacement(self):
        result = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        state = json.loads(self.state_path.read_text())['calls']
        prepared = state.index(['host-network-prepare'])
        self.assertLess(next(i for i, call in enumerate(state) if call[0] == 'load'), prepared)
        self.assertLess(prepared, state.index(['rm', '-f', 'mskdsp']))

    # 匹配平台与常见别名均允许安装，避免安装行为依赖当前测试机平台。
    def test_matching_host_and_payload_platforms_are_allowed(self) -> None:
        for host_arch, image_arch in (("x86_64", "amd64"), ("amd64", "x86_64"),
                                      ("aarch64", "arm64"), ("arm64", "aarch64")):
            with self.subTest(host_arch=host_arch, image_arch=image_arch):
                result = self.run_installer(host_arch=host_arch, payload_content=json.dumps(
                    {"os": "linux", "architecture": image_arch}).encode())
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("安装平台校验通过", result.stdout)
                self.assertEqual(len(self.calls("load")), 1)

    # 不匹配、未知平台、缺失字段与探测失败均在任何 Docker 或现场目录写入前拒绝。
    def test_platform_rejection_preserves_existing_installation(self) -> None:
        cases = [
            ({"host_arch": "aarch64"}, {"os": "linux", "architecture": "amd64"}),
            ({"host_arch": "x86_64"}, {"os": "linux", "architecture": "arm64"}),
            ({"host_arch": "riscv64"}, {"os": "linux", "architecture": "amd64"}),
            ({}, {"os": "linux", "architecture": "riscv64"}),
            ({}, {"os": "linux"}),
            ({}, {"architecture": "amd64"}),
            ({"host_os": "Windows"}, {"os": "linux", "architecture": "amd64"}),
            ({}, {"os": "windows", "architecture": "amd64"}),
            ({"host_probe_fail": True}, {"os": "linux", "architecture": "amd64"}),
        ]
        for host_options, config in cases:
            with self.subTest(host_options=host_options, config=config):
                for directory in ("module", "conf"):
                    preserved = self.root / "runtime" / directory
                    preserved.mkdir(parents=True, exist_ok=True)
                    (preserved / "preserve.txt").write_text("现场内容", encoding="utf-8")
                result = self.run_installer(payload_content=json.dumps(config).encode(), **host_options)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("宿主机平台", result.stderr)
                self.assertIn("安装包平台", result.stderr)
                state = json.loads(self.state_path.read_text(encoding="utf-8"))
                self.assertEqual(state["calls"], [], "平台校验失败不能进入 Docker 操作")
                for directory in ("module", "conf"):
                    preserved = self.root / "runtime" / directory
                    self.assertEqual([path.name for path in preserved.iterdir()], ["preserve.txt"])
                    self.assertEqual((preserved / "preserve.txt").read_text(encoding="utf-8"), "现场内容")
                self.assertEqual(list(self.payload_temp.iterdir()), [])

    # 已有同摘要镜像也不能绕过平台检查，以免跳过加载后仍替换容器和模块。
    def test_same_image_identity_cannot_bypass_platform_check(self) -> None:
        identity = image_archive(self.payload)
        result = self.run_installer(current_id=identity["config_id"], host_arch="aarch64")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(self.state_path.read_text(encoding="utf-8"))["calls"], [])

    # OCI 描述符的平台字段不能遮蔽宿主机探测模块，合法 OCI 包应正常安装。
    def test_matching_oci_platform_is_allowed(self) -> None:
        result = self.run_installer(oci=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("安装平台校验通过", result.stdout)
        self.assertEqual(len(self.calls("load")), 1)

    # 发布仅压缩归档载荷，关闭 gzip 文件名和时间戳，Docker 完整还原原始 tar。
    def test_generated_payload_is_reproducible_gzip(self) -> None:
        result = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        payload = self.package.read_bytes().split(b"\n__ARCHIVE_BELOW__\n", 1)[1]
        self.assertEqual(payload[:3], b"\x1f\x8b\x08")
        self.assertEqual(payload[3], 0, "gzip 不能记录原始文件名")
        self.assertEqual(payload[4:8], b"\0" * 4, "gzip 不能记录时间戳")
        self.assertEqual(gzip.decompress(payload), self.payload.read_bytes())
        expected = io.BytesIO()
        with gzip.GzipFile(filename="", fileobj=expected, mode="wb", compresslevel=6, mtime=0) as archive:
            archive.write(self.payload.read_bytes())
        self.assertEqual(payload, expected.getvalue())
        state = json.loads(self.state_path.read_text(encoding="utf-8"))
        self.assertTrue(state["loaded_gzip"])
        self.assertEqual(state["load_input_name"], "image.tar.gz")
        self.assertEqual(list(self.payload_temp.iterdir()), [])

    # 裸 tar、尾部 CRC 错误和截断必须在 Docker 加载、模块替换前拒绝并清理压缩临时文件。
    def test_invalid_compressed_payload_preserves_existing_installation(self) -> None:
        for fault in ("raw", "crc", "truncated"):
            with self.subTest(fault=fault):
                result = self.run_installer(payload_fault=fault)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                state = json.loads(self.state_path.read_text(encoding="utf-8"))
                self.assertEqual(state["calls"], [], "损坏载荷不能进入 Docker 操作")
                for directory in ("module", "conf"):
                    self.assertEqual((self.root / "runtime" / directory / "preserve.txt").read_text(encoding="utf-8"), "现场内容")
                self.assertEqual(list(self.payload_temp.iterdir()), [])

    # 与上位机保持单成员规则，额外 gzip 成员以及任何尾随字节都不能进入 Docker。
    def test_multiple_members_and_trailing_bytes_are_rejected(self) -> None:
        for fault in ("multiple_members", "trailing_data", "trailing_zero"):
            with self.subTest(fault=fault):
                result = self.run_installer(payload_fault=fault)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                state = json.loads(self.state_path.read_text(encoding="utf-8"))
                self.assertEqual(state["calls"], [])
                self.assertEqual(list(self.payload_temp.iterdir()), [])

    # 发布输入只接受原始 tar，提前拒绝 gzip/bzip2/xz，避免二次压缩生成无法安装的产物。
    def test_packager_rejects_already_compressed_input(self) -> None:
        for compression in ("gz", "bz2", "xz"):
            with self.subTest(compression=compression):
                result = self.run_installer(compressed_input=compression)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse((self.root / "images" / "mskdsp-test").exists())

    # 缩小发布阈值验证超限原始 tar 被拒绝，并清理已写入的安装脚本头，避免发布无法安装的包。
    def test_packager_rejects_oversized_source_tar(self) -> None:
        result = self.run_installer(build_archive_limit=10240)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((self.root / "images" / "mskdsp-test").exists())
        result = self.run_installer(build_archive_limit=4096)
        self.assertEqual(self.payload.stat().st_size, 10240)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.root / "images" / "mskdsp-test").exists())

    # 缩小实际生产上限来验证拒绝逻辑，避免测试生成 4 GiB 镜像；恰好达到上限仍允许安装。
    def test_decompressed_archive_size_is_bounded(self) -> None:
        result = self.run_installer(archive_limit=10240)
        self.assertEqual(self.payload.stat().st_size, 10240)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = self.run_installer(archive_limit=4096)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        state = json.loads(self.state_path.read_text(encoding="utf-8"))
        self.assertEqual(state["calls"], [])
        self.assertEqual(list(self.payload_temp.iterdir()), [])

    # 首次安装和版本更新重建容器时，都应默认共享宿主机设备目录。
    def test_install_and_update_share_host_devices(self) -> None:
        for current_id in ("", "sha256:" + "b" * 64):
            with self.subTest(current_id=current_id):
                result = self.run_installer(
                    current_id=current_id,
                    existing_content=b'{"architecture":"amd64","os":"linux","old":true}')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                run = self.calls("run")[0]
                volumes = [run[index + 1] for index, arg in enumerate(run[:-1]) if arg == "-v"]
                self.assertEqual(volumes.count("/dev:/dev"), 1)
                self.assertIn("--privileged", run)
                self.assertEqual(len(self.calls("load")), 1)
                state = json.loads(self.state_path.read_text(encoding="utf-8"))
                self.assertIn(["rm", "-f", "mskdsp"], state["calls"])
                self.assertLess(state["calls"].index(["rm", "-f", "mskdsp"]),
                                state["calls"].index(run))

    # 只在系统总线套接字存在时只读挂载，缺失时保持旧设备的安装能力。
    def test_install_optional_system_bus_socket(self) -> None:
        binding = "/run/dbus/system_bus_socket:/run/dbus/system_bus_socket:ro"
        for present in (False, True):
            with self.subTest(present=present):
                result = self.run_installer(system_bus_socket=present)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                run = self.calls("run")[0]
                self.assertEqual(binding in run, present)

    # host 网络容器仅在总线与 resolved stub 同时存在时使用共享环回 DNS。
    def test_install_uses_host_resolved_only_with_bus_and_stub(self) -> None:
        for bus, stub in ((True, True), (True, False), (False, True)):
            with self.subTest(bus=bus, stub=stub):
                result = self.run_installer(system_bus_socket=bus, resolved_stub=stub)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                run = self.calls("run")[0]
                self.assertEqual("--dns" in run, bus and stub)
                if bus and stub:
                    self.assertEqual(run[run.index("--dns") + 1], "127.0.0.53")

    def assert_platform_access_preserved(self, *, update: bool) -> None:
        token = "factory-installer-regression-device-token"
        conf = self.root / "runtime" / "conf"
        conf.mkdir(parents=True)
        path = conf / "platform_access.json"
        original = (json.dumps({"revision": 3, "enabled": True, "device_id": "factory-device-01",
                                "device_token": token, "ssh_enabled": True, "ssh_port": 10022},
                               separators=(",", ":")) + "\n").encode("utf-8")
        path.write_bytes(original)
        path.chmod(0o600)
        before_mode = stat.S_IMODE(path.stat().st_mode)
        # NTFS 不提供完整 POSIX 模式；Linux 回归必须实际具有并保留 0600。
        if os.name != "nt":
            self.assertEqual(before_mode, 0o600)
        identity = image_archive(self.payload)
        result = self.run_installer(
            current_id="sha256:" + "b" * 64 if update else identity["config_id"],
            existing_content=b'{"architecture":"amd64","os":"linux","old":true}' if update else None)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(path.read_bytes(), original)
        self.assertEqual(stat.S_IMODE(path.stat().st_mode), before_mode)
        self.assertNotIn(token, result.stdout)
        self.assertNotIn(token, result.stderr)
        self.assertEqual(len(self.calls("load")), 1 if update else 0)
        run = self.calls("run")[0]
        volumes = [run[index + 1] for index, arg in enumerate(run[:-1]) if arg == "-v"]
        # Git Bash 向 Windows Python 转换挂载路径；Linux 直接核对实际 Docker 参数。
        if os.name == "nt":
            bindings = [value.rsplit(";", 1) for value in volumes if ";" in value]
            matches = [target for host, target in bindings if Path(host) == conf]
            self.assertEqual(len(matches), 1)
            self.assertEqual(Path(matches[0]).parts[-3:], ("opt", "mskdsp", "conf"))
        else:
            self.assertEqual(volumes.count(f"{conf}:/opt/mskdsp/conf"), 1)
        self.assertNotIn(token, json.dumps(run))

    # 同版本重新创建容器仍绑定既有 conf，完整保留平台接入文件、权限及凭据保密。
    def test_same_version_rebuild_preserves_platform_access(self) -> None:
        self.assert_platform_access_preserved(update=False)

    # 镜像升级只更新模块启动策略，平台接入文件字节和 0600 权限不被重置。
    def test_image_update_preserves_platform_access(self) -> None:
        self.assert_platform_access_preserved(update=True)

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
        self.assertEqual(list(self.payload_temp.iterdir()), [])

    # Docker run 命令失败，安装器不能返回执行成功。
    def test_run_failure_is_not_success(self) -> None:
        result = self.run_installer(run_fail=True)
        self.assertNotEqual(result.returncode, 0)

    # Docker run 返回容器 ID 但容器已经退出，仍必须返回失败。
    def test_stopped_container_is_not_success(self) -> None:
        result = self.run_installer(run_stopped=True)
        self.assertNotEqual(result.returncode, 0)

    # 宿主机受管理更新必须保留旧镜像，直到独立任务验证后再轮换。
    def test_managed_update_preserves_old_images(self) -> None:
        result = self.run_installer(current_id="sha256:" + "a" * 64, managed_update=True,
                                    existing_content=b'{"architecture":"amd64","os":"linux","old":true}')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.calls("rmi"), [])
        self.assertIn(b"MSKDSP_MANAGED_UPDATE_V1=1", self.package.read_bytes())


if __name__ == "__main__":
    unittest.main()
