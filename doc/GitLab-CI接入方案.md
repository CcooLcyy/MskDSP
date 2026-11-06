# 下位机 GitLab CI 接入方案

## 已确认的目标与边界

- GitLab 为下位机代码与 CI 的唯一维护入口，移除旧 GitHub Actions、专用 Runner 部署模板和 R2/静态发布脚本；GitHub 仓库停用 Actions，历史提交仍可追溯。
- 本地仅保留 `gitlab` 远端，`master` 跟踪 `gitlab/master`，普通 `git push` 仅推送 GitLab。协议子模块固定提交保持不变，源地址改为 GitLab。
- master/main 与 `release/X.Y` 的 push 或手动流水线执行完整验证；Beta 分支和 MR 不触发。受保护 `vX.Y.Z` 标签独立运行完整验证后发布 Stable，见《GitLab-Stable标签发布方案》。
- ARM64、x64 均完成 GCC14 RelWithDebInfo 编译；2026-10-24 起 x64 编译与测试拆成独立作业，只有 `test-x64` 执行现有完整 CTest。ARM64 在 x64 上交叉编译，不编译或执行 ARM64 测试程序，不再创建 ARM64 测试作业。
- 下位机与通用 Linux Runner 共用容量上限为 24 GiB 的内存工作区，标签、工程目录和持久缓存分开；下位机接单并发为 2、通用 Linux 接单并发为 1，单作业 4 CPU、24 GiB 内存且禁止交换。Windows Runner 不接入。
- CMake 主工程及 vcpkg 默认并行度为 4，可分别通过 `CMAKE_BUILD_PARALLEL_LEVEL`、`VCPKG_MAX_CONCURRENCY` 设置正整数。两个 Linux Runner 的主脚本通过共享编译槽最多并行执行两个，CTest 并行度仍为 2；实际 I/O 改善以真实流水线测量为准。
- 保留 Docker 镜像、自解压 gzip 安装包、SHA256SUMS；ARM64、x64 均生成独立调试符号包，随各自构建归档保留 30 天。
- 不执行独立的 check_iec61850_package.sh；不减少已有协议单测。
- 安装包经 gRPC 上传平台软件库，校验完成后自动发布对应架构 CI 通道；不安装设备，不触发 R2 或 GitHub 发布。
- 主干发布 CI；维护分支仅保留 GitLab 产物，不覆盖平台全局 CI。停止新 Beta 发布；Stable 从受保护标签重新构建，不使用 CI 包改通道晋升。

## 作业与身份

阶段顺序为 `build → test → package → publish`：`build-x64`、`build-arm64` 编译，`test-x64` 独立测试，随后双架构打包和发布。两个架构使用同一套构建脚本和不同工具镜像，各自产出 release/<arch>/ 下的安装包、metadata.json、SHA256SUMS；独立符号位于 symbols/<arch>/，日志位于 logs/<arch>/。JUnit 报告仅由 `test-x64` 生成。两个打包作业均以 x64 完整测试成功为门禁，再完成双架构打包后执行发布，避免失败流水线发布另一个架构。发布作业不重新编译。

### x64 全量测试与 ARM64 交叉编译

ARM64 在独立 x64 工具镜像中使用 GCC14 的 aarch64 交叉工具链编译。按用户确认，取消原有 QEMU ARM64 完整测试，统一由原生 x64 完整 CTest 提供逻辑回归门禁；ARM64 交叉配置使用 `MSKDSP_BUILD_TESTS=OFF`，x64 保持 `ON`。CI 不再提供 ARM64 执行验证，目标架构运行行为需另行验证。

