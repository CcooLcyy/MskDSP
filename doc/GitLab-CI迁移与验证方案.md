# 下位机 GitHub Workflow 迁移到 GitLab CI

## 目标与边界

在仓库根目录新增 `.gitlab-ci.yml`，迁移 `.github/workflows/ci.yml`、`build-lower.yml`、`beta.yml`、`release.yml` 的触发、构建、测试、打包与渠道发布行为。复用现有 CMake/vcpkg、IEC61850 包检查、自解压包、更新清单及 R2 上传脚本，不修改业务源码。

GitHub 工作流继续保留。本次先验证 GitLab 编译和打包；`MSKDSP_PUBLISH_ENABLED` 默认 `false`，发布任务在这个状态直接退出，不读取发布凭据、不上传 R2、不创建或清理 Release。正式切换前必须确定唯一的发布端，避免 GitHub/GitLab 同时更新相同渠道并清理彼此的产物。

## 流程与输入输出

| 事件 | 构建与测试 | 安装包 | 更新渠道与 Release |
| --- | --- | --- | --- |
| Merge Request | x64、ARM64 CI | 无 | 无 |
| push `master` / `main` | x64、ARM64 CI | 两个架构 | 启用发布后分别发布 x64 CI、ARM64 CI；无 Release 页面 |
| push `beta/**` | x64、ARM64 CI，另加 ARM64 Beta | 只有 Beta 安装包 | 启用发布后更新 ARM64 Beta；仅清理相同 Beta 线的旧预发布 |
| push `v*` tag | ARM64 Stable | Stable 安装包 | 启用发布后更新 ARM64 Stable；创建或更新该 tag 的 Release |
| web，`MSKDSP_PIPELINE_MODE=ci` | x64、ARM64 CI | 无 | 无；这是新增手动验证入口 |
| web，`MSKDSP_PIPELINE_MODE=beta` | ARM64 Beta | Beta 安装包 | `BETA_REF` 指定 `beta/**` 分支，可从当前 Beta 分支推导 |
| web，`MSKDSP_PIPELINE_MODE=stable` | ARM64 Stable | Stable 安装包 | `RELEASE_TAG` 指定已有 `v*` tag，可从当前 tag 推导 |
| 其他分支 push、schedule、API 等未列事件 | 不创建本方案流水线 | 无 | 无 |

流水线依次包含 `build`、`package`、`publish` 三个阶段。每个打包任务只下载相同渠道、相同架构构建任务的产物；每个发布任务只下载对应打包任务的产物。两个架构构建任务没有彼此 `needs` 依赖，可并行调度。

构建使用 GCC 14、Ninja、`RelWithDebInfo`、vcpkg manifest baseline、对应动态 triplet，以及 `MSKDSP_BUILD_TESTS=ON`。CTest 保持 `--output-on-failure --parallel 2`，防止存量时间敏感测试在高并发时随机超时。ARM64 剥离调试信息并生成独立符号包，x64 保留完整调试信息。构建阶段导出测试报告和供打包使用的安装树；打包阶段执行 IEC61850 校验，生成 Docker 镜像、自解压安装包、SHA256SUMS、latest.json 及可选调试符号包。

缓存按架构分离 vcpkg 二进制缓存与 ccache，并在 vcpkg 缓存键中分离 CI/Beta 渠道。自动 Beta 按触发分支 slug 分离缓存；从主分支页面手动指定 `BETA_REF` 时，GitLab 在运行脚本前已选择缓存，所以使用 `beta-master` 等触发页面分支作用域，多条目标 Beta 线可能共享此缓存。vcpkg 自身的 ABI 校验仍负责依赖匹配；此缓存不会回写 CI 主线。Stable 只读取 `ci-master` 主线 vcpkg 缓存，不改写正式版本依赖缓存；未来切到 `main` 主线应同时调整 `VCPKG_CACHE_REF`。编译缓存可以继续累积，不能缓存凭据或运行时配置。GitLab 缓存后端不同，不直接继承 GitHub 缓存内容。

R2 前缀保持现有值：ARM64 CI 为 `mskdsp-lower/ci`，x64 CI 为 `mskdsp-lower-x64/ci`，Beta 为 `mskdsp-lower/beta`，Stable 为 `mskdsp-lower/stable`。上传顺序继续复用 `upload_r2.sh`：先安装包/校验文件/符号，再 latest.json，最后清理同渠道同平台的旧对象。

## Runner 与外部配置

