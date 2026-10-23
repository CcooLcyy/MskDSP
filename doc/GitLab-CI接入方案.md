# 下位机 GitLab CI 接入方案

## 已确认的目标与边界

- GitLab 为下位机代码与 CI 的唯一维护入口，移除旧 GitHub Actions、专用 Runner 部署模板和 R2/静态发布脚本；GitHub 仓库停用 Actions，历史提交仍可追溯。
- 本地仅保留 `gitlab` 远端，`master` 跟踪 `gitlab/master`，普通 `git push` 仅推送 GitLab。协议子模块固定提交保持不变，源地址改为 GitLab。
- 仅主分支 master/main 的 push 或手动流水线执行；MR、beta 分支和版本标签不重复构建。
- ARM64、x64 均完成 GCC14 RelWithDebInfo 编译及现有完整 CTest 测试；ARM64 由 x64 交叉编译，在现有 x64 主机通过 QEMU 执行完整单测。
- 使用独立下位机 Runner、标签、配置和缓存；并发 6，单作业 8 CPU、16 GiB。保留现有平台 Runner 配置。
- 为对照构建期间的 I/O 等待，每个作业的 CMake 构建及 vcpkg 依赖构建并行度由 8 降为 6；CI 变量、脚本实际命令和中文日志统一为 6。Runner 作业并发仍为 6，单作业 CPU/内存限制及 CTest 并行度 2 保持不变。脚本回归需核对实际构建参数；I/O 改善效果由后续真实流水线验证。
- 保留 Docker 镜像、自解压 gzip 安装包、SHA256SUMS；ARM64 独立调试符号包归档到 GitLab。
- 不执行独立的 check_iec61850_package.sh；不减少已有协议单测。
- 安装包经 gRPC 上传平台软件库，校验完成后自动发布对应架构 CI 通道；不安装设备，不触发 R2 或 GitHub 发布。
- Beta/Stable 从同一已通过 CI 的安装包晋升，文件及 SHA256 不改变。此次不创建 Beta/Stable 重构建流水线。

## 作业与身份

两个架构使用同一套构建脚本和不同工具镜像，各自产出 release/<arch>/ 下的安装包、metadata.json、SHA256SUMS、测试日志及可选符号包。先完成两个架构的编译测试与打包，再执行发布作业，避免失败流水线发布另一个架构。发布作业不重新编译。

### ARM64 编译与测试分离

首次实测发现，将 GCC14 编译器本身放在 QEMU 中运行开销很大：Abseil 单项约为 x64 的 20 倍，Protobuf 也持续表现出较高开销，8 小时作业上限存在风险。因此 ARM64 改为在独立 x64 工具镜像中使用 GCC14 的 aarch64 交叉工具链编译，随后在既有 ARM64 工具镜像中通过 QEMU 执行完整 CTest。x64 仍在原工具镜像中完成编译和测试。

- 交叉镜像基于既有 x64 工具镜像，额外安装 GCC14 aarch64 交叉编译器；不覆盖现有平台工具镜像。
- vcpkg 明确区分 host=x64 与 target=ARM64，protoc 与 grpc_cpp_plugin 由 host triplet 提供；目标库及主程序使用同一交叉工具链。
- 交叉镜像默认主机 `cc/c++` 必须为 x64 GCC14；交叉编译入口清除继承的 `CC/CXX`，避免 OpenSSL 等 make 端口沿用主机编译器而忽略目标工具链。
- 使用独立交叉 triplet、chainload toolchain 与缓存版本，避免复用旧 QEMU 编译器生成的 ARM64 二进制缓存。
- ARM64 编译作业传递完整 CTest 运行所需的构建目录、目标库和测试程序；测试作业保持相同仓库绝对路径，在 ARM64 镜像中执行全部测试，拒绝零测试。测试失败不能进入打包或发布。
- 安装及符号归档在可用的工具环境中完成；测试通过后才清理测试程序并归档交付包。发布作业继续只消费已测试的冻结产物。
- 独立安装脚本初始化 CMake 策略，提前查询并收集目标 GCC 运行库；ARM64 glibc 及加载器由 Ubuntu 基础镜像提供，其他缺失依赖仍拒绝交付。
- 验收必须包含实际交叉编译器版本及目标架构、真实 ARM64 完整 CTest、双架构打包发布、平台清单与实际下载 SHA256 核对；不以编译通过代替 ARM64 执行验证。

### ARM64 作业产物精简

仅通过作业级 `artifacts.exclude` 减少上传内容，不删除构建工作区或持久缓存，不改变编译、测试、安装及发布顺序：

