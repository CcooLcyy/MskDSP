# IEC61850 模块

## 简介

IEC 61850下位机动态模块。模块负责SCD/CID/ICD模型导入、IED配置与生命周期，并在统一模块内隔离MMS、GOOSE、SV和DataCenter发布路径。

当前阶段已提供模型和控制面底座；MMS已经接入自研建链、在线目录、RCB/GI和报告工作器，GOOSE接收计划由ExtRef编译，本地发布计划由目标IED的GSEControl、DataSet和ConnectedAP独立编译，真实设备互操作和高负载验收仍在逐步补齐。

### SV发布器阶段边界（2026-08）

当前补全阶段先固化SV发布报文编码契约，再接入现有AF_PACKET发送器和实时计划。编码器输入必须来自已校验的`ProtocolSvStreamPlan`：`svID`、`ConfRev`、`smpRate`、`nofASDU`、每个ASDU的`smpCnt`/`smpSynch`以及DataSet成员顺序均按计划原样编码。输出为包含APPID、PDU长度、`SavPdu[0]`、`noASDU[0]`、`seqASDU[2]`和ASDU上下文标签的SV二层载荷，不包含Ethernet/VLAN头；发送生命周期、A/B端点和停止回滚由协议栈运行时负责。

编码器拒绝空标识、零ConfRev/采样率/ASDU数、成员数量不一致、非法`smpCnt`序列和非有限浮点值；输出缓冲不足时保持`outputSize=0`。编码后的载荷必须可被现有`DecodeSvPayload`完整回放，作为离线互操作基线。真实网卡、双网冗余、采样时序抖动和目标IED验收不由离线编码测试宣称覆盖。

## 已实现能力

