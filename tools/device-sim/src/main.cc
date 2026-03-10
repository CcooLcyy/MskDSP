#include <pty.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

namespace {

constexpr uint8_t kFrameStart = 0x68;
constexpr uint8_t kFrameEnd = 0x16;
constexpr uint8_t kReadControl = 0x11;
constexpr size_t kMinFrameSize = 12;

std::atomic<bool> gStop{false};

void onSignal(int) {
  gStop.store(true);
}

std::string nowText() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
  const auto tt = system_clock::to_time_t(now);

  std::tm tm{};
  localtime_r(&tt, &tm);

  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
  return oss.str();
}

void logLine(const std::string& level, const std::string& instanceName, const std::string& text) {
  std::cout << '[' << nowText() << "] [" << level << "] [" << instanceName << "] " << text << std::endl;
}

bool isHexChar(char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

bool parseHexByte(std::string_view text, uint8_t* out) {
  if (out == nullptr || text.size() != 2) {
    return false;
  }
  if (!isHexChar(text[0]) || !isHexChar(text[1])) {
    return false;
  }
  *out = static_cast<uint8_t>(std::strtoul(std::string(text).c_str(), nullptr, 16));
  return true;
}

bool decodeHexString(std::string_view text, std::vector<uint8_t>* out) {
  if (out == nullptr || (text.size() % 2) != 0) {
    return false;
  }
  out->clear();
  out->reserve(text.size() / 2);
  for (size_t i = 0; i < text.size(); i += 2) {
    uint8_t value = 0;
    if (!parseHexByte(text.substr(i, 2), &value)) {
      return false;
    }
    out->push_back(value);
  }
  return true;
}

std::string formatHex(const std::vector<uint8_t>& data) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (const auto value : data) {
    oss << std::setw(2) << static_cast<int>(value);
  }
  return oss.str();
}

std::string formatHex(const std::array<uint8_t, 4>& data) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (const auto value : data) {
    oss << std::setw(2) << static_cast<int>(value);
  }
  return oss.str();
}

std::string formatHex(const std::array<uint8_t, 6>& data) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (const auto value : data) {
    oss << std::setw(2) << static_cast<int>(value);
  }
  return oss.str();
}

std::string trimAscii(const std::string& text) {
  size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return text.substr(begin, end - begin);
}

std::string stripJsonComments(const std::string& input) {
  std::string out;
  out.reserve(input.size());

  bool inString = false;
  bool escaped = false;
  bool inLineComment = false;
  bool inBlockComment = false;

  for (size_t i = 0; i < input.size(); ++i) {
    const char ch = input[i];
    const char next = (i + 1 < input.size()) ? input[i + 1] : '\0';

    if (inLineComment) {
      if (ch == '\n') {
        inLineComment = false;
        out.push_back(ch);
      }
      continue;
    }

    if (inBlockComment) {
      if (ch == '*' && next == '/') {
        inBlockComment = false;
        ++i;
      }
      continue;
    }

    if (!inString) {
      if (ch == '/' && next == '/') {
        inLineComment = true;
        ++i;
        continue;
      }
      if (ch == '/' && next == '*') {
        inBlockComment = true;
        ++i;
        continue;
      }
    }

    out.push_back(ch);

    if (inString) {
      if (!escaped && ch == '"') {
        inString = false;
      }
      escaped = (!escaped && ch == '\\');
      if (ch != '\\') {
        escaped = false;
      }
      continue;
    }

    if (ch == '"') {
      inString = true;
      escaped = false;
    }
  }

  return out;
}

std::optional<std::string> getRequiredString(const boost::json::object& obj,
                                             const std::string& key,
                                             std::string* err) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->value().is_string()) {
    if (err != nullptr) {
      *err = "字段 " + key + " 缺失或类型错误";
    }
    return std::nullopt;
  }
  return std::string(it->value().as_string().c_str());
}

std::optional<boost::json::array> getRequiredArray(const boost::json::object& obj,
                                                   const std::string& key,
                                                   std::string* err) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->value().is_array()) {
    if (err != nullptr) {
      *err = "字段 " + key + " 缺失或类型错误";
    }
    return std::nullopt;
  }
  return it->value().as_array();
}

