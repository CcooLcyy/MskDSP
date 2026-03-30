# GitHub Actions 与发版策略

## 1. 文档目的

本文档记录当前仓库已经落地的 GitHub Actions workflow、分支/渠道约定、构建产物形态与发布流程，作为后续维护 workflow、排查构建问题与安排版本发布的依据。

## 2. 当前 workflow 概览

当前仓库已落地 4 个 workflow：

- `ci.yml`
  - 触发：`pull_request`；`push` 到 `master`、`main`、`beta/**`
  - 作用：
    - 所有触发场景都执行 `x64 Debug` 编译与单元测试
    - 当 `push` 到 `master/main` 时，在 `x64 Debug` 校验通过后继续产出 `arm64` 测试安装包
- `nightly.yml`
  - 触发：每日定时 + `workflow_dispatch`
  - 来源：仓库当前 `default_branch`
  - 作用：产出 `arm64 Nightly` 自解压安装包、调试符号包与校验文件
- `beta.yml`
  - 触发：`push` 到 `beta/**`；定时；`workflow_dispatch`
  - 作用：先执行 `x64 Debug` 校验，再产出 `arm64 Beta` 自解压安装包；同时创建 GitHub 预发布页面
- `release.yml`
  - 触发：`push` tag `v*`
  - 作用：先执行发布前 `x64 Debug` 校验，再产出 `arm64` 正式安装包并创建/更新 GitHub Release

## 3. 统一实现约定

四个 workflow 的构建链路遵循以下统一约定：

- 主仓库 checkout 时不直接拉取 submodule，随后单独准备 `protobuf` 子模块访问凭据，再执行 `git submodule update --init --recursive`
- 子模块访问优先级：
  - 优先使用 `MSKDSP_PROTO_SSH_KEY`
  - 其次使用 `MSKDSP_PROTO_TOKEN`
  - 若均未配置，则回退为 HTTPS 方式拉取
- `vcpkg` 不直接使用固定仓库分支，而是读取 `vcpkg-configuration.json` 中的 baseline，再 clone/checkout 对应版本
- 所有 workflow 都启用了两层缓存：
  - `vcpkg` 二进制缓存
  - `ccache` 编译缓存
- `x64` 构建统一使用：
  - `ubuntu-24.04`
  - `gcc-14/g++-14`
  - `Ninja`
  - `MSKDSP_BUILD_TESTS=ON`
- `arm64` 打包链路统一使用交叉编译：
  - `RelWithDebInfo`
  - `aarch64-linux-gnu-gcc/g++`
  - `MSKDSP_BUILD_TESTS=OFF`
  - `MSKDSP_STRIP_DEBUG=ON`
- `arm64` 交付包的生成流程统一为：
  1. `cmake --install build-arm64` 将运行产物落到 `package/`
  2. 单独打包 `package/debug` 为调试符号包
  3. 使用 `Dockerfile` 构建 `arm64` 镜像
  4. `docker save` 导出镜像 tar
  5. 调用 `script/make_exe.sh` 生成自解压安装包
  6. 生成 `SHA256SUMS`

需要注意：

- 根目录 `Dockerfile` 默认基础镜像为 `localhost/arm64v8/ubuntu:noble`
- 在 GitHub Actions 中会先将该镜像名替换为 `arm64v8/ubuntu:noble`，再执行 `docker buildx build`

## 4. 分支与发布渠道

当前实际流程对应的渠道模型如下：

- `CI`
  - 面向开发校验
  - 由 `pull_request`、`master/main` push、`beta/**` push 触发
- `Nightly`
  - 面向默认开发主线的每日最新包
  - 由 `nightly.yml` 从仓库当前 `default_branch` 构建
- `Beta`
  - 常规功能候选包面向版本线 `beta/x.y`
  - 已发布 Stable 的 hotfix 建议按补丁版本派生 `beta/x.y.z` 维护线
  - 由 `beta.yml` 针对目标 `beta/*` 分支构建候选包
- `Stable`
  - 面向正式交付
  - 由 `release.yml` 在 `v*` tag 触发后构建

补充说明：

- `ci.yml` 为兼容历史仓库命名，同时监听 `master` 与 `main`
- `nightly.yml` 没有写死 `main`，而是跟随仓库的 `default_branch`
- `release.yml` 要求正式 tag 对应的 commit 必须来自某条 `origin/beta/*` 版本线

