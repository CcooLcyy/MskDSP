# 下位机 GitLab CI 接入方案

## 已确认的目标与边界

- 仅主分支 master/main 的 push 或手动流水线执行；MR、beta 分支和版本标签不重复构建。
- ARM64、x64 均完成 RelWithDebInfo 编译及现有完整 CTest 测试；ARM64 在现有 x64 主机通过 QEMU 执行。
- 使用独立下位机 Runner、标签、配置和缓存；并发 6，单作业 8 CPU、16 GiB。保留现有平台 Runner 配置。
- 保留 Docker 镜像、自解压 gzip 安装包、SHA256SUMS；ARM64 独立调试符号包归档到 GitLab。
- 不执行独立的 check_iec61850_package.sh；不减少已有协议单测。
- 安装包经 gRPC 上传平台软件库，校验完成后自动发布对应架构 CI 通道；不安装设备，不触发 R2 或 GitHub 发布。
- Beta/Stable 从同一已通过 CI 的安装包晋升，文件及 SHA256 不改变。此次不创建 Beta/Stable 重构建流水线。

## 作业与身份

两个架构使用同一套构建脚本和不同工具镜像，各自产出 release/<arch>/ 下的安装包、metadata.json、SHA256SUMS、测试日志及可选符号包。先完成两个架构的编译测试与打包，再执行发布作业，避免失败流水线发布另一个架构。发布作业不重新编译。

metadata.json 冻结 product、version、package_version、platform、file_name、size、sha256、image_id、source_repository、source_ref、source_sha、build_time、channel 及 pipeline_id。product 固定 mskdsp-lower；platform 为 linux-arm64/linux-x64；channel 固定 ci。版本包含 GitLab 流水线及作业身份，防止重试产生不同字节却复用同一版本。

发布前核对实际文件大小和 SHA256、CI 来源及远端主分支提交，拒绝落后的构建更新通道。按架构串行发布；已成功上传的文件可查询偏移续传，CompleteUpload 成功后才调用 PublishRelease，并核对公开目录及清单。平台负责生成 latest.json 和下载 SHA256SUMS。

## 协议、授权和镜像

软件仓库协议由单独的 MskDSPPlatformProto GitLab 仓库按固定提交获取到临时目录，不改变现有 protobuf 子模块及 GitHub 流程。CI_JOB_TOKEN 只读获取两个协议项目，并配置对应 allowlist。

平台上传使用已有通用 publisher 令牌，保存在下位机项目受保护、掩码的 CI 变量中；不复用仅允许平台自身包的 CI 上传令牌。客户端不输出令牌、认证头或底层 RPC 详情。现有平台业务代码不需要修改。

编译镜像复用已准备的 Ubuntu 24.04 x64/ARM64 工具链，按仓库 baseline 初始化 vcpkg，并按架构隔离 vcpkg 和 ccache。镜像打包通过独立 BuildKit 服务导出普通 Docker 归档，不将宿主机 Docker socket 挂载到作业容器。

## 验收标准

1. 先补元数据、上传续传、发布及流水线规则测试，再实现对应代码。
2. 静态检查和客户端契约测试通过，GitLab CI 配置验证通过。
3. 真实主分支流水线两个架构编译、CTest、打包及发布全部成功，记录测试数量和耗时。
4. 平台管理员软件版本页面可见两个架构安装包及 CI 通道，公开清单的提交、版本、大小、SHA256 与 GitLab 产物一致。
5. 下载安装包重新核对 SHA256；不执行安装脚本，不升级设备。核对既有平台 Runner 和平台服务保持正常。