- `build-arm64` 上传时排除 `build-arm64/**/*.o`。该作业已完成编译、链接和安装；后续 `test-arm64` 仅运行现有测试程序，不重新编译或安装。完整 CTest 结构、测试程序、`test_env`、目标动态库、运行脚本及原始 `package/debug` 继续传递。下载的构建产物不再支持直接使用对象文件重新链接，需要重新构建。
- `test-arm64` 执行完整测试并生成 `release/arm64/*debugsymbols*.tar.gz` 后，上传时排除重复的 `package/debug/**`，继续保留独立符号包、运行文件和测试日志。不得将此排除规则前移到 `.build` 或 `build-arm64`，否则测试作业无法生成符号包；测试失败时仍可从原编译作业取得原始调试符号。
- x64 作业、测试数量、失败门禁、最终安装包与符号包的收集规则保持原有设置。新增中文日志说明对象文件仅在上传时排除，以及测试成功后保留独立符号包的交付方式。

验收先检查排除规则仅作用于上述两个作业、运行依赖及符号包仍被收集，并使用实际 Runner 验证递归排除匹配；主分支 CI 再核对 ARM64/x64 各 90 项 CTest、符号包可解包、打包发布及 SHA256 完整性。对比原有产物大小、上传/下载阶段耗时及 Docker 超时告警；大小和耗时收益以真实归档为准，不以估算代替实测。

CI 完整测试的临时工作目录、系统临时目录和 CTest 过程日志使用作业独立的内存文件系统，避免缓存和大产物归档的共享磁盘争用触发 SQLite/SOE 处理等待超时或日志管道反压。内存目录使用短路径，保留 Unix socket 的路径长度余量。CTest 仍执行全部用例，保留原有断言、超时、并行度与失败门禁；测试种子文件读取磁盘原件，模块种子目录整体链接到磁盘以保留共享库的普通文件属性，避免内存挂载的 noexec 影响模块加载。成功及失败后均恢复原工作目录，并将 CTest 日志归档回磁盘。下位机 Runner 的共享内存上限为每作业 1 GiB，计入原有 16 GiB 内存限制。

所有隔离测试的工作目录均位于 `build-<arch>/test_env/`，包括模块接口测试；避免特殊工作目录绕过 CI 内存隔离。

metadata.json 冻结 product、version、package_version、platform、file_name、size、sha256、image_id、source_repository、source_ref、source_sha、build_time、channel 及 pipeline_id。product 固定 mskdsp-lower；platform 为 linux-arm64/linux-x64；channel 固定 ci。版本包含 GitLab 流水线及作业身份，防止重试产生不同字节却复用同一版本。

发布前核对实际文件大小和 SHA256、CI 来源及远端主分支提交，拒绝落后的构建更新通道。按架构串行发布；已成功上传的文件可查询偏移续传，CompleteUpload 成功后才调用 PublishRelease，并核对公开目录及清单。平台负责生成 latest.json 和下载 SHA256SUMS。

## 协议、授权和镜像

软件仓库协议由单独的 MskDSPPlatformProto GitLab 仓库按固定提交获取到临时目录；下位机 protobuf 子模块也直接指向 GitLab。CI_JOB_TOKEN 通过仅存于子进程环境的认证头只读获取两个协议项目，并配置对应 allowlist，不再使用自有 GitHub 仓库的地址重写。

平台上传使用已有通用 publisher 令牌，保存在下位机项目受保护、掩码的 CI 变量中；不复用仅允许平台自身包的 CI 上传令牌。客户端不输出令牌、认证头或底层 RPC 详情。现有平台业务代码不需要修改。

编译镜像复用已准备的 Ubuntu 24.04 x64/ARM64 工具链，按仓库 baseline 初始化 vcpkg，并按架构隔离 vcpkg 和 ccache。镜像打包通过独立 BuildKit 服务导出普通 Docker 归档，不将宿主机 Docker socket 挂载到作业容器。

### 下位机持久构建缓存

为省去每轮 GitLab cache ZIP 的扫描、归档及恢复，下位机独立 Runner 将宿主机 `/home/daniel/mskdsp-lower-ci/cache-direct` 挂载到作业容器 `/ci-cache`，构建作业设置 `LOWER_CACHE_ROOT=/ci-cache/mskdsp/gcc14-v1`、`LOWER_CACHE_MOUNT=/ci-cache`。脚本在配置和编译前验证挂载存在、缓存根目录位于挂载内且不属于项目工作区；挂载缺失时明确失败，拒绝悄悄写入临时容器。非 CI 的脚本调用未指定变量时仍使用项目 `.cache`。

