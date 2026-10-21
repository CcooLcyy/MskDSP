#!/usr/bin/env python3
"""宿主机独立更新任务；仅使用标准库，生产入口只允许 root 和固定目录。"""
from __future__ import annotations

import argparse
from contextlib import closing, contextmanager
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import socket
import sqlite3
import stat
import subprocess
import sys
import tarfile
import threading
import time
import uuid
from urllib.parse import quote

STATE_ROOT = Path("/var/lib/mskdsp-update")
HOST_ROOT = Path("/data/mskdsp")
TERMINAL = {"succeeded", "rolled_back", "rollback_failed", "failed"}
MAX_PACKAGE = 4 * 1024**3
MANAGED_MARKER = b"MSKDSP_MANAGED_UPDATE_V1=1"


def operation_id(value):
    try:
        if str(uuid.UUID(value)) != value:
            raise ValueError()
    except (ValueError, AttributeError):
        raise ValueError("更新任务编号必须为规范 UUID") from None
    return value


def ensure_secure(path):
    """拒绝符号链接和普通用户可替换的 root 执行路径。"""
    for entry in [path, *path.parents]:
        info = entry.lstat()
        if stat.S_ISLNK(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
            raise ValueError(f"路径不是 root 独占且不可替换: {entry}")


def atomic_json(path, data):
    temporary = path.with_name(path.name + ".new")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | getattr(os, "O_NOFOLLOW", 0), 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as stream:
        json.dump(data, stream, ensure_ascii=False)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)
    if os.name != "nt":
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)


@contextmanager
def exclusive(path):
    stream = open(path, "a+b")
    try:
        if os.name == "nt":
            import msvcrt
            if stream.tell() == 0:
                stream.write(b"0")
                stream.flush()
            stream.seek(0)
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield
    finally:
        stream.close()


def copy_regular(source, destination, expected=None):
    fd = os.open(source, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0))
    digest = hashlib.sha256()
    try:
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode) or source.is_symlink() or before.st_size > MAX_PACKAGE:
            raise ValueError("上传来源必须为非符号链接的有界普通文件")
        with os.fdopen(fd, "rb", closefd=False) as incoming, open(destination, "xb") as outgoing:
            os.chmod(destination, 0o600)
            total = 0
            for chunk in iter(lambda: incoming.read(1024**2), b""):
                total += len(chunk)
                if total > MAX_PACKAGE:
                    raise ValueError("上传文件超过大小限制")
                digest.update(chunk)
                outgoing.write(chunk)
            outgoing.flush()
            os.fsync(outgoing.fileno())
        after = os.fstat(fd)
        if (before.st_size, before.st_mtime_ns, before.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino):
            raise ValueError("复制期间上传文件发生变化")
        if expected and digest.hexdigest() != expected:
            raise ValueError("安装包 SHA256 摘要不一致")
        return digest.hexdigest()
    except BaseException:
        destination.unlink(missing_ok=True)
        raise
    finally:
        os.close(fd)


def sqlite_header(path):
    if path.is_symlink() or not path.is_file():
        return False
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    try:
        return os.read(fd, 16) == b"SQLite format 3\x00"
    finally:
        os.close(fd)


def copy_database(source, destination):
    """使用 SQLite 在线备份获得已提交的一致视图，不能逐文件拼接 WAL 快照。"""
    deadline = time.monotonic() + 30
    def progress(status, remaining, total):
        if time.monotonic() > deadline:
            raise TimeoutError("SQLite 一致快照等待超过 30 秒")
    try:
        with closing(sqlite3.connect("file:" + quote(source.resolve().as_posix(), safe="/:") + "?mode=ro", uri=True, timeout=5)) as incoming:
            with closing(sqlite3.connect(destination, timeout=5)) as outgoing:
                outgoing.set_progress_handler(lambda: int(time.monotonic() > deadline), 10000)
                incoming.backup(outgoing, pages=64, progress=progress, sleep=0.05)
                if outgoing.execute("PRAGMA quick_check").fetchone()[0] != "ok":
                    raise ValueError("SQLite 快照完整性检查失败")
        os.chmod(destination, 0o600)
    except BaseException:
        destination.unlink(missing_ok=True)
        raise


