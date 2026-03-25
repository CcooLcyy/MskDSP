# DLT645 模块

## 简介
DLT645 模块负责管理 DLT645 协议链路与点表，按设备维度支持标准版与 PCD 版配置。

## 能力清单
- 设备级协议变体选择：DLT645std / DLT645PCD。
- 设备级点表配置：tag/di/data_len/type/access/scale/offset/deadband。
- 支持数据块读取：按数据块 DI 读整块数据并拆分发布子点位。
- 支持通过 MQTTManager 对接 Lora/载波/串口（uartManager）。
- 点表下发时同步 DataCenter 连接与连接标签注册表。

## 接口与协议
- Protobuf：`protobuf/DLT645.proto`
- gRPC Service：`DLT645Proto::DLT645Service`
- 接口规划：当前顶层全局配置仅提供 `UpdateConfig` 写接口；后续建议补充 `GetConfig` 或 `GetGlobalConfig` 回读接口，供上位机做界面回显、配置对账与一致性校验；当前版本尚未实现。

### UpsertLink 语义
- `conn_name` 不存在：创建新链路，并向 DataCenter 绑定 `conn_id`。
- `conn_name` 已存在且 `create_only=true`：返回 `ALREADY_EXISTS`；同时会检查 DataCenter 注册表中是否已有同名连接。
- `conn_name` 已存在且 `create_only=false`：仅允许在链路处于 `STOPPED` 时更新配置；更新时保留原 `conn_id` 与点表。
- 链路处于 `RUNNING` 或 `PENDING_DELETE` 时调用 `UpsertLink(create_only=false)` 会返回 `FAILED_PRECONDITION`。
- 若需修改运行中链路配置，应先调用 `StopLink` 让链路退出运行态，再执行 `UpsertLink`。

## 运行与地址
- 对外 gRPC：随机选择 `0.0.0.0:<port>`（7001–7999）
- 内部 gRPC：`unix socket`：`./socket/DLT645.sock`
- 运行时可通过管理器 `GetRunningModuleInfo` 查询实际地址

## 配置与数据
- 配置来源：支持上位机直连下发，也支持由 ConfigPusher 批量下发；后者更适合作为初始化导入方式。
- 示例文件：`package/conf/configPusher/DLT645.jsonc`（仅示例）。
- MQTT 连接参数由 DLT645 在调用 MQTTManager 时携带，配置位于 `dlt645.mqtt`。
- 当前实现会将已下发的 MQTT/链路/点表配置本地持久化到 `./conf/DLT645/`，用于模块重启后的自动恢复；恢复出的链路若满足当前最小可运行条件，会由模块自动让链路进入运行态。

### 配置结构
顶层采用 `links` 组织方式，结构与 Modbus/IEC104 类似：

- `mqtt`
  - `host/port/client_id/username/password/keepalive_sec/clean_session/connect_timeout_ms`：MQTT 连接参数，全局复用。
- `link.config`
  - `conn_name`：链接名称（模块内唯一）。
  - `protocol_variant`：`DLT645std` 或 `DLT645PCD`。
  - `meter_addr`：表计地址；PCD 模式下为协议转换器地址。
  - `device_no`：PCD 专用，十六进制字符串（例如 `0A` 表示第 10 个设备）。
  - `transport_type`：传输类型；为空时默认 `TRANSPORT_MQTT_UART`。
  - `comm_mode`：通信方式：`COMM_MODE_LORA`/`COMM_MODE_CARRIER`/`COMM_MODE_SERIAL`。
  - `poll_interval_ms`：整轮扫描完成后的等待间隔（毫秒，0 使用默认值）。
  - `poll_item_interval_ms`：单次点抄收发完成后，到下一次点抄收发开始前的等待间隔（毫秒，0 表示无额外等待）。
  - `request_timeout_ms`：请求超时（毫秒，0 使用默认值）；对于 `COMM_MODE_LORA` 的正式轮询点抄，若收到响应但 `status!=0`，下一次点抄前的退避等待也复用该值。
  - `serial_port`：串口标识（`COMM_MODE_SERIAL` 必填，例如 `RS485-1`）。
  - `serial_baud_rate`：串口波特率（`COMM_MODE_SERIAL`，0 使用默认值 `9600`）。
  - `serial_data_bits`：串口数据位（`COMM_MODE_SERIAL`，0 使用默认值 `8`，仅支持 `5..8`）。
  - `serial_parity`：串口校验（`COMM_MODE_SERIAL`，默认 `SERIAL_PARITY_NONE`）。
  - `serial_stop_bits`：串口停止位（`COMM_MODE_SERIAL`，默认 `SERIAL_STOP_BITS_ONE`）。
  - `serial_byte_timeout_ms`：串口字节超时（`COMM_MODE_SERIAL`，默认 `100ms`）。
  - `serial_frame_timeout_ms`：串口帧超时（`COMM_MODE_SERIAL`，默认 `100ms`）。
  - `serial_est_size`：预估最大接收字节（`COMM_MODE_SERIAL`，默认 `256`）。