- x64 编译 Runner 标签：`linux`、`amd64`、`docker`、`ci`；使用 x64 构建镜像。
- ARM64 编译 Runner 标签：`linux`、`arm64-qemu`、`docker`、`ci`；使用 ARM64 镜像，当前服务器通过 QEMU 仿真执行，不能当成真 ARM 硬件验证。
- 打包任务使用独立 x64 Runner：标签 `linux`、`amd64`、`docker-package`、`mskdsp`。该 Runner 需要 Docker CLI/Buildx 与 Docker daemon 访问；保持非 privileged。Docker socket 仅提供给此专用 Runner，不改现有编译 Runner。
- 私有 protobuf 镜像必须保留主仓库固定的子模块 commit。在 `mskdsp/mskdspproto` 项目的 CI job token 授权中加入 `mskdsp/mskdsp`。构建脚本将 GitLab 镜像用于拉取，不改 `.gitmodules` 中现有 GitHub 地址。
- 启用发布后需要 `R2_ACCOUNT_ID`、`R2_ACCESS_KEY_ID`、`R2_SECRET_ACCESS_KEY`，并可覆盖默认 `R2_BUCKET`、`R2_PUBLIC_BASE_URL`。凭据只放 GitLab CI/CD 变量，使用 masked/protected 配置；不要写入仓库或构建产物。
- Release 页面迁移到当前 GitLab 项目，附件上传到 Generic Package Registry，避免 7 天后 job artifact 过期导致 Release 附件失效。需要 `GITLAB_RELEASE_TOKEN` 具有 `api` scope；Beta 清理旧 tag 至少需要项目 Maintainer 权限，Stable 至少需要 Developer 权限。项目必须启用 Package Registry。CI 渠道只发布 R2，不需要 Release token。
- 正式启用发布前应保护主分支、`beta/*` 分支、`v*` tag；打包 Runner 应限定为本项目可用，防止无关项目获得 Docker daemon 访问。

## 异常与兼容边界

错误目标分支/tag、缺少工具、protobuf 固定 commit 不可获取、编译/单测/包校验失败均必须失败退出，禁止继续发布。手动构建目标由显式输入决定，元数据记录实际 checkout 的 commit，不使用触发页面默认分支替代目标分支。

Stable 现有 `.github/workflows/release.yml` 只按 `v*` tag 构建，没有检查该 commit 是否位于远端 `beta/*` 分支。旧发布文档如果写有 Beta 血缘门禁，不能据此宣称当前下位机工作流已经实现；本次按现有源码行为迁移，不额外增加该门禁。

GitLab Release 没有 GitHub `isPrerelease` 的对应字段。本方案用 `beta-<版本线>-` tag 前缀、`Beta ` 标题及页面说明标记候选发布；清理必须同时满足当前版本线 tag 前缀和 Beta 标题，排除当前 tag。新 Release 成功后才删除同线旧页面/标签；遇到受保护旧 Beta tag 时在 R2 写入前失败。正式 tag 始终保留；已有正式 Release 只覆盖附件链接，保留原有发布说明。发布说明从最近可用 `v*` 基线到实际构建 commit 的 Git 提交生成，表达累计变更范围；GitHub 自动发布说明中的 PR/贡献者展示不能原样迁移。

## 验收与验证

1. 本地契约测试覆盖 MR、主分支、Beta 分支、正式 tag、三个 web 模式和不支持事件，直接读取实际 YAML 判断创建的任务，验证 `needs` 产物链和架构隔离。
2. 对发布脚本使用 fake HTTP/子进程测试：默认发布关闭时没有外部动作；Beta 清理不触及其他版本线或 Stable tag；CI 不创建 Release。测试不接触 R2。
3. 对新增 Bash 脚本运行语法检查，对新增 Python 脚本运行语法/单元测试，执行 `git diff --check`。
4. GitLab CI Lint 检查语法和展开的 jobs。语法通过不能证明 Runner 已经构建成功。
5. 远端首次运行保持 `MSKDSP_PUBLISH_ENABLED=false`，先验证 x64，再验证 ARM64/QEMU 和两个安装包。记录流水线 URL、commit、成功任务、实际 CTest 用例数、产物名称和校验结果。
6. 发布切换在构建/安装包证据齐备后进行，先确认唯一发布端、Release 后端和凭据，再启用发布。首次 R2/Release 写入验证单独记录，不包含在纯本地测试结论里。

本方案不会部署或修改真机配置。尚未跑过的 GitLab 任务、远端编译、设备安装和真实更新均不能以本地契约测试替代。

## 本次验证记录

- 已在 Windows 本地运行 `python -B test/gitlab_workflow_test.py`：23 条测试通过，覆盖实际 YAML 的任务选择/依赖/缓存/CTest 契约和 fake API 发布边界。发布测试未连接 GitLab/R2。
- 已执行 `git diff --check`，无空白错误。
- 主 agent 已执行 GitLab 官方 CI Lint：`valid=true`，无 warning；已配置默认 CI 文件路径、24 小时 job timeout、protobuf job token 授权、项目变量 `MSKDSP_PUBLISH_ENABLED=false`，专属 Docker 打包 Runner 5 已 online。
- Runner 实际构建、ARM64/QEMU 单测和真实安装包验证结果待补充。没有实际成功流水线和安装包证据前，结论应为“迁移配置已完成并通过本地检查”，不能写“GitLab 已全部跑通”。