bool ensureHexText(const std::string& text, size_t expectLen, std::string* err, const std::string& fieldName) {
  if (text.size() != expectLen) {
    if (err != nullptr) {
      *err = "字段 " + fieldName + " 长度错误";
    }
    return false;
  }
  for (const auto ch : text) {
    if (!isHexChar(ch)) {
      if (err != nullptr) {
        *err = "字段 " + fieldName + " 不是十六进制字符串";
      }
      return false;
    }
  }
  return true;
}

std::array<uint8_t, 6> encodeAddress(const std::string& meterAddr) {
  std::array<uint8_t, 6> out{};
  for (size_t i = 0; i < 6; ++i) {
    uint8_t value = 0;
    parseHexByte(std::string_view(meterAddr).substr(i * 2, 2), &value);
    out[5 - i] = value;
  }
  return out;
}

std::array<uint8_t, 4> encodeDiWire(const std::string& diText) {
  std::array<uint8_t, 4> out{};
  for (size_t i = 0; i < 4; ++i) {
    uint8_t value = 0;
    parseHexByte(std::string_view(diText).substr(i * 2, 2), &value);
    out[3 - i] = value;
  }
  return out;
}

std::string diWireToText(const std::array<uint8_t, 4>& diWire) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (size_t i = 0; i < 4; ++i) {
    oss << std::setw(2) << static_cast<int>(diWire[3 - i]);
  }
  return oss.str();
}

uint8_t checksum(const std::vector<uint8_t>& data) {
  uint32_t sum = 0;
  for (const auto item : data) {
    sum += item;
  }
  return static_cast<uint8_t>(sum & 0xFF);
}

void addOffset33(std::vector<uint8_t>* data) {
  if (data == nullptr) {
    return;
  }
  for (auto& item : *data) {
    item = static_cast<uint8_t>(item + 0x33);
  }
}

void subOffset33(std::vector<uint8_t>* data) {
  if (data == nullptr) {
    return;
  }
  for (auto& item : *data) {
    item = static_cast<uint8_t>(item - 0x33);
  }
}

struct RawFrame {
  std::array<uint8_t, 6> address{};
  uint8_t control{0};
  std::vector<uint8_t> data;
};

bool parseRawFrame(const std::vector<uint8_t>& bytes, RawFrame* out, std::string* err) {
  if (out == nullptr) {
    if (err != nullptr) {
      *err = "解析目标为空";
    }
    return false;
  }
  if (bytes.size() < kMinFrameSize) {
    if (err != nullptr) {
      *err = "帧长度不足";
    }
    return false;
  }
  if (bytes.front() != kFrameStart || bytes.back() != kFrameEnd) {
    if (err != nullptr) {
      *err = "帧起止符错误";
    }
    return false;
  }
  if (bytes[7] != kFrameStart) {
    if (err != nullptr) {
      *err = "帧结构错误";
    }
    return false;
  }
  const auto len = static_cast<size_t>(bytes[9]);
  const size_t expected = 10 + len + 2;
  if (expected != bytes.size()) {
    if (err != nullptr) {
      *err = "长度字段与实际长度不一致";
    }
    return false;
  }

  const auto cs = bytes[10 + len];
  const std::vector<uint8_t> csData(bytes.begin(), bytes.begin() + 10 + len);
  if (checksum(csData) != cs) {
    if (err != nullptr) {
      *err = "校验失败";
    }
    return false;
  }

  for (size_t i = 0; i < 6; ++i) {
    out->address[i] = bytes[1 + i];
  }
  out->control = bytes[8];
  out->data.assign(bytes.begin() + 10, bytes.begin() + 10 + len);
  return true;
}

std::vector<uint8_t> buildRawFrame(const std::array<uint8_t, 6>& address,
                                   uint8_t control,
                                   const std::vector<uint8_t>& data) {
  std::vector<uint8_t> frame;
  frame.reserve(12 + data.size());
  frame.push_back(kFrameStart);
  frame.insert(frame.end(), address.begin(), address.end());
  frame.push_back(kFrameStart);
  frame.push_back(control);
  frame.push_back(static_cast<uint8_t>(data.size()));
  frame.insert(frame.end(), data.begin(), data.end());
  frame.push_back(checksum(frame));
  frame.push_back(kFrameEnd);
  return frame;
}

