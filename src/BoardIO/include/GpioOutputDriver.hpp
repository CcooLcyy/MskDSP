#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace BoardIO {

struct GpioOutputValue {
  uint32_t offset = 0;
  bool value = false;
};

class GpioOutputDriver {
public:
  virtual ~GpioOutputDriver();

  virtual bool Open(const std::string& chipPath,
                    const std::vector<GpioOutputValue>& initialValues,
                    std::string* error);
  virtual bool SetValue(uint32_t offset, bool value, std::string* error);
  virtual bool Close(std::string* error);

private:
  bool OpenV2(std::string* error);
  bool OpenV1(std::string* error);
  bool SetAllValues(const std::vector<bool>& values, std::string* error);
  void Release();

  int chipFd_ = -1;
  int lineFd_ = -1;
  bool usingV2_ = false;
  std::vector<uint32_t> offsets_;
  std::vector<bool> values_;
};

}  // namespace BoardIO
