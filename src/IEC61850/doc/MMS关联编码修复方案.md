# MMS 关联编码修复方案（PRS-7961G-Z-G16E4 互操作）

## 一、问题现象

目标装置：长园深瑞 PRS-7961G-Z-G16E4（61850 交换机），`192.168.2.254:102`，
现场配置 MMS=Enable、IED name=`PRSI`、CSD 解析=Enable。

本模块现象（持续复现）：TCP/COTP 建链成功后

```text
IEC61850 MMS会话建立失败: 通道=1, 错误码=9, 原因=IEC61850 MMS服务器未返回Session ACCEPT
```

## 二、实测手段与证据来源

下位机（192.168.1.219，aarch64）上部署有参考客户端 `/home/megsky/ref_client_aarch64`。
本次通过 `tcpdump -i eth0.102` 抓取「参考客户端 ↔ 装置」的完整报文，得到两类会话层 CONNECT：

1. **重传/被拒形式**（`16 01 00`、无 S-SEL、无 CP、EXTERNAL 用 OID）：
   装置回 `19 07 11 01 05 31 02 32 16`（Session ABORT）。
2. **被接受形式**（参考客户端第 5 次尝试，含 TPKT+COTP 共 187 字节）：
   `03 00 00 bb 02 f0 80 0d b2 05 06 13 01 00 16 01 02 14 02 00 02 33 02 00 01 34 02 00 01 c1 9c 31 81 99 …`
   即 Version Number=02、带 33/34 S-SEL、CP 内 `a2 81 91 81 04 00 00 00 01 82 04 00 00 00 01 a4 23 …`、
   EXTERNAL 用 `28 2d 02 01 03`（indirect-reference=3）、MMS 用 `a0 28 a8 26 80 03 00 fd e8 …`。

**注意**：参考客户端在 COTP 建链完成后即打印 `Connected`，随后所有 MMS 操作报错
（`Failed to read value (error code: 10)`）。因此 `Connected` 并不能证明 MMS 关联成功；
本次以「装置是否回 Session ACCEPT」为唯一判据，并以抓包中第 2 类报文作为目标报文。

### A/B 对照探测（决定性证据）

仅对照「被接受的那一帧」不足以确定哪些字段是必须的，因此在同一台下位机上用 Python
逐项改动发帧、同步 `tcpdump` 抓包（脚本 `/home/megsky/ab_probe2.py`，抓包 `/tmp/ab2.pcap`）。
每次探测都是「COTP CR → 发帧 → 看装置回什么」，结果如下：

| 探测帧 | 与本模块当前编码的差异 | 装置应答 |
| --- | --- | --- |
| `ctrl_accepted` | 参考客户端原文（阳性对照） | **Session ACCEPT（163 字节）** |
| `t1_theircp_oureaarq` | 参考客户端的 CP 结构 + 本模块 AARQ | **Session ACCEPT（147 字节）** |
| `fix2_nested_libcp` | 同 t1，但 user-data 没有 PDV-list 包装 | Session ABORT |
| `t3_ourpsel_with_theircp` | 同 t1，但 P-SEL 写成 5 字节 | Session ABORT |
| `ours_flat` / `fix1_nested` / `fix3_ourcp_head` | 本模块当时的 CP（5 字节 P-SEL、user-data 与 a2 平级） | Session ABORT |

结论（本模块必须逐项满足）：

- P-SEL 必须是 **4 字节** `00 00 00 01`（写 5 字节被 ABORT）；
- user-data 必须放在 **normal-mode-parameters 内**，并以 **PDV-list** 形式承载 ACSE：
  `61 { 30 { 02 01 01, a0 { <AARQ> } } }`（缺少 `30/02/a0` 这层会被 ABORT）；
- 传输语法名称用 `30 { 06 02 51 01 }`（OID 2.1.1），与参考客户端一致；
- 会话层连接项、`indirect-reference=3` 的 EXTERNAL 沿用原修复，装置接受。

`t1_theircp_oureaarq` 与「本模块 AARQ + 上述 CP 结构」逐字节等价，因此本模块发出的
CONNECT 就是实测被装置接受的那一帧（含 TPKT+COTP 共 163 字节）。

### 装置 ACCEPT 的解析（解码侧同样必须支持）

装置回的 Session ACCEPT 里，CP 是 CPA 形式，与本模块请求侧布局不同：

```text
31 81 89
  a0 03 80 01 01                        mode-selector = normal-mode
  a2 81 81                              normal-mode-parameters
    83 04 00 00 00 01                   [3] 应答表示选择子（4字节，只回一个）
    a5 12 …                             [5] 上下文定义结果列表：30 { 80 结果, 81 传输语法 }
    88 02 06 00                         可选字段，只要求 TLV 完整
    61 61 30 5f 02 01 01 a0 5a 61 58 …  user-data（同样在 a2 内，PDV-list 承载 AARE）
```

解码侧因此必须：接受 `[3]` 单个选择子、接受 `a5` 结果列表的 `80/81` 条目、
从 normal-mode-parameters 内取 user-data，并把 PDV-list 剥到 ACSE 本体。

## 三、必须修复的编码偏差

### 修复 1：会话 CONNECT 连接项参数

- 位置：`IEC61850MmsIsoSession.cc` → `BuildConnectParameters()`。
- 旧实现：`05 06 13 01 00 16 01 00 14 02 00 02`（12 字节，版本号 00 且缺少 S-SEL）。
- 新实现：`05 06 13 01 00 16 01 02 14 02 00 02 33 02 00 01 34 02 00 01`（20 字节）。
  `0x16`（Version Number）必须为 `02`；必须补 `0x33`（Calling S-SEL）与
  `0x34`（Called S-SEL），取值 `00 01`。