struct ReadRequest {
  std::array<uint8_t, 6> address{};
  uint8_t control{0};
  std::array<uint8_t, 4> di{};
  uint8_t deviceNo{0};
};

bool parseReadRequest(const RawFrame& frame, ReadRequest* out, std::string* err) {
  if (out == nullptr) {
    if (err != nullptr) {
      *err = "请求目标为空";
    }
    return false;
  }
  if (frame.control != kReadControl) {
    if (err != nullptr) {
      *err = "当前仅支持读控制码 0x11";
    }
    return false;
  }
  if (frame.data.size() < 5) {
    if (err != nullptr) {
      *err = "读请求数据长度不足";
    }
    return false;
  }

  auto decoded = frame.data;
  subOffset33(&decoded);

  out->address = frame.address;
  out->control = frame.control;
  for (size_t i = 0; i < 4; ++i) {
    out->di[i] = decoded[i];
  }
  out->deviceNo = decoded[4];
  return true;
}

std::vector<uint8_t> buildReadResponse(const ReadRequest& request, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> data;
  data.reserve(5 + payload.size());
  data.insert(data.end(), request.di.begin(), request.di.end());
  data.push_back(request.deviceNo);
  data.insert(data.end(), payload.begin(), payload.end());
  addOffset33(&data);
  return buildRawFrame(request.address, static_cast<uint8_t>(request.control | 0x80), data);
}

std::vector<uint8_t> buildErrorResponse(const ReadRequest& request, uint8_t errorCode) {
  std::vector<uint8_t> data{errorCode};
  addOffset33(&data);
  return buildRawFrame(request.address, static_cast<uint8_t>(request.control | 0xC0), data);
}

std::string makeDiKey(const std::array<uint8_t, 4>& diWire) {
  return formatHex(diWire);
}

bool writeAll(int fd, const std::vector<uint8_t>& data) {
  size_t offset = 0;
  while (offset < data.size()) {
    const auto n = ::write(fd, data.data() + offset, data.size() - offset);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    offset += static_cast<size_t>(n);
  }
  return true;
}

class PtyEndpoint {
public:
  PtyEndpoint() = default;
  ~PtyEndpoint() {
    closeAll();
  }

  PtyEndpoint(const PtyEndpoint&) = delete;
  PtyEndpoint& operator=(const PtyEndpoint&) = delete;

  bool openAndLink(const std::string& linkPath, std::string* err) {
    closeAll();

    char slaveName[128] = {0};
    if (openpty(&masterFd_, &slaveFd_, slaveName, nullptr, nullptr) != 0) {
      if (err != nullptr) {
        *err = "创建 PTY 失败，errno=" + std::to_string(errno);
      }
      closeAll();
      return false;
    }

    termios tio{};
    if (tcgetattr(slaveFd_, &tio) == 0) {
      cfmakeraw(&tio);
      tcsetattr(slaveFd_, TCSANOW, &tio);
    }

    slavePath_ = slaveName;
    linkPath_ = linkPath;

    std::error_code ec;
    std::filesystem::remove(linkPath_, ec);
    std::filesystem::create_symlink(slavePath_, linkPath_, ec);
    if (ec) {
      if (err != nullptr) {
        *err = "创建串口软链接失败，errno=" + std::to_string(ec.value());
      }
      closeAll();
      return false;
    }

    return true;
  }

  int masterFd() const {
    return masterFd_;
  }

  const std::string& slavePath() const {
    return slavePath_;
  }

  const std::string& linkPath() const {
    return linkPath_;
  }

  void closeMaster() {
    if (masterFd_ >= 0) {
      ::close(masterFd_);
      masterFd_ = -1;
    }
  }

private:
  void closeAll() {
    if (!linkPath_.empty()) {
      std::error_code ec;
      std::filesystem::remove(linkPath_, ec);
    }
    if (masterFd_ >= 0) {
      ::close(masterFd_);
      masterFd_ = -1;
    }
    if (slaveFd_ >= 0) {
      ::close(slaveFd_);
      slaveFd_ = -1;
    }
    slavePath_.clear();
    linkPath_.clear();
  }

