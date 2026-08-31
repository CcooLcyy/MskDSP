#pragma once

#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

namespace DigitalInput {

struct GpioEvent;

class GpioEventReader {
public:
  explicit GpioEventReader(std::string chipPath,
                           std::vector<uint32_t> offsets = {114, 116, 113, 115});
  ~GpioEventReader();

  GpioEventReader(const GpioEventReader&) = delete;
  GpioEventReader& operator=(const GpioEventReader&) = delete;

  bool Open(std::string* error);
  void Close();
  bool Wait(std::stop_token stopToken, GpioEvent* event);

  bool IsOpen() const;
  bool UsingV2() const;
  const std::string& LastError() const;

private:
  bool OpenV2(std::string* error);
  bool OpenV1(std::string* error);
  bool WaitV2(std::stop_token stopToken, GpioEvent* event);
  bool WaitV1(std::stop_token stopToken, GpioEvent* event);
  static int64_t TimestampToUnixMs(uint64_t timestampNs);
  void SetError(std::string error);

  std::string chipPath_;
  std::vector<uint32_t> offsets_;
  int chipFd_ = -1;
  int lineFd_ = -1;
  std::vector<int> v1LineFds_;
  bool usingV2_ = false;
  uint32_t lastV2Sequence_ = 0;
  std::string lastError_;
};

}  // namespace DigitalInput
