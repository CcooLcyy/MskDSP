# tools 目录说明

`tools/` 用于放置集成测试辅助程序（例如协议设备模拟器、数据注入器、联调辅助脚本），不承载生产模块逻辑。

## 当前约定（设备模拟）

- 设备模拟相关代码统一放在 `tools/device-sim/`。
- 设备模拟器按独立进程运行，不接入主模块生命周期管理。
- 本项目不负责模拟进程的启动/停止/守护，联调时由使用方自行拉起。
- 串口模拟器通过 PTY 伪串口实现；IEC 61850 MMS 模拟器使用独立 TCP 进程。
- 模拟协议实现需输出中文收发日志，并包含报文十六进制内容。

## 构建开关

顶层 CMake 提供开关：

```bash
-DMSKDSP_BUILD_TOOLS=ON
```

默认值为 `OFF`，避免影响常规构建流程。

## 目录建议

- 每个工具单独子目录，例如：`tools/dlt645_sim/`、`tools/agc_sim/`
- 每个子目录独立维护 `CMakeLists.txt` 与源码
- 可执行文件建议输出到 `build/tools/`（仅用于本地联调）

## 新增工具最小示例

1. 新建目录 `tools/<tool_name>/`
2. 在 `tools/<tool_name>/CMakeLists.txt` 中声明 `add_executable(...)`
3. 设置输出目录到 `${MSKDSP_TOOLS_OUTPUT_DIR}`
4. 在 `tools/CMakeLists.txt` 增加 `add_subdirectory(<tool_name>)`

## 已接入工具

- `device-sim`：设备模拟器集合。
  - 说明文档：`tools/device-sim/doc/README.md`
  - 示例配置：`tools/device-sim/conf/dlt645_pcd_sim.jsonc`
  - DLT645PCD 目标：`dlt645_pcd_sim`
  - IEC 61850 MMS 目标：`iec61850_mms_sim`
  - IEC 61850 MMS 工作器 smoke 目标：`iec61850_mms_worker_smoke`
  - IEC 61850 MMS 参数和联调说明见设备模拟器文档中的对应章节。