## 5. `ci.yml`

`ci.yml` 负责开发过程中的持续集成校验。

### 5.1 触发条件

- `pull_request`
- `push` 到 `master`
- `push` 到 `main`
- `push` 到 `beta/**`

### 5.2 当前行为

- `x64-debug-test`
  - 配置 `x64 Debug`
  - 编译全部目标
  - 执行 `ctest --test-dir build --output-on-failure --parallel "$(nproc)"`
  - 失败时上传 `CMakeCache.txt`、`CMakeConfigureLog.yaml` 与 `build/Testing`
- `arm64-master-package`
  - 仅在 `push` 到 `master/main` 时运行
  - 交叉编译 `arm64 RelWithDebInfo`
  - 生成自解压测试安装包、调试符号包与 `SHA256SUMS`
  - 通过 artifact 上传，不创建 GitHub Release

### 5.3 产物命名

- 测试安装包：`mskdsp-<VERSION>-<branch>-ci-<YYYYMMDD>-<sha>-linux-arm64`
- 调试符号包：`mskdsp-<VERSION>-<branch>-ci-<YYYYMMDD>-<sha>-debugsymbols-linux-arm64.tar.gz`

## 6. `nightly.yml`

`nightly.yml` 负责每日 Nightly 交付包。

### 6.1 触发条件

- `schedule`
  - 当前 cron：`0 18 * * *`
- `workflow_dispatch`

说明：

- GitHub Actions 的 cron 使用 UTC
- 当前配置 `0 18 * * *` 对应北京时间次日 `02:00`

### 6.2 当前行为

- checkout 仓库 `default_branch`
- 交叉编译 `arm64 RelWithDebInfo`
- 生成自解压 Nightly 安装包
- 打包独立调试符号
- 生成 `SHA256SUMS`
- 通过 artifact 上传

### 6.3 产物命名

- Nightly 安装包：`mskdsp-<VERSION>-nightly-<YYYYMMDD>-<sha>-linux-arm64`
- 调试符号包：`mskdsp-<VERSION>-nightly-<YYYYMMDD>-<sha>-debugsymbols-linux-arm64.tar.gz`

## 7. `beta.yml`

`beta.yml` 负责 Beta 候选包与预发布页面。

### 7.1 触发条件

- `push` 到 `beta/**`
- `schedule`
  - 当前 cron：`0 18 */2 * *`
- `workflow_dispatch`
  - 支持可选输入 `beta_ref`

### 7.2 目标版本线解析规则

`beta.yml` 会按如下顺序解析本次要构建的目标 Beta 分支：

1. 手动触发参数 `beta_ref`
2. 仓库变量 `MSKDSP_ACTIVE_BETA_REF`
3. 当前 `GITHUB_REF_NAME`

若最终结果不匹配 `beta/*`，workflow 会直接失败。

### 7.3 当前行为

- 先执行 `x64 Debug` 校验
- 校验通过后再执行 `arm64` 交付链路
- 上传 Beta 包 artifact
- 删除当前 Beta 线旧的 GitHub prerelease
- 创建新的 GitHub prerelease 页面

### 7.4 Beta 发布说明基线

- 若仓库中存在最近的正式 tag（匹配 `v*`），则：
  - 以该 tag 作为 `--notes-start-tag`
  - 发布说明中明确写入“基线正式版本”
- 若当前仓库尚无正式 tag，则：
  - 不传 `--notes-start-tag`
  - 将本次视为该版本线的首个 Beta 预发布
  - 在附加说明中明确写入“当前仓库暂无正式版本 tag（匹配 v*）”

### 7.5 当前保留策略

当前实现不是“同一版本线保留多个 Beta 候选包”，而是：

- `beta/x.y` 分支始终持续向前推进
- 同一条 Beta 线在发布新候选包前，会先清理旧的 GitHub prerelease
- 因此 GitHub Release 页面上默认只保留当前 Beta 线最新的一份预发布

### 7.6 产物命名

- Beta 安装包：`mskdsp-<VERSION>-beta-<x.y>-<YYYYMMDD-HHMMSS>-<sha>-linux-arm64`
  - 实际文件名中的分支部分会将 `/` 转成 `-`
