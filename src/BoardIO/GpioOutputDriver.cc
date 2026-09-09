#include "GpioOutputDriver.hpp"

#include <fcntl.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>
#include <string_view>
#include <system_error>
#include <utility>

namespace BoardIO {
namespace {

void CopyConsumerLabel(char* destination, std::size_t size) {
  constexpr std::string_view kConsumer = "MskDSP BoardIO Output";
  if (destination == nullptr || size == 0) {
    return;
  }
  const auto count = std::min(size - 1, kConsumer.size());
  std::memcpy(destination, kConsumer.data(), count);
  destination[count] = '\0';
}

std::string ErrnoText(const char* operation, int errorNumber = errno) {
  return std::format("{}失败: {}", operation,
                     std::system_category().message(errorNumber));
}

uint64_t ValuesToBits(const std::vector<bool>& values) {
  uint64_t bits = 0;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (values[index]) {
      bits |= uint64_t{1} << index;
    }
  }
  return bits;
}

uint64_t LineMask(std::size_t count) {
  return count >= 64 ? ~uint64_t{0} : (uint64_t{1} << count) - 1;
}

}  // namespace

GpioOutputDriver::~GpioOutputDriver() {
  Release();
}

bool GpioOutputDriver::Open(
    const std::string& chipPath,
    const std::vector<GpioOutputValue>& initialValues,
    std::string* error) {
  if (error != nullptr) {
    error->clear();
  }
  Release();
  if (chipPath.empty() || initialValues.empty() ||
      initialValues.size() > GPIO_V2_LINES_MAX) {
    if (error != nullptr) {
      *error = "GPIO 设备路径或输出 line 配置无效";
    }
    return false;
  }

  offsets_.reserve(initialValues.size());
  values_.reserve(initialValues.size());
  for (const auto& item : initialValues) {
    if (std::find(offsets_.begin(), offsets_.end(), item.offset) !=
        offsets_.end()) {
      if (error != nullptr) {
        *error = std::format("GPIO 输出 offset {}重复", item.offset);
      }
      Release();
      return false;
    }
    offsets_.push_back(item.offset);
    values_.push_back(item.value);
  }

  chipFd_ = open(chipPath.c_str(), O_RDONLY | O_CLOEXEC);
  if (chipFd_ < 0) {
    if (error != nullptr) {
      *error = ErrnoText("打开 GPIO chip");
    }
    Release();
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

  if (error != nullptr) {
    *error = std::format("GPIO v2/v1 输出 line 请求均失败，v2={}，v1={}",
                         v2Error, v1Error);
  }
  Release();
  return false;
}

bool GpioOutputDriver::SetValue(uint32_t offset, bool value,
                                std::string* error) {
  if (error != nullptr) {
    error->clear();
  }
  if (lineFd_ < 0) {
    if (error != nullptr) {
      *error = "GPIO 输出 line 尚未打开";
    }
    return false;
  }
  const auto found = std::find(offsets_.begin(), offsets_.end(), offset);
  if (found == offsets_.end()) {
    if (error != nullptr) {
      *error = std::format("GPIO 输出 offset {}未申请", offset);
    }
    return false;
  }

  auto nextValues = values_;
  nextValues[static_cast<std::size_t>(found - offsets_.begin())] = value;
  if (!SetAllValues(nextValues, error)) {
    return false;
  }
  values_ = std::move(nextValues);
  return true;
}

bool GpioOutputDriver::Close(std::string* error) {
  if (error != nullptr) {
    error->clear();
  }
  Release();
  return true;
}

bool GpioOutputDriver::OpenV2(std::string* error) {
  gpio_v2_line_request request{};
  request.num_lines = static_cast<__u32>(offsets_.size());
  CopyConsumerLabel(request.consumer, sizeof(request.consumer));
  for (std::size_t index = 0; index < offsets_.size(); ++index) {
    request.offsets[index] = offsets_[index];
  }
  request.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
  request.config.num_attrs = 1;
  request.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
  request.config.attrs[0].attr.values = ValuesToBits(values_);
  request.config.attrs[0].mask = LineMask(values_.size());

  if (ioctl(chipFd_, GPIO_V2_GET_LINE_IOCTL, &request) < 0) {
    const int errorNumber = errno;
    if (error != nullptr) {
      *error = ErrnoText("申请 GPIO v2 输出 line", errorNumber);
    }
    return false;
  }
  lineFd_ = request.fd;
  if (lineFd_ < 0) {
    if (error != nullptr) {
      *error = "GPIO v2 输出 line ioctl 返回无效 fd";
    }
    return false;
  }
  return true;
}

bool GpioOutputDriver::OpenV1(std::string* error) {
  gpiohandle_request request{};
  request.lines = static_cast<__u32>(offsets_.size());
  request.flags = GPIOHANDLE_REQUEST_OUTPUT;
  CopyConsumerLabel(request.consumer_label, sizeof(request.consumer_label));
  for (std::size_t index = 0; index < offsets_.size(); ++index) {
    request.lineoffsets[index] = offsets_[index];
    request.default_values[index] = values_[index] ? 1 : 0;
  }
  if (ioctl(chipFd_, GPIO_GET_LINEHANDLE_IOCTL, &request) < 0) {
    const int errorNumber = errno;
    if (error != nullptr) {
      *error = ErrnoText("申请 GPIO v1 输出 line", errorNumber);
    }
    return false;
  }
  lineFd_ = request.fd;
  if (lineFd_ < 0) {
    if (error != nullptr) {
      *error = "GPIO v1 输出 line ioctl 返回无效 fd";
    }
    return false;
  }
  return true;
}

bool GpioOutputDriver::SetAllValues(const std::vector<bool>& values,
                                    std::string* error) {
  if (usingV2_) {
    gpio_v2_line_values request{};
    request.bits = ValuesToBits(values);
    request.mask = LineMask(values.size());
    if (ioctl(lineFd_, GPIO_V2_LINE_SET_VALUES_IOCTL, &request) < 0) {
      const int errorNumber = errno;
      if (error != nullptr) {
        *error = ErrnoText("设置 GPIO v2 输出值", errorNumber);
      }
      return false;
    }
    return true;
  }

  gpiohandle_data request{};
  for (std::size_t index = 0; index < values.size(); ++index) {
    request.values[index] = values[index] ? 1 : 0;
  }
  if (ioctl(lineFd_, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &request) < 0) {
    const int errorNumber = errno;
    if (error != nullptr) {
      *error = ErrnoText("设置 GPIO v1 输出值", errorNumber);
    }
    return false;
  }
  return true;
}

void GpioOutputDriver::Release() {
  if (lineFd_ >= 0) {
    close(lineFd_);
    lineFd_ = -1;
  }
  if (chipFd_ >= 0) {
    close(chipFd_);
    chipFd_ = -1;
  }
  usingV2_ = false;
  offsets_.clear();
  values_.clear();
}

}  // namespace BoardIO
