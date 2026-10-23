#pragma once

#include <grpcpp/support/status.h>

#include <algorithm>
#include <boost/json.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>

namespace mskdsp {
inline constexpr size_t kMaxModbusTemplateBindingsBytes = 32u * 1024u * 1024u;
inline constexpr int kMaxModbusPointTableMessageBytes = 64 * 1024 * 1024;

// 来源是控制面元数据；校验身份与映射结构，点表运行逻辑不依赖平台字段。
inline grpc::Status ValidateTemplateBindings(std::string_view json, uint32_t expectedAddressBase = 0) {
  const auto invalid = [] { return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "模板来源结构非法、映射重复或超过大小限制"); };
  if (json.empty())
    return grpc::Status::OK;
  if (json.size() > kMaxModbusTemplateBindingsBytes)
    return invalid();
  boost::system::error_code error;
  const auto value = boost::json::parse(boost::json::string_view(json.data(), json.size()), error);
  if (error || !value.is_object())
    return invalid();
  const auto &root = value.as_object();
  const auto *schema = root.if_contains("schema_version");
  const auto *applications = root.if_contains("applications");
  if (!schema || !schema->is_int64() || schema->as_int64() != 1 || !applications || !applications->is_array())
    return invalid();
  if (applications->as_array().size() > 1000)
    return invalid();
  const auto stringField = [](const boost::json::object &object, const char *key, bool allowEmpty = false) {
    const auto *field = object.if_contains(key);
    return field && field->is_string() && (allowEmpty || !field->as_string().empty()) && field->as_string().find('\0') == boost::json::string::npos;
  };
  const auto versionField = [&stringField](const boost::json::object &object, const char *key) {
    if (!stringField(object, key))
      return false;
    const auto &version = object.at(key).as_string();
    if (version.size() > 20 || (version.size() == 20 && version > "18446744073709551615"))
      return false;
    return version.front() != '0' && std::all_of(version.begin(), version.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
  };
  std::unordered_set<std::string> applicationIds;
  std::unordered_set<std::string> targetTags;
  size_t mappingCount = 0;
  for (const auto &application : applications->as_array()) {
    if (!application.is_object())
      return invalid();
    const auto &item = application.as_object();
    if (!stringField(item, "application_id") || !stringField(item, "platform_id") || !stringField(item, "template_id") ||
        !versionField(item, "revision") || !stringField(item, "prefix", true))
      return invalid();
    if (!applicationIds.insert(std::string(item.at("application_id").as_string())).second)
      return invalid();
    const auto *snapshot = item.if_contains("template_snapshot");
    const auto *addressBase = item.if_contains("address_base");
    const auto *mappings = item.if_contains("mappings");
    if (!snapshot || !snapshot->is_object() || !addressBase || !mappings || !mappings->is_array())
      return invalid();
    if (!addressBase->is_int64() || (addressBase->as_int64() != 1 && addressBase->as_int64() != 2) ||
        item.at("application_id").as_string().size() > 256 || item.at("platform_id").as_string().size() > 2048 ||
        item.at("template_id").as_string().size() > 256 || item.at("prefix").as_string().size() > 256)
      return invalid();
    if (expectedAddressBase != 0 && addressBase->as_int64() != expectedAddressBase) {
      return {grpc::StatusCode::ABORTED, "连接地址基准与模板应用记录不一致，请重新读取并比对"};
    }
    std::unordered_set<std::string> sourceIds;
    for (const auto &mapping : mappings->as_array()) {
      if (++mappingCount > 10000 || !mapping.is_object())
        return invalid();
      const auto &fields = mapping.as_object();
      if (!stringField(fields, "source_point_id") || !stringField(fields, "target_tag") || !versionField(fields, "source_revision"))
        return invalid();
      if (fields.at("source_point_id").as_string().size() > 128 || fields.at("target_tag").as_string().size() > 256)
        return invalid();
      const auto *baseline = fields.if_contains("baseline");
      const auto *source = fields.if_contains("source_snapshot");
      if (!baseline || !baseline->is_object() || !source || !source->is_object() ||
          !stringField(baseline->as_object(), "tag") || !stringField(source->as_object(), "id"))
        return invalid();
      if (baseline->as_object().at("tag").as_string().size() > 256 || source->as_object().at("id") != fields.at("source_point_id"))
        return invalid();
      if (!sourceIds.insert(std::string(fields.at("source_point_id").as_string())).second ||
          !targetTags.insert(std::string(fields.at("target_tag").as_string())).second)
        return invalid();
    }
  }
  return grpc::Status::OK;
}
}  // 命名空间结束
