# ProtoFileStore 最小重构方案

## 1. 背景

当前仓库中，以下 5 个模块各自维护了一份 `detail/ProtoFileStore.hpp`：

- `src/IEC104/include/detail/ProtoFileStore.hpp`
- `src/DataCenter/include/detail/ProtoFileStore.hpp`
- `src/DLT645/include/detail/ProtoFileStore.hpp`
- `src/AGVC/AGC/include/detail/ProtoFileStore.hpp`
- `src/ModbusRTU/include/detail/ProtoFileStore.hpp`

这 5 份实现均为 217 行，核心逻辑一致，实际差异仅体现在：

- 所属 namespace 不同
- 个别文件存在格式化差异

它们共同承载了 `.pb/.bak/.tmp/.corrupt.<timestamp>` 的持久化行为，包括：

- `Save` 前校验 protobuf
- 写入 `.tmp`
- 回读并再次校验
- 将旧主文件转成 `.bak`
- 用新文件替换主文件
- `Load` 时优先读主文件，失败后回退 `.bak`
- 主文件损坏时隔离为 `.corrupt.<timestamp>`

该逻辑已经形成跨模块约定，测试、文档与 `ModuleManager` 的文件痕迹判断均依赖这套行为。

## 2. 重构目标

本轮重构目标仅限于“消除 `ProtoFileStore` 的重复实现”，不改变外部行为。

- 将 5 份重复实现收敛为 1 份公共模板实现
- 保持现有 `Save/Load/path/backupPath/tmpPath` 接口不变
- 保持现有 `.pb/.bak/.tmp/.corrupt` 命名与恢复语义不变
- 保持各模块现有外层 store 类、校验函数与默认文件名不变
- 尽量减少调用点与 include 改动范围

## 3. 非目标

本轮不做以下事项：

- 不合并各模块外层 store 类
- 不调整 protobuf 消息结构
- 不修改配置文件名、目录结构与 `ModuleManager` 的持久化痕迹规则
- 不将 DataCenter 的历史文件名兼容逻辑下沉到公共层
- 不顺手修改日志、错误码、异常语义或落盘时机
- 不在本轮引入新的持久化策略，例如文件锁、fsync、版本头等

## 4. 现状与约束

### 4.1 模块特有逻辑仍然存在

虽然 `ProtoFileStore` 本体重复，但模块差异主要位于外层：

- 校验规则由各模块自己的 `Validate*Config` 或同类函数负责
- 默认文件名由各模块 store 头文件维护
- DataCenter 连接标签注册表存在历史文件名兼容逻辑：当 `conn_tags.pb` 缺失时，兼容读取 `point_tables.pb`
- 外层 store 还承担少量日志与兼容处理

因此，本轮应当只抽公共内核，不应把外层 store 一并合并。

### 4.2 构建组织现状

当前各模块基本只公开自己的 `include/` 目录：

- `src/DataCenter/CMakeLists.txt`
- `src/IEC104/CMakeLists.txt`
- `src/DLT645/CMakeLists.txt`
- `src/ModbusRTU/CMakeLists.txt`
- `src/AGVC/AGC/CMakeLists.txt`

仓库当前没有现成的跨模块公共 include 目录可直接复用，因此本轮重构除了代码收敛外，还需要补一个可被多模块共享的头文件落点。

### 4.3 已有测试覆盖

当前已有测试已经对 `ProtoFileStore` 的关键行为形成约束，至少包括：

- AGC 控制组持久化测试
- DataCenter 连接注册表持久化测试
- DataCenter 连接标签注册表持久化测试
- DataCenter 路由持久化测试
- IEC104 持久化集成测试

因此，本轮必须遵循“先补失败测试，再替换实现”的顺序，避免在无保护下直接合并 5 份实现。

## 5. 推荐方案

### 5.1 公共头文件落点

建议新增公共头文件：

- `src/common/include/mskdsp/detail/ProtoFileStore.hpp`

命名与路径选择理由：

- 放在 `src/common/include/` 下，语义上属于“跨模块公共实现”
- 使用 `mskdsp::detail` 作为中立命名空间，避免把公共持久化工具挂到某个具体模块名下
- 保留 `detail` 层级，表达“这是内部实现细节，不是对外领域接口”

### 5.2 公共模板接口

公共模板保持现有签名与职责不变：

```cpp
namespace mskdsp::detail {
template <typename ProtoT>
class ProtoFileStore {
public:
  using ValidateFn = grpc::Status (*)(const ProtoT&);

  ProtoFileStore(std::filesystem::path path, ValidateFn validate);

  grpc::Status Save(const ProtoT& config) const;
  grpc::Status Load(ProtoT* out) const;

  const std::filesystem::path& path() const;
  std::filesystem::path backupPath() const;
  std::filesystem::path tmpPath() const;
};
}  // namespace mskdsp::detail
```

本轮不调整任何方法命名、返回值类型或错误码风格。

