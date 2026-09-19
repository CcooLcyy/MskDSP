# GitHub Actions 与发版策略

## 1. 文档目的

本文档记录当前仓库已经落地的 GitHub Actions workflow、分支/渠道约定、构建产物形态与发布流程，作为后续维护 workflow、排查构建问题与安排版本发布的依据。

## 2. 当前 workflow 概览

当前仓库已落地 3 个 workflow：

- `ci.yml`
  - 触发：`pull_request`；`push` 到 `master`、`main`、`beta/**`
  - 作用：
    - 所有触发场景都在 GitHub 托管 ARM64 runner 上执行 `arm64 RelWithDebInfo` 编译与单元测试
    - 当 `push` 到 `master/main` 时，在 ARM64 单元测试通过后继续产出测试安装包
- `beta.yml`
  - 触发：`push` 到 `beta/**`；`workflow_dispatch`
  - 作用：执行 ARM64 原生编译与单元测试，再产出 `arm64 Beta` 自解压安装包；同时创建 GitHub 预发布页面
- `release.yml`
  - 触发：`push` tag `v*`；`workflow_dispatch`
  - 作用：执行发布前 ARM64 原生编译与单元测试，再产出 `arm64` 正式安装包并创建/更新 GitHub Release

## 3. 统一实现约定

当前 3 个 workflow（`ci.yml`、`beta.yml`、`release.yml`）全部属于构建型 workflow，遵循以下统一约定：

- 主仓库 checkout 时不直接拉取 submodule，随后单独准备 `protobuf` 子模块访问凭据，再执行 `git submodule update --init --recursive`
- 子模块访问优先级：
  - 优先使用 `MSKDSP_PROTO_SSH_KEY`
  - 其次使用 `MSKDSP_PROTO_TOKEN`
  - 若均未配置，则回退为 HTTPS 方式拉取
- `vcpkg` 不直接使用固定仓库分支，而是读取 `vcpkg-configuration.json` 中的 baseline，再 clone/checkout 对应版本
- 所有构建型 workflow 都启用了两层缓存：
  - `vcpkg` 二进制缓存
  - `ccache` 编译缓存
- `arm64` 构建统一使用：
  - GitHub 托管 `ubuntu-24.04-arm` runner
  - `gcc-14/g++-14`
  - `Ninja`
  - `RelWithDebInfo`
  - `MSKDSP_BUILD_TESTS=ON`
- `arm64` 构建使用 `VCPKG_HOST_TRIPLET=arm64-linux-dynamic` 和 `VCPKG_TARGET_TRIPLET=arm64-linux-dynamic`。
- `arm64-linux-dynamic` 由仓库内 `cmake/vcpkg-triplets/arm64-linux-dynamic.cmake` 覆盖，并设置 `VCPKG_BUILD_TYPE=release`，因此 vcpkg 依赖只编译 Release；项目本体仍使用 `RelWithDebInfo` 并保留独立调试符号包。
- 编译后直接在 ARM64 runner 上执行 `ctest --test-dir build-arm64 --output-on-failure --parallel "$(nproc)"`。
- 当前构建型 workflow 不再包含独立 x64 编译、测试或 x64 artifact。
- `arm64` 打包链路统一使用原生编译：
  - `gcc-14/g++-14`
  - `MSKDSP_STRIP_DEBUG=ON`
- `arm64` 交付包的生成流程统一为：
  1. ARM64 单元测试通过后，通过 `cmake --install build-arm64` 将运行产物落到 `package/`
  2. 清理 `package/` 根目录下的 `*_test` 测试可执行文件
  3. 单独打包 `package/debug` 为调试符号包
  4. 使用 `Dockerfile` 构建 `arm64` 镜像
  5. 读取 Docker image config ID，写入下位机 `latest.json` 的 `image_id`
  6. `docker save` 导出镜像 tar
  7. 调用 `script/make_exe.sh` 生成自解压安装包
  8. 生成 `SHA256SUMS`

需要注意：

- 根目录 `Dockerfile` 默认基础镜像为 `localhost/arm64v8/ubuntu:noble`
- 在 GitHub Actions 中会先将该镜像名替换为 `arm64v8/ubuntu:noble`，再执行 `docker buildx build`

## 4. 分支与发布渠道

当前实际流程对应的渠道模型如下：

- `CI`
  - 面向开发校验
  - 由 `pull_request`、`master/main` push、`beta/**` push 触发
- `Beta`
  - 常规功能候选包面向版本线 `beta/x.y`
  - 已发布 Stable 的 hotfix 建议按补丁版本派生 `beta/x.y.z` 维护线
  - 由 `beta.yml` 针对目标 `beta/*` 分支构建候选包
- `Stable`
  - 面向正式交付
  - 由人工创建 `v*` tag
  - 最终统一由 `release.yml` 构建并发布

补充说明：

