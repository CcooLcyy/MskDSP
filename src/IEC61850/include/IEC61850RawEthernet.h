#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <optional>
#include <string>
#include <string_view>

#include <grpcpp/support/status.h>

namespace IEC61850 {

struct RawEthernetFilter {
  std::array<std::uint8_t, 6> destinationMac{};
  std::uint16_t etherType = 0x88b8;
  std::uint16_t appId = 0;
  bool vlanTagged = false;
  std::uint16_t vlanId = 0;
  std::optional<std::uint8_t> vlanPriority;
};

enum class RawEthernetTimestampSource : std::uint8_t {
  NONE = 0,
  SOFTWARE = 1,
  HARDWARE = 2,
};

struct RawEthernetFrameView {
  // 从目标EtherType开始的二层载荷，首两个字节为网络字节序APPID。
  std::span<const std::uint8_t> payload;
  // Linux内核提供的CLOCK_REALTIME软件接收时间戳；未启用或不可用时为0。
  std::int64_t kernelTimestampNs = 0;
  // 可选的硬件/PHC时间戳；时钟域由网卡和PTP配置决定，不能直接与steady_clock比较。
  std::int64_t hardwareTimestampNs = 0;
  bool hardwareTimestampValid = false;
  // 当前帧收到的时间戳来源；硬件有效时优先标记HARDWARE，软件时间戳仍保存在kernelTimestampNs。
  RawEthernetTimestampSource timestampSource =
      RawEthernetTimestampSource::NONE;
  std::uint16_t etherType = 0;
  std::uint16_t appId = 0;
  bool vlanTagged = false;
  std::uint16_t vlanId = 0;
  std::uint8_t vlanPriority = 0;
  std::array<std::uint8_t, 6> destinationMac{};
  std::array<std::uint8_t, 6> sourceMac{};
};

// 接收队列只用于吸收突发报文；单帧仍由调用方提供固定上限缓冲。
inline constexpr std::size_t kRawEthernetReceiveBufferBytes = 256 * 1024;

std::optional<std::array<std::uint8_t, 6>> ParseRawMac(
    std::string_view text);

// 解析一份已经接收的完整Ethernet帧。该函数不访问网卡，便于回放测试。
// 返回OK且payload为空表示帧被过滤；返回RESOURCE_EXHAUSTED表示帧被截断。
grpc::Status DecodeRawEthernetFrame(std::span<const std::uint8_t> frame,
                                    const RawEthernetFilter& filter,
                                    RawEthernetFrameView* view);

// Linux AF_PACKET接收适配器。只负责网卡绑定、二层过滤和有界报文缓冲。
class RawEthernetSocket {
public:
  RawEthernetSocket() = default;
  ~RawEthernetSocket();

  RawEthernetSocket(const RawEthernetSocket&) = delete;
  RawEthernetSocket& operator=(const RawEthernetSocket&) = delete;

  grpc::Status Open(std::string_view interfaceName,
                    const RawEthernetFilter& filter,
                    std::size_t receiveBufferBytes =
                        kRawEthernetReceiveBufferBytes);
  void Close() noexcept;
  bool IsOpen() const noexcept;

  // 非阻塞读取；返回OK且frame为空表示当前没有报文。
  grpc::Status Receive(std::span<std::uint8_t> storage,
                       RawEthernetFrameView* frame);
  // 发送接口接收完整Ethernet帧，不负责自动组装目的MAC、VLAN或EtherType。
  grpc::Status Send(std::span<const std::uint8_t> ethernetFrame);

  std::string interfaceName() const;
  std::array<std::uint8_t, 6> localMac() const noexcept;

private:
  int fd_ = -1;
  int ifIndex_ = 0;
  RawEthernetFilter filter_;
  std::string interfaceName_;
  std::array<std::uint8_t, 6> localMac_{};
  bool timestampingEnabled_ = false;
};

}  // namespace IEC61850