### 修复 2：表示层 CP 的结构必须与实测被接受的帧一致

- 位置：`IEC61850MmsWorker.cc` → `MmsSessionWorker::Establish()`（客户端建链）；
  编解码在 `IEC61850MmsIsoSession.cc` → `EncodeMmsPresentationCp()` /
  `DecodeMmsPresentationCp()`。
- 旧实现：把 `EncodeMmsAarq()` 的输出直接作为 Session CONNECT 的 `c1` 用户数据（裸 AARQ）；
  后续版本虽加了 CP，但 P-SEL 为 5 字节、user-data 与 `a2` 平级且无 PDV-list 包装，
  A/B 抓包证明这几种形式都被装置回 Session ABORT。
- 新实现：AARQ 先包 CP，CP 内 user-data 以 PDV-list 承载并放进 normal-mode-parameters；
  P-SEL 用 4 字节缺省值 `00 00 00 01`。编码结果与 `t1_theircp_oureaarq` 帧逐字节一致。

### 修复 3：ACSE 的 EXTERNAL 必须用 indirect-reference

- 位置：`IEC61850MmsIsoSession.cc` → `EncodeExternalUserInformation()`。
- 旧实现：`28 <len> 06 05 28 ca 22 02 01 a0 <len> <MMS>`（抽象语法 OID）。
- 新实现：`28 <len> 02 01 03 a0 <len> <MMS>`（indirect-reference=3，指向 CP 中的 MMS 上下文）。

### 修复 4：MMS InitiateRequest 编码与协商值

- 位置：`IEC61850MmsWorker.cc` → `DefaultInitiateRequest()`；结构编码在
  `IEC61850MmsPdu.cc` → `EncodePdu(MmsInitiateRequest)` / `EncodeDetail()`。
- 结构核对结论：`a8 26` 外层 + `80/81/82/83` + `a4 16` initRequestDetail（`80` 版本、
  `81` 参数支持、`82` 服务支持）在现网源码中已经正确；本次按被接受报文对齐协商值：
  `localDetailCalling=65000`、`proposedMaxServOutstandingCalling/Called=5`、
  `proposedDataStructureNestingLevel=10`、参数支持位 `05 00 00`。
- `servicesSupported` 按本模块真实支持填写，不虚报。

### 修复 5：解码侧同时接受 indirect-reference

- 位置：`IEC61850MmsIsoSession.cc` → `DecodeUserInformation()`。
- 旧实现：EXTERNAL 内只接受抽象语法 OID，装置回的 `28 2d 02 01 03 a0 28 …` 会被拒绝，
  导致 ACCEPT 之后的 AARE 解析失败。
- 新实现：`02`（indirect-reference，值 3）与 `06 …`（OID）都接受；仍强制 `a0` single-ASN1-type。

### 其它同步改动

- `IsoSessionPduType` 新增 `REFUSE = 0x0c`：装置在 ACSE 阶段拒绝关联时回 REFUSE；
  建链与运行期都识别并输出中文日志，并把原始 SPDU 十六进制打日志。
- 表示层选择子解码兼容：请求侧 `81/82`，装置 CPA 的 `[3]`（应答选择子）与 `[4]` 都接受；
  CPA 只回一个选择子，因此不再要求调用/被调选择子同时存在。
- 上下文定义列表解码兼容两种形式：`a4`（请求侧，含抽象语法，校验 MMS OID）与
  `a5`（装置 CPA 的结果列表，条目为 `30 { 80 结果, 81 传输语法 }`）。
- ACSE 剥封装兼容三种形式：裸 AARQ/AARE、旧的 `61 { 60 … }`、以及实测的 PDV-list
  `61 { 30 { 02 01 01, a0 { … } } }`（从 CP 内取值时入口直接是 `30 { … }`）。
- Session ACCEPT 解码校验连接项：会话要求（`05`）内必须出现版本号（`16`）子项且为 `02`；
  S-SEL 缺失只提示不拒绝（实测装置 ACCEPT 不带 33/34）。
- `tools/device-sim` 模拟器：ACCEPT 改为「AARE + CP」；解码 AARQ 兼容带 CP/不带 CP 两种客户端。
- `MmsServer` 复用同一套编解码，随修复 3/5 自动同步。

## 四、验收与测试

- `test/iec61850_mms_iso_session_test.cc` 的 golden 用例逐字节比对
  InitiateRequest、AARQ（indirect-reference）、CP 与 Session CONNECT；
  其中 CONNECT 必须等于实测被装置接受的那一帧（`kDeviceAcceptedConnect`，163 字节）。
- 解码用例覆盖装置两种真实 ACCEPT：对照组 163 字节与「本模块 CONNECT 得到的」147 字节，
  断言解出 AARE（result=0、MMS 应用上下文）与 InitiateResponse（nesting=5、version=1）。
- 兼容用例：带 CP 的 AARQ、旧的不带 CP 的 AARQ、旧的 `61 { 60 … }`、`0x0c` REFUSE、
  错误会话版本号拒绝、截断报文拒绝。
- 现场验收（由用户执行）：连接 `192.168.2.254:102` 后日志应依次出现
  `MMS ISO-on-TCP通道已建立` → `收到Session ACCEPT` → `AARE 关联已接受` →
  `在线目录核对完成` → RCB 配置/GI → `READY / 活动通道=1`。

## 五、范围与约束

- 不修改现场装置任何配置。
- COTP CR 参数与 TPDU size 未改动。
- OSI 寻址字段（`a2/a3/a6/a7`）按参考报文可省略，本期不从 SCL 解析。
