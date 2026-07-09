#include "AVCDefaultPoints.h"

#include <array>

namespace AVC {
namespace {

constexpr std::array<DefaultPointDefinition, 10> kDefaultPoints{{
    {
        .kind = AVCProto::DEFAULT_POINT_KIND_THEORETICAL_LOWER,
        .tag = "理论可调无功下限",
        .name = "理论可调无功下限",
        .description = "仅由可控成员最小无功约束汇总得到的理论总目标下限",
    },
    {
        .kind = AVCProto::DEFAULT_POINT_KIND_THEORETICAL_UPPER,
        .tag = "理论可调无功上限",
        .name = "理论可调无功上限",
        .description = "仅由可控成员最大无功约束汇总得到的理论总目标上限",
    },
    {
        .kind = AVCProto::DEFAULT_POINT_KIND_DYNAMIC_LOWER,
        .tag = "当前可调无功下限",
        .name = "当前可调无功下限",
        .description = "按当前已采到的不可控成员无功实测修正后的总目标下限；缺测按 0 处理",
    },
    {
        .kind = AVCProto::DEFAULT_POINT_KIND_DYNAMIC_UPPER,
        .tag = "当前可调无功上限",
        .name = "当前可调无功上限",
        .description = "按当前已采到的不可控成员无功实测修正后的总目标上限；缺测按 0 处理",
    },
    {
        .kind = AVCProto::DEFAULT_POINT_KIND_COMMAND_ECHO,
        .tag = "调节返回值",
        .name = "调节返回值",
        .description = "对主站下发到 AVC 命令点的工程量、质量与时间戳做回显",
    },
    {
        .kind = AVCProto::DEFAULT_POINT_KIND_CURRENT_VOLTAGE,
        .tag = "当前电压",
        .name = "当前电压",
        .description = "AVC 当前采集到的主电压测量值",
    },
    {
        .kind = AVCProto::DEFAULT_POINT_KIND_TOTAL_Q_TARGET,
        .tag = "总无功目标",
        .name = "总无功目标",
        .description = "AVC 本轮实际可下发的总无功目标值",
    },
    {
        .kind = AVCProto::DEFAULT_POINT_KIND_TOTAL_Q_MEAS,
        .tag = "总无功实测",
        .name = "总无功实测",
        .description = "由成员无功量测汇总得到的当前总无功实测值",
    },
    {
        .kind = AVCProto::DEFAULT_POINT_KIND_TOTAL_Q_ERROR,
        .tag = "总无功偏差",
        .name = "总无功偏差",
        .description = "总无功目标与总无功实测之间的偏差值",
    },
    {
        .kind = AVCProto::DEFAULT_POINT_KIND_VOLTAGE_ERROR,
        .tag = "电压偏差",
        .name = "电压偏差",
        .description = "目标电压与当前电压之间的偏差值；仅目标电压模式下发布",
    },
}};

}  // namespace

std::span<const DefaultPointDefinition> DefaultPointDefinitions() {
  return kDefaultPoints;
}

bool IsReservedDefaultPointTag(std::string_view tag) {
  for (const auto& point : kDefaultPoints) {
    if (point.tag == tag) {
      return true;
    }
  }
  return false;
}

void FillDefaultPointInfos(google::protobuf::RepeatedPtrField<AVCProto::DefaultPointInfo>* out) {
  if (out == nullptr) {
    return;
  }
  out->Clear();
  for (const auto& point : kDefaultPoints) {
    auto* info = out->Add();
    info->set_kind(point.kind);
    info->set_tag(point.tag.data(), static_cast<int>(point.tag.size()));
    info->set_name(point.name.data(), static_cast<int>(point.name.size()));
    info->set_description(point.description.data(), static_cast<int>(point.description.size()));
  }
}

}  // namespace AVC
