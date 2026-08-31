#include "GpioEventReader.hpp"

#include <fcntl.h>
#include <linux/gpio.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <string_view>
#include <system_error>

#include "DigitalInputEventProcessor.hpp"

namespace DigitalInput {
namespace {

constexpr int kPollTimeoutMs = 500;

void CopyConsumerLabel(char* destination, std::size_t size) {
  constexpr std::string_view kConsumer = "MskDSP DigitalInput";
  if (destination == nullptr || size == 0) {
    return;
  }
  const auto count = std::min(size - 1, kConsumer.size());
  std::memcpy(destination, kConsumer.data(), count);
  destination[count] = '\0';
}

std::string ErrnoText(const char* operation) {
  return std::format("{}失败: {}", operation,
                     std::system_category().message(errno));
}

}  // namespace

GpioEventReader::GpioEventReader(std::string chipPath,
                                 std::vector<uint32_t> offsets) :
  chipPath_(std::move(chipPath)), offsets_(std::move(offsets)) {}

GpioEventReader::~GpioEventReader() {
  Close();
}

bool GpioEventReader::Open(std::string* error) {
  Close();
  lastError_.clear();
  if (chipPath_.empty() || offsets_.empty() || offsets_.size() > GPIO_V2_LINES_MAX) {
    const std::string message = "GPIO 设备路径或 line offset 配置无效";
    SetError(message);
    if (error != nullptr) {
      *error = message;
    }
    return false;
  }

  chipFd_ = open(chipPath_.c_str(), O_RDONLY | O_CLOEXEC);
  if (chipFd_ < 0) {
    const auto message = ErrnoText("打开 GPIO chip");
    SetError(message);
    if (error != nullptr) {
      *error = message;
    }
    return false;
  }

  std::string v2Error;
  if (OpenV2(&v2Error)) {
    usingV2_ = true;
    return true;
  }

  std::string v1Error;
  if (OpenV1(&v1Error)) {
    usingV2_ = false;
    return true;
  }

  Close();
  const auto message = std::format("GPIO v2/v1 line event 请求均失败，v2={}，v1={}",
                                   v2Error, v1Error);
  SetError(message);
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

void GpioEventReader::Close() {
  if (lineFd_ >= 0) {
    close(lineFd_);
    lineFd_ = -1;
  }
  for (auto& fd : v1LineFds_) {
    if (fd >= 0) {
      close(fd);
      fd = -1;
    }
  }
  v1LineFds_.clear();
  if (chipFd_ >= 0) {
    close(chipFd_);
    chipFd_ = -1;
  }
  usingV2_ = false;
  lastV2Sequence_ = 0;
}

bool GpioEventReader::Wait(std::stop_token stopToken, GpioEvent* event) {
  if (!IsOpen() || event == nullptr) {
    SetError("GPIO event reader 未打开或输出参数为空");
    return false;
  }
  lastError_.clear();
  if (usingV2_) {
    return WaitV2(stopToken, event);
  }
  return WaitV1(stopToken, event);
}

bool GpioEventReader::IsOpen() const {
  if (chipFd_ < 0) {
    return false;
  }
  if (usingV2_) {
    return lineFd_ >= 0;
  }
  return !v1LineFds_.empty();
}

bool GpioEventReader::UsingV2() const {
  return usingV2_;
}

const std::string& GpioEventReader::LastError() const {
  return lastError_;
}

bool GpioEventReader::OpenV2(std::string* error) {
  gpio_v2_line_request request{};
  request.num_lines = static_cast<__u32>(offsets_.size());
  request.event_buffer_size = static_cast<__u32>(offsets_.size() * 16);
  CopyConsumerLabel(request.consumer, sizeof(request.consumer));
  for (std::size_t index = 0; index < offsets_.size(); ++index) {
    request.offsets[index] = offsets_[index];
  }
  request.config.flags = GPIO_V2_LINE_FLAG_INPUT |
                         GPIO_V2_LINE_FLAG_EDGE_RISING |
                         GPIO_V2_LINE_FLAG_EDGE_FALLING;

  if (ioctl(chipFd_, GPIO_V2_GET_LINE_IOCTL, &request) < 0) {
    if (error != nullptr) {
      *error = ErrnoText("申请 GPIO v2 line");
    }
    return false;
  }
  lineFd_ = request.fd;
  if (lineFd_ < 0) {
    if (error != nullptr) {
      *error = "GPIO v2 line ioctl 返回无效 fd";
    }
    return false;
  }
  return true;
}

bool GpioEventReader::OpenV1(std::string* error) {
  v1LineFds_.clear();
  v1LineFds_.reserve(offsets_.size());
  for (const auto offset : offsets_) {
    gpioevent_request request{};
    request.lineoffset = offset;
    request.handleflags = GPIOHANDLE_REQUEST_INPUT;
    request.eventflags = GPIOEVENT_REQUEST_BOTH_EDGES;
    CopyConsumerLabel(request.consumer_label, sizeof(request.consumer_label));
    const int ioctlResult = ioctl(chipFd_, GPIO_GET_LINEEVENT_IOCTL, &request);
    const int ioctlErrno = errno;
    if (ioctlResult < 0) {
      if (error != nullptr) {
        *error = std::format("申请 GPIO v1 line {}失败: {}", offset,
                             std::system_category().message(ioctlErrno));
      }
      for (auto fd : v1LineFds_) {
        close(fd);
      }
      v1LineFds_.clear();
      return false;
    }
    if (request.fd < 0) {
      if (error != nullptr) {
        *error = std::format("申请 GPIO v1 line {}失败: ioctl 返回无效 fd", offset);
      }
      for (auto fd : v1LineFds_) {
        close(fd);
      }
      v1LineFds_.clear();
      return false;
    }
    v1LineFds_.push_back(request.fd);
  }
  return !v1LineFds_.empty();
}

bool GpioEventReader::WaitV2(std::stop_token stopToken, GpioEvent* event) {
  pollfd descriptor{.fd = lineFd_, .events = POLLIN | POLLPRI, .revents = 0};
  while (!stopToken.stop_requested()) {
    const int result = poll(&descriptor, 1, kPollTimeoutMs);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      SetError(ErrnoText("等待 GPIO v2 事件"));
      return false;
    }
    if (result == 0) {
      continue;
    }
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      SetError("GPIO v2 事件 fd 已关闭或发生错误");
      return false;
    }

    gpio_v2_line_event raw{};
    const auto bytes = read(lineFd_, &raw, sizeof(raw));
    if (bytes != static_cast<ssize_t>(sizeof(raw))) {
      if (bytes < 0 && errno == EINTR) {
        continue;
      }
      SetError(bytes < 0 ? ErrnoText("读取 GPIO v2 事件") : "GPIO v2 事件长度异常");
      return false;
    }
    if (raw.id != GPIO_V2_LINE_EVENT_RISING_EDGE &&
        raw.id != GPIO_V2_LINE_EVENT_FALLING_EDGE) {
      continue;
    }
    event->offset = raw.offset;
    event->physicalHigh = raw.id == GPIO_V2_LINE_EVENT_RISING_EDGE;
    event->timestampMs = TimestampToUnixMs(raw.timestamp_ns);
    event->sequenceGap = lastV2Sequence_ != 0 &&
                         raw.seqno != lastV2Sequence_ + 1;
    lastV2Sequence_ = raw.seqno;
    return true;
  }
  return false;
}

