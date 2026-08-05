#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "IEC61850MmsPdu.h"
#include "IEC61850MmsService.h"
#include "IEC61850MmsIsoSession.h"
#include "IEC61850MmsTransport.h"

namespace {

using namespace std::chrono_literals;

std::atomic<bool> gStop{false};

void OnSignal(int) { gStop.store(true, std::memory_order_release); }

void Log(std::string_view level, std::string_view message) {
  std::cout << "[IEC61850模拟IED] [" << level << "] " << message << '\n';
}

void AppendTlv(std::vector<std::uint8_t>* output, std::uint8_t tag,
               std::span<const std::uint8_t> value) {
  if (output == nullptr || value.size() > 0xffffu) {
    return;
  }
  output->push_back(tag);
  if (value.size() <= 127u) {
    output->push_back(static_cast<std::uint8_t>(value.size()));
  } else if (value.size() <= 0xffu) {
    output->push_back(0x81);
    output->push_back(static_cast<std::uint8_t>(value.size()));
  } else {
    output->push_back(0x82);
    output->push_back(static_cast<std::uint8_t>(value.size() >> 8));
    output->push_back(static_cast<std::uint8_t>(value.size()));
  }
  output->insert(output->end(), value.begin(), value.end());
}

bool AppendEncoded(std::vector<std::uint8_t>* output, grpc::Status status,
                   const std::vector<std::uint8_t>& encoded) {
  if (output == nullptr || !status.ok()) {
    return false;
  }
  output->insert(output->end(), encoded.begin(), encoded.end());
  return true;
}

std::string HexDump(std::span<const std::uint8_t> bytes) {
  std::string result;
  result.reserve(bytes.size() * 3);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index != 0) {
      result.push_back(' ');
    }
    result += std::format("{:02x}", bytes[index]);
  }
  return result;
}

std::string Upper(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::toupper(character));
                 });
  return result;
}

bool IsSupportedValueType(std::string_view valueType) {
  const auto type = Upper(valueType);
  return type == "BOOLEAN" || type == "INT32" || type == "INT32U" ||
         type == "FLOAT32" || type == "FLOAT64" || type == "QUALITY" ||
         type == "TIMESTAMP" || type.starts_with("VISSTRING");
}