- SCL默认命名空间和通信模型解析。
- MMS、GOOSE、SMV地址参数解析。
- IED、AP、LD、LN、DataSet、FCDA、ReportControl、GSEControl、SampledValueControl和Inputs/ExtRef解析。
- LNodeType、DOType和DAType展开为稳定数据引用。
- Server下的LN、DO、DA、DataSet、控制块和ExtRef保留AccessPoint归属，多AP模型不会合并进入同一协议启动计划。
- AccessPoint名称在同一IED内必须非空且唯一，空名称不会被当作旧模型兼容归属。
- SCL仅校验、导入、摘要查询和删除接口。
- IED与A/B通道配置接口。
- 点映射配置接口。
- 聚合配置SQLite持久化。
- DataCenter稳定连接和连接标签同步。
- DataCenter `CommandExecutor` 同步命令入口：按目标IED和点映射转为MMS控制，支持直控、普通SBO和增强SBOw的选择-执行闭环。
- DataCenter同步命令的`timeout_ms`作为整次MMS控制序列（包括SBO选择、Oper和增强安全CommandTermination等待）的上限；未设置时继续使用在线`sboTimeout/operTimeout`和兼容默认值。
- 每个串行MMS控制请求保存绝对截止时间，队列等待、Confirmed响应和CommandTermination只消费同一份剩余预算，不因阶段切换重复延长调用方超时。
- CommandExecutor会把gRPC server deadline与请求`timeout_ms`取较小值传入MMS控制；执行期间请求取消不会把已发送的MMS报文伪报为成功。
- CommandExecutor为每次RPC建立独立的取消状态；服务线程只观察当前`ServerContext`，MMS Worker只读取共享原子状态。请求取消会取消排队项并中止确认交换，已发送但未确认的控制按不确定结果处理。
- 控制请求超时后，尚未发送的队列项必须取消并不得迟发；已经发送但未确认的控制不得伪报成功，按结果不确定处理并保守锁定控制对象，同时关闭当前MMS会话并按完整目录、RCB和GI流程重建，隔离迟到响应。
- SBO选择成功但总截止时间已耗尽且尚未发送Oper时，清除本地选择保持并返回超时；远端选择保持由IED自身`sboTimeout`回收。
- 可替换协议栈适配接口及IED通信功能启停状态机。
- Manager按 `ied_name + access_point` 构造单IED协议启动计划，协议栈只接收按目标AP裁剪后的IED模型和匹配的ConnectedAP通信记录。
- MMS报告内部值、品质和时标适配接口。
- MMS在线控制能力模型及当前会话内SBO选择保持、Oper/Cancel前置校验。
- MMS控制命令按在线`sboTimeout/operTimeout`选择等待窗口，缺失参数时使用兼容默认值。
- 在线`ctlModel`必须是`INTEGER`、`sboTimeout/operTimeout`必须是`UNSIGNED`，其编码Data标签也必须一致；普通SBO Read只接受单个非空VisibleString，并核对返回对象引用。
- MMS专用控制入口必须命中当前在线能力模型；`ctlModel=0`不可控、`1/3`只允许直接Oper、`2`要求普通SBO、`4`要求增强SBOw，空模型和成员矩阵不一致不会降级放行。
- 控制结构中的`ctlNum`按无符号8位编码；Cancel只携带`origin/ctlNum/T/Test/Check`，不携带`ctlVal/operTm`，单项控制Write只接受单个成功响应项。
- Initiate始终提出Write以支持受控通用写入，并按物理会话保存实际协商结果；未协商时通用Write在入队前拒绝，启动计划含RCB或FC=CO时缺少Write的通道不会进入在线目录或READY。
- 专用控制的能力/SBO校验和请求入队绑定同一A/B物理会话；入队时原子复核活动READY通道，避免切换窗口跨会话发送。
- `ctlModel=3/4`的Oper在收到Write成功确认后仍等待同一物理会话的CommandTermination；入队后同一对象只能有一个执行中请求，仅在终止报告明确包含目标`$Oper`且结果成功时完成控制。终止报告带有`LastApplError`时，`ctlModel=4`且在线存在`$Cancel`才保留SBO选择并标记待Cancel，禁止再次Oper；`ctlModel=3`因没有`$Cancel`，在错误已完整核对后释放本地执行占用，允许下一次独立Oper；`ctlModel=4`缺少`$Cancel`时直接标记结果不确定并锁定对象。对象、ctlNum、结构或会话无法确认时同样标记结果不确定并锁定对象。
- 等待CommandTermination期间，当前收发线程优先处理同一对象的显式Cancel；Cancel成功后原Oper返回取消状态并释放执行占用，未完成的Cancel不改变原Oper的不确定性保护。
- 控制状态表达到容量上限且无法记录新的不确定对象时进入全局保守锁定；该锁定只在MMS会话重建清空状态后解除。
- `operTm`在`ctlModel=3/4`下与增强安全Oper一样等待CommandTermination；`ctlModel=3`的明确LastApplError释放执行占用，`ctlModel=4`可在等待期间显式Cancel；`ctlModel=1/2`以Write成功作为协议终态，不建立永久pending。断线、重连和停止会清除待完成/不确定状态，禁止跨MMS会话继续使用旧控制上下文。
- 首期配置化保护/联锁规则引擎：稳定引用解析、质量/新鲜度检查、动作延时、GOOSE动作确认和失败重试。
- SV首期支持单ASDU数值流的单周期RMS派生量；派生信号使用`SV_DERIVED/<stream>/RMS/<input>`稳定引用，并在实时消费者线程进入保护引擎。
- 保护动作使用预分配单生产者/单消费者队列；实时消费者只入队，独立发送线程调用协议栈，完成结果回到实时线程确认。
- 按IED隔离的MMS有界事件队列、点映射转换和DataCenter批量发布线程。
- 运行统计查询接口骨架。
- MMS未映射、类型不匹配、无效工程量和死区过滤的分项运行统计。
- GOOSE/SV创建AF_PACKET套接字时，Linux返回`EPERM/EACCES`会转换为
  `PERMISSION_DENIED`，便于目标板区分缺少`CAP_NET_RAW`与网卡不可用；其他系统错误仍返回
  `UNAVAILABLE`。