- `point_table`
  - `replace`：是否全量替换点表。
  - `points[]`：点位定义。
  - `blocks[]`：数据块定义（按 items 顺序拼接）。

### 点表字段说明
- `tag`：与 DataCenter 对应的点名。
- `di`：4 字节数据标识，使用 8 位十六进制字符串表示；配置为人读顺序（高字节在前），模块发送时按字节逆序（低字节在前）。
- `data_len`：数据域字节数（不包含 DI 或设备序号）。
- `type`：数据类型（例如 `DATA_TYPE_UINT16/UINT32/FLOAT/STRING/BCD/BOOL`）。
- `access`：读写属性（`ACCESS_READ_ONLY/ACCESS_WRITE_ONLY/ACCESS_READ_WRITE`）。
- `scale/offset`：工程量换算 `value = raw * scale + offset`（`scale=0` 视为 1）。
- `deadband`：工程量单位，`<=0` 不过滤；BOOL 忽略 `scale/offset/deadband`。

### 数据块字段说明
- `block_di`：数据块 DI，8 位十六进制字符串；配置为人读顺序（高字节在前），模块发送时按字节逆序（低字节在前）。
- `block_data_len`：数据块数据域总长度（不包含 DI 或设备序号）。
- `items[]`：子项定义，按表格顺序拼接；`data_len` 之和必须等于 `block_data_len`。
- `trim_right_space`：ASCII 字段右侧空格裁剪（未配置默认裁剪）。

### 读块写点规则
- 同一 `tag` 同时出现在 `points` 与 `blocks.items` 时：
  - 读：优先使用数据块（单点读会被跳过）。
  - 写：仍允许使用单点配置（需 `ACCESS_WRITE_ONLY/ACCESS_READ_WRITE`）。
  - 要求：`data_len/type/scale/offset/deadband` 必须一致，否则拒绝配置。

### 配置示例
```jsonc
{
  "dlt645": {
    "mqtt": {
      "host": "127.0.0.1",
      "port": 1883,
      "client_id": "dlt645",
      "username": "",
      "password": "",
      "keepalive_sec": 30,
      "clean_session": true,
      "connect_timeout_ms": 3000
    },
    "links": [
      {
        "link": {
          "config": {
            "conn_name": "表计-01",
            "protocol_variant": "DLT645PCD",
            "meter_addr": "123456789012",
            "device_no": "0A",
            "transport_type": "TRANSPORT_MQTT_UART",
            "comm_mode": "COMM_MODE_LORA",
            "poll_interval_ms": 1000,
            "poll_item_interval_ms": 200,
            "request_timeout_ms": 3000
          }
        },
        "point_table": {
          "replace": true,
          "points": [
            {
              "tag": "A相电压",
              "di": "00010000",
              "data_len": 2,
              "type": "DATA_TYPE_UINT16",
              "access": "ACCESS_READ_ONLY",
              "scale": 1.0,
              "offset": 0.0,
              "deadband": 0.0
            }
          ],
          "blocks": [
            {
              // 电压数据块：DI=0201FF00，总长度 9（A/B/C 三相各 3 字节 BCD）
              "block_di": "0201FF00",
              "block_data_len": 9,
              "items": [
                {
                  "tag": "A相电压",
                  "data_len": 3,
                  "type": "DATA_TYPE_BCD",
                  "access": "ACCESS_READ_ONLY",
                  "scale": 0.01,
                  "offset": 0.0,
                  "deadband": 0.0
                },
                {
                  "tag": "B相电压",
                  "data_len": 3,
                  "type": "DATA_TYPE_BCD",
                  "access": "ACCESS_READ_ONLY",
                  "scale": 0.01,
                  "offset": 0.0,
                  "deadband": 0.0
                },
                {
                  "tag": "C相电压",
                  "data_len": 3,
                  "type": "DATA_TYPE_BCD",
                  "access": "ACCESS_READ_ONLY",
                  "scale": 0.01,
                  "offset": 0.0,
                  "deadband": 0.0
                }
              ]
            }
          ]
        },
        "start": true
      }
    ]
  }
}
```

