# DataCenter 模块

## 定位与边界
DataCenter 是进程内的“数据总线/转发枢纽”，用于在不同协议连接之间转发数据（例如 Modbus 抄表 → IEC104 上报），以实现跨协议的数据互通。

## 能力清单
- 连接级隔离：通过 `connId` 区分不同连接/通信链路
- 点位对齐：通过 `tag`（逻辑点名，可中文）对齐跨协议的同一业务量
- 有向路由：按点位维度配置 `src -> dst` 的转发规则，支持一对一与一对多
- 最新值缓存：可用于订阅端启动时拉取最新值（当前规划）

不在当前范围内：
- 历史数据存储/查询
- 聚合/判据/计算点（例如“多个遥信判断一个遥信状态”建议由独立规则/计算模块实现，订阅原始点后发布派生点）
- 多对一仲裁（多个源同时写入同一目的点时暂不保证行为）

## 关键概念
- `connId`：连接的全局唯一 ID（建议使用无符号整型），由各协议模块配置产生；要求重启后保持不变。
- `tag`：逻辑点名（UTF-8，可使用中文），用于跨协议对齐同一业务量；协议地址（IOA/寄存器等）应由各协议模块自身点表维护。
- `Endpoint`：`(connId, tag)`，表示某连接中的一个逻辑点。
- `Route`：有向路由绑定，表示数据从 `src Endpoint` 转发到一个或多个 `dst Endpoint`。

## 数据方向与路由
- 路由按点位维度配置，且为有向：`src -> dst`；双向传输需要配置两条路由。
- 支持关系：
  - 一对一：`(1, tagA) -> (2, tagA)`
  - 一对多：`(1, tagA) -> (2, tagA)` + `(1, tagA) -> (3, tagA)` + ...
- 多对一：多个源同时写入同一目的点的行为当前不做仲裁，暂不保证一致性（可能出现顺序相关/last-write-wins），建议避免配置此类规则。

## 接口与协议
- Protobuf：`protobuf/DataCenter.proto`
- gRPC Service：`DataCenterProto::DataCenterService`

## 配置与使用流程（建议）
1. 在各协议模块（IEC104/Modbus/DLT645/…）中为每条连接配置 `connId`（全局唯一）并完成点表配置；为每个点配置 `tag`（逻辑点名，可中文）。
2. 在 DataCenter 中配置路由方向（有向绑定）：基于源/目的两侧点表中已知的 `connId + tag` 建立 `src -> dst` 规则（支持一对多）。
3. 启动模块：通过管理器启动 DataCenter 与各协议模块；采集端向 DataCenter 发布数据，上送端订阅/获取对应 `tag` 的更新进行上报。

## 运行与地址
- 对外 gRPC：随机选择 `0.0.0.0:<port>`（7001–7999）
- 内部 gRPC：`unix socket`：`./socket/dataCenter.sock`
- 运行时可通过管理器 `GetRunningModuleInfo` 查询实际地址

## 构建产物
- 共享库：`package/lib/libdataCenter.so.<version>`（版本见 `src/DataCenter/include/dataCenterLibInfo.h`）
