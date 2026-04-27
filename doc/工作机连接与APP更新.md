# 工作机连接与 APP 更新

## 1. 文档目的

本文档说明 Codex 在需要连接工作机并更新 AGVC-APP 时的操作约定。

工作机连接只用于当前会话内的联调、排查和更新操作。连接密码不得写入仓库文件、普通日志或命令行参数；展示时统一写为 `[已隐藏]`。

## 2. 当前工作机连接信息

| 字段 | 值 |
| --- | --- |
| `ip` | `192.168.1.219` |
| `port` | `22` |
| `user` | `root` |
| `passwd` | `[已隐藏]` |

说明：

- 连接工作机时使用 `$ssh-device-debug`。
- 该技能按 `ip`、`port`、`user`、`passwd` 的顺序收集连接信息。
- 如果未显式提供 `port`，默认使用 `22`。
- 密码只通过 `SSH_DEVICE_PASSWORD` 环境变量或标准输入传递给连接脚本，不写入文档。

## 3. 连接验证

在当前 Codex 环境中，使用以下脚本验证 SSH 连接：

```bash
python /data/code/skills/skills/ssh-device-debug/scripts/ssh_device.py check \
  --host 192.168.1.219 \
  --port 22 \
  --user root \
  --auth-mode password \
  --connect-timeout 8 \
  --json
```

执行前需要在当前 shell 中设置 `SSH_DEVICE_PASSWORD`，不要把密码拼进命令行。

如果本机缺少依赖，先安装：

```bash
python -m pip install -r /data/code/skills/skills/ssh-device-debug/requirements.txt
```

连接验证成功后，Codex 可以在当前会话中继续执行上传安装包、查看日志和运行远端命令等后续操作。

## 4. 获取最新 CI 安装包

默认从 GitHub Actions `ci.yml` 最新成功的 `master/main` push 构建中获取 arm64 测试安装包。

CI 安装包命名格式为：

```text
mskdsp-<VERSION>-<branch>-ci-<YYYYMMDD>-<sha>-linux-arm64
```

在已安装并登录 `gh` 的环境中，可按以下方式下载最新成功包：

```bash
CI_BRANCH=master
DOWNLOAD_DIR=/tmp/mskdsp-ci-latest

rm -rf "${DOWNLOAD_DIR}"
mkdir -p "${DOWNLOAD_DIR}"

RUN_ID="$(gh run list \
  --workflow ci.yml \
  --branch "${CI_BRANCH}" \
  --event push \
  --status success \
  --limit 1 \
  --json databaseId \
  --jq '.[0].databaseId')"

gh run download "${RUN_ID}" \
  --pattern 'mskdsp-*-linux-arm64' \
  --dir "${DOWNLOAD_DIR}"

APP_PACKAGE="$(find "${DOWNLOAD_DIR}" -type f -path '*/images/mskdsp-*-linux-arm64' | sort | tail -n 1)"
SHA_FILE="$(find "${DOWNLOAD_DIR}" -type f -name SHA256SUMS | sort | tail -n 1)"

test -n "${APP_PACKAGE}"
test -n "${SHA_FILE}"
```

如主线分支为 `main`，将 `CI_BRANCH=master` 改为 `CI_BRANCH=main`。

如果要更新 Beta、Nightly 或 Stable 包，应改用对应 workflow 或 GitHub Release 资产；各渠道产物规则见 [GitHub Actions 与发版策略](./GitHub-Actions与发版策略.md)。

## 5. 上传安装包到工作机

安装包需要落到目标宿主机 `root` 用户家目录：

```text
/root/
```

使用连接脚本上传安装包：

```bash
python /data/code/skills/skills/ssh-device-debug/scripts/ssh_device.py deploy \
  --host 192.168.1.219 \
  --port 22 \
  --user root \
  --auth-mode password \
  --src "${APP_PACKAGE}" \
  --dest "/root/$(basename "${APP_PACKAGE}")" \
  --json
```

如需要同时保留校验文件，可继续上传 `SHA256SUMS`：

```bash
python /data/code/skills/skills/ssh-device-debug/scripts/ssh_device.py deploy \
  --host 192.168.1.219 \
  --port 22 \
  --user root \
  --auth-mode password \
  --src "${SHA_FILE}" \
  --dest /root/SHA256SUMS \
  --json
```

上传操作同样通过 `SSH_DEVICE_PASSWORD` 获取密码，不在命令行中展示密码。

## 6. 安装更新 AGVC-APP

上传完成后，在工作机宿主机 `/root` 目录执行安装包的 `start` 动作：

```bash
cd /root
chmod +x ./mskdsp-<version>
./mskdsp-<version> start
```

也可以通过连接脚本远端执行：

```bash
APP_BASENAME="$(basename "${APP_PACKAGE}")"

python /data/code/skills/skills/ssh-device-debug/scripts/ssh_device.py run \
  --host 192.168.1.219 \
  --port 22 \
  --user root \
  --auth-mode password \
  --remote-command "cd /root && chmod +x ./${APP_BASENAME} && ./${APP_BASENAME} start" \
  --json
```

`start` 会更新宿主机侧运行文件，停止旧的 AGVC-APP 运行实例，并启动新的 AGVC-APP 运行实例。现场配置目录 `/data/mskdsp/conf/` 已存在时不会被默认覆盖。

安装包的详细行为见 [AGVC-APP 管理文档](./AGVC-APP管理文档.md)。

## 7. 更新后检查

更新后优先检查 AGVC-APP 容器状态：

```bash
docker ps --filter name=mskdsp --format '{{.Names}} {{.Image}}'
```

如需要查看运行日志：

```bash
ls -lah /data/mskdsp/log
tail -n 200 /data/mskdsp/log/RTU.log
```

如果容器未运行或日志显示异常，应保留当前安装包文件、`/data/mskdsp/conf/`、`/data/mskdsp/log/`，再继续定位。