- `ci.yml` 为兼容历史仓库命名，同时监听 `master` 与 `main`
- `release.yml` 要求正式 tag 对应的 commit 必须来自某条 `origin/beta/*` 版本线

## 5. `ci.yml`

`ci.yml` 负责开发过程中的持续集成校验。

### 5.1 触发条件

- `pull_request`
- `push` 到 `master`
- `push` 到 `main`
- `push` 到 `beta/**`

### 5.2 当前行为

- `arm64-master-package`
  - 所有触发场景都原生编译 `arm64 RelWithDebInfo` 并运行 ARM64 单元测试
  - `pull_request` 和 `beta/**` push 只执行编译与测试，不生成交付包
  - 仅在 `push` 到 `master/main` 时生成自解压测试安装包、调试符号包与 `SHA256SUMS`
  - 主分支产物通过 artifact 上传，不创建 GitHub Release

### 5.3 产物命名

- 测试安装包：`mskdsp-<VERSION>-<branch>-ci-<YYYYMMDD>-<sha>-linux-arm64`
- 调试符号包：`mskdsp-<VERSION>-<branch>-ci-<YYYYMMDD>-<sha>-debugsymbols-linux-arm64.tar.gz`

## 6. `beta.yml`

`beta.yml` 负责 Beta 候选包与预发布页面。

### 6.1 触发条件

- `push` 到 `beta/**`
- `workflow_dispatch`
  - 支持可选输入 `beta_ref`

### 6.2 目标版本线解析规则

`beta.yml` 会按如下顺序解析本次要构建的目标 Beta 分支：

1. 手动触发参数 `beta_ref`
2. 当前 `GITHUB_REF_NAME`

若最终结果不匹配 `beta/*`，workflow 会直接失败。

### 6.3 当前行为

- 原生编译 `arm64 RelWithDebInfo` 并运行 ARM64 单元测试
- 单元测试通过后执行 `arm64` 交付链路
- 上传 Beta 包 artifact
- 删除当前 Beta 线旧的 GitHub prerelease
- 创建新的 GitHub prerelease 页面
- 仅在 Beta 分支有新提交或手动指定目标分支时才会产出新 Beta
- 不再按日历定时重建 Beta 候选包

### 6.4 Beta 发布说明基线

- 若仓库中存在最近的正式 tag（匹配 `v*`），则：
  - 以该 tag 作为 `--notes-start-tag`
  - 发布说明中明确写入“基线正式版本”
- 若当前仓库尚无正式 tag，则：
  - 不传 `--notes-start-tag`
  - 将本次视为该版本线的首个 Beta 预发布
  - 在附加说明中明确写入“当前仓库暂无正式版本 tag（匹配 v*）”

### 6.5 当前保留策略

当前实现不是“同一版本线保留多个 Beta 候选包”，而是：

- `beta/x.y` 分支始终持续向前推进
- 同一条 Beta 线在发布新候选包前，会先清理旧的 GitHub prerelease
- 因此 GitHub Release 页面上默认只保留当前 Beta 线最新的一份预发布
- 若同一条 Beta 线没有新提交，则不会生成新的 Beta prerelease

### 6.6 产物命名

- Beta 安装包：`mskdsp-<VERSION>-beta-<x.y>-<YYYYMMDD-HHMMSS>-<sha>-linux-arm64`
  - 实际文件名中的分支部分会将 `/` 转成 `-`
- Beta 调试符号包：`mskdsp-<VERSION>-beta-<x.y>-<YYYYMMDD-HHMMSS>-<sha>-debugsymbols-linux-arm64.tar.gz`
- GitHub prerelease tag：`beta-<x.y>-<YYYYMMDD-HHMMSS>-<sha>`
- GitHub prerelease 标题：`Beta <x.y> <YYYYMMDD-HHMMSS>-<sha>`

### 6.7 Hotfix 维护线建议

- 若 `vX.Y.Z` 已经完成 Stable 发布，且发现必须尽快交付给当前 Stable 用户的紧急缺陷修复，则建议从 `vX.Y.Z` 对应提交派生 `beta/X.Y.(Z+1)` 维护线。
- `beta/X.Y.(Z+1)` 维护线只承载本次 hotfix，不应混入下一功能版本（如 `beta/X.(Y+1)`）已经在开发中的新功能或重构。
- 推送该维护线后，仍由 `beta.yml` 生成 Beta 候选包；由于该分支从 `vX.Y.Z` 派生，Beta 发布说明默认会以最近可达的 `v*` tag 作为基线，说明范围更容易保持为本次 hotfix 的增量。
- Hotfix 验证通过后，可直接在该维护线对应的修复提交上打 `vX.Y.(Z+1)` tag，触发 `release.yml` 完成正式发布。
- 若团队希望长期维护同一条 `X.Y` 维护线，也可继续使用 `beta/X.Y` 承载多个补丁版本；但需接受 Beta 页面标题与最终 Stable tag 可能不完全一致。当前文档更推荐“一次 hotfix 对应一条 `beta/X.Y.(Z+1)` 维护线”，以便减少命名歧义。
- Hotfix 发布完成后，应将相同修复同步到仍在推进的开发分支，例如 `master` 与下一条功能候选线；已完成使命、不再继续演进的旧 Beta 分支通常不需要回灌。
- 若某个缺陷可以随下一功能版本一起交付，而不需要为当前 Stable 单独出补丁，则直接修入 `master` 并同步到对应的下一条 `beta/x.y` 即可，不需要额外创建 hotfix 维护线。
- 正式版本一律由人工打 `v*` tag（或手动触发 `release.yml`）产生；多条 Beta 线并存时，直接给需要转正的那条维护线打对应 `v*` tag 即可。

