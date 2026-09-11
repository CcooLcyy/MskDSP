#pragma once

#include <google/protobuf/repeated_field.h>

#include <span>
#include <string_view>

#include "AGC.pb.h"
#include "mskdsp/Decimal20.hpp"

namespace AGC {

struct DefaultPointDefinition {
  AGCProto::DefaultPointKind kind;
  std::string_view tag;
  std::string_view name;
  std::string_view description;
};

std::span<const DefaultPointDefinition> DefaultPointDefinitions();
bool IsReservedDefaultPointTag(std::string_view tag);
void FillDefaultPointInfos(google::protobuf::RepeatedPtrField<AGCProto::DefaultPointInfo> *out);
mskdsp::numeric::Decimal20 ComputeInstalledCapacityKw(const AGCProto::GroupConfig &config);

}  // namespace AGC
