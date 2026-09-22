# MMS 关联编码修复方案（PRS-7961G-Z-G16E4 互操作）

## 一、问题现象

目标装置：长园深瑞 PRS-7961G-Z-G16E4（61850 交换机），`192.168.2.254:102`，
现场配置 MMS=Enable、IED name=`PRSI`、CSD 解析=Enable。

本模块现象（持续复现）：TCP/COTP 建链成功后

```text
IEC61850 MMS会话建立失败: 通道=1, 错误码=9, 原因=IEC61850 MMS服务端未返回Session ACCEPT
```

用 libiec61850 官方客户端（aarch64 静态二进制 `/home/megsky/ref_client_aarch64`）实测可连上该装置并输出
`Connected`；通过用户态 TCP 代理抓到其完整关联报文与装置回应的 Session ACCEPT，再用
「原样回放 + 逐项摘除/替换」对照实验，把原因定位到 4 处编码偏差与 1 处解码兼容性问题。

## 二、被装置接受的权威报文（实测）

- COTP CR（22 字节，本模块当前实现亦被接受，未改动）：
  `03 00 00 16 11 e0 00 00 00 01 00 c0 01 0d c2 02 00 01 c1 02 00 01`
- 会话层 SPDU（TPKT+COTP 之后）：连接项 20 字节 `05 06 13 01 00 16 01 02 14 02 00 02`
  `33 02 00 01 34 02 00 01`，用户数据（`c1`）为表示层 CP（BER 标签 `31`）；
  CP 内的 ACSE AARQ 使用 `be … 28 … 02 01 03 …`（EXTERNAL 的 indirect-reference=3）。
- 装置回应的 Session ACCEPT（163 字节，含 TPKT+COTP）见单元测试夹具
  `test/iec61850_mms_iso_session_test.cc` 的 `kDeviceSessionAccept`。

## 三、根因与修复

### 修复 1：会话 CONNECT 连接项参数

- 位置：`IEC61850MmsIsoSession.cc` → `BuildConnectParameters()`。
- 旧实现：`05 06 13 01 00 16 01 00 14 02 00 02`（12 字节，Version Number=00，且缺少 S-SEL）。
- 新实现：`05 06 13 01 00 16 01 02 14 02 00 02 33 02 00 01 34 02 00 01`（20 字节）。
  `0x16`(Version Number) 必须为 `02`；补 `0x33`(Calling S-SEL)/`0x34`(Called S-SEL)，
  缺省 `00 01`，可通过 `IsoSessionSelectors` 由 SCL 的 OSI-SSEL/TSEL 覆盖。
- 证据：旧参数 + 正确 CP/AARQ → ABORT；换成上述字节 → ACCEPT。

### 修复 2：缺少表示层 CP

- 位置：`IEC61850MmsWorker.cc` → `MmsSessionWorker::Establish()`；编解码新增在
  `IEC61850MmsIsoSession.cc` → `EncodeMmsPresentationCp()` / `DecodeMmsPresentationCp()`。
- 旧实现：把 `EncodeMmsAarq()` 的输出直接当作 Session CONNECT 的 `c1` 用户数据（裸 AARQ）。
- 新实现：先按 BER `31` 构造 CP（mode-selector=normal-mode、P-SEL 两侧 `00 00 00 00 01`、
  ACSE/MMS 两个表示上下文定义、user-data=AARQ），再把 CP 作为 `c1` 用户数据。
- 证据：lib 会话参数 + 裸 AARQ → ABORT；加 CP → ACCEPT。
- 服务端/模拟器解码同时兼容「带 CP」和「旧的不带 CP」两种客户端（`DecodeMmsAarq`/`DecodeMmsAare`
  入口先判 `31` 标签）。

### 修复 3：ACSE 的 EXTERNAL 必须用 indirect-reference

- 位置：`IEC61850MmsIsoSession.cc` → `EncodeExternalUserInformation()`。
- 旧实现：`28 <len> 06 05 28 ca 22 02 01 a0 <len> <MMS>`（抽象语法 OID）。
- 新实现：`28 <len> 02 01 03 a0 <len> <MMS>`（indirect-reference=3，指向 CP 中的 MMS 上下文）。
- 证据：OID → 装置回 Session ABORT；`02 01 03` → ACCEPT。

### 修复 4：MMS InitiateRequest 协商值

- 位置：`IEC61850MmsWorker.cc` → `DefaultInitiateRequest()`；结构体编码在
  `IEC61850MmsPdu.cc` → `EncodePdu(MmsInitiateRequest)` / `EncodeDetail()`。
- 现状核对：`a8 26` 外层 + `80/81/82/83` + `a4 16` initRequestDetail（`80` 版本、`81` 参数支持、
  `82` 服务支持）结构已正确；本次把协商值对齐实测量：`localDetailCalling=65000`、
  `proposedMaxServOutstandingCalling/Called=5`、`proposedDataStructureNestingLevel=10`。
- `servicesSupported` 按本模块真实支持填写（NameList、Identify、Read、变量属性、Write、
  Named Variable List 属性、文件打开/读/关/目录），不虚报。
- 证据：旧编码 → 装置回 Session REFUSE(0x0c)；标准结构 → ACCEPT。

### 修复 5：解码侧同时接受 indirect-reference

- 位置：`IEC61850MmsIsoSession.cc` → `DecodeUserInformation()`。
- 旧实现：EXTERNAL 内只接受抽象语法 OID（`06 … 28ca220201`），装置回的
  `be 2f 28 2d 02 01 03 a0 28 …` 会被拒绝，导致 ACCEPT 之后的 AARE 解析失败。
- 新实现：`02`(indirect-reference，值 3) 与 `06 …`(OID) 都接受；仍强制要求 `a0` single-ASN1-type。

### 其它同步改动

- `IsoSessionPduType` 新增 `REFUSE = 0x0c`：装置在 ACSE 阶段拒绝关联时回 REFUSE 而不是 ABORT；
  建链与运行期都识别并输出中文日志，同时把原始 SPDU 十六进制打日志，便于现场定位。
- Session ACCEPT 解码校验连接项：必须存在会话要求参数（`13`）且 Version Number（`16`）为 `02`；
  S-SEL 缺失只记录不拒绝，避免把不同实现的 ACCEPT 误判为失败。
- `tools/device-sim` 模拟器：ACCEPT 改为「AARE + CP」；解码 AARQ 兼容带 CP/不带 CP 两种客户端。
- `MmsServer` 复用同一套编解码，随修复 3/5 自动同步（无需单独改动）。

## 四、验收与测试

- 单元测试 `test/iec61850_mms_iso_session_test.cc` 增加 golden 用例，逐字节比对：
  40 字节 InitiateRequest、AARQ（indirect-reference）、表示层 CP、Session CONNECT 连接项；
  并断言能解析实测 163 字节 Session ACCEPT、AARE 与 InitiateResponse。
- 解码兼容用例：带 CP 的 AARQ、旧的不带 CP 的 AARQ、`0x0c` REFUSE、错误的会话版本号。
- 现场验收（由用户执行）：连接 `192.168.2.254:102` 后日志应依次出现
  `MMS ISO-on-TCP通道已建立` → `收到Session ACCEPT` → `AARE 关联已接受` →
  `在线目录核对完成` → RCB 配置/GI → `READY / 活动通道=1`。
- 与 `tools/device-sim` 的模拟器联调保持通过。

## 五、范围与约束

- 不修改现场装置任何配置。
- COTP CR 参数顺序、TPDU size 未改动（实测装置对其不敏感）。
- OSI 寻址字段（`a2/a3/a6/a7`）按实测可省略，本期不从 SCL 解析。
