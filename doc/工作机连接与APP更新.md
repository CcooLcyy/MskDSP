# 工作机连接与 APP 更新

## 1. 文档目的

本文档说明 Codex 在需要连接工作机并更新 AGVC-APP 时的操作约定。

工作机连接只用于联调、排查和更新操作。按当前约定，连接密码只记录在两处：本文件第 2 节的表格，以及工作区根 `AGENTS.md` 的「下位机设备（真机，192.168.1.219）」一节。除此之外不得写进普通日志、命令行参数、截图或对外分享的文档；对外展示时统一写为 `[已隐藏]`。

## 2. 当前工作机连接信息

| 字段 | 值 |
| --- | --- |
| `ip` | `192.168.1.219` |
| `port` | `10022` |
| `user` | `megsky` |
| `passwd` | `Meg@admin123` |
| 提权 | `megsky` 属于 `sudo` 组，`sudo` 需要密码（与 SSH 密码相同）；不在 `docker` 组，`docker` 命令需要提权 |

说明：

- 仓库内统一用 `script/dev/lower_device_ssh.py` 连接，用法见第 3 节；它把密码从 `--password` 或环境变量 `SSH_DEVICE_PASSWORD` 读入，脚本内不含凭据。
- Codex 环境内原有的 `$ssh-device-debug` 技能（`ssh_device.py`）仍可用，但必须使用上表的 `port`/`user`，不要再用历史值 `22`/`root`。
- 密码只在上述两处记录；传给脚本时用环境变量或标准输入，不要拼进命令行参数。

## 3. 连接验证

统一使用仓库内脚本 `script/dev/lower_device_ssh.py`（依赖 Python 3 与 `paramiko`，Windows 侧已验证 `paramiko` 3.5.1）：

```bash
export SSH_DEVICE_PASSWORD='<第 2 节的密码>'      # 仅当前 shell，不要写进脚本或仓库文件

python script/dev/lower_device_ssh.py check                        # 连通自检：主机名/架构/用户/组
python script/dev/lower_device_ssh.py run  "ls -1 /data/mskdsp"    # 单行远端命令
python script/dev/lower_device_ssh.py bash ./remote_check.sh       # 多行脚本（base64 传输）
python script/dev/lower_device_ssh.py sudo "docker ps"             # 需要 root 的读取（docker 属于提权操作）
```

三种方式各自的要点：

- `check` / `run`：直接走 `paramiko` 密码认证；Windows 侧不要用 `ssh` 命令，它无法免交互传密码。
- `bash`：多行脚本一律 base64 传输，并在脚本内把 CRLF 归一化为 LF，规避 Windows 换行 `\r` 污染 `fi`/`done`。
- `sudo`：用 `sudo -S -p ''`，密码经 SSH 通道标准输入发送，不进入命令行参数。

连接验证成功后，可以继续执行上传安装包、查看日志和运行远端命令等后续操作。

> 上传较大安装包时不走本脚本（它只覆盖上面的三类操作）；用 `$ssh-device-debug` 的 `deploy` 或 `scp -P 10022` 上传，注意目标是 `megsky` 的家目录 `/home/megsky`。

## 4. 从平台软件库获取安装包

