#pragma once

#include <chrono>
#include <optional>
#include <string_view>

#include "ModuleManager.grpc.pb.h"

namespace ConfigPusher {
std::optional<ModuleManagerProto::ModuleInfo> findModuleInfo(
    const ModuleManagerProto::ModuleInfos &infos, std::string_view moduleName);
std::optional<ModuleManagerProto::ModuleRunningInfo> findRunningInfo(
    const ModuleManagerProto::ModuleRunningInfos &infos, std::string_view moduleName);

bool fetchModuleInfos(ModuleManagerProto::ModuleManage::StubInterface *stub,
                      ModuleManagerProto::ModuleInfos *out);
bool fetchRunningModuleInfos(ModuleManagerProto::ModuleManage::StubInterface *stub,
                             ModuleManagerProto::ModuleRunningInfos *out);
bool startModule(ModuleManagerProto::ModuleManage::StubInterface *stub,
                 const ModuleManagerProto::ModuleInfo &info);
std::optional<ModuleManagerProto::ModuleRunningInfo> waitForModule(
    ModuleManagerProto::ModuleManage::StubInterface *stub,
    std::string_view moduleName,
    std::chrono::milliseconds timeout);
}  // namespace ConfigPusher