### DI 字节序说明
- 配置 `di` 使用高字节在前的字符串顺序，例如 `02010100`。
- 模块发送时按字节逆序，实际发送为 `00 01 01 02`；PCD 会在 DI 后追加 `device_no`。
- 集成侧展示/录入时按配置顺序理解，无需手工反转。

## 协议差异（DLT645PCD）
- 标准版帧结构：`68 + 地址 + 68 + DI(4字节) + data + CS + 16`。
- PCD 版帧结构：`68 + 地址 + 68 + DI(4字节) + 设备序号(1字节) + data + CS + 16`。
- `device_no` 用于区分协议转换器下的设备，响应帧与请求一致。
- PCD 的设备序号视作 DI 第 5 字节，按 0x33 编解码规则处理。

## MQTT 对接说明（Lora/载波/串口）
- DLT645 统一通过 MQTTManager 与对下通信 APP 交互。
- 请求主题：
  - Lora：`AGVC/loraManager/JSON/action/request/monitorNode`
  - 载波：`AGVC/ccoRouter/JSON/action/request/monitorNode`
  - 串口：`AGVC/uartManager/JSON/transparant/notification/{serial_port}/data`
- 响应主题：
  - Lora：`loraManager/AGVC/JSON/action/response/monitorNode`
  - 载波：`ccoRouter/AGVC/JSON/action/response/monitorNode`
  - 串口：`uartManager/AGVC/JSON/transparant/notification/{serial_port}/data`
- 请求 JSON 字段：
  - Lora/载波：`token/timestamp/prio/acqAddr/data(base64)`。
  - 串口：`token/timestamp/port/prio/prm/byteTimeout/frameTimeout/taskTimeout/param/data(base64)`。
- 响应 JSON 字段：
  - Lora/载波：`token/timestamp/prio/acqAddr/data(base64)/status`。
  - 串口：`token/timestamp/port/prm/status/data(base64)`。
- `token` 约束：统一使用字符串类型；DLT645 请求中发送的 `token` 为自增字符串，对端响应必须原样回传相同字符串，不能改成数字类型。
- 若存在中间层透传 `token`，应按字符串处理并保持原值，避免做数值化转换导致请求响应无法匹配。
- `status` 语义：
  - `0/ok/success`：成功。
  - `1/fail`：失败。
  - `2/Frametimeout`：帧超时。
  - `3/Porterror`：端口错误。
  - `4/Buffull`：缓冲区满。
  - `5/Formaterror`：格式错误。
  - 其他未识别值：统一按失败处理。
- 对 `addslaveNode/delslaveNode` 而言，`status` 缺失、空值或类型异常会直接按失败处理，不再默认视为成功。
- 对普通抄表/写入请求，`status=2` 仍按“帧超时”处理；其中 `COMM_MODE_LORA` 的正式轮询点抄若收到任意 `status!=0`，模块会在继续下一次点抄前按 `request_timeout_ms` 退避等待，而不是固定等待 5 秒。
- 对 Lora/载波 `addslaveNode` 响应，数值型 `status=2`（含字符串 `"2"`）表示“档案已存在”，模块会按档案添加成功处理，不再进入后台重试，并直接继续后续抄表流程；字符串 `Frametimeout` 仍按“帧超时”失败处理。
- 档案管理（仅 Lora/载波）：按 `(comm_mode, meter_addr)` 做地址级引用计数；首个连接进入运行态时发送 `addslaveNode`，最后一个连接退出运行态时发送 `delslaveNode`。
- 若首轮 `addslaveNode` 失败（不含数值型 `status=2`/字符串 `"2"` 的“档案已存在”），链路会保持 `STOPPED`，模块后台每 5 秒重试一次档案添加；重试成功后会自动继续启动连接功能。`StopLink`、`UpsertLink`、`UpsertPointTable` 会终止该后台重试，并以最新配置重新判定后续行为。

### 并发模型说明（Lora/载波/串口）
- `COMM_MODE_LORA`：由于 Lora 头端与尾端之间不支持并发，DLT645 模块内部会对所有 Lora 请求执行全局串行；同一时刻仅允许一个 Lora 请求处于“发送并等待响应”阶段。
- 串行范围：覆盖点抄、写入、档案添加、档案删除等所有通过 Lora 发出的请求。
- `COMM_MODE_CARRIER` 与 `COMM_MODE_SERIAL`：不受上述限制，仍按各连接独立线程并发执行。
- 集成与运维侧在评估 Lora 吞吐时，应按“模块内全局串行”估算总周期，而不是按连接数线性并发估算。