bool WriteAll(int fd, std::span<const std::uint8_t> bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto written = send(fd, bytes.data() + offset, bytes.size() - offset,
                              MSG_NOSIGNAL);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

bool ReadExact(int fd, std::span<std::uint8_t> output,
               std::chrono::milliseconds timeout = 2s) {
  std::size_t offset = 0;
  while (offset < output.size()) {
    pollfd descriptor{.fd = fd, .events = POLLIN, .revents = 0};
    const auto boundedTimeout = std::min<std::int64_t>(
        timeout.count(), std::numeric_limits<int>::max());
    const auto ready = poll(&descriptor, 1, static_cast<int>(boundedTimeout));
    if (ready <= 0) {
      return false;
    }
    const auto received = recv(fd, output.data() + offset,
                               output.size() - offset, 0);
    if (received > 0) {
      offset += static_cast<std::size_t>(received);
      continue;
    }
    if (received < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

bool ReadTpktFrame(int fd, std::vector<std::uint8_t>* frame) {
  if (frame == nullptr) {
    return false;
  }
  std::array<std::uint8_t, 4> header{};
  if (!ReadExact(fd, header) || header[0] != 0x03 || header[1] != 0x00) {
    return false;
  }
  const auto length = static_cast<std::size_t>(
      (static_cast<std::uint16_t>(header[2]) << 8) | header[3]);
  if (length < 7u) {
    return false;
  }
  frame->assign(length, 0);
  std::copy(header.begin(), header.end(), frame->begin());
  return ReadExact(fd, std::span<std::uint8_t>(frame->data() + 4, length - 4));
}

bool ReadCotpPayload(int fd, std::vector<std::uint8_t>* payload) {
  if (payload == nullptr) {
    return false;
  }
  payload->clear();
  for (;;) {
    std::vector<std::uint8_t> frame;
    if (!ReadTpktFrame(fd, &frame)) {
      return false;
    }
    std::vector<std::uint8_t> segment(frame.size());
    std::size_t segmentSize = 0;
    bool endOfTransport = false;
    if (!IEC61850::DecodeCotpDataFrame(frame, segment, &segmentSize,
                                       &endOfTransport)
             .ok()) {
      return false;
    }
    payload->insert(payload->end(), segment.begin(),
                    segment.begin() + segmentSize);
    if (endOfTransport) {
      return true;
    }
    if (payload->size() > 4 * 1024 * 1024) {
      return false;
    }
  }
}

bool SendCotpPayload(int fd, std::span<const std::uint8_t> payload) {
  std::vector<std::vector<std::uint8_t>> frames;
  if (!IEC61850::EncodeCotpDataSegments(payload, &frames).ok()) {
    return false;
  }
  for (const auto& frame : frames) {
    if (!WriteAll(fd, frame)) {
      return false;
    }
    Log("调试", std::format("发送COTP数据段: {}", HexDump(frame)));
  }
  return true;
}

std::vector<std::uint8_t> WrapMmsPdu(std::span<const std::uint8_t> mmsPdu) {
  std::array<std::uint8_t, 4096> presentation{};
  std::size_t presentationSize = 0;
  if (!IEC61850::EncodeMmsPresentationData(mmsPdu, presentation,
                                           &presentationSize)
           .ok()) {
    return {};
  }
  std::array<std::uint8_t, 4096> session{};
  std::size_t sessionSize = 0;
  if (!IEC61850::EncodeIsoSessionData(
           std::span<const std::uint8_t>(presentation.data(), presentationSize),
           session, &sessionSize)
           .ok()) {
    return {};
  }
  return {session.begin(), session.begin() + sessionSize};
}

std::vector<std::uint8_t> MakeSessionAccept() {
  IEC61850::MmsInitiateResponse response;
  response.negotiatedParameterSupport.size = 2;
  response.negotiatedParameterSupport.unusedBits = 5;
  response.negotiatedServiceSupport.size = 11;
  response.negotiatedServiceSupport.unusedBits = 3;
  response.negotiatedServiceSupport.bytes[0] = 0x4e;
  response.negotiatedServiceSupport.bytes[1] = 0x08;

  std::array<std::uint8_t, 4096> initiate{};
  std::size_t initiateSize = 0;
  if (!IEC61850::EncodeMmsInitiateResponse(response, initiate, &initiateSize)
           .ok()) {
    return {};
  }
  std::array<std::uint8_t, 4096> aare{};
  std::size_t aareSize = 0;
  if (!IEC61850::EncodeMmsAare(
           IEC61850::kMmsApplicationContextOid, 0,
           std::span<const std::uint8_t>(initiate.data(), initiateSize), aare,
           &aareSize)
           .ok()) {
    return {};
  }
  std::array<std::uint8_t, 4096> session{};
  std::size_t sessionSize = 0;
  if (!IEC61850::EncodeIsoSessionAccept(
           std::span<const std::uint8_t>(aare.data(), aareSize), session,
           &sessionSize)
           .ok()) {
    return {};
  }
  return {session.begin(), session.begin() + sessionSize};
}

std::vector<std::uint8_t> MakeNameListResponse(
    std::uint32_t invokeId, const std::vector<std::string>& identifiers) {
  std::vector<std::uint8_t> list;
  for (const auto& identifier : identifiers) {
    AppendTlv(&list, 0x1a,
              std::span<const std::uint8_t>(
                  reinterpret_cast<const std::uint8_t*>(identifier.data()),
                  identifier.size()));
  }
  std::vector<std::uint8_t> service;
  AppendTlv(&service, 0xa0, list);
  AppendTlv(&service, 0x81, std::array<std::uint8_t, 1>{0x00});
  std::array<std::uint8_t, 4096> mms{};
  std::size_t mmsSize = 0;
  if (!IEC61850::EncodeMmsConfirmedResponse(invokeId, 1, service, mms,
                                            &mmsSize)
           .ok()) {
    return {};
  }
  return WrapMmsPdu(std::span<const std::uint8_t>(mms.data(), mmsSize));
}

std::vector<std::uint8_t> MakeTypeSpecification(std::string_view valueType) {
  const auto type = Upper(valueType);
  if (type == "BOOLEAN") {
    return {0x83, 0x00};
  }

  const auto MakeWidth = [](std::uint8_t width) {
    return std::vector<std::uint8_t>{width};
  };
  const auto MakeFormatWidth = [&MakeWidth](std::uint8_t width) {
    std::vector<std::uint8_t> encoded;
    const auto rawWidth = MakeWidth(width);
    AppendTlv(&encoded, 0x02, rawWidth);
    return encoded;
  };
  if (type == "INT32") {
    const auto width = MakeWidth(32);
    std::vector<std::uint8_t> encoded;
    AppendTlv(&encoded, 0x85, width);
    return encoded;
  }
  if (type == "INT32U") {
    const auto width = MakeWidth(32);
    std::vector<std::uint8_t> encoded;
    AppendTlv(&encoded, 0x86, width);
    return encoded;
  }
  if (type == "FLOAT32" || type == "FLOAT64") {
    std::vector<std::uint8_t> fields;
    const auto formatWidth =
        MakeFormatWidth(type == "FLOAT32" ? 32 : 64);
    const auto exponentWidth = MakeFormatWidth(8);
    fields.insert(fields.end(), formatWidth.begin(), formatWidth.end());
    fields.insert(fields.end(), exponentWidth.begin(), exponentWidth.end());
    std::vector<std::uint8_t> encoded;
    AppendTlv(&encoded, 0xa7, fields);
    return encoded;
  }
  if (type == "QUALITY") {
    const auto width = MakeWidth(13);
    std::vector<std::uint8_t> encoded;
    AppendTlv(&encoded, 0x84, width);
    return encoded;
  }
  if (type == "TIMESTAMP") {
    return {0x91, 0x00};
  }
  if (type.starts_with("VISSTRING")) {
    const auto width = MakeWidth(32);
    std::vector<std::uint8_t> encoded;
    AppendTlv(&encoded, 0x8a, width);
    return encoded;
  }
  return {};
}

std::vector<std::uint8_t> MakeTypedValue(std::string_view valueType) {
  const auto type = Upper(valueType);
  std::vector<std::uint8_t> encoded;
  grpc::Status status;
  if (type == "BOOLEAN") {
    status = IEC61850::EncodeMmsDataBoolean(true, &encoded);
  } else if (type == "INT32") {
    status = IEC61850::EncodeMmsDataSigned(-42, &encoded);
  } else if (type == "INT32U") {
    status = IEC61850::EncodeMmsDataUnsigned(42, &encoded);
  } else if (type == "FLOAT32") {
    status = IEC61850::EncodeMmsDataFloatingPoint(12.5, 0x08, &encoded);
  } else if (type == "FLOAT64") {
    status = IEC61850::EncodeMmsDataFloatingPoint(12.5, 0x0b, &encoded);
  } else if (type == "QUALITY") {
    status = IEC61850::EncodeMmsDataBitString(
        3, std::array<std::uint8_t, 2>{0x00, 0x00}, &encoded);
  } else if (type == "TIMESTAMP") {
    status = IEC61850::EncodeMmsDataUtcTime(1700000000000LL, true, &encoded);
  } else if (type.starts_with("VISSTRING")) {
    status = IEC61850::EncodeMmsDataVisibleString("sim-value", &encoded);
  } else {
    return {};
  }
  return status.ok() ? encoded : std::vector<std::uint8_t>{};
}

std::vector<std::uint8_t> MakeTypeAttributesResponse(
    std::uint32_t invokeId, std::string_view valueType) {
  std::vector<std::uint8_t> service;
  AppendTlv(&service, 0x80, std::array<std::uint8_t, 1>{0x00});
  const auto typeSpecification = MakeTypeSpecification(valueType);
  if (typeSpecification.empty()) {
    return {};
  }
  AppendTlv(&service, 0xa2, typeSpecification);
  std::array<std::uint8_t, 4096> mms{};
  std::size_t mmsSize = 0;
  if (!IEC61850::EncodeMmsConfirmedResponse(invokeId, 6, service, mms,
                                            &mmsSize)
           .ok()) {
    return {};
  }
  return WrapMmsPdu(std::span<const std::uint8_t>(mms.data(), mmsSize));
}

std::vector<std::uint8_t> MakeNamedVariableListAttributesResponse(
    std::uint32_t invokeId, std::string_view domain,
    const std::vector<std::string>& variables) {
  std::vector<std::uint8_t> list;
  for (const auto& variable : variables) {
    std::vector<std::uint8_t> domainName;
    AppendTlv(&domainName, 0x1a,
              std::span<const std::uint8_t>(
                  reinterpret_cast<const std::uint8_t*>(domain.data()),
                  domain.size()));
    AppendTlv(&domainName, 0x1a,
              std::span<const std::uint8_t>(
                  reinterpret_cast<const std::uint8_t*>(variable.data()),
                  variable.size()));

    std::vector<std::uint8_t> objectName;
    AppendTlv(&objectName, 0xa1, domainName);
    std::vector<std::uint8_t> specification;
    AppendTlv(&specification, 0xa0, objectName);
    AppendTlv(&list, 0x30, specification);
  }

  std::vector<std::uint8_t> service;
  AppendTlv(&service, 0x80, std::array<std::uint8_t, 1>{0x00});
  AppendTlv(&service, 0xa1, list);
  std::array<std::uint8_t, 4096> mms{};
  std::size_t mmsSize = 0;
  if (!IEC61850::EncodeMmsConfirmedResponse(invokeId, 12, service, mms,
                                            &mmsSize)
           .ok()) {
    return {};
  }
  return WrapMmsPdu(std::span<const std::uint8_t>(mms.data(), mmsSize));
}

std::vector<std::uint8_t> MakeReadResponse(std::uint32_t invokeId,
                                           std::string_view valueType) {
  IEC61850::MmsReadResponse response;
  auto& item = response.items.emplace_back();
  item.success = true;
  item.encodedData = MakeTypedValue(valueType);
  if (item.encodedData.empty()) {
    return {};
  }
  std::array<std::uint8_t, 4096> mms{};
  std::size_t mmsSize = 0;
  if (!IEC61850::EncodeMmsReadResponse(invokeId, response, mms, &mmsSize)
           .ok()) {
    return {};
  }
  return WrapMmsPdu(std::span<const std::uint8_t>(mms.data(), mmsSize));
}

std::vector<std::uint8_t> MakeEncodedReadResponse(
    std::uint32_t invokeId, std::vector<std::uint8_t> encodedData) {
  IEC61850::MmsReadResponse response;
  auto& item = response.items.emplace_back();
  item.success = true;
  item.encodedData = std::move(encodedData);
  if (item.encodedData.empty()) {
    return {};
  }
  std::array<std::uint8_t, 4096> mms{};
  std::size_t mmsSize = 0;
  if (!IEC61850::EncodeMmsReadResponse(invokeId, response, mms, &mmsSize)
           .ok()) {
    return {};
  }
  return WrapMmsPdu(std::span<const std::uint8_t>(mms.data(), mmsSize));
}

std::vector<std::uint8_t> MakeRcbDataStructure() {
  std::vector<std::uint8_t> fields;
  std::vector<std::uint8_t> encoded;
  if (!AppendEncoded(&fields,
                     IEC61850::EncodeMmsDataVisibleString("RPT1", &encoded),
                     encoded) ||
      !AppendEncoded(&fields, IEC61850::EncodeMmsDataBoolean(false, &encoded),
                     encoded) ||
      !AppendEncoded(&fields, IEC61850::EncodeMmsDataBoolean(false, &encoded),
                     encoded) ||
      !AppendEncoded(&fields, IEC61850::EncodeMmsDataVisibleString(
                                  "IED1LD0/LLN0$ds1", &encoded),
                     encoded) ||
      !AppendEncoded(&fields, IEC61850::EncodeMmsDataUnsigned(7, &encoded),
                     encoded)) {
    return {};
  }
  // SqNum、TimeOfEntry、ReasonCode和ConfRev均由GI报告携带。
  const std::array<std::uint8_t, 2> optionalFields{0x70, 0x80};
  if (!AppendEncoded(&fields,
                     IEC61850::EncodeMmsDataBitString(6, optionalFields,
                                                       &encoded),
                     encoded) ||
      !AppendEncoded(&fields, IEC61850::EncodeMmsDataUnsigned(20, &encoded),
                     encoded) ||
      !AppendEncoded(&fields, IEC61850::EncodeMmsDataUnsigned(0, &encoded),
                     encoded)) {
    return {};
  }
  const std::array<std::uint8_t, 1> triggerOptions{0x44};
  if (!AppendEncoded(&fields,
                     IEC61850::EncodeMmsDataBitString(2, triggerOptions,
                                                       &encoded),
                     encoded) ||
      !AppendEncoded(&fields, IEC61850::EncodeMmsDataUnsigned(5000, &encoded),
                     encoded) ||
      !AppendEncoded(&fields, IEC61850::EncodeMmsDataBoolean(false, &encoded),
                     encoded)) {
    return {};
  }
  std::vector<std::uint8_t> structure;
  AppendTlv(&structure, 0xa2, fields);
  return structure;
}

std::vector<std::uint8_t> MakeGeneralInterrogationReport(
    std::string_view valueType) {
  constexpr std::int64_t kReportTimestampMs = 1700000000000LL;
  std::vector<std::uint8_t> accessResults;
  std::vector<std::uint8_t> encoded;
  if (!AppendEncoded(&accessResults,
                     IEC61850::EncodeMmsDataVisibleString("RPT1", &encoded),
                     encoded)) {
    return {};
  }
  const std::array<std::uint8_t, 2> optionalFields{0x70, 0x80};
  std::vector<std::uint8_t> reportTimestamp;
  if (!AppendEncoded(&accessResults,
                     IEC61850::EncodeMmsDataBitString(6, optionalFields,
                                                       &encoded),
                     encoded) ||
      !AppendEncoded(&accessResults,
                     IEC61850::EncodeMmsDataUnsigned(1, &encoded), encoded) ||
      !AppendEncoded(&accessResults,
                     IEC61850::EncodeMmsDataBinaryTime(kReportTimestampMs,
                                                       &reportTimestamp),
                     reportTimestamp) ||
      !AppendEncoded(&accessResults,
                     IEC61850::EncodeMmsDataUnsigned(7, &encoded), encoded)) {
    return {};
  }
  const std::array<std::uint8_t, 1> inclusion{0x80};
  const auto typedValue = MakeTypedValue(valueType);
  if (typedValue.empty()) {
    return {};
  }
  if (!AppendEncoded(&accessResults,
                     IEC61850::EncodeMmsDataBitString(7, inclusion, &encoded),
                     encoded) ||
      !AppendEncoded(&accessResults,
                     grpc::Status::OK, typedValue)) {
    return {};
  }
  const std::array<std::uint8_t, 1> reason{0x08};
  if (!AppendEncoded(&accessResults,
                     IEC61850::EncodeMmsDataBitString(3, reason, &encoded),
                     encoded)) {
    return {};
  }

  std::vector<std::uint8_t> reportName;
  AppendTlv(&reportName, 0x80,
            std::array<std::uint8_t, 3>{'R', 'P', 'T'});
  std::vector<std::uint8_t> variableListName;
  AppendTlv(&variableListName, 0xa1, reportName);
  std::vector<std::uint8_t> accessList;
  AppendTlv(&accessList, 0xa0, accessResults);
  std::vector<std::uint8_t> report;
  report.insert(report.end(), variableListName.begin(), variableListName.end());
  report.insert(report.end(), accessList.begin(), accessList.end());
  std::vector<std::uint8_t> reportChoice;
  AppendTlv(&reportChoice, 0xa0, report);
  std::vector<std::uint8_t> unconfirmed;
  AppendTlv(&unconfirmed, 0xa3, reportChoice);
  return WrapMmsPdu(unconfirmed);
}

std::vector<std::uint8_t> MakeWriteResponse(std::uint32_t invokeId,
                                            std::size_t itemCount) {
  if (itemCount == 0) {
    return {};
  }
  IEC61850::MmsWriteResponse response;
  response.items.resize(itemCount);
  for (auto& item : response.items) {
    item.success = true;
  }
  std::array<std::uint8_t, 4096> mms{};
  std::size_t mmsSize = 0;
  if (!IEC61850::EncodeMmsWriteResponse(invokeId, response, mms, &mmsSize)
           .ok()) {
    return {};
  }
  return WrapMmsPdu(std::span<const std::uint8_t>(mms.data(), mmsSize));
}

struct Options {
  std::string listenIp = "127.0.0.1";
  std::uint16_t port = 102;
  std::string iedName = "IED1";
  std::string accessPoint = "AP1";
  std::string domain = "IED1LD0";
  std::vector<std::string> variables;
  std::vector<std::string> dataSets;
  std::string valueType = "BOOLEAN";
  std::string readyFile;
  bool rcb = false;
  bool once = false;
};

bool ParsePort(std::string_view text, std::uint16_t* port) {
  if (port == nullptr || text.empty()) {
    return false;
  }
  try {
    const auto value = std::stoul(std::string(text));
    if (value > 65535u) {
      return false;
    }
    *port = static_cast<std::uint16_t>(value);
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseArguments(int argc, char** argv, Options* options) {
  if (options == nullptr) {
    return false;
  }
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    const auto requireValue = [&](std::string* output) {
      if (output == nullptr || index + 1 >= argc) {
        return false;
      }
      *output = argv[++index];
      return true;
    };
    std::string value;
    if (argument == "--listen-ip" && requireValue(&value)) {
      options->listenIp = std::move(value);
    } else if (argument == "--port" && requireValue(&value) &&
               ParsePort(value, &options->port)) {
    } else if (argument == "--ied" && requireValue(&value)) {
      options->iedName = std::move(value);
    } else if (argument == "--access-point" && requireValue(&value)) {
      options->accessPoint = std::move(value);
    } else if (argument == "--domain" && requireValue(&value)) {
      options->domain = std::move(value);
    } else if (argument == "--variable" && requireValue(&value)) {
      options->variables.emplace_back(std::move(value));
    } else if (argument == "--dataset" && requireValue(&value)) {
      options->dataSets.emplace_back(std::move(value));
    } else if (argument == "--type" && requireValue(&value) &&
               IsSupportedValueType(value)) {
      options->valueType = Upper(value);
    } else if (argument == "--ready-file" && requireValue(&value)) {
      options->readyFile = std::move(value);
    } else if (argument == "--rcb") {
      options->rcb = true;
    } else if (argument == "--once") {
      options->once = true;
    } else {
      std::cerr << "用法: iec61850_mms_sim [--listen-ip IPv4] [--port 102] "
                   "[--ied 名称] [--access-point 名称] [--domain 名称] "
                   "[--variable 名称]... [--dataset 名称]... [--type 类型] "
                   "[--ready-file 路径] [--rcb] [--once]\n";
      return false;
    }
  }
  if (options->iedName.empty() || options->accessPoint.empty()) {
    return false;
  }
  return true;
}

class Simulator final {
public:
  explicit Simulator(Options options) : options_(std::move(options)) {}

  int Run() {
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
      Log("错误", std::format("创建监听套接字失败: {}", std::strerror(errno)));
      return 1;
    }
    int reuse = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    in_addr listenAddress{};
    if (inet_pton(AF_INET, options_.listenIp.c_str(), &listenAddress) != 1) {
      Log("错误", "监听地址不是有效IPv4地址");
      Close();
      return 1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr = listenAddress;
    address.sin_port = htons(options_.port);
    if (bind(listenFd_, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0 || listen(listenFd_, 4) != 0) {
      Log("错误", std::format("绑定或监听失败: {}", std::strerror(errno)));
      Close();
      return 1;
    }
    socklen_t addressLength = sizeof(address);
    getsockname(listenFd_, reinterpret_cast<sockaddr*>(&address),
                &addressLength);
    Log("信息", std::format("基础MMS模拟IED已监听 {}:{}，IED={}, AccessPoint={}",
                              options_.listenIp, ntohs(address.sin_port),
                              options_.iedName, options_.accessPoint));
    if (!options_.readyFile.empty()) {
      std::ofstream ready(options_.readyFile,
                          std::ios::out | std::ios::trunc);
      if (!ready) {
        Log("错误", std::format("写入MMS模拟IED就绪文件失败: {}",
                                  options_.readyFile));
        Close();
        return 1;
      }
      ready << ntohs(address.sin_port) << '\n';
      ready.flush();
      if (!ready) {
        Log("错误", std::format("刷新MMS模拟IED就绪文件失败: {}",
                                  options_.readyFile));
        Close();
        return 1;
      }
      Log("调试", std::format("已写入MMS模拟IED就绪文件: {}",
                                options_.readyFile));
    }
    while (!gStop.load(std::memory_order_acquire)) {
      pollfd descriptor{.fd = listenFd_, .events = POLLIN, .revents = 0};
      if (poll(&descriptor, 1, 100) <= 0) {
        continue;
      }
      const auto client = accept(listenFd_, nullptr, nullptr);
      if (client < 0) {
        continue;
      }
      Log("信息", "接收MMS客户端连接");
      HandleClient(client);
      shutdown(client, SHUT_RDWR);
      close(client);
      if (options_.once) {
        break;
      }
    }
    Close();
    return 0;
  }

private:
  void HandleClient(int client) {
    std::vector<std::uint8_t> frame;
    if (!ReadTpktFrame(client, &frame)) {
      Log("警告", "读取COTP连接请求失败");
      return;
    }
    constexpr std::array<std::uint8_t, 13> confirm{
        0x03, 0x00, 0x00, 0x0d, 0x08, 0xd0, 0x00, 0x01,
        0x00, 0x01, 0x00, 0xc0, 0x01};
    if (!WriteAll(client, confirm)) {
      return;
    }
    std::vector<std::uint8_t> connectPayload;
    if (!ReadCotpPayload(client, &connectPayload)) {
      Log("警告", "读取Session CONNECT失败");
      return;
    }
    IEC61850::IsoSessionPduView connect;
    if (!IEC61850::DecodeIsoSessionPdu(connectPayload, &connect).ok() ||
        connect.type != IEC61850::IsoSessionPduType::CONNECT) {
      Log("警告", "Session CONNECT报文无效");
      return;
    }
    const auto accept = MakeSessionAccept();
    if (accept.empty() || !SendCotpPayload(client, accept)) {
      return;
    }

    for (;;) {
      std::vector<std::uint8_t> payload;
      if (!ReadCotpPayload(client, &payload)) {
        return;
      }
      IEC61850::IsoSessionPduView session;
      if (!IEC61850::DecodeIsoSessionPdu(payload, &session).ok()) {
        Log("警告", "Session数据报文解码失败");
        return;
      }
      if (session.type != IEC61850::IsoSessionPduType::DATA) {
        return;
      }
      std::span<const std::uint8_t> mmsPdu;
      if (!IEC61850::DecodeMmsPresentationData(session.userData, &mmsPdu)
               .ok()) {
        Log("警告", "Presentation数据解码失败");
        return;
      }
      Log("调试", std::format("收到MMS报文: {}", HexDump(mmsPdu)));
      bool generalInterrogation = false;
      if (options_.rcb) {
        IEC61850::MmsConfirmedPduView confirmed;
        IEC61850::MmsWriteRequest write;
        std::uint32_t invokeId = 0;
        if (IEC61850::DecodeMmsConfirmedRequest(mmsPdu, &confirmed).ok() &&
            confirmed.serviceTag == 5 &&
            IEC61850::DecodeMmsWriteRequest(mmsPdu, &invokeId, &write).ok()) {
          generalInterrogation = std::any_of(
              write.items.begin(), write.items.end(), [](const auto& item) {
                return item.variable.identifier.ends_with("$GI");
              });
        }
      }
      const auto response = MakeResponse(mmsPdu);
      if (response.empty()) {
        Log("警告", "发送MMS响应失败");
        return;
      }
      if (generalInterrogation) {
        const auto report = MakeGeneralInterrogationReport(options_.valueType);
        if (report.empty() || !SendCotpPayload(client, report)) {
          Log("警告", "发送GI InformationReport失败");
          return;
        }
      }
      if (!SendCotpPayload(client, response)) {
        Log("警告", "发送MMS响应失败");
        return;
      }
    }
  }

  std::vector<std::uint8_t> MakeResponse(
      std::span<const std::uint8_t> mmsPdu) const {
    IEC61850::MmsConfirmedPduView request;
    if (!IEC61850::DecodeMmsConfirmedRequest(mmsPdu, &request).ok()) {
      return {};
    }
    if (request.serviceTag == 1) {
      IEC61850::MmsGetNameListRequest nameList;
      std::uint32_t invokeId = 0;
      if (!IEC61850::DecodeMmsGetNameListRequest(mmsPdu, &invokeId, &nameList)
               .ok()) {
        return {};
      }
      if (nameList.objectClass == IEC61850::MmsObjectClass::DOMAIN) {
        return MakeNameListResponse(invokeId,
                                     options_.domain.empty()
                                         ? std::vector<std::string>{}
                                         : std::vector<std::string>{options_.domain});
      }
      if (nameList.objectClass == IEC61850::MmsObjectClass::NAMED_VARIABLE) {
        return MakeNameListResponse(invokeId, VariableIdentifiers());
      }
      if (nameList.objectClass ==
          IEC61850::MmsObjectClass::NAMED_VARIABLE_LIST) {
        return MakeNameListResponse(invokeId, DataSetIdentifiers());
      }
      return MakeNameListResponse(invokeId, {});
    }
    if (request.serviceTag == 6) {
      return MakeTypeAttributesResponse(request.invokeId, options_.valueType);
    }
    if (request.serviceTag == 12) {
      IEC61850::MmsObjectName objectName;
      if (!IEC61850::DecodeMmsObjectName(request.serviceValue, &objectName)
               .ok()) {
        return {};
      }
      const auto domain = objectName.domain.empty() ? options_.domain
                                                     : objectName.domain;
      return MakeNamedVariableListAttributesResponse(
          request.invokeId, domain, DataSetMembers());
    }
    if (request.serviceTag == 4) {
      IEC61850::MmsReadRequest read;
      std::uint32_t invokeId = 0;
      if (!IEC61850::DecodeMmsReadRequest(mmsPdu, &invokeId, &read).ok() ||
          read.variables.empty()) {
        return {};
      }
      if (options_.rcb &&
          read.variables.front().identifier.ends_with("$UR$urcb1")) {
        return MakeEncodedReadResponse(invokeId, MakeRcbDataStructure());
      }
      return MakeReadResponse(invokeId, options_.valueType);
    }
    if (request.serviceTag == 5) {
      IEC61850::MmsWriteRequest write;
      std::uint32_t invokeId = 0;
      if (!IEC61850::DecodeMmsWriteRequest(mmsPdu, &invokeId, &write).ok()) {
        return {};
      }
      return MakeWriteResponse(invokeId, write.items.size());
    }
    Log("警告", std::format("不支持的MMS服务选择: {}", request.serviceTag));
    return {};
  }

  std::vector<std::string> VariableIdentifiers() const {
    if (!options_.variables.empty()) {
      return options_.variables;
    }
    if (options_.rcb) {
      return {"LLN0", "LLN0$Beh$stVal", "LLN0$UR$urcb1"};
    }
    return {};
  }

  std::vector<std::string> DataSetIdentifiers() const {
    if (!options_.dataSets.empty()) {
      return options_.dataSets;
    }
    return options_.rcb ? std::vector<std::string>{"LLN0$ds1"}
                       : std::vector<std::string>{};
  }

  std::vector<std::string> DataSetMembers() const {
    if (options_.rcb) {
      return {"LLN0$Beh$stVal"};
    }
    return options_.variables;
  }

  void Close() noexcept {
    if (listenFd_ >= 0) {
      shutdown(listenFd_, SHUT_RDWR);
      close(listenFd_);
      listenFd_ = -1;
    }
  }

  Options options_;
  int listenFd_ = -1;
};

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!ParseArguments(argc, argv, &options)) {
    return 2;
  }
  struct sigaction action{};
  action.sa_handler = OnSignal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
  return Simulator(std::move(options)).Run();
}