GitLab CI 完成双架构编译、x64 完整测试及打包后，自动上传平台软件库并发布到 CI 通道。登录平台的[软件版本页面](http://192.168.10.198:8082/software)，选择「下位机 / CI」及 `linux-arm64` 即可下载安装包和 `SHA256SUMS`。

命令行也可通过平台公开清单下载，并校验实际文件：

```bash
PLATFORM_BASE_URL=http://192.168.10.198:8082
CHANNEL=ci
DOWNLOAD_DIR="$(mktemp -d /tmp/mskdsp-ci.XXXXXX)"

curl --fail --location "${PLATFORM_BASE_URL}/mskdsp-lower/${CHANNEL}/latest.json" \
  --output "${DOWNLOAD_DIR}/latest.json"

APP_NAME="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["asset"]["name"])' "${DOWNLOAD_DIR}/latest.json")"
APP_URL="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["asset"]["url"])' "${DOWNLOAD_DIR}/latest.json")"
SHA_URL="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["checksum"]["url"])' "${DOWNLOAD_DIR}/latest.json")"
APP_PACKAGE="${DOWNLOAD_DIR}/${APP_NAME}"
SHA_FILE="${DOWNLOAD_DIR}/SHA256SUMS"

curl --fail --location "${APP_URL}" --output "${APP_PACKAGE}"
curl --fail --location "${SHA_URL}" --output "${SHA_FILE}"
(cd "${DOWNLOAD_DIR}" && sha256sum --check SHA256SUMS)
```

文件名由平台清单提供，包含版本、GitLab 流水线及作业身份、提交短哈希和目标架构。

Beta/Stable 由平台将已经通过 CI 的 ARM64 安装包晋升产生，不重复构建。需要这些通道时将 `CHANNEL` 改为 `beta` 或 `stable`；具体构建发布规则见 [GitLab CI 接入方案](./GitLab-CI接入方案.md)。设备安装仍需按后文单独执行，下载不会自动升级设备。

## 5. 上传安装包到工作机

安装包需要落到目标宿主机 `megsky` 用户家目录：

```text
/home/megsky/
```

使用连接脚本上传安装包：

```bash
python /data/code/skills/skills/ssh-device-debug/scripts/ssh_device.py deploy \
  --host 192.168.1.219 \
  --port 10022 \
  --user megsky \
  --auth-mode password \
  --src "${APP_PACKAGE}" \
  --dest "/home/megsky/$(basename "${APP_PACKAGE}")" \
  --json
```

如需要同时保留校验文件，可继续上传 `SHA256SUMS`：

```bash
python /data/code/skills/skills/ssh-device-debug/scripts/ssh_device.py deploy \
  --host 192.168.1.219 \
  --port 10022 \
  --user megsky \
  --auth-mode password \
  --src "${SHA_FILE}" \
  --dest /home/megsky/SHA256SUMS \
  --json
```

上传操作同样通过 `SSH_DEVICE_PASSWORD` 获取密码，不在命令行中展示密码。

## 6. 安装更新 AGVC-APP

上传完成后，在工作机宿主机 `/home/megsky` 目录执行安装包的 `start` 动作。`start` 内部要操作 Docker，
而 `megsky` 不在 `docker` 组，因此**必须提权**：

```bash
cd /home/megsky
chmod +x ./mskdsp-<version>
echo "$SSH_DEVICE_PASSWORD" | sudo -S -p '' ./mskdsp-<version> start
```

也可以直接用仓库脚本执行（密码经 SSH 通道标准输入发送，不进入命令行参数）：

```bash
APP_BASENAME="$(basename "${APP_PACKAGE}")"

python script/dev/lower_device_ssh.py sudo \
  "cd /home/megsky && chmod +x ./${APP_BASENAME} && ./${APP_BASENAME} start"
```

`start` 会更新宿主机侧运行文件，停止旧的 AGVC-APP 运行实例，并启动新的 AGVC-APP 运行实例。现场配置目录 `/data/mskdsp/conf/` 已存在时不会被默认覆盖。

安装包的详细行为见 [AGVC-APP 管理文档](./AGVC-APP管理文档.md)。

## 7. 更新后检查

更新后优先检查 AGVC-APP 容器状态（`docker` 需要提权）：

```bash
python script/dev/lower_device_ssh.py sudo "docker ps --filter name=mskdsp --format '{{.Names}} {{.Image}}'"
```

如需要查看运行日志（日志文件对普通用户可读，无需提权）：

```bash
python script/dev/lower_device_ssh.py run "ls -lah /data/mskdsp/log; tail -n 200 /data/mskdsp/log/moduleManager/moduleManager.log"
```

如果容器未运行或日志显示异常，应保留当前安装包文件、`/data/mskdsp/conf/`、`/data/mskdsp/log/`，再继续定位。
