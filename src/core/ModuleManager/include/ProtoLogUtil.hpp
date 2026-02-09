#pragma once

#include <google/protobuf/message.h>

#include <string>

inline std::string formatProtoForLog(const google::protobuf::Message &message) {
#ifdef MSKDSP_TEST_DISABLE_PROTO_LOG
  return "测试环境跳过报文内容";
#else
  auto text = message.ShortDebugString();
  if (text.empty()) {
    return "空";
  }
  return text;
#endif
}