- 交叉镜像基于既有 x64 工具镜像，额外安装 GCC14 aarch64 交叉编译器；不覆盖现有平台工具镜像。
- vcpkg 明确区分 host=x64 与 target=ARM64，protoc 与 grpc_cpp_plugin 由 host triplet 提供；目标库及主程序使用同一交叉工具链。
- 交叉镜像默认主机 `cc/c++` 必须为 x64 GCC14；交叉编译入口清除继承的 `CC/CXX`，避免 OpenSSL 等 make 端口沿用主机编译器而忽略目标工具链。
- 使用独立交叉 triplet、chainload toolchain 与缓存版本，避免复用旧 QEMU 编译器生成的 ARM64 二进制缓存。
- `build-x64` 仅编译、安装、归档独立符号并记录原构建身份和绝对路径上下文；上传完整构建目录及测试程序，排除中间 `.o` 文件和已有压缩归档的原始 `package/debug/**`。`test-x64` 在相同绝对路径校验流水线、提交、仓库和上下文后执行全部 CTest，拒绝零测试，成功后清理交付目录中的测试程序；不重新编译或安装。失败时保留测试日志，两个架构均不得打包或发布。
- `package-x64` 只下载 `test-x64` 的产物；`package-arm64` 只下载 `build-arm64` 的产物，对 `test-x64` 使用 `artifacts: false`，防止同名 `package/` 被 x64 文件覆盖。
- ARM64 安装及独立符号归档在交叉编译作业内完成，保留原 `build_job_id`。取消 ARM64 路径上下文与独立 ARM64 测试入口；原生 ARM64 构建入口也拒绝。独立 `test` 模式仅接受 x64。
- 独立安装脚本初始化 CMake 策略，提前查询并收集目标 GCC 运行库；ARM64 glibc 及加载器由 Ubuntu 基础镜像提供，其他缺失依赖仍拒绝交付。
- 验收覆盖仅 x64 测试、交叉编译不调用 CTest、符号包生成、x64 失败阻断双架构交付及产物架构隔离。实际流水线另核对交叉编译器版本、x64 完整 CTest、双架构打包发布和 SHA256；不得将这些结果表述为 ARM64 测试通过。

### 双架构作业产物精简

取消 ARM64 测试作业后，构建目录不再向下游传递；保留打包所需运行文件、构建身份及独立符号包，不删除构建工作区或持久缓存：

- 两架构均在构建作业生成 `symbols/$ARCH/*debugsymbols*.tar.gz`、符号 SHA256SUMS 和原始构建身份；构建归档保留 30 天，符号缺失、为空或校验失败时拒绝成功。延长保留期会同时保留构建归档内的运行文件和测试依赖。
- `build-arm64` 上传 `package/`、`release/arm64/`、`logs/arm64/`、`symbols/arm64/`，排除重复的 `package/debug/**`；不上传整个构建目录，也不生成 ARM64 JUnit 报告。
- x64 全量测试、测试数量和测试工作区规则不变，测试在已分离符号的生产库上执行。安装包 SHA256SUMS 与符号 SHA256SUMS 分开保存；打包不消费符号，发布归档不包含符号。GitLab needs 仍下载整个上游归档，因此 x64 测试和 ARM64 打包仍会收到各自构建归档中的压缩符号，但不会将其再次向后上传。
- `build-x64` 上传 `package/`、`release/x64/`、`logs/x64/`、`build-x64/`、`symbols/x64/`，排除 `build-x64/**/*.o` 和 `package/debug/**`；`test-x64` 上传运行产物、原身份、测试日志和 `build-x64/Testing/`，排除原始 debug、不再上传 symbols，提供 `logs/x64/ctest.xml` 报告。测试作业不声明构建缓存变量或架构资源锁，主脚本继续使用公共内存工作区的两个编译槽。
- 打包作业归档仅含 `release/$ARCH/mskdsp-*` 安装包、metadata.json、安装包 SHA256SUMS 和 `logs/$ARCH/package.log`；发布不再取回符号和旧构建/测试日志。仅发布使用 `GIT_DEPTH=1`，构建仍保留完整历史，远端 HEAD 及冻结来源校验不变。

验收检查 x64 编译阶段不执行测试、独立测试作业的来源校验和失败门禁、两种架构的产物隔离及符号包收集；主分支 CI 再核对 x64 完整 CTest、符号包可解包、打包发布及 SHA256 完整性。大小和耗时收益以真实归档为准。

此前独立 x64 测试改造的本地验证：CI 配置测试 10/10、WSL Bash 编排测试 29/29 通过，覆盖 x64 编译与测试分离、来源及绝对路径拒绝、ARM64 符号包和 SHA256、失败门禁及诊断保留；另有元数据、交叉工具链及镜像配置检查 7 项通过，2 项因缺少 CMake 或未启用真实 Docker 镜像验证跳过。共享内存及发布交接优化的契约与验收记录见《CI共享内存构建方案》。