## 7. `release.yml`

`release.yml` 负责正式发布。

### 7.1 触发条件

- `push` tag `v*`
- `workflow_dispatch`
  - 支持输入 `release_tag`

### 7.2 发布前约束

正式发布前会执行以下校验：

- 拉取远端 `origin/beta/*`
- 检查当前 tag 对应 commit 是否包含于至少一条 `origin/beta/*`
- 若不属于任何 Beta 版本线，则 workflow 直接失败

### 7.3 当前行为

- 原生编译 `arm64 RelWithDebInfo` 并运行发布前 ARM64 单元测试
- ARM64 单元测试通过后生成交付包
- 生成正式自解压安装包、调试符号包与 `SHA256SUMS`
- 若 GitHub Release 已存在，则执行 `gh release upload --clobber`
- 若 GitHub Release 不存在，则执行 `gh release create --verify-tag --generate-notes`
- `v*` tag 一律由人工创建，不存在自动晋升链路
- 无论 tag 来源如何，正式发布链路保持一致

### 7.4 产物命名

- 正式安装包：`mskdsp-<tag>-linux-arm64`
- 调试符号包：`mskdsp-<tag>-debugsymbols-linux-arm64.tar.gz`

## 8. 各渠道产物去向

当前各渠道的产物去向如下：

- `CI`
  - PR 与 `beta/**` push 仅执行 ARM64 编译和单元测试
  - `master/main` push 上传 arm64 测试安装包等 GitHub Actions artifact
  - 不创建 Release 页面
- `Beta`
  - 上传 GitHub Actions artifact
  - 创建 GitHub prerelease
- `Stable`
  - 创建或更新 GitHub Release

## 9. 当前命名与交付清单

当前 `arm64` 交付默认包含以下资产：

- 自解压安装包
- 调试符号包（如 `package/debug` 存在）
- `SHA256SUMS`
- `latest.json`（包含发布渠道、安装包信息和 Docker `image_id`）

说明：

- 交付主包不是直接上传 `package/` 目录，而是上传由 `script/make_exe.sh` 生成的自解压安装包
- `latest.json` 同时记录 Docker `image_id`，供上位机校验目标机实际运行的镜像构建
- 生成交付包的流程都保留了独立调试符号包，便于问题定位

## 10. 维护建议

后续如需调整 workflow，应优先同步关注以下点：

- `beta.yml` 的目标分支解析逻辑是否变化
- 正式发布是否仍要求 tag 来源于 `beta/*`
- 安装包命名规则、自解压脚本行为与 Release 页面资产是否同步变化
- `protobuf` 子模块访问方式与密钥命名是否变化
- vcpkg triplet 或其引用文件变化时，是否同步纳入 vcpkg 缓存 key 的 `hashFiles`

## 11. Cloudflare R2 更新包发布

当前构建完成后，CI、Beta 和 Stable workflow 会直接将下位机更新资产上传到 Cloudflare R2。R2 对象路径为：

```text
mskdsp-lower/<channel>/<platform>/<资产文件>
mskdsp-lower/<channel>/latest.json
```

安装包、调试包和 `SHA256SUMS` 先上传，`latest.json` 最后上传。上传完成后仅清理同一渠道和平台下不再使用的旧对象，不影响其他渠道。客户端清单地址示例：

```text
https://pub-19f3d71852b04011b120b1b814141c12.r2.dev/mskdsp-lower/stable/latest.json
```

首次启用前，在仓库 `Settings → Secrets and variables → Actions` 配置：

- Secrets：`R2_ACCOUNT_ID`、`R2_ACCESS_KEY_ID`、`R2_SECRET_ACCESS_KEY`
- Variables：`R2_BUCKET`（默认 `mskdsp-update`）、`R2_PUBLIC_BASE_URL`（当前为上述 `r2.dev` 地址）

`r2.dev` 仅用于当前开发联调；绑定自定义域名后，可通过修改 `R2_PUBLIC_BASE_URL` 和对应的更新基地址切换正式地址，无需修改清单格式或下载逻辑。
