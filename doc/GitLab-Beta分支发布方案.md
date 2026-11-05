# 下位机 Beta 分支发布

> 历史方案：已停止 Beta 分支触发及新 Beta 发布。当前规则为主干 CI、维护分支完整验证及受保护标签 Stable，见《GitLab-Stable标签发布方案》。下述内容保留供历史包追溯，不是当前操作指引。

## 目标与边界

`master` / `main` 保留原 CI 通道。`beta/<主版本.次版本.修订版本>` 的 push / web 流水线使用该分支冻结提交，完成 ARM64 交叉构建、x64 构建与全量 CTest、网络与安装器测试，以及两个架构打包后，仅将 ARM64 包发布到平台 Beta 通道。当前锁定分支为 `beta/0.4.0`，分支版本必须与 `VERSION` 一致。

Beta 身份示例：`0.4.0-beta-p<流水线序号>-b<构建作业号>-j<打包作业号>-<短提交>`，`source_ref` 必须为真实 Beta 分支，不能将 CI 安装包晋升或改写标签冒充 Beta。x64 Beta 构建、测试与打包仅提供 GitLab 产物，禁止上传平台 Beta，也禁止更新平台 x64 CI 通道。

不修改业务实现、设备部署或平台业务。保留原始来源、流水线与提交一致性校验，发布前再次读取同一远端分支 HEAD，拒绝过期构建。安装包实际大小、SHA256、镜像摘要、公开目录及清单身份校验继续生效；平台签名生成及客户端既有签名校验流程保持原状。

## 验收与测试

先补身份、通道及分支门禁测试，再实现流水线与客户端：

- 有效 Beta 分支生成真实 Beta 版本，两架构与重试均保留原构建身份；分支版本不匹配 `VERSION`、非法 Beta 名称、MR 和标签流水线均拒绝。
- 打包拒绝跨流水线、跨来源或通道篡改；发布客户端仅接收主分支 CI 或 ARM64 Beta，拒绝 x64 Beta、Beta 冒充 CI 及 CI 冒充 Beta。
- ARM64 Beta 发布等待双架构打包与既有完整测试门禁，原 CI 发布作业在 Beta 不运行。
- gRPC 契约测试验证 Beta 上传、发布请求与公开目录使用 Beta，支持断点续传、发布响应丢失确认，并保留完整性失败与过期分支拒绝场景。

上述测试在 GitLab 的独立发布契约测试作业执行，禁止在本地运行；发布作业依赖该作业。完整 x64 CTest 沿用现有 test-x64，不减用例、不跳过失败。ARM64 交叉构建不能替代 ARM64 真机运行验证。

## 环境前提

- GitLab 将 `beta/*` 设为受保护分支；Runner 与受保护变量可用于该分支。
- `ci-build` 环境仍需 `BUILDKIT_HOST` 与既有 BuildKit mTLS 证书变量，供构建和打包使用。
- Beta 发布复用现有 `ci` 环境的 `PLATFORM_UPLOAD_TOKEN`（通用 publisher 权限，不能使用限定 CI 的令牌）、`PLATFORM_GRPC_ENDPOINT`，以及现有 TLS 配置 `PLATFORM_GRPC_CA_FILE` 或显式内网 `PLATFORM_GRPC_PLAINTEXT=1`；建议配置 `PLATFORM_PUBLIC_BASE_URL` 核验公开清单。环境名仅用于 GitLab 凭据作用域；发布通道严格取自冻结元数据，Beta 使用独立资源锁，不更新 CI 通道。
- GitLab 自动注入的 `CI_JOB_TOKEN` 必须可拉取固定平台协议仓库并读取来源分支。
- 平台已支持 ARM64 下位机 Beta 与 `/mskdsp-lower/beta/latest.json`，平台签名密钥已配置；x64 平台只支持 CI，本方案不扩展平台能力。

实施后由主 agent 审查、提交推送，建立 Beta 分支并跟踪其对应提交流水线；未完成这些步骤前，不能宣称 Beta 发布成功。
