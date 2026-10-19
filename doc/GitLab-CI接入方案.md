# 下位机 GitLab CI 接入方案

## 已确认的目标与边界

- 仅主分支 master/main 的 push 或手动流水线执行；MR、beta 分支和版本标签不重复构建。
- ARM64、x64 均完成 GCC14 RelWithDebInfo 编译及现有完整 CTest 测试；ARM64 由 x64 交叉编译，在现有 x64 主机通过 QEMU 执行完整单测。
- 使用独立下位机 Runner、标签、配置和缓存；并发 6，单作业 8 CPU、16 GiB。保留现有平台 Runner 配置。
- 保留 Docker 镜像、自解压 gzip 安装包、SHA256SUMS；ARM64 独立调试符号包归档到 GitLab。
- 不执行独立的 check_iec61850_package.sh；不减少已有协议单测。
- 安装包经 gRPC 上传平台软件库，校验完成后自动发布对应架构 CI 通道；不安装设备，不触发 R2 或 GitHub 发布。
- Beta/Stable 从同一已通过 CI 的安装包晋升，文件及 SHA256 不改变。此次不创建 Beta/Stable 重构建流水线。

## 作业与身份

两个架构使用同一套构建脚本和不同工具镜像，各自产出 release/<arch>/ 下的安装包、metadata.json、SHA256SUMS、测试日志及可选符号包。先完成两个架构的编译测试与打包，再执行发布作业，避免失败流水线发布另一个架构。发布作业不重新编译。

### ARM64 编译与测试分离

首次实测发现，将 GCC14 编译器本身放在 QEMU 中运行开销很大：Abseil 单项约为 x64 的 20 倍，Protobuf 也持续表现出较高开销，8 小时作业上限存在风险。因此 ARM64 改为在独立 x64 工具镜像中使用 GCC14 的 aarch64 交叉工具链编译，随后在既有 ARM64 工具镜像中通过 QEMU 执行完整 CTest。x64 仍在原工具镜像中完成编译和测试。

- 交叉镜像基于既有 x64 工具镜像，额外安装 GCC14 aarch64 交叉编译器；不覆盖现有平台或 GitHub 工具镜像。
- vcpkg 明确区分 host=x64 与 target=ARM64，protoc 与 grpc_cpp_plugin 由 host triplet 提供；目标库及主程序使用同一交叉工具链。
- 交叉镜像默认主机 `cc/c++` 必须为 x64 GCC14；交叉编译入口清除继承的 `CC/CXX`，避免 OpenSSL 等 make 端口沿用主机编译器而忽略目标工具链。
- 使用独立交叉 triplet、chainload toolchain 与缓存版本，避免复用旧 QEMU 编译器生成的 ARM64 二进制缓存。
- ARM64 编译作业传递完整 CTest 运行所需的构建目录、目标库和测试程序；测试作业保持相同仓库绝对路径，在 ARM64 镜像中执行全部测试，拒绝零测试。测试失败不能进入打包或发布。
- 安装及符号归档在可用的工具环境中完成；测试通过后才清理测试程序并归档交付包。发布作业继续只消费已测试的冻结产物。
- 独立安装脚本初始化 CMake 策略，提前查询并收集目标 GCC 运行库；ARM64 glibc 及加载器由 Ubuntu 基础镜像提供，其他缺失依赖仍拒绝交付。
- 验收必须包含实际交叉编译器版本及目标架构、真实 ARM64 完整 CTest、双架构打包发布、平台清单与实际下载 SHA256 核对；不以编译通过代替 ARM64 执行验证。

CI 完整测试的临时工作目录、系统临时目录和 CTest 过程日志使用作业独立的内存文件系统，避免缓存和大产物归档的共享磁盘争用触发 SQLite/SOE 处理等待超时或日志管道反压。内存目录使用短路径，保留 Unix socket 的路径长度余量。CTest 仍执行全部用例，保留原有断言、超时、并行度与失败门禁；测试种子文件读取磁盘原件，模块种子目录整体链接到磁盘以保留共享库的普通文件属性，避免内存挂载的 noexec 影响模块加载。成功及失败后均恢复原工作目录，并将 CTest 日志归档回磁盘。下位机 Runner 的共享内存上限为每作业 1 GiB，计入原有 16 GiB 内存限制。

