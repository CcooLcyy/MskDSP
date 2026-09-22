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

1. **重传/被拒形式**（`16 01 00`、无 S-SEL、CP 内 `a2 03 81 01 03`、EXTERNAL 用 OID）：
   装置回 `0d…19 07 11 01 05 31 02 32 16`（Session ABORT）。
2. **被接受形式**（pcap 中第 5 个 CONNECT，181 字节）：
   `0d 52 05 06 13 01 00 16 01 02 14 02 00 02 33 02 00 01 34 02 00 01 c1 9c 31 81 99 …`
   即 Version Number=02、带 33/34 S-SEL、CP 内 `a2 81 91 81 04 … 82 04 …`、
   EXTERNAL 用 `28 2d 02 01 03`（indirect-reference=3）、MMS 用 `a0 28 a8 26 80 03 00 fd e8 …`。

**注意**：参考客户端在 COTP 建链完成后即打印 `Connected`，随后所有 MMS 操作报错
（`Failed to read value (error code: 10)`）。因此 `Connected` 并不能证明 MMS 关联成功；
本次以「装置是否回 Session ACCEPT」为唯一判据，并以抓包中第 2 类报文作为目标报文。

## 三、必须修复的编码偏差

### 修复 1：会话 CONNECT 连接项参数

- 位置：`IEC61850MmsIsoSession.cc` → `BuildConnectParameters()`。
- 旧实现：`05 06 13 01 00 16 01 00 14 02 00 02`（12 字节，版本号 00 且缺少 S-SEL）。
- 新实现：`05 06 13 01 00 16 01 02 14 02 00 02 33 02 00 01 34 02 00 01`（20 字节）。
  `0x16`（Version Number）必须为 `02`；必须补 `0x33`（Calling S-SEL）与
  `0x34`（Called S-SEL），取值 `00 01`。

### 修复 2：缺少表示层 CP

- 位置：`IEC61850MmsWorker.cc` → `MmsSessionWorker::Establish()`；编解码在
  `IEC61850MmsIsoSession.cc` → `EncodeMmsPresentationCp()` / `DecodeMmsPresentationCp()`。
- 旧实现：把 `EncodeMmsAarq()` 的输出直接作为 Session CONNECT 的 `c1` 用户数据（裸 AARQ）。
- 新实现：先构造 BER `31`（CP：mode-selector=normal-mode、两侧 P-SEL、ACSE/MMS 两个
  表示上下文定义、user-data=AARQ），再把 CP 作为 `c1` 用户数据。

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
- 表示层 P-SEL 标签兼容：除 `81/82` 外同时接受 ISO 8823 全限定形式 `83/84`
  （现场装置 ACCEPT 使用的就是 `83/84` + 4 字节 P-SEL）。
- Session ACCEPT 解码校验连接项：会话要求（`05`）内必须出现版本号（`16`）子项且为 `02`；
  S-SEL 缺失只提示不拒绝。
- `tools/device-sim` 模拟器：ACCEPT 改为「AARE + CP」；解码 AARQ 兼容带 CP/不带 CP 两种客户端。
- `MmsServer` 复用同一套编解码，随修复 3/5 自动同步。

## 四、验收与测试

- `test/iec61850_mms_iso_session_test.cc` 增加 golden 用例，逐字节比对
  InitiateRequest、AARQ（indirect-reference）与 CP；并直接用**真机抓包中被接受的
  181 字节 CONNECT** 断言解码器可用、且我们的连接项与它逐字节一致。
- 解码兼容用例：带 CP 的 AARQ、旧的不带 CP 的 AARQ、`0x0c` REFUSE、错误会话版本号拒绝。
- 现场验收（由用户执行）：连接 `192.168.2.254:102` 后日志应依次出现
  `MMS ISO-on-TCP通道已建立` → `收到Session ACCEPT` → `AARE 关联已接受` →
  `在线目录核对完成` → RCB 配置/GI → `READY / 活动通道=1`。

## 五、范围与约束

- 不修改现场装置任何配置。
- COTP CR 参数与 TPDU size 未改动。
- OSI 寻址字段（`a2/a3/a6/a7`）按参考报文可省略，本期不从 SCL 解析。
