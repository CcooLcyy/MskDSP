#pragma once

#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>

#include <string>

#include "Logger.h"

inline std::string formatProtoForLog(const google::protobuf::Message &message) {
#ifdef MSKDSP_TEST_DISABLE_PROTO_LOG
  return "测试环境跳过报文内容";
#else
  google::protobuf::util::JsonPrintOptions options;
  options.add_whitespace = false;
  options.preserve_proto_field_names = true;

  std::string json;
  const auto status = google::protobuf::util::MessageToJsonString(message, &json, options);
  if (status.ok()) {
    if (json.empty() || json == "{}") {
      return "空";
    }
    return json;
  }

  LOG_WARNING("Proto 转 JSON 失败，改用短格式输出: 类型={}, 原因={}",
              message.GetTypeName(),
              status.ToString());

  auto text = message.ShortDebugString();
  if (text.empty()) {
    return "空";
  }
  return text;
#endif
}