### 同地址档案生命周期去重说明（Lora/载波）
- 适用对象：`COMM_MODE_LORA` 与 `COMM_MODE_CARRIER`。
- 去重键：`(comm_mode, meter_addr)`。
- 行为规则：
  - 若同地址第一个连接进入运行态：执行档案添加。
  - 若同地址第一个连接首轮档案添加返回数值型 `status=2`（含字符串 `"2"`）：视为档案已存在，直接建立首个引用并进入后续抄表流程，不进入后台重试。
  - 若同地址第一个连接首轮档案添加失败：不会建立引用计数；后台重试成功后才建立首个引用并进入运行态。
  - 若同地址已有运行连接，其他连接再次进入运行态：跳过档案添加，仅增加引用计数。
  - 若同地址仍有其他运行连接，其中一个连接退出运行态：跳过档案删除，仅减少引用计数。
  - 仅当同地址最后一个连接退出运行态：执行档案删除。
- 目的：降低重复档案调用次数，避免同地址重复 add/del 导致的无效开销与噪声日志。

### LoRa 批量档案下发约束（TDD 需求）
- 适用对象：`COMM_MODE_LORA`。
- 当同一次批量启动或自动启动流程中存在多个待添加档案时，模块应只发送 1 条 `addslaveNode` MQTT 请求。
- 该请求的 `body` 必须为数组；数组中的每个元素对应 1 个档案，多个档案应通过同一条消息的 `body` 多元素一次下发，不允许拆成多条 `addslaveNode` 消息。
- 验收口径：抓包或 mock 校验时，应同时检查 `addslaveNode` 调用次数为 `1`，且 `body` 数组元素数与本批次待下发档案数一致。

### 集成说明
涉及批量设备建模、页面展示、串口参数展示与运维交互的统一说明，见 `doc/上位机设计指导.md`。本模块的设备号展开规则、并发模型、档案生命周期与串口/MQTT 约束仍以本文档为准。

### 发送时机与可靠性说明
- `addslaveNode` 在同地址首个连接进入运行态时触发发送（按地址生命周期一次）；消息默认不保留（retain=false）。
- 若首轮 `addslaveNode` 返回数值型 `status=2`（含字符串 `"2"`），表示档案已存在；模块不会继续重发 `addslaveNode`，而是直接继续后续抄表流程。
- 若首轮 `addslaveNode` 返回失败（不含数值型 `status=2`/字符串 `"2"` 的“档案已存在”），模块会在后台每 5 秒继续重发，直到成功、连接功能被停止，或链路配置/点表被再次更新。
- `COMM_MODE_LORA` 的正式轮询点抄若收到非零 `status`，模块不会进入档案后台重试逻辑；仅在当前轮询线程内按 `request_timeout_ms` 等待后继续抄下一个点或数据块。
- `StartLink` 首次调用可能直接返回失败，但后台仍会继续重试档案添加；`UpsertLink/UpsertPointTable` 返回成功也只代表配置已生效，不代表连接功能已经进入 `RUNNING`。集成侧应通过 `GetLink/ListLinks` 的 `state` 与 `last_error` 判断最终状态。
- 如订阅端在发送之后才订阅，可能错过该消息；建议订阅端使用 QoS 1 + 持久会话（`clean_session=false`）。
- 即使首轮 `addslaveNode` 返回数值型 `status=2`（含字符串 `"2"`）而未重试，后续当该地址生命周期结束时（例如上位机调用 `StopLink` 停止连接功能，且该地址已无其他运行连接），模块仍会按生命周期发送 `delslaveNode` 删除档案。
- 若只是首轮发送失败，无需人工重新触发；模块会按上述策略自动后台重试。若需要重新开始新的地址生命周期，需先让该地址全部连接退出运行态，再重新让任一连接进入运行态触发。

## 配置持久化（当前实现）
DLT645 会将模块内已生效的配置落盘到工作目录下的 `./conf/DLT645/`，用于进程重启后的自动恢复。

### 文件与策略
- MQTT 配置：`./conf/DLT645/mqtt.pb`
- 链路配置：`./conf/DLT645/links.pb`
- 点表配置：`./conf/DLT645/point_tables.pb`
- 备份文件：对应主文件的 `.bak`
- 临时文件：对应主文件的 `.tmp`
- 隔离文件：对应主文件的 `.corrupt.<timestamp>`