  int masterFd_{-1};
  int slaveFd_{-1};
  std::string slavePath_;
  std::string linkPath_;
};

struct DeviceConfig {
  std::string name;
  std::string meterAddrText;
  std::array<uint8_t, 6> meterAddr{};
  std::string deviceNoText;
  uint8_t deviceNo{0};
  std::string ptyLink;
  std::unordered_map<std::string, std::vector<uint8_t>> readPayloadByDi;
};

struct SimConfig {
  std::vector<DeviceConfig> devices;
};

bool loadConfigFile(const std::string& configPath, SimConfig* out, std::string* err) {
  if (out == nullptr) {
    if (err != nullptr) {
      *err = "配置输出对象为空";
    }
    return false;
  }

  std::ifstream fin(configPath);
  if (!fin.is_open()) {
    if (err != nullptr) {
      *err = "打开配置文件失败";
    }
    return false;
  }
  std::stringstream buffer;
  buffer << fin.rdbuf();

  const std::string source = stripJsonComments(buffer.str());
  boost::system::error_code ec;
  const auto value = boost::json::parse(source, ec);
  if (ec) {
    if (err != nullptr) {
      *err = "解析配置失败，错误码=" + std::to_string(ec.value());
    }
    return false;
  }
  if (!value.is_object()) {
    if (err != nullptr) {
      *err = "配置根节点必须是对象";
    }
    return false;
  }

  const auto root = value.as_object();
  auto devicesValue = getRequiredArray(root, "instances", err);
  if (!devicesValue.has_value()) {
    return false;
  }
  if (devicesValue->empty()) {
    if (err != nullptr) {
      *err = "instances 不能为空";
    }
    return false;
  }

  SimConfig cfg;
  cfg.devices.reserve(devicesValue->size());
  std::unordered_set<std::string> nameSet;
  std::unordered_set<std::string> linkSet;
  std::unordered_set<std::string> addrDeviceSet;

  for (size_t i = 0; i < devicesValue->size(); ++i) {
    const auto& item = (*devicesValue)[i];
    if (!item.is_object()) {
      if (err != nullptr) {
        *err = "instances 元素必须是对象";
      }
      return false;
    }

    const auto obj = item.as_object();
    auto name = getRequiredString(obj, "name", err);
    auto meterAddr = getRequiredString(obj, "meter_addr", err);
    auto deviceNo = getRequiredString(obj, "device_no", err);
    auto ptyLink = getRequiredString(obj, "pty_link", err);
    auto readMap = getRequiredArray(obj, "read_map", err);
    if (!name.has_value() || !meterAddr.has_value() || !deviceNo.has_value() || !ptyLink.has_value() ||
        !readMap.has_value()) {
      return false;
    }

    const auto nameTrim = trimAscii(*name);
    if (nameTrim.empty()) {
      if (err != nullptr) {
        *err = "name 不能为空";
      }
      return false;
    }
    if (!nameSet.insert(nameTrim).second) {
      if (err != nullptr) {
        *err = "name 重复: " + nameTrim;
      }
      return false;
    }

    if (!ensureHexText(*meterAddr, 12, err, "meter_addr")) {
      return false;
    }
    if (!ensureHexText(*deviceNo, 2, err, "device_no")) {
      return false;
    }

    uint8_t deviceNoByte = 0;
    if (!parseHexByte(*deviceNo, &deviceNoByte)) {
      if (err != nullptr) {
        *err = "device_no 解析失败";
      }
      return false;
    }

    DeviceConfig device;
    device.name = nameTrim;
    device.meterAddrText = *meterAddr;
    device.meterAddr = encodeAddress(*meterAddr);
    device.deviceNoText = *deviceNo;
    device.deviceNo = deviceNoByte;
    device.ptyLink = trimAscii(*ptyLink);

    if (device.ptyLink.empty()) {
      if (err != nullptr) {
        *err = "pty_link 不能为空";
      }
      return false;
    }
    if (!linkSet.insert(device.ptyLink).second) {
      if (err != nullptr) {
        *err = "pty_link 重复: " + device.ptyLink;
      }
      return false;
    }

    const std::string addrDeviceKey = device.meterAddrText + "|" + device.deviceNoText;
    if (!addrDeviceSet.insert(addrDeviceKey).second) {
      if (err != nullptr) {
        *err = "meter_addr 与 device_no 组合重复: " + addrDeviceKey;
      }
      return false;
    }

    for (const auto& mapEntry : *readMap) {
      if (!mapEntry.is_object()) {
        if (err != nullptr) {
          *err = "read_map 元素必须是对象";
        }
        return false;
      }
      const auto mapObj = mapEntry.as_object();
      auto di = getRequiredString(mapObj, "di", err);
      auto dataHex = getRequiredString(mapObj, "data_hex", err);
      if (!di.has_value() || !dataHex.has_value()) {
        return false;
      }
      if (!ensureHexText(*di, 8, err, "di")) {
        return false;
      }
      std::vector<uint8_t> payload;
      if (!decodeHexString(*dataHex, &payload)) {
        if (err != nullptr) {
          *err = "data_hex 必须是偶数长度十六进制字符串";
        }
        return false;
      }

      const auto diWire = encodeDiWire(*di);
      const auto key = makeDiKey(diWire);
      device.readPayloadByDi[key] = std::move(payload);
    }

    cfg.devices.push_back(std::move(device));
  }

  *out = std::move(cfg);
  return true;
}