- MMS文件客户端支持`FileDirectory`目录分页和`ObtainFile`下载流程。下载流程在协议层
  内部依次使用`FileOpen`、一个或多个`FileRead`和`FileClose`，不把服务端文件句柄暴露给
  上层调用方；文件内容先写入调用方指定路径旁的临时文件，全部读取成功后再原子改名。
  目录路径、远端文件名和本地目标路径拒绝空值、父目录穿越和目录目标，文件大小、上传
  分片和PDU均受有界上限约束，超时或取消会尽力关闭远端句柄并清理临时文件。
- `MmsFileClient::Upload`按`InitiateDownload`、`DownloadSegment`、`TerminateDownload`
  完成下位机上传；`DeleteFile`和`RenameFile`执行路径检查、取消、超时和响应服务选择核对。
  `mvl_defs.h`中标注为未实现的厂商`InitiateDownload`声明没有被当作功能依据。
- `MmsJournalClient`提供`JournalStatus`、`InitializeJournal`和`ReadJournal`，支持时间范围、
  EntryID续读、moreFollows分页、共享绝对截止时间和取消；输出保留EVENT/ALARM/SOE/ANNOTATION
  结构和变量ReasonCode。`MmsDynamicDataSetClient`提供运行时Named Variable List创建/删除，
  通过能力开关拒绝未协商服务，并在远端校验或RCB绑定失败时回滚。
- 上述新增事务目前是可复用的MMS客户端适配层，尚未接入`MmsSessionWorker`的公开业务入口；
  Worker的Initiate支持位继续只声明已实现的目录、Read/Write、RCB和文件读取服务。当前仓库
  的`MmsServer`只提供无监听器的Confirmed PDU/Initiate模型分发器，供COTP/Session适配层和
  协议测试复用；它不等同于生产MMS TCP Server，控制、报告/RCB和真实客户端并发断线验收仍待
  后续阶段完成。

### DataCenter同步命令边界

DataCenter调用IEC61850模块的 `CommandExecutor.ExecuteCommand` 时，目标端点必须包含
`conn_name`（或当前有效的 `conn_id`）和点 `tag`。点映射必须是 `POINT_SOURCE_MMS`；
FC=CO的点按控制对象处理，控制对象由 `data_ref` 去掉末尾 `.ctlVal` 后转换为
MMS Domain/Item引用。当前命令值支持BOOL、INTEGER和FLOATING-POINT标量，工程量点按
`scale/offset`反向换算，BOOL忽略换算参数。

命令执行策略由在线 `ctlModel` 决定：直控模型直接发送 `$Oper`；普通SBO先读取
`$SBO`再发送`$Oper`；增强SBOw先写入`$SBOw`再发送`$Oper`。每一步都复用当前活动
MMS通道的串行控制队列，并以IED的协议确认作为 `COMMAND_ACCEPTED` 的依据。目标未运行、
点映射不存在、值类型不匹配、质量为坏/不确定或MMS确认失败时返回明确的
`COMMAND_REJECTED`、`COMMAND_TARGET_UNAVAILABLE`、`COMMAND_TIMEOUT`或
`COMMAND_INTERNAL_ERROR`，不更新DataCenter命令缓存。该入口运行在普通gRPC线程，
不进入GOOSE/SV实时回调和保护动作线程。

## 接口与协议

- Protobuf：`protobuf/IEC61850.proto`
- gRPC Service：`IEC61850Proto::IEC61850Service`
- 完整设计：`doc/IEC61850下位机实现方案.md`

业务RPC包括：

- `ApplyTargetConfig`
- `ImportScl`、`GetModelSummary`、`ListModels`、`DeleteModel`
- `UpsertIed`、`GetIed`、`ListIeds`、`DeleteIed`
- `StartIed`、`StopIed`
- `UpsertPointMappings`、`GetPointMappings`
- `GetRuntimeStatistics`

## 运行与地址

