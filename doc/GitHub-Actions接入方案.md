# 下位机 GitHub Actions

## 目标与验收标准

主仓库与协议仓库从 GitHub 获取。使用 GitHub 托管的 `ubuntu-24.04`（x64）与 `ubuntu-24.04-arm`（原生 ARM64）两个独立任务，并行完成编译和各自完整 CTest；架构任务之间没有依赖，也不因另一架构失败而取消。编译与测试位于同一任务和绝对路径，保留原有测试断言、并行度 2、失败日志与 JUnit 报告。

网络、安装器、更新和发布契约检查独立运行。全部检查及两个架构编译测试通过后才允许打包；双架构安装包成功后才允许平台发布。跨任务交付目录先使用 tar 保存权限和符号链接，始终校验同一来源提交和运行身份。缓存按原生架构、工具链和固定 vcpkg 基线隔离。

主干发布双架构 CI；`release/X.Y` 完整验证并保存产物但不发布平台全局 CI；受保护 `vX.Y.Z` 标签自身重新构建、完整测试、打包后仅发布 ARM64 Stable，版本等于 VERSION，x64 正式包仅保留产物。Beta、定时任务与外部 PR 不进入生产流程。

## 实施约束

GitHub 运行身份包含 run_id、run_number、run_attempt、阶段与架构，不能直接将字符串 job 名作为旧整数作业编号。正式来源必须是受保护标签，不能伪造保护状态。发布前继续核对远端分支 HEAD，或正式标签 SHA 及主干/对应维护线历史。构建可取消过时任务，发布不能被后续提交的全局取消规则打断。

失败后选择 **Re-run all jobs**。仅重跑失败作业会产生新的 run_attempt，但上游成功任务不重跑；本流程不会下载旧批次产物冒充新批次。产物名称包含 run_id 与 run_attempt，元数据另行复核两者。GitHub 整数作业身份采用运行编号和重跑次数的唯一配对，再区分阶段与架构，避免不同工作流的 run_number 相同而重复生成版本。

托管 Runner 在本机原生制作 Docker 安装包。GitHub Actions 以平台 gRPC 的入库和通道发布成功响应为准。上传校验和断点续传仍在发布流程内执行。发布响应丢失时按可重试错误处理，重试耗尽仍未收到成功响应则报告结果无法确认。

## 配置与凭据

仓库 `ci` 环境仅允许 master/main 分支和 v* 标签部署；v* 标签使用真实 GitHub ruleset 保护更新与删除，身份校验继续拒绝不合法版本和未保护正式引用。构建、检查与打包任务仅有仓库只读权限，不接收平台上传令牌。

`ci` 环境保存 `PLATFORM_UPLOAD_TOKEN` secret，`PLATFORM_GRPC_ENDPOINT` 填写 Runner 可达的 `host:port`。使用 TLS 时，`PLATFORM_GRPC_PLAINTEXT` 留空或设为 `0`。私有 CA 使用 `PLATFORM_GRPC_CA_B64` secret，在发布临时主机写为受限文件后清理。

公网入口采用明文 gRPC 时，须显式设置 `PLATFORM_GRPC_PLAINTEXT=1` 和 `PLATFORM_GRPC_ALLOW_PUBLIC_PLAINTEXT=1`，并清空 CA 配置。此时发布令牌和安装包传输均不加密。

GitHub Actions 的发布任务仅访问 gRPC 发布接口。HTTP 清单可由使用该更新链路的客户端在内网读取。

## 验证边界

先补 GitHub 身份、原生 ARM、打包和工作流门禁用例，再实现。所有构建、自动化测试和静态检查由 CI 执行，本地仅阅读、编辑和差异审查。提交推送后结束，不自动查询或等待流水线。本次不升级设备、不修改平台服务或平台协议。