def copy_tree(source, destination, secure, link_root=None):
    if source.is_symlink() or not source.is_dir():
        raise ValueError(f"备份目录无效或含符号链接: {source}")
    if secure:
        ensure_secure(source)
    destination.mkdir(mode=0o700)
    databases = {entry.name for entry in source.iterdir() if sqlite_header(entry)} if link_root is None else set()
    for entry in source.iterdir():
        if any(entry.name == database + suffix for database in databases for suffix in ("-wal", "-shm", "-journal")):
            continue
        mode = entry.lstat().st_mode
        if stat.S_ISDIR(mode):
            copy_tree(entry, destination / entry.name, secure, link_root)
        elif stat.S_ISREG(mode):
            if entry.name in databases:
                copy_database(entry, destination / entry.name)
            else:
                copy_regular(entry, destination / entry.name)
        elif stat.S_ISLNK(mode) and link_root is not None:
            target = os.readlink(entry)
            if os.path.isabs(target) or not entry.resolve().is_relative_to(link_root.resolve()):
                raise ValueError(f"模块符号链接越界: {entry}")
            (destination / entry.name).symlink_to(target)
        else:
            raise ValueError(f"备份拒绝符号链接、设备和特殊文件: {entry}")


class Backend:
    def command(self, *args, timeout=60):
        result = subprocess.run(args, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)
        if result.returncode:
            raise RuntimeError(f"命令执行失败 {args[0]}: {result.stderr[-2000:]}")
        return result.stdout.strip()

    def schedule(self, operation, helper):
        self.command("systemd-run", "--quiet", "--collect", "--unit=mskdsp-update-" + operation,
                     "--property=Type=exec", "--property=Restart=no", "--", "/usr/bin/python3", str(helper),
                     "run", "--operation-id", operation)

    def scheduled(self, operation):
        return self.command("systemctl", "show", "--property=ActiveState", "--value",
                            "mskdsp-update-" + operation + ".service") in {"active", "activating", "reloading"}

    def inspect(self):
        return json.loads(self.command("docker", "inspect", "mskdsp"))[0]

    def preflight(self, package, expected):
        for name in ("docker", "bash", "systemctl", "systemd-run"):
            if not shutil.which(name):
                raise ValueError(f"缺少独立更新所需命令: {name}")
        self.command("systemctl", "show", "--property=Version", "--value")
        with open(package, "rb") as stream:
            header = bytearray()
            while len(header) <= 1024**2:
                line = stream.readline(8192)
                if not line:
                    raise ValueError("安装包缺少载荷分界")
                if line == b"__ARCHIVE_BELOW__\n":
                    break
                header.extend(line)
            else:
                raise ValueError("安装包头超过限制")
            if MANAGED_MARKER not in header:
                raise ValueError("安装包不支持受管理更新，请重新发布安装包")
            with tarfile.open(fileobj=stream, mode="r|gz") as archive:
                metadata = {}
                total = 0
                for count, member in enumerate(archive):
                    if count > 100000:
                        raise ValueError("安装包归档条目超过限制")
                    total += member.size
                    if total > MAX_PACKAGE:
                        raise ValueError("安装包载荷超过大小限制")
                    if member.name == "manifest.json" or re.fullmatch(r"(?:blobs/sha256/)?" + re.escape(expected[7:]) + r"(?:\.json)?", member.name):
                        if not member.isfile() or member.size > 1024**2 or member.name in metadata:
                            raise ValueError("安装包镜像元数据无效")
                        metadata[member.name] = archive.extractfile(member).read()
                        if len(metadata) > 4096:
                            raise ValueError("安装包元数据条目超过限制")
                manifest = json.loads(metadata["manifest.json"])
                if len(manifest) != 1:
                    raise ValueError("安装包只能包含一个镜像")
                config = metadata.get(manifest[0]["Config"])
                if config is None:
                    raise ValueError("安装包镜像身份与请求不一致")
                if "sha256:" + hashlib.sha256(config).hexdigest() != expected:
                    raise ValueError("安装包镜像身份与请求不一致")
                config = json.loads(config)
                arch = {"x86_64": "amd64", "aarch64": "arm64"}.get(platform.machine())
                if arch is None or config.get("os") != "linux" or config.get("architecture") != arch:
                    raise ValueError("安装包架构与宿主机不匹配")
        snapshot = self.inspect()
        docker_root = Path(self.command("docker", "info", "--format", "{{.DockerRootDir}}"))
        if not docker_root.is_absolute() or shutil.disk_usage(docker_root).free < total * 2 + 256 * 1024**2:
            raise ValueError("Docker 存储空间不足以保留候选及恢复镜像")
        self.validate_deployment(snapshot)
        snapshot["modules"] = self.module_sockets(snapshot)
        snapshot["update_payload_bytes"] = total
        snapshot["update_candidate_tag"] = manifest[0]["RepoTags"][0]
        self.health(snapshot["Image"], snapshot["modules"], timeout=5)
        return snapshot

    def validate_deployment(self, data):
        """第一版只接受当前安装器默认部署，拒绝不能精确恢复的定制参数。"""
        host = data["HostConfig"]
        config = data["Config"]
        expected = {"/dev:/dev", "/data/mskdsp/conf:/opt/mskdsp/conf", "/data/mskdsp/module:/opt/mskdsp/module", "/data/mskdsp/log:/opt/mskdsp/log"}
        actual = {binding.removesuffix(":rw") for binding in host.get("Binds", [])}
        if (actual != expected or host.get("NetworkMode") != "host" or not host.get("Privileged")
                or host.get("RestartPolicy", {}).get("Name") != "unless-stopped"
                or host.get("LogConfig", {}).get("Type") != "none"
                or config.get("Entrypoint") != ["./MskDSP"] or config.get("Cmd") not in (None, [])
                or config.get("WorkingDir") != "/opt/mskdsp" or config.get("User", "") not in ("", "root", "0")
                or not data.get("State", {}).get("Running")):
            raise ValueError("当前容器不是可可靠恢复的默认部署，拒绝更新")
        image = json.loads(self.command("docker", "image", "inspect", data["Image"]))[0]
        if config.get("Env") != image["Config"].get("Env"):
            raise ValueError("容器具有自定义环境变量，拒绝无法精确恢复的更新")
        for key in ("Devices", "DeviceRequests", "CapAdd", "CapDrop", "SecurityOpt", "Dns", "DnsOptions", "DnsSearch", "ExtraHosts", "VolumesFrom", "Links", "Tmpfs", "PortBindings", "Mounts", "GroupAdd", "Ulimits", "Sysctls", "PidMode", "UTSMode", "DeviceCgroupRules", "StorageOpt", "CgroupParent", "VolumeDriver", "ContainerIDFile"):
            if host.get(key):
                raise ValueError(f"容器具有不支持的自定义配置: {key}")
        if any(host.get(key) for key in ("ReadonlyRootfs", "Init", "Memory", "NanoCpus", "CpuQuota", "PidsLimit")):
            raise ValueError("容器具有不支持的资源或运行限制")
        if host.get("IpcMode", "private") != "private" or host.get("Runtime", "runc") != "runc":
            raise ValueError("容器具有不支持的 IPC 或运行时配置")
        if any(config.get(key) for key in ("Tty", "OpenStdin", "StdinOnce", "Domainname", "StopTimeout")):
            raise ValueError("容器具有不支持的交互或停止配置")
        for key in ("Healthcheck", "StopSignal", "Labels", "Volumes", "ExposedPorts", "Shell"):
            if (config.get(key) or None) != (image["Config"].get(key) or None):
                raise ValueError(f"容器具有不支持的自定义镜像配置: {key}")
        if config.get("Hostname") and config.get("Hostname") != data.get("Id", "")[:12]:
            raise ValueError("容器具有不支持的自定义主机名")

    def module_sockets(self, snapshot):
        root = Path("/proc") / str(snapshot["State"]["Pid"]) / "root/opt/mskdsp/socket"
        modules = []
        if not root.is_dir():
            raise ValueError("无法枚举原运行模块的本地 socket")
        for path in root.glob("*.sock"):
            if self.connect_unix(path):
                modules.append(path.stem)
        return sorted(modules)

    @staticmethod
    def connect_unix(path):
        try:
            with socket.socket(socket.AF_UNIX) as connection:
                connection.settimeout(1)
                connection.connect(str(path))
            return True
        except OSError:
            return False

    def health(self, expected, modules, timeout=90):
        deadline = time.monotonic() + timeout
        error = "本地服务未就绪"
        while time.monotonic() < deadline:
            try:
                data = self.inspect()
                if not data["State"]["Running"] or not self.image_matches(data["Image"], expected):
                    raise ValueError("容器未运行或镜像身份不一致")
                with socket.create_connection(("127.0.0.1", 17000), timeout=2):
                    pass
                root = Path("/proc") / str(data["State"]["Pid"]) / "root/opt/mskdsp/socket"
                if not all(self.connect_unix(root / (name + ".sock")) for name in modules):
                    raise ValueError("原已运行模块的本地 socket 尚未就绪")
                # 返回已核验身份；containerd 原始 manifest ID 与配置 ID 可不同。
                self.last_runtime_image = data["Image"]
                return expected
            except (RuntimeError, ValueError, OSError, KeyError) as failure:
                error = str(failure)
                time.sleep(2)
        raise RuntimeError("本地健康检查超时（容器、镜像、17000、原模块 socket）: " + error)

    def pin_image(self, image, tag):
        self.command("docker", "tag", image, tag)

    def remove_pin(self, tag):
        self.command("docker", "image", "rm", tag)

    def image_matches(self, actual, expected):
        if actual == expected:
            return True
        # Docker containerd 可能返回 manifest 摘要，需流式校验镜像配置摘要。
        process = subprocess.Popen(["docker", "image", "save", actual], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        timer = threading.Timer(60, process.kill)
        timer.start()
        matched = False
        try:
            with tarfile.open(fileobj=process.stdout, mode="r|*") as archive:
                for member in archive:
                    if member.isfile() and member.size <= 1024**2 and re.fullmatch(r"(?:blobs/sha256/)?" + re.escape(expected[7:]) + r"(?:\.json)?", member.name):
                        content = archive.extractfile(member).read()
                        matched = "sha256:" + hashlib.sha256(content).hexdigest() == expected
            # 消费 tar 尾部，避免 Docker 因管道写满而无法退出。
            for chunk in iter(lambda: process.stdout.read(1024**2), b""):
                pass
            return process.wait(timeout=10) == 0 and matched
        finally:
            timer.cancel()
            process.stdout.close()
            if process.poll() is None:
                process.kill()
            process.wait()

    def cleanup_image(self, image, keep):
        if not image or image in keep:
            return
        try:
            data = json.loads(self.command("docker", "image", "inspect", image))[0]
            for tag in data.get("RepoTags") or []:
                if tag.startswith("mskdsp:") or tag.startswith("mskdsp-rollback:"):
                    self.command("docker", "image", "rm", tag)
            # 不强制删除仍被其他容器或其他业务标签引用的镜像。
            self.command("docker", "image", "rm", image)
        except RuntimeError:
            pass

    def install(self, package, log):
        environment = {"PATH": "/usr/sbin:/usr/bin:/sbin:/bin", "HOME": "/root", "LANG": "C.UTF-8",
                       "MSKDSP_MANAGED_UPDATE": "1"}
        with open(log, "ab") as output:
            process = subprocess.Popen(["/bin/bash", str(package), "start"], stdout=output, stderr=subprocess.STDOUT,
                                       env=environment, start_new_session=True)
            try:
                code = process.wait(timeout=1800)
            except subprocess.TimeoutExpired:
                # 先终止整个安装进程组，避免残留 docker cp 与回退同时写目录。
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                raise RuntimeError("安装命令超过 30 分钟，安装进程组已终止") from None
        if code:
            raise RuntimeError(f"安装命令失败，退出码 {code}")

    def restore(self, snapshot, log):
        try:
            self.command("docker", "rm", "-f", "mskdsp")
        except RuntimeError:
            pass
        self.command("docker", "run", "-d", "--name", "mskdsp", "--privileged", "--restart", "unless-stopped",
                     "--log-driver", "none", "--network", "host", "-v", "/dev:/dev",
                     "-v", "/data/mskdsp/conf:/opt/mskdsp/conf", "-v", "/data/mskdsp/module:/opt/mskdsp/module",
                     "-v", "/data/mskdsp/log:/opt/mskdsp/log", snapshot["Image"])


class TaskManager:
    def __init__(self, root=STATE_ROOT, host=HOST_ROOT, backend=None, secure=True):
        self.root, self.host, self.backend, self.secure = Path(root), Path(host), backend or Backend(), secure
        self.root.mkdir(mode=0o700, parents=True, exist_ok=True)
        if secure:
            ensure_secure(self.root)

    def state(self, operation):
        return json.loads((self.root / operation_id(operation) / "state.json").read_text(encoding="utf-8"))

    def save(self, data, phase, message, **fields):
        data.update(phase=phase, message=message, updated_at=int(time.time()), **fields)
        atomic_json(self.root / data["operation_id"] / "state.json", data)
        self.log(data["operation_id"], message)

    def log(self, operation, message):
        with open(self.root / operation / "task.log", "a", encoding="utf-8") as stream:
            stream.write(time.strftime("%Y-%m-%d %H:%M:%S ") + message + "\n")

    def query(self, operation):
        try:
            data = self.state(operation)
        except FileNotFoundError:
            raise ValueError("LOWER_UPDATE_TASK_NOT_FOUND: 未找到原更新任务") from None
        return self.render(data)

    def reconcile(self, operation):
        with exclusive(self.root / "admission.lock"):
            data = self.state(operation)
            if data["phase"] == "accepted" and not self.backend.scheduled(operation):
                self.backend.schedule(operation, self.root / operation / "helper.py")
            elif data["phase"] not in TERMINAL and data["phase"] != "accepted" and not self.backend.scheduled(operation):
                # 非 accepted 状态可能已替换业务，严禁自动重装同一个任务。
                phase = "failed" if data["phase"] == "preparing" else "rollback_failed"
                self.save(data, phase, "宿主机任务异常中断，请人工核查；恢复材料已保留，未自动重装", exit_code=2)
            return self.render(data)

    def render(self, data):
        operation = data["operation_id"]
        log = self.root / operation / "task.log"
        tail = ""
        if log.exists():
            with open(log, "rb") as stream:
                stream.seek(max(0, log.stat().st_size - 8192))
                tail = stream.read(8192).decode("utf-8", errors="replace")
        return {key: data.get(key) for key in ("operation_id", "phase", "message", "expected_image_id", "actual_image_id", "exit_code")} | {"log_tail": tail}

    def submit(self, operation, package, digest, expected):
        operation_id(operation)
        if not re.fullmatch("[a-f0-9]{64}", digest) or not re.fullmatch("sha256:[a-f0-9]{64}", expected):
            raise ValueError("安装包摘要或镜像身份格式错误")
        with exclusive(self.root / "admission.lock"):
            directory = self.root / operation
            if (directory / "state.json").exists():
                data = self.state(operation)
                if data["package_sha256"] != digest or data["expected_image_id"] != expected:
                    raise ValueError("相同任务编号的内容不同，拒绝替换")
            else:
                active = self.root / "active.json"
                if active.exists():
                    active_id = json.loads(active.read_text())["operation_id"]
                    active_phase = self.state(active_id)["phase"]
                    if active_phase not in TERMINAL or active_phase == "rollback_failed":
                        raise ValueError("LOWER_UPDATE_TASK_REJECTED: 设备已有更新任务 " + active_id + "，不能并行安装；恢复失败需人工处理")
                if directory.exists():
                    # 上次复制中断尚未受理；该 root 独占目录可安全重建。
                    shutil.rmtree(directory)
                directory.mkdir(mode=0o700)
                try:
                    copy_regular(Path(package), directory / "package", digest)
                    copy_regular(Path(__file__), directory / "helper.py")
                    data = {"operation_id": operation, "package_sha256": digest, "expected_image_id": expected,
                            "actual_image_id": None, "exit_code": None}
                    self.save(data, "accepted", "任务已持久受理，等待宿主机独立执行")
                except BaseException:
                    shutil.rmtree(directory)
                    raise
            if data["phase"] not in TERMINAL:
                atomic_json(self.root / "active.json", {"operation_id": operation})
            if data["phase"] == "accepted" and not self.backend.scheduled(operation):
                self.backend.schedule(operation, directory / "helper.py")
            return self.query(operation)

    def run(self, operation):
        with exclusive(self.root / "execution.lock"):
            data = self.state(operation)
            if data["phase"] in TERMINAL:
                return self.query(operation)
            if data["phase"] != "accepted":
                raise ValueError("任务曾中断，需要人工核查恢复材料，不能重复安装")
            directory = self.root / operation
            backup = directory / "backup"
            snapshot = None
            changed = False
            committed = False
            old = None
            try:
                self.save(data, "preparing", "检查包身份、架构、部署及本地恢复条件")
                package = directory / "package"
                with open(package, "rb") as package_stream:
                    package_hash = hashlib.sha256()
                    for chunk in iter(lambda: package_stream.read(1024**2), b""):
                        package_hash.update(chunk)
                    package_digest = package_hash.hexdigest()
                if package_digest != data["package_sha256"]:
                    raise ValueError("root 暂存安装包摘要发生变化")
                if self.secure:
                    ensure_secure(package)
                    ensure_secure(self.host)
                snapshot = self.backend.preflight(package, data["expected_image_id"])
                required = package.stat().st_size * 3 + snapshot.get("update_payload_bytes", 0) * 2
                for name in ("module", "conf"):
                    required += sum(item.stat().st_size for item in (self.host / name).rglob("*") if item.is_file())
                if shutil.disk_usage(self.root).free < required + 256 * 1024**2:
                    raise ValueError("磁盘空间不足以同时保留当前、先前和候选版本")
                if shutil.disk_usage(self.host).free < snapshot.get("update_payload_bytes", 0) + 256 * 1024**2:
                    raise ValueError("宿主机模块目录空间不足以展开候选版本")
                backup.mkdir(mode=0o700)
                for name in ("module", "conf"):
                    copy_tree(self.host / name, backup / name, self.secure,
                              self.host / "module" if name == "module" else None)
                atomic_json(backup / "deployment.json", snapshot)
                tag = "mskdsp-rollback:" + operation
                self.backend.pin_image(snapshot["Image"], tag)
                self.save(data, "installing", "恢复材料已保存，正在执行应用安装")
                changed = True
                self.backend.install(package, directory / "task.log")
                self.save(data, "verifying", "核对新版本容器、镜像、管理端口和原模块本地 socket")
                actual = self.backend.health(data["expected_image_id"], snapshot["modules"])
                runtime_image = getattr(self.backend, "last_runtime_image", actual)
                self.log(operation, "本地镜像校验通过，运行时摘要 " + runtime_image + "，核验摘要 " + actual)
                previous = self.root / "previous.json"
                old = json.loads(previous.read_text()) if previous.exists() else None
                atomic_json(previous, {"operation_id": operation, "image_id": snapshot["Image"], "tag": tag})
                committed = True
                self.save(data, "succeeded", "更新成功；已保留一个先前版本，本地检查不包含平台在线状态", actual_image_id=actual, exit_code=0)
            except Exception as failure:
                if committed:
                    # 新版本和 previous 指针已经提交，状态/日志落盘问题不能再回退。
                    self.save(data, "succeeded", "更新已通过检查，保存最终结果时出现警告: " + str(failure), actual_image_id=actual, exit_code=0)
                elif not changed:
                    self.save(data, "failed", "更新预检失败，未替换业务: " + str(failure), exit_code=1)
                else:
                    self.save(data, "rolling_back", "更新失败，宿主机正在自动恢复: " + str(failure), exit_code=1)
                    try:
                        # 先停止候选容器，防止恢复配置时仍有进程写入。
                        if isinstance(self.backend, Backend):
                            try:
                                self.backend.command("docker", "rm", "-f", "mskdsp")
                            except RuntimeError:
                                pass
                        for name in ("module", "conf"):
                            target = self.host / name
                            if self.secure:
                                ensure_secure(target)
                            if target.exists():
                                shutil.rmtree(target)
                            copy_tree(backup / name, target, self.secure,
                                      backup / "module" if name == "module" else None)
                        self.backend.restore(snapshot, directory / "task.log")
                        actual = self.backend.health(snapshot["Image"], snapshot["modules"])
                        self.save(data, "rolled_back", "更新失败，已自动恢复原镜像、模块和配置并通过本地检查", actual_image_id=actual, exit_code=1)
                    except Exception as rollback:
                        self.save(data, "rollback_failed", "更新和自动恢复均失败，恢复材料已保留，需要人工处理: " + str(rollback), exit_code=2)
            # 非关键历史清理在终态之后执行；失败只留日志，不改变安装/恢复结果。
            try:
                self.cleanup_terminal(data, snapshot, old)
            except Exception as warning:
                self.log(operation, "非关键历史清理失败，稍后可人工清理: " + str(warning))
            return self.query(operation)

    def cleanup_terminal(self, data, snapshot, old):
        operation = data["operation_id"]
        directory = self.root / operation
        backup = directory / "backup"
        if data["phase"] == "succeeded" and old and old["operation_id"] != operation:
            old_dir = self.root / operation_id(old["operation_id"])
            if (old_dir / "backup").exists():
                shutil.rmtree(old_dir / "backup")
            self.backend.remove_pin(old["tag"])
            if isinstance(self.backend, Backend):
                self.backend.cleanup_image(old["image_id"], {data["actual_image_id"], snapshot["Image"]})
            (old_dir / "package").unlink(missing_ok=True)
        elif data["phase"] == "rolled_back":
            # 已恢复的快照与当前版本相同，无需占用第二份先前版本。
            if backup.exists():
                shutil.rmtree(backup)
            self.backend.remove_pin("mskdsp-rollback:" + operation)
            if isinstance(self.backend, Backend):
                keep = {snapshot["Image"]}
                previous = self.root / "previous.json"
                if previous.exists():
                    keep.add(json.loads(previous.read_text())["image_id"])
                # Docker 的 manifest/config 摘要可能不同，以实际候选标签定位镜像。
                candidate = json.loads(self.backend.command("docker", "image", "inspect", snapshot["update_candidate_tag"]))[0]["Id"]
                self.backend.cleanup_image(candidate, keep)
        elif data["phase"] == "failed" and backup.exists():
            shutil.rmtree(backup)
        if data["phase"] in {"succeeded", "rolled_back", "failed"}:
            (directory / "package").unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description="宿主机独立应用更新任务")
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("submit", "query", "run"):
        sub = commands.add_parser(name)
        sub.add_argument("--operation-id", required=True)
        if name == "submit":
            sub.add_argument("--package-path", required=True)
            sub.add_argument("--package-sha256", required=True)
            sub.add_argument("--expected-image-id", required=True)
    args = parser.parse_args()
    try:
        if sys.version_info < (3, 9):
            raise ValueError("独立更新任务需要 Python 3.9 或更高版本")
        if not hasattr(os, "geteuid") or os.geteuid() != 0:
            raise ValueError("独立更新任务必须由 root 执行")
        os.umask(0o077)
        os.environ["PATH"] = "/usr/sbin:/usr/bin:/sbin:/bin"
        ensure_secure(Path(__file__).absolute())
        manager = TaskManager()
        if args.command == "submit":
            result = manager.submit(args.operation_id, Path(args.package_path), args.package_sha256, args.expected_image_id)
        elif args.command == "query":
            manager.query(args.operation_id)
            result = manager.reconcile(args.operation_id)
        else:
            result = manager.run(args.operation_id)
        print(json.dumps(result, ensure_ascii=False))
    except Exception as error:
        print("更新任务操作失败: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