- 启动IEC61850模块：由ModuleManager加载 `libIEC61850.so`。
- 启动IED通信功能：调用 `StartIed`，与启动模块不是同一操作。
- 对外gRPC：随机选择 `0.0.0.0:<port>`（17001–17999）。
- 内部gRPC：`unix:./socket/IEC61850.sock`。
- 运行时通过ModuleManager `GetRunningModuleInfo`查询实际地址。

## 配置与数据

- 模块运行配置由ConfigPusher或上位机通过gRPC下发。
- 模块不直接读取 `package/conf/configPusher/iec61850.jsonc`。
- SQLite数据库：`./conf/config.db`。
- 模块名：`IEC61850`。
- 动态库导出统一 `create()` 和 `GetModuleManifestPb()`；manifest不声明DataCenter硬依赖，避免DataCenter暂时不可用时阻止GOOSE/SV内部功能启动。
- 聚合配置键：`config`。
- 一个逻辑IED对应一个DataCenter连接；A/B网是该IED下的传输通道。
- `IedConfig.realtime_cpu_indices` 配置实时线程的 CPU 亲和性；为空时不调用亲和性设置接口，保留系统默认 CPU 集合。配置阶段拒绝重复索引、超出 `CPU_SETSIZE` 的索引以及超过集合上限的列表；目标 CPU 是否在线、是否被进程允许使用，由线程启动时的 Linux 接口最终判定。
- `IedConfig.realtime_scheduling` 支持 `UNSPECIFIED`、`FIFO`（对应 Linux `SCHED_FIFO`）和 `ROUND_ROBIN`（对应 `SCHED_RR`）。未指定时使用 `SCHED_OTHER`；`realtime_priority` 在普通调度下必须为0，在 FIFO/RR 下必须落在目标系统报告的实时优先级范围内。
- `IedConfig.realtime_failure_mode` 支持 `DEGRADE` 和 `STRICT`；未指定时按 `DEGRADE` 处理。该策略用于 GOOSE/SV 接收、GOOSE 重发/超时、实时消费者和保护动作发送线程，不改变 MMS 会话或 DataCenter 发布线程的普通调度策略。
- 协议栈MMS连接事件携带IED聚合状态、活动通道和完整A/B状态快照；Manager校验后原子替换，并按会话代际拒绝停止或重启后的迟到事件。
- `StartIed`成功只表示会话引擎已启动；启用MMS但未收到连接快照时保持启动中，不假报连接。MMS通道 `CONNECTED` 后仍需完成目录核对、RCB启用和首次GI准备，协议栈报告 `READY` 后IED才进入运行态。
- MMS适配器必须按启动计划核对在线目录、RCB类型/DataSet/ConfRev和启用结果；启用GI的ReportControl必须启用ReasonCode，只有明确GI原因且完整覆盖DataSet的首次GI（含分段合并）完成后才报告 `READY`。目录、ConfRev、GI原因或成员覆盖不一致时不得静默降级继续运行。
- MMS通道尚未 `READY` 时，只有当前待完成GI且契约核对成功的完整报告可以继续处理；普通变化、品质、更新报告以及GI核对失败的报告直接丢弃并记录中文原因，不能提前进入内部实时数据链。
- `active_channel`只表示MMS活动通道，GOOSE/SV双网不复用该字段。
- 断线进入降级态，A/B切换通过单份完整快照生效；`reconnect_count`统计协议栈报告的重连尝试次数，而不是连接成功次数。
- 活动A通道明确断线后，B通道必须重新取得RCB配置权、执行完整RCB Write和GI；B完成GI前只能保持 `CONNECTED`，不能复用A会话的 `READY` 状态。
- DataCenter不可用时保留本地目标配置；GOOSE/SV内部实时路径不得依赖DataCenter。
- DataCenter连接删除失败时保留IED删除墓碑，由启动时和周期对账自动重试；外部连接删除成功或已不存在后才清理本地IED和点映射。
- MMS发布参数为0时使用默认值：队列4096个待处理点值、批量256点、窗口20毫秒；显式配置上限分别为65536个待处理点值、4096点和1000毫秒。
- MMS固定限制单个STRING/BYTES为256 KiB、单份报告逻辑保留内存为4 MiB、每IED队列逻辑保留内存为16 MiB、单个DataCenter批次序列化大小为3 MiB；报告按固定开销加实际内容字节计算，队列和发布批次同时受点数与字节数约束。
- MMS按IED使用独立发布工作线程；死区基准只在DataCenter成功接受后推进，品质变化不受数值死区过滤。
- DataCenter重新分配 `conn_id` 时使旧在途批次和死区基准失效，后续报告中的相同值可向新连接发布首值。
- GOOSE二层解码必须要求APPID载荷声明的PDU长度与实际接收载荷完全一致，并要求必需的`gocbRef`、`timeAllowedToLive`、`datSet`、`goID`、`UtcTime[4]`、`stNum`、`sqNum`、`test`、`confRev`、`ndsCom`、`numDatSetEntries`和`allData`各出现一次；缺少或重复字段、UtcTime长度错误、截断TLV和超出固定缓冲的字段均拒绝，完整帧再交给GOOSE实时状态机执行身份、TTL、序号和A/B去重校验。
- GOOSE `allData` 的浮点成员必须按 `0x87` + `format-width` + IEEE-754大端值编码：FLOAT32使用`0x08`并跟随4字节，FLOAT64使用`0x0B`并跟随8字节；宽度由启动计划固化，缺少format-width、宽度不匹配、非有限值或非法长度时拒绝整帧，禁止使用裸4/8字节浮点编码。
- GOOSE发布默认按1/2/4/8毫秒指数曲线重发，间隔上限为1000毫秒；状态变化或显式发布会重置重发曲线，停止IED通信功能时先停止重发线程再关闭发布器。
- SCL `smpRate`按每额定周期采样数解释，绝对采样率为`smpRate * nominalFrequencyHz`；IED配置`nominal_frequency_hz`为0时默认50Hz，显式值只允许50Hz或60Hz，数学窗口上限4096点。
- SV二层解码必须要求APPID载荷长度与实际接收载荷一致，强制核对SavPdu的`noASDU [0]`与`seqASDU [2]`及实际ASDU数量，并拒绝重复的外层/ASDU必需字段；通过结构校验后才交给采样计数状态引擎。
- SV ASDU按IEC 61850-9-2校验上下文标签：`refrTm [5]`（`0x85`）必须为8字节UtcTime，`smpRate [6]`（`0x86`）必须与启动计划一致；不能把两个字段互换或按普通采样值继续解析。
- 协议栈启动前必须校验GOOSE订阅和SV采样流的身份、端点和唯一ID；每个启用的订阅/采样流至少要有一个有效二层端点，重复ID、空控制引用、空DataSet、空成员或空网卡地址不得启动。校验失败必须在创建线程和套接字前返回，不能进入假运行态。
- DataCenter发布适配层抛异常时会清理在途Context、计入发布失败并继续处理后续报告。
- 停止IED通信功能保留累计统计；真正删除IED后释放其MMS线程、队列、映射和统计。
- 停止IED通信功能时先使MMS报告入口和会话令牌失效，并先收敛GOOSE/SV实时消费者及保护动作发送线程，再调用协议栈停止；两项清理都执行。即使协议栈停止失败，迟到报告也不得计数或发布，最后连接快照和活动协议栈会话会保留用于诊断及重试停止。
- 启动IED通信功能采用事务语义：MMS、GOOSE或SV任一阶段失败时，必须按“先停止MMS工作器、再请求GOOSE/SV线程退出、等待线程收敛、最后关闭二层套接字和发布器”的顺序回滚已成功创建的资源；失败返回后不得留下工作线程、报告回调或可占用的网卡套接字，同名IED必须可以再次启动。
- 保护动作发送器的 `Start/Stop` 由独立生命周期锁串行化；发送线程异常退出时会停止接收新动作、丢弃无法回传的在途完成项并记录统计，MMS配置失败和保护线程启动失败也必须走统一实时运行时回滚，不能只清除指针。
- 停止MMS入口会取消在途DataCenter调用；1.5秒内未收敛返回超时并让IED进入错误态，不假报停止成功。协议栈已停止时清除MMS网络快照；协议栈停止失败时优先返回其错误并记录入口清理诊断。
- 协议栈会话和MMS管线分别记录待停止状态；MMS尚未收敛时重复停止继续返回错误，调用返回后可再次停止完成收敛。重复模块关闭也会重试遗留会话。
- IED即使处于错误态，只要协议栈会话或MMS管线仍活动，就禁止修改IED配置和点映射，必须先重试停止并完成资源收敛。
- MMS报告回调携带会话代际令牌并弱引用管线状态，停止、重启、删除或Manager析构后的旧回调不会污染新会话。
- 配置阶段交错到达的未确认MMS报文使用5秒有界等待窗口和256份/4MiB暂存上限；超过上限按先进先出丢弃，不使用固定32份报文作为确认等待上限。

