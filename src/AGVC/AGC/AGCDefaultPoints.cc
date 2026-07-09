#include "AGCDefaultPoints.h"

#include <array>

namespace AGC {
namespace {

constexpr std::array<DefaultPointDefinition, 5> kDefaultPoints{{
    {
        .kind = AGCProto::DEFAULT_POINT_KIND_THEORETICAL_LOWER,
        .tag = "理论可调有功下限",
        .name = "理论可调有功下限",
        .description = "仅由可控成员最小出力约束汇总得到的理论总目标下限",
    },
    {
        .kind = AGCProto::DEFAULT_POINT_KIND_THEORETICAL_UPPER,
        .tag = "理论可调有功上限",
        .name = "理论可调有功上限",
        .description = "仅由可控成员最大出力约束汇总得到的理论总目标上限",
    },
    {
        .kind = AGCProto::DEFAULT_POINT_KIND_DYNAMIC_LOWER,
        .tag = "当前可调有功下限",
        .name = "当前可调有功下限",
        .description = "按当前已采到的不可控实际出力修正后的总目标下限；缺测的不可控成员按 0 处理",
    },
    {
        .kind = AGCProto::DEFAULT_POINT_KIND_DYNAMIC_UPPER,
        .tag = "当前可调有功上限",
        .name = "当前可调有功上限",
        .description = "按当前已采到的不可控实际出力修正后的总目标上限；缺测的不可控成员按 0 处理",
    },
    {
        .kind = AGCProto::DEFAULT_POINT_KIND_COMMAND_ECHO,
        .tag = "调节返回值",
        .name = "调节返回值",
        .description = "对主站下发到 AGC 总设定点的工程量、质量与时间戳做回显",
    },
}};

}  // namespace

std::span<const DefaultPointDefinition> DefaultPointDefinitions() {
  return kDefaultPoints;
}

bool IsReservedDefaultPointTag(std::string_view tag) {
  for (const auto &point : kDefaultPoints) {
    if (point.tag == tag) {
      return true;
    }
  }
  return false;
}

void FillDefaultPointInfos(google::protobuf::RepeatedPtrField<AGCProto::DefaultPointInfo> *out) {
  if (out == nullptr) {
    return;
  }
  out->Clear();
  for (const auto &point : kDefaultPoints) {
    auto *info = out->Add();
    info->set_kind(point.kind);
    info->set_tag(point.tag.data(), static_cast<int>(point.tag.size()));
    info->set_name(point.name.data(), static_cast<int>(point.name.size()));
    info->set_description(point.description.data(), static_cast<int>(point.description.size()));
  }
}

}  // namespace AGC
