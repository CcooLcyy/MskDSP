# DLT645 LoRa 档案添加 `status=2` 语义调整方案

## 背景
- 当前 DLT645 模块在 LoRa/载波链路启动连接功能时，会先通过 `addslaveNode` MQTT 接口向对下通信 APP 下发档案，再进入后续抄表流程。
- 现实现对 `addslaveNode` 的响应采用统一判定：`status != 0` 一律视为失败，并记录 `last_error`、进入后台重试。
- 现场联调中已明确：对于 `addslaveNode` 接口，响应中的数值型 `status=2` 表示“档案已存在”，不应继续重复下发档案，而应直接进入后续抄表流程。

## 目标
- 仅针对 `addslaveNode` 接口调整状态语义：
  - 数值型 `status=2`（含数值字符串 `"2"`）表示“档案已存在”。
  - 模块收到该响应后，不再重试档案添加，直接继续后续抄表流程。
- 模块仍将该档案纳入本地生命周期管理：
  - 同地址多连接继续复用本地档案引用计数。
  - 最后一个同地址连接停止连接功能时，仍发送 `delslaveNode` 删除档案。

## 非目标
- 不调整 645 报文收发 `monitorNode` 接口的 `status` 语义。
- 不调整 `delslaveNode` 接口的 `status` 语义。
- 不修改 gRPC 接口与 protobuf 定义。
- 不改变现有后台重试框架；仅让 `addslaveNode` 的“档案已存在”分支不进入重试。

## 作用范围
- 模块：`src/DLT645/`
- 接口：LoRa/载波 `addslaveNode`
- 受影响流程：
  - `StartLink`
  - 自动启动
  - 档案后台重试
  - `StopLink`

## 输入输出与行为约束

### 输入
- 前置条件：
  - 链路配置合法。
  - MQTT 已配置。
  - 点表已配置。
  - 当前通信方式为 `COMM_MODE_LORA` 或 `COMM_MODE_CARRIER`。
- 对下响应：
  - `addslaveNode` 返回 JSON。
  - `status` 字段可能为数值、数字字符串、状态关键字字符串。

### 输出
- 当 `addslaveNode` 返回数值型 `status=2` 或数字字符串 `"2"` 时：
  - `StartLink` / 自动启动继续成功推进。
  - 链路进入 `RUNNING`。
  - 不写入“档案添加失败”类 `last_error`。
  - 不进入档案后台重试。
  - 建立本地档案引用计数，后续 `StopLink` 仍按最后引用发送 `delslaveNode`。

### 兼容性约束
- `addslaveNode` 若返回字符串 `Frametimeout`，仍按“帧超时”失败处理，避免把历史失败语义误判为“档案已存在”。
- `monitorNode` 中 `status=2/Frametimeout` 仍表示“帧超时”，保持原有收发语义不变。

## 详细语义

### `addslaveNode` 状态解释
- `0/ok/success`：档案添加成功。
- 数值型 `2` 或数字字符串 `"2"`：档案已存在，跳过重复下发，继续进入后续抄表流程。
- `Frametimeout`：档案添加失败，原因=帧超时。
- 其他非零状态：继续按失败处理。
- 缺少 `status`、`status` 类型异常、响应体非法：继续按失败处理。

### 生命周期管理
- 对同一 `(comm_mode, meter_addr)`：
  - 首个连接启动时：
    - 若 `addslaveNode` 返回成功或“档案已存在”，都建立本地档案引用计数。
  - 后续连接启动时：
    - 若本地已有同地址档案引用，跳过重复 `addslaveNode`。
  - 停止连接功能时：
    - 非最后引用：仅减少本地引用计数，不发 `delslaveNode`。
    - 最后引用：发送 `delslaveNode` 删除档案。

## 异常场景
- `addslaveNode` 返回字符串 `Frametimeout`：
  - 链路保持 `STOPPED`。
  - 记录中文错误原因。
  - 进入后台重试。
- `addslaveNode` 返回数值型 `2`，但后续启动流程中连接被删除：
  - 按现有启动回滚逻辑释放本地档案引用，并执行相应清理。
- `delslaveNode` 删除失败：
  - 保持现有行为，记录中文错误日志与错误原因。

## 测试策略
- 先补单元测试，再实现：
  - `addslaveNode` 返回数值型 `status=2` 时，链路可进入 `RUNNING`。
  - 上述路径不触发档案后台重试。
  - 上述路径下 `StopLink` 仍会发送 `delslaveNode`。
  - 同地址多连接场景下，`status=2` 也能正确复用档案引用计数，仅在末停时删除档案。
  - `addslaveNode` 返回字符串 `Frametimeout` 时，仍保持原失败与重试行为。

## 验收标准
- 文档、测试、实现三者语义一致。
- `addslaveNode` 数值型 `status=2` 不再触发“档案添加失败”日志与后台重试。
- 链路可直接进入后续抄表流程。
- `StopLink` 在最后引用释放时仍会发送 `delslaveNode`。
- `monitorNode` 与其他非 `addslaveNode` 接口行为不受影响。