## 协议栈状态

当前默认适配器的MMS建链使用项目内自研ISO-on-TCP/ACSE/Initiate工作器；旧项目找到的 `libiec61850comm.so` 未被引用。已补充独立的Confirmed PDU、Confirmed-ErrorPDU、GetNameList、GetVariableAccessAttributes、GetNamedVariableListAttributes、Read和Write请求/响应编解码，以及在线Domain/Named Variable/Named Variable List分页读取、DataSet成员顺序、基础目录类型和RCB根对象Data.structure核对。会话工作器已接入唯一通道的RCB三阶段Write、独立GI Write、基础InformationReport解码和分段接收；A通道在B建链前失败时，B会重新排队接管RCB配置；受控MMS Read/Write入口已由Manager转发到活动通道串行队列，READY只在所有RCB启用及GI报告完成后发布。另提供无监听器的MMS服务端模型分发器，可在COTP/Session上层交付Initiate和Confirmed PDU，按模型实际回调协商能力并处理NameList、Read/Write、动态NVL、Journal Read和FileDirectory；尚未接入生产TCP监听、控制、报告/RCB服务。脚本化Worker集成测试同时覆盖多个RCB，确认各RCB必须分别完成配置和GI后才能进入READY。

真实协议栈必须通过 `ProtocolStackAdapter` 接入，不能把厂商API扩散到模型、Manager、gRPC或DataCenter代码。本项目优先采用自研的 ISO-on-TCP、COTP、Session、Presentation、ACSE 和 MMS 编解码，不直接引入开源 IEC 61850 实现；当前已补充独立的 Session/Presentation/ACSE 报文边界层以及有界 `InitiateRequest/InitiateResponse` 编解码器。后续如因目标IED互操作性确需外部协议库，必须单独确认许可证、目标架构和发布方式。