所有隔离测试的工作目录均位于 `build-<arch>/test_env/`，包括模块接口测试；避免特殊工作目录绕过 CI 内存隔离。

metadata.json 冻结 product、version、package_version、platform、file_name、size、sha256、image_id、source_repository、source_ref、source_sha、build_time、channel 及 pipeline_id。product 固定 mskdsp-lower；platform 为 linux-arm64/linux-x64；channel 固定 ci。版本包含 GitLab 流水线及作业身份，防止重试产生不同字节却复用同一版本。

发布前核对实际文件大小和 SHA256、CI 来源及远端主分支提交，拒绝落后的构建更新通道。按架构串行发布；已成功上传的文件可查询偏移续传，CompleteUpload 成功后才调用 PublishRelease，并核对公开目录及清单。平台负责生成 latest.json 和下载 SHA256SUMS。

## 协议、授权和镜像

软件仓库协议由单独的 MskDSPPlatformProto GitLab 仓库按固定提交获取到临时目录，不改变现有 protobuf 子模块及 GitHub 流程。CI_JOB_TOKEN 只读获取两个协议项目，并配置对应 allowlist。

平台上传使用已有通用 publisher 令牌，保存在下位机项目受保护、掩码的 CI 变量中；不复用仅允许平台自身包的 CI 上传令牌。客户端不输出令牌、认证头或底层 RPC 详情。现有平台业务代码不需要修改。

编译镜像复用已准备的 Ubuntu 24.04 x64/ARM64 工具链，按仓库 baseline 初始化 vcpkg，并按架构隔离 vcpkg 和 ccache。镜像打包通过独立 BuildKit 服务导出普通 Docker 归档，不将宿主机 Docker socket 挂载到作业容器。

下位机 Runner 为 `gitlab-runner-lower`，独立 BuildKit 为 `mskdsp-lower-ci-buildkit`，配置保存在服务器 `/home/daniel/mskdsp-lower-ci/`。BuildKit 使用独立状态卷，通过内网 `tcp://192.168.10.198:1235` 提供 mTLS 服务，并配置 Docker Hub 镜像加速；其网络使用宿主机网络，以避开既有容器网络中的外网访问超时。平台原 Runner、原 BuildKit 的配置和状态卷保持不变。项目 `ci-build` 环境的 `BUILDKIT_HOST` 指向下位机独立实例，证书变量受保护且掩码保存。

服务器直连 GitHub 源码归档入口可能超时；通过 vcpkg 自定义资源下载源将 GitHub tar.gz 归档映射到同一仓库、同一 ref 的官方 codeload 地址。下载器与 vcpkg 均保留 SHA512 校验，不改变依赖版本或校验值。依赖缓存失败时也保存，降低网络重试和首次构建成本。

CI 按配置 baseline 获取并校验 vcpkg 本地仓库，同时保留完整历史，供版本解析读取旧 revision 的端口树；已有浅克隆先补齐历史。随后依赖索引的同一 GitHub 仓库地址仅在当前构建子进程中映射到该本地仓库，避免索引重复联网拉取 HEAD。依赖 baseline、版本和内容校验保持不变；不写全局 Git 配置，不影响项目及协议仓库认证。

## 验收标准

1. 先补元数据、上传续传、发布及流水线规则测试，再实现对应代码。
2. 静态检查和客户端契约测试通过，GitLab CI 配置验证通过。
3. 真实主分支流水线两个架构编译、CTest、打包及发布全部成功，记录测试数量和耗时。
4. 平台管理员软件版本页面可见两个架构安装包及 CI 通道，公开清单的提交、版本、大小、SHA256 与 GitLab 产物一致。
5. 下载安装包重新核对 SHA256；不执行安装脚本，不升级设备。核对既有平台 Runner 和平台服务保持正常。