### 保存时机与语义
- `UpdateConfig` 成功后自动落盘 MQTT 配置。
- `UpsertLink` 成功后自动落盘链路配置。
- `DeleteLink` 成功后会同步删除本地链路/点表配置并落盘。
- 若 `DeleteLink` 因 DataCenter 删除失败而进入 `PENDING_DELETE`，会将待删除状态一并落盘，便于上位机重启后继续重试删除。
- `UpsertPointTable` 成功后自动落盘点表配置。
- 落盘失败会返回错误，但内存中的已生效配置不会回滚；上位机或 ConfigPusher 可重试。

### 启动恢复
- 模块启动时会先加载 `mqtt.pb`、`links.pb`、`point_tables.pb`。
- 每条链路恢复时会重新向 DataCenter 调用 `GetOrCreateConnection(conn_name)` 绑定当前 `conn_id`，并将点表 tags 重新同步到 DataCenter 连接标签注册表。
- 若持久化文件损坏，会优先尝试 `.bak`；若主/备均不可用，则跳过该类配置恢复并保留损坏文件的隔离副本。
- DLT645 当前采用的“可运行最小条件”为：链路对象状态不是 `PENDING_DELETE`、已经恢复出有效 `conn_id`、链路对应的点表对象已经成功恢复或下发并通过当前校验，同时 MQTT 全局配置也已经恢复或下发成功。
- 模块启动后，恢复出的链路若满足上述最小条件，会自动进入运行态；模块已经启动后，若上位机补齐 MQTT、点表或数据块配置并达到上述条件，也会自动再次尝试让链路进入运行态。
- 若自动启动仅因 `addslaveNode` 失败、Lora/载波瞬时异常而未完成，DLT645 会记录中文日志与 `last_error`，并在后台每 5 秒重试档案添加；若是 `links.pb` / `point_tables.pb` / `mqtt.pb` 解析失败、点表或数据块非法、链路与点表不匹配等配置问题，则保持模块服务在线，等待上位机后续修正配置。
- `StartLink` RPC 仍保留用于兼容，连接已在运行时直接返回成功；当首轮 `addslaveNode` 失败时，本次 RPC 可能先返回失败，但模块仍会在后台继续重试档案添加，最终是否进入运行态应以 `GetLink/ListLinks` 的查询结果为准。

### 常见问题排查
- 启动连接失败且日志显示 `Deadline Exceeded`：通常为 MQTT 短暂断连或订阅/发布阻塞导致；检查 `MQTTManager.log` 是否有 `Disconnected/订阅失败/发布失败`。
- 链路持续停留在 `STOPPED` 且 `last_error` 为 `档案添加失败: 帧超时/端口错误/缓冲区满/格式错误/失败`：说明模块仍在后台重试 `addslaveNode`；应同步检查 MQTT 头端、Lora/载波头端、现场地址与对应响应主题是否正常。
- 订阅端偶尔收不到：请确认订阅时机、broker 地址是否一致，并建议使用 QoS 1 + 持久会话。

### 变更记录
- 2026-03-25：LoRa 正式轮询点抄收到非零 `status` 时，下一次点抄前的等待由固定 5 秒改为复用 `request_timeout_ms`；档案后台重试仍保持每 5 秒。
- 2026-03-23：档案添加/删除响应 `status` 改为输出中文失败原因；首轮 `addslaveNode` 失败后转为后台每 5 秒无限重试，并补充上位机状态展示指导文档。
- 2026-02-03：新增数据块配置（blocks/items/trim_right_space）及读块写点规则说明。
- 2026-03-04：新增 `COMM_MODE_SERIAL` 串口通道实现，支持 uartManager 主题与串口参数下发。
- 2026-03-05：Lora/载波同地址档案管理改为地址级去重（首启添加、末停删除），支持同一协议转换器下多逻辑连接共用档案生命周期。
- 2026-03-12：Lora 请求改为模块内全局串行执行，载波与串口继续保持按连接并发。

## 线程与日志
- 模块内部线程统一使用 `ModuleManager::StartModuleThread(模块LibInfo.LIB_NAME, ...)` 创建，自动绑定日志模块名上下文。
- 无需在入口手动创建 `ModuleManager::LogModuleScope`，统一规则见 `src/core/ModuleManager/doc/README.md`。

## 构建产物
- 共享库：`package/module/libDLT645.so.<version>`（版本见 `src/DLT645/include/DLT645LibInfo.h`）
