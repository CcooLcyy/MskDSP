# 下位机 Stable 标签发布

## 目标与边界

主干保留 CI、`beta/x.y.z` 保留 Beta。正式 `vX.Y.Z` 标签只允许受保护引用的 push / web 流水线：完整双架构构建、x64 全量 CTest、网络与安装器测试、发布契约测试和两个架构打包全部通过后，只发布 ARM64 Stable。x64 正式构建和包仅作 GitLab 产物，不上传 Stable 或改写 x64 CI。

本次目标标签 `v0.4.0`。标签版本必须等于 `VERSION`；冻结 `channel=stable`、`version=0.4.0`、`package_version=0.4.0-linux-arm64`、`source_ref=v0.4.0` 和实际提交 SHA。不能把 Beta 包改通道冒充正式包。

主 agent 先将发布配置提交同步到 `beta/0.4.0`，等待该 SHA 的完整 Beta 流水线成功，再给同一 SHA 创建受保护标签。发布客户端每次发布前核对远端标签（兼容轻量与附注标签，附注标签取 peeled commit）以及 `refs/heads/beta/0.4.0` 都等于冻结 SHA。标签移动、Beta 前进、来源不明或查询失败均拒绝；Beta CI 成功证据由主 agent 在打标签前核对。

## 验收与测试

先补测试，再实现身份和工作流：

- 正式身份两架构版本均为纯 `0.4.0`，打包不附流水线序号；原构建/打包作业号仍保留为来源字段。
- 非法标签、未保护标签、同时携带分支、标签版本与 VERSION 不符、MR/schedule 来源、来源或通道篡改均拒绝。
- 发布端只接受 ARM64 Stable；标签与 Beta 的同 SHA 门禁覆盖轻量、附注、移动及缺失情况。
- Stable gRPC 契约覆盖成功、续传、完整性失败、过期拒绝和响应丢失；公开 Stable 清单核对真实身份、文件大小与 SHA256。
- 新 Stable 作业独立资源锁，继承现有完整门禁；CI/Beta 发布规则互斥。

仅修改发布配置、脚本、测试及文档，不修改业务、平台或设备。使用既有工具镜像，契约作业保留临时容器安装 python3-yaml。所有测试通过对应提交 CI 执行，本地不运行构建、测试或语法检查。

## 环境前提

GitLab 保护 `v*` 标签并使 `CI_COMMIT_REF_PROTECTED=true`；`ci-build` 与 `ci` 环境变量及权限沿用 Beta，无需复制密钥。Stable 发布仍使用现有通用 publisher 令牌、gRPC/TLS、公开清单地址和平台签名配置。签名与安装包完整性既有链路保持原状；Stable 清单路径为 `/mskdsp-lower/stable/latest.json`。