CI 完整测试的临时工作目录、系统临时目录和 CTest 过程日志使用作业独立的内存文件系统，避免缓存和大产物归档的共享磁盘争用触发 SQLite/SOE 处理等待超时或日志管道反压。内存目录使用短路径，保留 Unix socket 的路径长度余量。CTest 仍执行全部用例，保留原有断言、超时、并行度与失败门禁；测试种子文件读取磁盘原件，模块种子目录整体链接到磁盘以保留共享库的普通文件属性，避免内存挂载的 noexec 影响模块加载。成功及失败后均恢复原工作目录，并将 CTest 日志归档回磁盘。下位机 Runner 的 `/dev/shm` 上限为每作业 1 GiB，计入作业的 24 GiB 内存限制；它与所有 Linux 作业共用的 24 GiB `/builds` 卷是两个不同的文件系统。

所有隔离测试的工作目录均位于 `build-<arch>/test_env/`，包括模块接口测试；避免特殊工作目录绕过 CI 内存隔离。

metadata.json 冻结 product、version、package_version、platform、file_name、size、sha256、image_id、source_repository、source_ref、source_sha、build_time、channel 及 pipeline_id。product 固定 mskdsp-lower；platform 为 linux-arm64/linux-x64；channel 固定 ci。版本包含 GitLab 流水线及作业身份，防止重试产生不同字节却复用同一版本。

发布前核对实际文件大小和 SHA256、CI 来源及远端主分支提交，拒绝落后的构建更新通道。按架构串行发布；已成功上传的文件可查询偏移续传，CompleteUpload 成功后才调用 PublishRelease，并核对公开目录及清单。平台负责生成 latest.json 和下载 SHA256SUMS。

## 协议、授权和镜像

软件仓库协议从公开 GitHub 仓库 `https://github.com/CcooLcyy/MskDSPPlatformProto.git` 按固定提交获取到临时目录，固定提交不变；该获取过程无需 GitLab 作业令牌，清除继承的 Git 认证头配置，不向 GitHub 发送 GitLab 凭据。下位机 protobuf 子模块也已指向 GitHub，实际地址以 `.gitmodules` 为准。旧 GitLab 脚本共用的 `auth_environment` 保留，避免改变发布来源校验及历史流程的认证行为。

引用迁移验收：无 `CI_SERVER_URL`、`CI_JOB_TOKEN` 也能获取固定平台协议；继承的认证头不带入 GitHub 请求；固定提交校验、既有目录保护及共用 GitLab 认证工具行为保持不变。对应脚本回归应由 CI 执行，本地不运行构建或测试。

平台上传使用已有通用 publisher 令牌，保存在下位机项目受保护、掩码的 CI 变量中；不复用仅允许平台自身包的 CI 上传令牌。客户端不输出令牌、认证头或底层 RPC 详情。现有平台业务代码不需要修改。

编译与测试复用已准备的 Ubuntu 24.04 x64 工具镜像及 ARM64 交叉工具镜像，不使用 ARM64 测试镜像；按仓库 baseline 初始化 vcpkg，并按架构隔离 vcpkg 和 ccache。镜像打包通过独立 BuildKit 服务导出普通 Docker 归档，不将宿主机 Docker socket 挂载到作业容器。

### 下位机持久构建缓存

为省去每轮 GitLab cache ZIP 的扫描、归档及恢复，下位机独立 Runner 将宿主机 `/home/daniel/mskdsp-lower-ci/cache-direct` 挂载到作业容器 `/ci-cache`，构建作业设置 `LOWER_CACHE_ROOT=/ci-cache/mskdsp/gcc14-v1`、`LOWER_CACHE_MOUNT=/ci-cache`。脚本在配置和编译前验证挂载存在、缓存根目录位于挂载内且不属于项目工作区；挂载缺失时明确失败，拒绝悄悄写入临时容器。非 CI 的脚本调用未指定变量时仍使用项目 `.cache`。