class DeviceRuntime {
public:
  explicit DeviceRuntime(DeviceConfig config) : config_(std::move(config)) {}

  DeviceRuntime(const DeviceRuntime&) = delete;
  DeviceRuntime& operator=(const DeviceRuntime&) = delete;

  ~DeviceRuntime() {
    stop();
  }

  bool start() {
    std::string err;
    if (!pty_.openAndLink(config_.ptyLink, &err)) {
      logLine("错误", config_.name, "创建模拟串口失败: " + err);
      return false;
    }

    logLine("信息", config_.name,
            "实例启动完成，表地址=" + config_.meterAddrText + "，设备号=" + config_.deviceNoText +
                "，串口=" + pty_.linkPath() + "，真实端=" + pty_.slavePath());

    stop_.store(false);
    worker_ = std::thread([this]() { runLoop(); });
    return true;
  }

  void stop() {
    stop_.store(true);
    pty_.closeMaster();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

private:
  void runLoop() {
    std::vector<uint8_t> recvBuffer;
    recvBuffer.reserve(1024);

    std::array<uint8_t, 512> temp{};
    while (!stop_.load()) {
      const auto n = ::read(pty_.masterFd(), temp.data(), temp.size());
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (!stop_.load()) {
          logLine("错误", config_.name, "串口读取失败，errno=" + std::to_string(errno));
        }
        break;
      }
      if (n == 0) {
        if (!stop_.load()) {
          logLine("警告", config_.name, "串口连接已关闭");
        }
        break;
      }

      recvBuffer.insert(recvBuffer.end(), temp.begin(), temp.begin() + n);
      consumeFrames(&recvBuffer);
    }

    logLine("信息", config_.name, "实例停止");
  }

  void consumeFrames(std::vector<uint8_t>* buffer) {
    if (buffer == nullptr) {
      return;
    }
    while (true) {
      auto it = std::find(buffer->begin(), buffer->end(), kFrameStart);
      if (it != buffer->begin()) {
        buffer->erase(buffer->begin(), it);
      }

      if (buffer->size() < kMinFrameSize) {
        return;
      }

      if ((*buffer)[7] != kFrameStart) {
        buffer->erase(buffer->begin());
        continue;
      }

      const auto expected = static_cast<size_t>((*buffer)[9]) + 12;
      if (expected < kMinFrameSize) {
        buffer->erase(buffer->begin());
        continue;
      }
      if (buffer->size() < expected) {
        return;
      }

      std::vector<uint8_t> frame(buffer->begin(), buffer->begin() + expected);
      buffer->erase(buffer->begin(), buffer->begin() + expected);
      handleFrame(frame);
    }
  }

