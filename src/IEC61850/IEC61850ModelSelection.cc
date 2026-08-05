#include "IEC61850ModelSelection.h"

#include <format>
#include <string>

namespace IEC61850 {
namespace {

template <typename Repeated>
bool HasUnscoped(const Repeated& objects) {
  for (const auto& object : objects) {
    if (object.access_point().empty()) {
      return true;
    }
  }
  return false;
}

template <typename Repeated>
std::size_t NormalizeOwnership(Repeated* objects,
                               const std::string& accessPoint) {
  if (objects == nullptr) {
    return 0;
  }
  std::size_t normalized = 0;
  for (auto& object : *objects) {
    if (object.access_point().empty()) {
      object.set_access_point(accessPoint);
      ++normalized;
    }
  }
  return normalized;
}

template <typename Repeated, typename Add>
void CopyOwned(const Repeated& objects,
               std::string_view accessPoint,
               std::size_t serverAccessPointCount,
               Add add) {
  for (const auto& object : objects) {
    if (!BelongsToAccessPoint(object.access_point(), accessPoint,
                              serverAccessPointCount)) {
      continue;
    }
    auto* copied = add();
    *copied = object;
    copied->set_access_point(std::string(accessPoint));
  }
}

}  // namespace

std::size_t CountServerAccessPoints(const IEC61850Proto::SclIed& ied) {
  std::size_t count = 0;
  for (const auto& accessPoint : ied.access_points()) {
    if (accessPoint.has_server()) {
      ++count;
    }
  }
  return count;
}

bool HasUnscopedAccessPointObjects(const IEC61850Proto::SclIed& ied) {
  return HasUnscoped(ied.logical_nodes()) ||
         HasUnscoped(ied.data_objects()) ||
         HasUnscoped(ied.data_attributes()) ||
         HasUnscoped(ied.data_sets()) ||
         HasUnscoped(ied.report_controls()) ||
         HasUnscoped(ied.gse_controls()) ||
         HasUnscoped(ied.sampled_value_controls()) ||
         HasUnscoped(ied.ext_refs());
}

bool BelongsToAccessPoint(std::string_view objectAccessPoint,
                          std::string_view selectedAccessPoint,
                          std::size_t serverAccessPointCount) {
  return objectAccessPoint == selectedAccessPoint ||
         (objectAccessPoint.empty() && serverAccessPointCount == 1);
}

std::size_t NormalizeSingleServerAccessPointOwnership(
    IEC61850Proto::SclIed* ied) {
  if (ied == nullptr || CountServerAccessPoints(*ied) != 1) {
    return 0;
  }
  std::string accessPoint;
  for (const auto& candidate : ied->access_points()) {
    if (candidate.has_server()) {
      accessPoint = candidate.name();
      break;
    }
  }
  std::size_t normalized = 0;
  normalized += NormalizeOwnership(ied->mutable_logical_nodes(), accessPoint);
  normalized += NormalizeOwnership(ied->mutable_data_objects(), accessPoint);
  normalized += NormalizeOwnership(ied->mutable_data_attributes(), accessPoint);
  normalized += NormalizeOwnership(ied->mutable_data_sets(), accessPoint);
  normalized += NormalizeOwnership(ied->mutable_report_controls(), accessPoint);
  normalized += NormalizeOwnership(ied->mutable_gse_controls(), accessPoint);
  normalized += NormalizeOwnership(ied->mutable_sampled_value_controls(),
                                   accessPoint);
  normalized += NormalizeOwnership(ied->mutable_ext_refs(), accessPoint);
  return normalized;
}

grpc::Status BuildAccessPointIedModel(
    const IEC61850Proto::SclIed& source,
    std::string_view accessPoint,
    IEC61850Proto::SclIed* selected) {
  if (selected == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "目标IED模型不能为空");
  }
  const IEC61850Proto::SclAccessPoint* selectedAccessPoint = nullptr;
  for (const auto& candidate : source.access_points()) {
    if (candidate.name() == accessPoint) {
      selectedAccessPoint = &candidate;
      break;
    }
  }
  if (selectedAccessPoint == nullptr) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format("当前IED中不存在AccessPoint: IED={}, AccessPoint={}",
                    source.name(), accessPoint));
  }
  if (!selectedAccessPoint->has_server()) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format("当前AccessPoint未包含Server: IED={}, AccessPoint={}",
                    source.name(), accessPoint));
  }
  const auto serverAccessPointCount = CountServerAccessPoints(source);
  if (serverAccessPointCount > 1 &&
      HasUnscopedAccessPointObjects(source)) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format(
            "IED存在多个Server AccessPoint但模型对象缺少AP归属，请重新导入SCL: IED={}",
            source.name()));
  }

  selected->Clear();
  selected->set_name(source.name());
  selected->set_manufacturer(source.manufacturer());
  selected->set_type(source.type());
  selected->set_description(source.description());
  *selected->add_access_points() = *selectedAccessPoint;
  CopyOwned(source.logical_nodes(), accessPoint, serverAccessPointCount,
            [&]() { return selected->add_logical_nodes(); });
  CopyOwned(source.data_objects(), accessPoint, serverAccessPointCount,
            [&]() { return selected->add_data_objects(); });
  CopyOwned(source.data_attributes(), accessPoint, serverAccessPointCount,
            [&]() { return selected->add_data_attributes(); });
  CopyOwned(source.data_sets(), accessPoint, serverAccessPointCount,
            [&]() { return selected->add_data_sets(); });
  CopyOwned(source.report_controls(), accessPoint, serverAccessPointCount,
            [&]() { return selected->add_report_controls(); });
  CopyOwned(source.gse_controls(), accessPoint, serverAccessPointCount,
            [&]() { return selected->add_gse_controls(); });
  CopyOwned(source.sampled_value_controls(), accessPoint,
            serverAccessPointCount,
            [&]() { return selected->add_sampled_value_controls(); });
  CopyOwned(source.ext_refs(), accessPoint, serverAccessPointCount,
            [&]() { return selected->add_ext_refs(); });
  return grpc::Status::OK;
}

}  // namespace IEC61850
