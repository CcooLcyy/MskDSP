# Modbus 枚举遥信解码

## 目标与范围

上位机模板中的枚举状态需要按 `raw == enum_value` 发布 BOOL。枚举码不是位图，不能按 `bit_index` 拆分。扩展共用 Modbus 点表与 RTU 单点读取、RTU 区间读取、TCP 读取；配置仍由现有 gRPC 下发，不新增模块或 Calc 运算。

## 接口与规则

- `ModbusRTUProto.Point` 新增 `optional uint32 enum_value = 18`，ModbusTCP 继续复用该消息。
- 字段仅允许 BOOL 与 0x03/0x04。寄存器 BOOL 必须且只能配置 `bit_index`、`enum_value` 中一个；线圈和写点不允许 `enum_value`。
- `reg_count` 允许 1/2（0 按 BOOL 默认 1）；单寄存器枚举码范围为 0..65535，双寄存器为 0..4294967295。0 是有效枚举码，必须保留字段存在性。
- 先按配置的字节序与字序拼接无符号 raw，再执行相等判断；BOOL 忽略 scale/offset/deadband。
- 同功能码、同起始地址、同长度和字节/字序的不同枚举码可共存；重复枚举码拒绝。枚举点与其他占用相同寄存器的点拒绝混用，避免不同解码规则造成歧义。
- 未知原始枚举码会使该寄存器所有已配置枚举 BOOL 为 false，接收与解码成功时品质为 GOOD。响应长度错误或通信失败沿用现有失败处理，不伪造 GOOD 值。
- 旧线圈、数值和 bit BOOL 的行为保持不变。旧下位机不支持新枚举点；上线前必须更新下位机协议与模块。

## 验收

先增加点表验证、共址/重复/重叠冲突、字段 0 值与查询往返测试，再实现字段与解码。公共 `DecodeRegisterBool` 统一三条读取路径的相等/取位语义；覆盖 16/32 位、字节/字序、已知/未知枚举和原有 bit 解码。静态检查确认三条实际读取路径调用公共函数。

当前 Windows 工作机没有下位机编译环境，禁止为此搭建环境或自行远程编译部署；本地执行可用静态检查，GTest 编译与真实通信验收待已有构建环境执行。

## 验证范围与执行入口

- `modbusRtuPointTable_test`：字段互斥、类型/功能码/范围校验、双向地址冲突、0 值与查询往返、公共相等/取位函数。
- `modbusRtuLinkManager_test`：PTY 模拟 0x03/0x04 真实响应，覆盖单点和显式区间、1/2 个寄存器、BA 字节序与 LH 字序，检查已知枚举 true、其他枚举 false 和 GOOD 品质。
- `modbusTcpEnum_test`：随机本机端口模拟 TCP 响应，覆盖 0x03/0x04、1/2 个寄存器、BA/LH 与 BOOL 发布。
- 已有 Linux 构建环境可执行 `cmake --build build --target modbusRtuPointTable_test modbusRtuLinkManager_test modbusTcpEnum_test`，再运行 `ctest --test-dir build -R 'modbus(RtuPointTable|RtuLinkManager|TcpEnum)_test' --output-on-failure`。
- 本次本地检查：proto 描述符编译、公共纯函数的独立 Clang 编译与断言运行、diff 空白检查；这些检查不等同于模块/GTest 构建或设备联调。