  void handleFrame(const std::vector<uint8_t>& frameBytes) {
    logLine("信息", config_.name, "接收报文=" + formatHex(frameBytes));

    RawFrame frame;
    std::string parseErr;
    if (!parseRawFrame(frameBytes, &frame, &parseErr)) {
      logLine("警告", config_.name, "收到非法帧，原因=" + parseErr);
      return;
    }

    if (frame.address != config_.meterAddr) {
      logLine("警告", config_.name,
              "地址不匹配，期望=" + formatHex(config_.meterAddr) + "，实际=" + formatHex(frame.address));
      return;
    }

    ReadRequest request;
    if (!parseReadRequest(frame, &request, &parseErr)) {
      logLine("警告", config_.name, "请求解析失败，原因=" + parseErr);

      ReadRequest fallback;
      fallback.address = frame.address;
      fallback.control = frame.control;
      auto errFrame = buildErrorResponse(fallback, 0x01);
      sendFrame(errFrame, "请求格式错误");
      return;
    }

    if (request.deviceNo != config_.deviceNo) {
      logLine("警告", config_.name,
              "设备号不匹配，期望=" + config_.deviceNoText + "，实际=" +
                  formatHex(std::vector<uint8_t>{request.deviceNo}));
      auto errFrame = buildErrorResponse(request, 0x04);
      sendFrame(errFrame, "设备号不匹配");
      return;
    }

    const auto key = makeDiKey(request.di);
    const auto it = config_.readPayloadByDi.find(key);
    if (it == config_.readPayloadByDi.end()) {
      const auto diText = diWireToText(request.di);
      logLine("警告", config_.name, "未配置 DI 数据，DI=" + diText);
      auto errFrame = buildErrorResponse(request, 0x02);
      sendFrame(errFrame, "DI 未配置");
      return;
    }

    const auto diText = diWireToText(request.di);
    auto response = buildReadResponse(request, it->second);
    sendFrame(response, "读请求应答，DI=" + diText + "，数据=" + formatHex(it->second));
  }

  void sendFrame(const std::vector<uint8_t>& frame, const std::string& reason) {
    if (!writeAll(pty_.masterFd(), frame)) {
      logLine("错误", config_.name, "发送报文失败，errno=" + std::to_string(errno));
      return;
    }
    logLine("信息", config_.name, "发送报文=" + formatHex(frame) + "，说明=" + reason);
  }

  DeviceConfig config_;
  PtyEndpoint pty_;
  std::atomic<bool> stop_{false};
  std::thread worker_;
};

void printUsage(const char* argv0) {
  std::cout << "用法: " << argv0 << " --config <配置文件路径>" << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
  signal(SIGINT, onSignal);
  signal(SIGTERM, onSignal);

  std::string configPath;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      configPath = argv[++i];
      continue;
    }
    if (arg == "-h" || arg == "--help") {
      printUsage(argv[0]);
      return 0;
    }
    std::cerr << "未知参数: " << arg << std::endl;
    printUsage(argv[0]);
    return 1;
  }

  if (configPath.empty()) {
    std::cerr << "缺少 --config 参数" << std::endl;
    printUsage(argv[0]);
    return 1;
  }

  SimConfig cfg;
  std::string err;
  if (!loadConfigFile(configPath, &cfg, &err)) {
    std::cerr << "加载配置失败: " << err << std::endl;
    return 1;
  }

  std::vector<std::unique_ptr<DeviceRuntime>> runtimes;
  runtimes.reserve(cfg.devices.size());

  for (auto& device : cfg.devices) {
    auto runtime = std::make_unique<DeviceRuntime>(std::move(device));
    if (!runtime->start()) {
      std::cerr << "启动设备实例失败" << std::endl;
      return 2;
    }
    runtimes.push_back(std::move(runtime));
  }

  std::cout << "DLT645PCD 模拟器已启动，共 " << runtimes.size() << " 个实例。按 Ctrl+C 退出。" << std::endl;

  while (!gStop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  for (auto& runtime : runtimes) {
    runtime->stop();
  }
  std::cout << "DLT645PCD 模拟器已退出" << std::endl;
  return 0;
}
