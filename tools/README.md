# tools 目录说明

`tools/` 用于放置集成测试辅助程序（例如协议设备模拟器、数据注入器、联调辅助脚本），不承载生产模块逻辑。

## 构建开关

顶层 CMake 提供开关：

```bash
-DMSKDSP_BUILD_TOOLS=ON
```

默认值为 `OFF`，避免影响常规构建流程。

## 目录建议

- 每个工具单独子目录，例如：`tools/dlt645_sim/`、`tools/agc_sim/`
- 每个子目录独立维护 `CMakeLists.txt` 与源码
- 可执行文件建议输出到 `package/tools/`

## 新增工具最小示例

1. 新建目录 `tools/<tool_name>/`
2. 在 `tools/<tool_name>/CMakeLists.txt` 中声明 `add_executable(...)`
3. 设置输出目录到 `${MSKDSP_TOOLS_OUTPUT_DIR}`
4. 在 `tools/CMakeLists.txt` 增加 `add_subdirectory(<tool_name>)`