- vcpkg 源码及其 buildtrees/packages、downloads、二进制 archives 和 ccache 按 `x64`、`arm64-cross` 分开持久保存；项目 `build-<arch>`、package、日志及发布产物仍属于本次作业。
- 两种架构可以并行；同一项目同一架构的构建通过独立 `resource_group` 串行，覆盖依赖准备、配置、编译及安装整个阶段，避免同时 checkout 或修改同一 vcpkg 工作树。
- CI 移除原 ZIP cache 配置，`after_script` 从同一持久根目录读取 ccache 统计。缓存路径和源码复用状态写入中文构建日志，baseline、SHA512 校验、编译参数和完整 CTest 门禁保持原有规则。
- 切换时先暂停下位机专用 Runner 接新作业，等待活动作业结束并备份配置，再将现有两份 ZIP 一次性恢复到新目录并保留文件权限和符号链接。迁移保留源码 Git 库、vcpkg 可执行文件、downloads、archives 和 ccache，跳过可能包含旧绝对路径的 buildtrees/packages 临时目录。原 ZIP 不删除，只有迁移完成后才启用新挂载并恢复接单。后续容器直接访问展开的缓存，不再重新打包；首次命中情况以实际日志为准。
- 工具链或缓存布局不兼容升级时变更 `gcc14-v1` 目录代号；清理旧代目录前须确认没有作业使用，不能在活动构建期间自动删除。缓存仅存在这台 Runner 主机，新加 Runner 必须准备各自的目录。
- ARM64 构建到测试作业的 artifacts 归档传输继续保留，不随构建缓存调整。回退时恢复旧 Runner 配置和原 CI 缓存配置；保留的新目录不参与旧 ZIP 恢复。

验收先覆盖外部缓存路径、缺失挂载拒绝、架构隔离、相同架构资源锁及原脚本回归，再在主分支连续执行两轮双架构 CI，核对挂载、命中统计、完整测试、打包发布及对应缓存恢复/保存阶段消失；同时记录耗时、I/O 延迟和 Runner Docker 超时告警。不通过缩短测试或改变构建并行度混淆对照结果。

下位机 Runner 为 `gitlab-runner-lower`，独立 BuildKit 为 `mskdsp-lower-ci-buildkit`，配置保存在服务器 `/home/daniel/mskdsp-lower-ci/`。BuildKit 使用独立状态卷，通过内网 `tcp://192.168.10.198:1235` 提供 mTLS 服务，并配置 Docker Hub 镜像加速；其网络使用宿主机网络，以避开既有容器网络中的外网访问超时。平台原 Runner、原 BuildKit 的配置和状态卷保持不变。项目 `ci-build` 环境的 `BUILDKIT_HOST` 指向下位机独立实例，证书变量受保护且掩码保存。

服务器直连 GitHub 源码归档入口可能超时；通过 vcpkg 自定义资源下载源将 GitHub tar.gz 归档映射到同一仓库、同一 ref 的官方 codeload 地址。下载器与 vcpkg 均保留 SHA512 校验，不改变依赖版本或校验值。依赖缓存失败时也保存，降低网络重试和首次构建成本。

CI 按配置 baseline 获取并校验 vcpkg 本地仓库，同时保留完整历史，供版本解析读取旧 revision 的端口树；已有浅克隆先补齐历史。随后依赖索引的同一 GitHub 仓库地址仅在当前构建子进程中映射到该本地仓库，避免索引重复联网拉取 HEAD。依赖 baseline、版本和内容校验保持不变；不写全局 Git 配置，不影响项目及协议仓库认证。

## 验收标准

1. 先补元数据、上传续传、发布及流水线规则测试，再实现对应代码。
2. 静态检查和客户端契约测试通过，GitLab CI 配置验证通过。
3. 真实主分支流水线两个架构编译、CTest、打包及发布全部成功，记录测试数量和耗时。
4. 平台管理员软件版本页面可见两个架构安装包及 CI 通道，公开清单的提交、版本、大小、SHA256 与 GitLab 产物一致。
5. 下载安装包重新核对 SHA256；不执行安装脚本，不升级设备。核对既有平台 Runner 和平台服务保持正常。

## 流水线 132 的符号处理顺序修正

ARM64 作业 605 在 `dataCenterGrpcService_test` 链接时报告 `CommandExecutor::Stub` 符号和 DSO 错误。该目标已显式链接协议库，链接命令也包含该库及 `--no-as-needed`，因此不增加重复链接参数。

现有符号分离目标只等待生产目标，排除了所有测试目标；它会原地处理同一份协议共享库，存在与测试程序并行链接的确定竞态。修正为符号处理等待全部非导入可执行文件、共享库和模块目标完成，包括测试目标。符号脚本仍跳过测试程序，不改变符号包内容和架构设置。先用独立最小 CMake 工程验证真实依赖图，要求生产库和测试链接都先于符号处理，并保持测试未被剥离。该竞态是否直接触发本次 DSO 报错，仍以修正后的 ARM64 流水线结果确认。

本地最小 CMake 工程复用真实符号配置块，通过 File API 依赖图和 Ninja dry-run 两项测试：原配置均失败，修复后 2/2 通过。既有 CI 规则测试 10/10 通过。该检查不调用真实编译器、不执行符号分离，不能替代 ARM64 实际链接及完整流水线。