bool GpioEventReader::WaitV1(std::stop_token stopToken, GpioEvent* event) {
  std::vector<pollfd> descriptors;
  descriptors.reserve(v1LineFds_.size());
  for (const auto fd : v1LineFds_) {
    descriptors.push_back(pollfd{.fd = fd, .events = POLLIN | POLLPRI, .revents = 0});
  }

  while (!stopToken.stop_requested()) {
    const int result = poll(descriptors.data(), descriptors.size(), kPollTimeoutMs);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      SetError(ErrnoText("等待 GPIO v1 事件"));
      return false;
    }
    if (result == 0) {
      continue;
    }
    for (std::size_t index = 0; index < descriptors.size(); ++index) {
      if ((descriptors[index].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        SetError("GPIO v1 事件 fd 已关闭或发生错误");
        return false;
      }
      if ((descriptors[index].revents & (POLLIN | POLLPRI)) == 0) {
        continue;
      }
      gpioevent_data raw{};
      const auto bytes = read(descriptors[index].fd, &raw, sizeof(raw));
      if (bytes != static_cast<ssize_t>(sizeof(raw))) {
        if (bytes < 0 && errno == EINTR) {
          continue;
        }
        SetError(bytes < 0 ? ErrnoText("读取 GPIO v1 事件") : "GPIO v1 事件长度异常");
        return false;
      }
      if (raw.id != GPIOEVENT_EVENT_RISING_EDGE &&
          raw.id != GPIOEVENT_EVENT_FALLING_EDGE) {
        continue;
      }
      event->offset = offsets_[index];
      event->physicalHigh = raw.id == GPIOEVENT_EVENT_RISING_EDGE;
      event->timestampMs = TimestampToUnixMs(raw.timestamp);
      return true;
    }
  }
  return false;
}

int64_t GpioEventReader::TimestampToUnixMs(uint64_t timestampNs) {
  if (timestampNs == 0) {
    return 0;
  }

  // 目标 GPIO character device ABI 的事件时间戳来自 CLOCK_MONOTONIC；
  // 将其映射到 DataCenter 使用的 Unix 毫秒时间轴。
  const auto systemNow = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  const auto steadyNow = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  return systemNow - steadyNow + static_cast<int64_t>(timestampNs / 1'000'000ULL);
}

void GpioEventReader::SetError(std::string error) {
  lastError_ = std::move(error);
}

}  // namespace DigitalInput