### 5.3 各模块兼容包装层

为减少改动面，建议保留各模块现有的：

- `src/<module>/include/detail/ProtoFileStore.hpp`

但把它们改成很薄的兼容转发层，例如：

```cpp
#pragma once

#include "mskdsp/detail/ProtoFileStore.hpp"

namespace IEC104::detail {
template <typename ProtoT>
using ProtoFileStore = mskdsp::detail::ProtoFileStore<ProtoT>;
}  // namespace IEC104::detail
```

其他模块做同样处理。

这样做的好处：

- 现有调用点仍然继续使用 `detail::ProtoFileStore`
- 现有 `#include "detail/ProtoFileStore.hpp"` 不需要批量修改
- 可先完成“去重”，后续若要进一步统一 include，再单独安排

### 5.4 CMake 最小调整

建议只做最小可用的 include 调整，不额外引入新的公共库目标。

建议新增公共 include 根目录：

- `${CMAKE_SOURCE_DIR}/src/common/include`

并将其加入到以下目标的 `target_include_directories(...)` 中：

- `DataCenter`
- `IEC104`
- `DLT645`
- `ModbusRTU`
- `AGC`
- 直接编译对应 store 源文件的测试目标

说明：

- 由于 `ProtoFileStore` 是模板头文件，本轮没有必要为了它新增一个独立静态库或共享库
- 若后续 `src/common/include` 中继续增长多项公共实现，再考虑抽象成统一的公共头文件组或 interface target

## 6. 测试策略

### 6.1 先补共享层测试

在切换 5 个模块到公共实现之前，建议先新增 1 组直接面向公共模板的测试，例如：

- `test/proto_file_store_test.cc`

优先覆盖以下行为：

- 文件不存在时 `Load` 返回空配置
- `Save -> Load` roundtrip
- 非法配置时 `Save` 返回 `INVALID_ARGUMENT`
- 主文件损坏时 `Load` 回退 `.bak`
- 从 `.bak` 恢复主文件时保留 `.corrupt.<timestamp>`
- `backupPath/tmpPath` 派生规则保持不变
- `ValidateFn == nullptr` 时返回 `INTERNAL`

### 6.2 保留现有模块测试

现有测试保持不删不降级，继续作为兼容保障：

- `test/agc_group_store_test.cc`
- `test/data_center_connection_store_test.cc`
- `test/data_center_point_table_store_test.cc`
- `test/data_center_route_store_test.cc`
- `test/iec104_persistence_test.cc`

这些测试可以验证：

- 公共层替换后，模块外层行为没有漂移
- DataCenter 的历史文件名兼容逻辑没有被误伤
- `ModuleManager` 依赖的文件痕迹语义仍然保持一致

## 7. 实施步骤

建议按以下顺序推进：

1. 新增本方案文档并确认范围
2. 新增公共测试 `test/proto_file_store_test.cc`，先让测试表达公共层预期行为
3. 新增 `src/common/include/mskdsp/detail/ProtoFileStore.hpp`
4. 将 5 个模块的 `detail/ProtoFileStore.hpp` 改为兼容转发层
5. 补充相关 CMake include 目录
6. 仅在用户明确要求时执行编译与测试
7. 若测试通过，再考虑是否需要第二阶段清理直接 include 公共头

## 8. 风险与注意事项

### 8.1 行为漂移风险

公共实现必须逐字保持以下语义不变：

- `.bak/.tmp/.corrupt` 后缀规则
- `Save` 与 `Load` 的状态码
- 备份回退与 best-effort 主文件恢复流程
- 文件缺失时返回空配置而不是错误

### 8.2 DataCenter 兼容逻辑误收敛风险

DataCenter 的 `conn_tags.pb -> point_tables.pb` 历史兼容逻辑必须继续保留在 `DataCenterConnTagsStore` 外层，不得下沉到公共模板，否则会把模块特有行为错误推广为全局默认行为。

### 8.3 构建范围扩大风险

如果本轮顺手批量修改所有 include 路径或引入新的公共 target，会显著放大回归面；本轮应优先采用“公共头 + 模块薄包装”的兼容式落地方式。

## 9. 验收标准

满足以下条件即可认为本轮重构达标：

- 仓库中只保留 1 份 `ProtoFileStore` 核心实现
- 5 个模块的对外行为与当前保持一致
- DataCenter 历史文件名兼容仍然保留
- 现有持久化测试继续能够表达相同行为约束
- 不新增上位机接口变更，不新增 ConfigPusher 配置变更，不修改 `ModuleManager` 的痕迹规则

## 10. 对上位机与配置链路的影响

本次重构属于模块内部实现收敛：

- 不修改 protobuf 接口
- 不修改 gRPC 服务
- 不修改 configPusher 下发流程
- 不修改上位机调用链路

因此，本轮无需新增上位机接口适配动作；只需在实现阶段确保持久化行为与现有文档描述保持一致。