MMS传输层先独立实现 A/B 通道的本地地址绑定、连接超时、TPKT/COTP 建链、EOT分段重组和报文十六进制日志。配置了 `interface_name` 时严格使用 `SO_BINDTODEVICE` 绑定对应网卡；未配置时由系统路由选择出口，便于不需要固定接口的TCP联调和受限运行环境。GOOSE/SV仍要求端点明确指定二层网卡，不能使用系统路由替代。传输层只交付完整的 COTP 数据载荷，不解析 MMS PDU，也不持有 SCL、点表或 DataCenter 对象；上层 MMS 会话负责 ACSE/MMS 服务和会话状态。一次 MMS 关联建立（TCP/COTP、Session CONNECT/ACCEPT）使用一份总的绝对截止时间，后续每个阶段只能消费剩余预算；不能因为切换协议层而重新获得完整超时。每次多分段发送或接收共享一次绝对截止时间，不能因分段数量而重复延长调用方超时。若已收到完整的非EOT段但下一段尚未到达，接收超时会保留重组前缀供下一次轮询继续；若TPKT自身只收到半帧则关闭通道，避免下次读取从半帧中间失步。ACSE/MMS 使用同一套自研的有界 BER 编解码器，拒绝无限长度、截断TLV和整数溢出。

当前 `MmsSessionWorker` 已将自研传输层、ISO Session、ACSE、Initiate、在线目录、RCB写入、GI触发和基础InformationReport接收串成独立工作线程，并向 `ProtocolStackAdapter` 提供完整A/B状态快照；它不会把尚未完成目录、RCB和首次GI的会话标记为 `READY`。Initiate支持位只声明当前已接入的NameList、Read、Write、变量属性和文件目录/文件传输服务，后续服务必须按实际实现逐项开启并校验服务端协商结果。