- Beta 调试符号包：`mskdsp-<VERSION>-beta-<x.y>-<YYYYMMDD-HHMMSS>-<sha>-debugsymbols-linux-arm64.tar.gz`
- GitHub prerelease tag：`beta-<x.y>-<YYYYMMDD-HHMMSS>-<sha>`
- GitHub prerelease 标题：`Beta <x.y> <YYYYMMDD-HHMMSS>-<sha>`

### 7.7 Hotfix 维护线建议

- 若 `vX.Y.Z` 已经完成 Stable 发布，且发现必须尽快交付给当前 Stable 用户的紧急缺陷修复，则建议从 `vX.Y.Z` 对应提交派生 `beta/X.Y.(Z+1)` 维护线。
- `beta/X.Y.(Z+1)` 维护线只承载本次 hotfix，不应混入下一功能版本（如 `beta/X.(Y+1)`）已经在开发中的新功能或重构。
- 推送该维护线后，仍由 `beta.yml` 生成 Beta 候选包；由于该分支从 `vX.Y.Z` 派生，Beta 发布说明默认会以最近可达的 `v*` tag 作为基线，说明范围更容易保持为本次 hotfix 的增量。
- Hotfix 验证通过后，可直接在该维护线对应的修复提交上打 `vX.Y.(Z+1)` tag，触发 `release.yml` 完成正式发布。
- 若团队希望长期维护同一条 `X.Y` 维护线，也可继续使用 `beta/X.Y` 承载多个补丁版本；但需接受 Beta 页面标题与最终 Stable tag 可能不完全一致。当前文档更推荐“一次 hotfix 对应一条 `beta/X.Y.(Z+1)` 维护线”，以便减少命名歧义。
- Hotfix 发布完成后，应将相同修复同步到仍在推进的开发分支，例如 `master` 与下一条功能候选线；已完成使命、不再继续演进的旧 Beta 分支通常不需要回灌。
- 若某个缺陷可以随下一功能版本一起交付，而不需要为当前 Stable 单独出补丁，则直接修入 `master` 并同步到对应的下一条 `beta/x.y` 即可，不需要额外创建 hotfix 维护线。

## 8. `release.yml`

`release.yml` 负责正式发布。

### 8.1 触发条件

- `push` tag `v*`

### 8.2 发布前约束

正式发布前会执行以下校验：

- 拉取远端 `origin/beta/*`
- 检查当前 tag 对应 commit 是否包含于至少一条 `origin/beta/*`
- 若不属于任何 Beta 版本线，则 workflow 直接失败

### 8.3 当前行为

- 先执行 `x64 Debug` 发布前校验
- 校验通过后交叉编译 `arm64 RelWithDebInfo`
- 生成正式自解压安装包、调试符号包与 `SHA256SUMS`
- 若 GitHub Release 已存在，则执行 `gh release upload --clobber`
- 若 GitHub Release 不存在，则执行 `gh release create --verify-tag --generate-notes`

### 8.4 产物命名

- 正式安装包：`mskdsp-<tag>-linux-arm64`
- 调试符号包：`mskdsp-<tag>-debugsymbols-linux-arm64.tar.gz`

## 9. 各渠道产物去向

当前各渠道的产物去向如下：

- `CI`
  - 上传 GitHub Actions artifact
  - 不创建 Release 页面
- `Nightly`
  - 上传 GitHub Actions artifact
  - 不创建 Release 页面
- `Beta`
  - 上传 GitHub Actions artifact
  - 创建 GitHub prerelease
- `Stable`
  - 创建或更新 GitHub Release

## 10. 当前命名与交付清单

当前 `arm64` 交付默认包含以下资产：

- 自解压安装包
- 调试符号包（如 `package/debug` 存在）
- `SHA256SUMS`

说明：

- 交付主包不是直接上传 `package/` 目录，而是上传由 `script/make_exe.sh` 生成的自解压安装包
- 测试与发布流程都保留了独立调试符号包，便于问题定位

## 11. 维护建议

后续如需调整 workflow，应优先同步关注以下点：

- 仓库默认分支是否变化，避免 Nightly 来源与文档不一致
- `beta.yml` 的目标分支解析逻辑是否变化
- 正式发布是否仍要求 tag 来源于 `beta/*`
- 安装包命名规则、自解压脚本行为与 Release 页面资产是否同步变化
- `protobuf` 子模块访问方式与密钥命名是否变化
