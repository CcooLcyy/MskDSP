# DLT645 LoRa 点抄异常等待与档案重试间隔调整方案

## 背景
- 当前 DLT645 模块在 LoRa 正式轮询点抄时，若 `monitorNode` 返回响应但 `status!=0`，会固定等待 5 秒后再抄下一个点或数据块。
- 同一模块内，`addslaveNode` 档案添加失败后也采用固定 5 秒后台重试。
- 现场联调已确认：正式点抄阶段与档案后台重试虽然都存在“等待后继续”的行为，但语义并不相同。
  - 正式点抄阶段更适合复用链路当前的 `request_timeout_ms`，让“等待响应时长”和“异常状态后的退避时长”保持一致。
  - 档案后台重试仍应保持固定 5 秒，以维持启动恢复节奏稳定、避免与单次收发超时参数耦合。

## 目标
- 仅调整 `COMM_MODE_LORA` 正式轮询点抄流程：
  - 当 `monitorNode` 返回响应且 `status!=0` 时，下一次点抄前的等待改为复用当前链路的 `request_timeout_ms`。
- 保持档案添加失败后的后台重试周期不变：
  - `addslaveNode` 失败后仍每 5 秒重试一次。

## 非目标
- 不调整 `addslaveNode` / `delslaveNode` 的重试周期。
- 不调整 `monitorNode` 请求的超时等待逻辑；请求本身仍按 `request_timeout_ms` 等待响应。
- 不调整 `COMM_MODE_CARRIER` 与 `COMM_MODE_SERIAL` 的现有行为。
- 不修改 gRPC 接口与 protobuf 定义。

## 作用范围
- 模块：`src/DLT645/`
- 受影响流程：
  - LoRa 正式轮询点抄中的点请求分支
  - LoRa 正式轮询点抄中的数据块请求分支
- 不受影响流程：
  - `addslaveNode`
  - `delslaveNode`
  - 档案后台重试线程

## 输入输出与行为约束

### 输入
- 前置条件：
  - 链路配置合法。
  - 点表已配置。
  - MQTT 已配置。
  - 当前通信方式为 `COMM_MODE_LORA`。
- 对下响应：
  - `monitorNode` 返回 JSON。
  - `status` 字段可能为数值、数字字符串、关键字字符串。

### 输出
- 当正式轮询点抄收到非零 `status` 时：
  - 当前请求仍按失败处理，不发布数据。
  - 下一次点抄前等待 `request_timeout_ms` 毫秒。
  - 若 `request_timeout_ms=0`，仍按默认值 3000ms 生效。
- 当正式轮询点抄请求本身超时或 RPC 失败时：
  - 维持现有逻辑，直接进入下一条，仅叠加 `poll_item_interval_ms`，不额外补一个 `request_timeout_ms` 退避。
- 当 `addslaveNode` 失败时：
  - 维持现有逻辑，链路保持 `STOPPED`，后台每 5 秒重试一次。

## 详细语义

### 正式点抄
- 成功：`status=0/ok/success`，按现有逻辑解析并发布数据。
- 异常状态：`status!=0`
  - 点请求：等待 `request_timeout_ms` 后再抄下一个点。
  - 数据块请求：等待 `request_timeout_ms` 后再抄下一个数据块。
- 请求超时 / RPC 失败：
  - 不追加新的异常退避，保持现有失败后继续流程。

### 档案重试
- `addslaveNode` 首轮失败：
  - 继续按每 5 秒后台重试。
- `StopLink`、`UpsertLink`、`UpsertPointTable`：
  - 继续维持现有行为，可终止档案后台重试。

## 异常场景
- `request_timeout_ms` 配置较大：
  - 正式 LoRa 点抄收到非零 `status` 后，下一次点抄间隔也会同步增大；这是本次变更的预期行为。
- `request_timeout_ms` 取默认值：
  - 正式 LoRa 点抄收到非零 `status` 后，下一次点抄等待约 3000ms，而不是原来的 5000ms。
- 档案添加失败：
  - 不受本次改动影响，仍每 5 秒重试。

## 测试策略
- 先补测试，再改实现：
  - LoRa 点请求分支：收到非零 `status` 后，等待约 `request_timeout_ms` 再发下一条。
  - LoRa 数据块分支：收到非零 `status` 后，等待约 `request_timeout_ms` 再发下一条。
  - 档案后台重试测试不改语义，仅确保现有 5 秒行为不受影响。

## 验收标准
- 文档、测试、实现三者语义一致。
- 正式 LoRa 点抄中，非零 `status` 后的等待不再固定为 5 秒。
- 该等待改为复用链路 `request_timeout_ms`。
- 档案后台重试仍保持每 5 秒，不受本次改动影响。