文件服务使用Confirmed Service选择`fileOpen [72]`、`fileRead [73]`、`fileClose [74]`和
`fileDirectory [77]`，这些是BER高标签号选择，不能按低标签单字节处理。目录条目保留远端
文件名、长度和可选修改时间；下载按`moreFollows`循环读取，空文件也必须完成一次Open/Close
闭环。编解码和脚本传输测试覆盖分页、分段、句柄关闭、路径/长度上限及invokeID错误；不同
厂商对文件名序列和时间字段的互操作仍需真实IED或协议模拟器验收。

确认服务收发由独立的 `ExchangeMmsConfirmedRequest` 统一分派：异步
`Unconfirmed-PDU` 进入报告入口，确认响应必须按 `invokeID` 匹配后才能交给当前
请求；`Confirmed-ErrorPDU [2]` 解析 `ServiceError` 后按匹配的 `invokeID` 返回远端
服务失败，不匹配的错误响应继续等待。交换器依赖模块内最小传输契约，生产环境仍
使用自研 `MmsTcpTransport`，测试使用脚本化假传输覆盖报文交错顺序，不引入第三方
协议实现。传输契约的带预算 `Connect`、`Send` 和 `Receive` 接口均为必实现入口；
交换器和会话工作器把当前操作的剩余截止时间传给这些接口，自定义传输实现必须在
网络操作内部消费该预算，不能只在返回后报告迟到。非法帧、半帧和重组上限错误
都会关闭当前MMS通道，后续由会话状态机重新建链。

MMS服务请求和响应解码采用事务式输出：只有完整校验 `listOfVariable`、`listOfData`
及其数量、结果选择和尾部字段后，才提交 `invokeID`、请求项或响应项；任一字段损坏
时输出保持清空，禁止上层误用部分解析结果。

受控MMS Read/Write使用 `MmsSessionWorker` 的串行命令队列。调用方只能向已经完成
在线目录、RCB启用和GI准备的活动通道提交请求；请求在该通道收发线程内与报告接收
复用同一份传输对象，避免并发读写TCP。控制交换期间收到的InformationReport会在同一
收发线程完成确认响应后立即排空并交给报告分段器，不因一次Read/Write而滞留到重连。
请求等待有界超时，通道断线或停止时返回
中文错误；调用线程超时后请求会在自身互斥保护下标记取消，尚未发送的请求不会继续发送；已经发送到IED的Write不承诺远端撤销，结果不确定时关闭当前MMS会话并重建，不能在后续重连会话中继续发送。协议栈
适配器只暴露该受控入口，不把 `MmsTransport` 或阻塞网络细节泄漏给Manager、gRPC
或实时GOOSE/SV线程。