- vcpkg 源码及其 buildtrees/packages、downloads、二进制 archives 和 ccache 按 `x64`、`arm64-cross` 分开持久保存；项目 `build-<arch>`、package、日志及发布产物仍属于本次作业。
- 两种架构可以并行；同一项目同一架构的构建通过独立 `resource_group` 串行，覆盖依赖准备、配置、编译及安装整个阶段，避免同时 checkout 或修改同一 vcpkg 工作树。
- CI 移除原 ZIP cache 配置，`after_script` 从同一持久根目录读取 ccache 统计。缓存路径和源码复用状态写入中文构建日志，baseline、SHA512 校验、编译参数和完整 CTest 门禁保持原有规则。
- 切换时先暂停下位机专用 Runner 接新作业，等待活动作业结束并备份配置，再将现有两份 ZIP 一次性恢复到新目录并保留文件权限和符号链接。迁移保留源码 Git 库、vcpkg 可执行文件、downloads、archives 和 ccache，跳过可能包含旧绝对路径的 buildtrees/packages 临时目录。原 ZIP 不删除，只有迁移完成后才启用新挂载并恢复接单。后续容器直接访问展开的缓存，不再重新打包；首次命中情况以实际日志为准。
- 工具链或缓存布局不兼容升级时变更 `gcc14-v1` 目录代号；清理旧代目录前须确认没有作业使用，不能在活动构建期间自动删除。缓存仅存在这台 Runner 主机，新加 Runner 必须准备各自的目录。
- ARM64 构建产物直接传递到打包作业，持久缓存仍只由构建作业使用。缓存方案回退时恢复旧 Runner 配置和原 CI 缓存配置；保留的新目录不参与旧 ZIP 恢复。

验收先覆盖外部缓存路径、缺失挂载拒绝、架构隔离、相同架构资源锁及原脚本回归，再在主分支连续执行两轮双架构 CI，核对挂载、命中统计、完整测试、打包发布及对应缓存恢复/保存阶段消失；同时记录耗时、I/O 延迟和 Runner Docker 超时告警。不通过缩短测试或改变构建并行度混淆对照结果。

下位机 Runner 为 `gitlab-runner-lower`，独立 BuildKit 为 `mskdsp-lower-ci-buildkit`，配置保存在服务器 `/home/daniel/mskdsp-lower-ci/`。BuildKit 使用独立状态卷，通过内网 `tcp://192.168.10.198:1235` 提供 mTLS 服务，并配置 Docker Hub 镜像加速；其网络使用宿主机网络，以避开既有容器网络中的外网访问超时。通用 Linux Runner 同时接入共享内存工作区，平台与下位机 BuildKit 的持久状态卷保持不变。项目 `ci-build` 环境的 `BUILDKIT_HOST` 指向下位机独立实例，证书变量受保护且掩码保存。

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

## 流水线 138 的异步测试等待修正

x64 作业 634 编译通过，但 MMS 最小会话测试在等待 READY 两秒后失败，随后收到 READY；保护动作异常测试在固定 100 次一毫秒轮询结束后失败，随后发送线程才记录发布异常。先以这两个既有失败用例作为回归入口，修正测试等待方式；当前日志支持调度延迟的判断，不据此修改生产线程或协议行为。

- MMS 最小会话用例改用统一的 READY 等待辅助函数，以条件变量谓词等待，上限十秒；条件满足立即继续。超时输出已收到的事件数量、最后状态及错误。本轮只接入该失败用例，其他 MMS 测试等待及协议自身超时、请求次数、通道和结果断言保持原有要求。原用例注入 2500 毫秒连接延迟，覆盖超过旧两秒等待预算的场景。
- 保护动作异常测试采用单调时钟十秒截止时间，持续读取真实完成队列；发布回调开始或抛异常均不能代替完成入队。原回调注入 200 毫秒延迟，覆盖超过旧轮询预算的场景。保留失败结果和发送线程仍在运行的断言，超时补充实际等待耗时、运行状态及发送失败统计。
- 只修改测试和本文档，不增加生产接口、不改生产逻辑、不跳过用例或重试整个测试。先明确本方案，再修改现有测试；本机缺少下位机编译环境，不能本地重放 CI 调度负载，因此不另建工具链，运行验证交由现有 CI 环境。

验收要求：两个原失败用例重复运行通过，x64 与 ARM64 全量 CTest 通过且用例数不减少。静态检查确认最小会话 READY 使用统一等待、保护异常用例只以完成队列结果结束等待，保留全部业务断言；记录实际测试命令、次数和结果，不以静态检查代替编译或运行验证。

本轮验证：使用 `mskdsp-ci-tools:ubuntu24.04-x64`，只读挂载提交 `d169117` 的 CI 构建缓存，按原 Ninja 编译、链接参数重编两个测试源文件，输出到独立临时目录。相同注入延迟下，旧等待逻辑两个用例各运行一次均失败，修复后各连续五次通过。两个受影响可执行文件完整运行：原始协议栈 93 项中 89 项通过、4 项因环境条件跳过；保护引擎与动作分发 13 项全部通过。修改行格式检查和 `git diff --check` 通过。尚未运行新提交的完整 x64/ARM64 流水线，不能将上述结果视为全量 CTest 或真机验证。