增强安全控制的完成语义单独处理：`ctlModel=3`（direct-with-enhanced-security）和
`ctlModel=4`（sbo-with-enhanced-security）的`$Oper` Write成功只代表服务端接受了
请求，不代表动作已经完成。Worker必须在同一MMS物理会话中继续接收
`Unconfirmed-PDU[3]`的CommandTermination InformationReport，核对变量列表中目标
`Domain/$Oper`对象、结果数量和`LastApplError`结构。仅有一个目标Oper且没有
`LastApplError`才算成功；包含`LastApplError`时按其中的Error、ctlNum、AddCause和
对象引用返回失败，不能把Write ACK当作最终成功。CommandTermination、错误报告和
`operTm`完成等待均受在线`operTimeout`及调用超时上限约束；超时后本地只报告结果
不确定并锁定对象，避免同一MMS会话重复执行。定时Oper收到Write ACK后保持执行状态，
在CommandTermination成功或显式Cancel完成前不得清除SBO保持；`ctlModel=3`的明确
LastApplError没有SBO保持，核对完成后只释放本地执行占用。

Manager层只保留内部 `ReadMms`/`WriteMms` 包装，不新增原始MMS gRPC RPC。Manager先
核对IED存在、MMS已启用、会话已启动且状态为 `RUNNING`，再把请求交给
`ProtocolStackAdapter`；启动中、降级、停止、断线或A/B切换状态不会发起网络请求。
Manager不在锁内等待响应，也不直接接触传输对象；异常统一转换为中文 `INTERNAL`
状态并记录日志。遥控选择-执行、定值权限和联锁仍属于更高层控制策略。

`MmsSessionWorker` 的传输工厂只用于注入有界测试传输；工厂在 `Start` 创建工作线程
前按 A/B 通道同步创建独立对象，返回空对象或抛出异常时不会启动任何会话线程。生产
调用不传工厂时仍由工作器创建自研 `MmsTcpTransport`，重连复用该通道对象，停止并
收敛线程后释放对象，下一次启动重新创建。

## 线程与日志

- MMS事件管线使用 `ModuleManager::StartModuleThread` 创建并绑定日志模块名，MMS会话工作线程使用普通 `std::jthread`；这些线程不应用 IED 的实时线程策略。GOOSE、SV、GOOSE重发、GOOSE超时、实时消费者和保护动作发送线程通过 `StartThreadWithRuntimePolicy` 在入口同步应用同一份 IED 策略。
- 控制面和SQLite允许阻塞，不得从GOOSE/SV实时线程调用。
- 实时策略应用包括两步：先按 `realtime_cpu_indices` 设置 CPU 亲和性，再按 `realtime_scheduling`/`realtime_priority` 调用 Linux 线程调度接口。空 CPU 列表或 `SCHED_OTHER` 不执行对应的修改；策略应用状态包含实际策略、实际优先级、亲和性是否生效及是否降级，保护动作发送器和实时运行时保存相应状态，其他协议线程通过中文日志报告结果。
- `DEGRADE` 模式下亲和性或 FIFO/RR 设置失败会记录中文警告，线程继续执行，已成功应用的设置保留，失败的部分使用系统当前设置。`STRICT` 模式下任一运行时设置失败都会让线程入口返回明确的权限/可用性错误，线程不执行任务函数，IED通信功能启动路径随后回滚已创建资源并返回失败。
- 配置值本身在启动前校验；运行时仍可能因 CPU 不可用、进程受限或 Linux 权限不足失败。`CAP_SYS_NICE` 或 `rtprio`/cgroup 等系统配置通常是启用 `SCHED_FIFO/RR` 所需的现场条件。
- 该策略已接入代码，但不等同于已经满足保护实时性指标；目标板仍需验证 CPU 亲和性实际落地、调度权限、线程抖动、CPU占用和端到端动作时延。
- 真实协议收发实现必须记录中文收发报文日志；高频GOOSE/SV日志需要支持限频或诊断开关。
- 协议栈启动、停止或释放接口抛出的异常由Manager转换为中文内部错误并保留可重试状态，不得穿出模块线程。
- 错误信息、日志和新增注释使用中文。

## 构建产物

- 共享库：`package/module/libIEC61850.so.0.0.1`
- 依赖：`dspProto`、`moduleManager`、`mskdspConfigStore`、`pugixml`
