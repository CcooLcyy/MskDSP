#include "IEC61850RawEthernet.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>
#include <limits>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/if_ether.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace IEC61850 {
namespace {

grpc::Status SystemError(std::string_view operation) {
  const int error = errno;
  const auto code = error == EPERM || error == EACCES
                        ? grpc::StatusCode::PERMISSION_DENIED
                        : grpc::StatusCode::UNAVAILABLE;
  return grpc::Status(code,
                      std::format("{}失败: {}", operation, std::strerror(error)));
}

bool MatchMac(const std::uint8_t* actual,
              const std::array<std::uint8_t, 6>& expected) noexcept {
  return std::memcmp(actual, expected.data(), expected.size()) == 0;
}

bool IsZeroMac(const std::array<std::uint8_t, 6>& mac) noexcept {
  return std::all_of(mac.begin(), mac.end(),
                     [](std::uint8_t value) { return value == 0; });
}

bool ConvertTimestamp(const timespec& timestamp,
                      std::int64_t* timestampNs) noexcept {
  if (timestampNs == nullptr || timestamp.tv_sec < 0 ||
      timestamp.tv_nsec < 0 || timestamp.tv_nsec >= 1'000'000'000 ||
      timestamp.tv_sec >
          (std::numeric_limits<std::int64_t>::max() - timestamp.tv_nsec) /
              1'000'000'000) {
    return false;
  }
  *timestampNs = static_cast<std::int64_t>(timestamp.tv_sec) * 1'000'000'000 +
                 timestamp.tv_nsec;
  return true;
}

}  // namespace

RawEthernetSocket::~RawEthernetSocket() { Close(); }

grpc::Status DecodeRawEthernetFrame(std::span<const std::uint8_t> frame,
                                    const RawEthernetFilter& filter,
                                    RawEthernetFrameView* view) {
  if (view == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850二层帧视图参数无效");
  }
  *view = {};
  if (frame.size() < ETH_HLEN) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "IEC61850二层报文长度不足以解析Ethernet头");
  }
  const auto* bytes = frame.data();
  if (!MatchMac(bytes, filter.destinationMac)) {
    return grpc::Status::OK;
  }
  const auto etherType = static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(bytes[12]) << 8) | bytes[13]);
  std::size_t offset = ETH_HLEN;
  bool tagged = false;
  std::uint16_t vlanId = 0;
  std::uint8_t vlanPriority = 0;
  std::uint16_t payloadType = etherType;
  if (etherType == ETH_P_8021Q || etherType == ETH_P_8021AD) {
    if (frame.size() < ETH_HLEN + 4) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "IEC61850二层VLAN报文长度不足");
    }
    tagged = true;
    const auto vlanTci = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[14]) << 8) | bytes[15]);
    vlanId = static_cast<std::uint16_t>(vlanTci & 0x0fff);
    vlanPriority = static_cast<std::uint8_t>((vlanTci >> 13) & 0x07);
    payloadType = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[16]) << 8) | bytes[17]);
    offset += 4;
  }
  if (payloadType != filter.etherType || filter.vlanTagged != tagged ||
      (tagged && filter.vlanId != vlanId) ||
      (tagged && filter.vlanPriority.has_value() &&
       *filter.vlanPriority != vlanPriority)) {
    return grpc::Status::OK;
  }
  if (frame.size() < offset + 2) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "IEC61850二层报文缺少APPID");
  }
  const auto appId = static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(bytes[offset]) << 8) | bytes[offset + 1]);
  if (appId != filter.appId) {
    return grpc::Status::OK;
  }
  view->payload = frame.subspan(offset);
  view->etherType = payloadType;
  view->appId = appId;
  view->vlanTagged = tagged;
  view->vlanId = vlanId;
  view->vlanPriority = vlanPriority;
  std::copy_n(bytes, view->destinationMac.size(), view->destinationMac.begin());
  std::copy_n(bytes + 6, view->sourceMac.size(), view->sourceMac.begin());
  return grpc::Status::OK;
}

grpc::Status RawEthernetSocket::Open(std::string_view interfaceName,
                                     const RawEthernetFilter& filter,
                                     std::size_t receiveBufferBytes) {
  Close();
  if (interfaceName.empty() || receiveBufferBytes < 64 ||
      receiveBufferBytes > 1024 * 1024 || filter.etherType == 0 ||
      filter.appId == 0 || IsZeroMac(filter.destinationMac) ||
      (filter.vlanPriority.has_value() && *filter.vlanPriority > 7)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850二层网卡过滤参数无效");
  }
  std::string name(interfaceName);
  if (name.size() >= IFNAMSIZ) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850网卡名称过长");
  }
  ifIndex_ = static_cast<int>(if_nametoindex(name.c_str()));
  if (ifIndex_ == 0) {
    return SystemError("IEC61850获取网卡索引");
  }
  fd_ = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
  if (fd_ < 0) {
    return SystemError("IEC61850创建AF_PACKET套接字");
  }
  const int flags = fcntl(fd_, F_GETFL, 0);
  if (flags < 0 || fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
    const auto status = SystemError("IEC61850设置二层套接字非阻塞");
    Close();
    return status;
  }
  const auto requested = static_cast<int>(receiveBufferBytes);
  if (setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &requested, sizeof(requested)) < 0) {
    const auto status = SystemError("IEC61850设置二层接收缓冲");
    Close();
    return status;
  }
  bool softwareTimestampEnabled = false;
#ifdef SO_TIMESTAMPNS
  const int enableTimestamp = 1;
  if (setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPNS, &enableTimestamp,
                 sizeof(enableTimestamp)) == 0) {
    softwareTimestampEnabled = true;
  }
#endif

  bool timestampingEnabled = false;
#ifdef SO_TIMESTAMPING
  // 同时请求系统硬件、原始硬件和软件接收时间戳；驱动不支持硬件时，
  // 内核仍会返回软件时间戳，不改变现有接收路径。
  constexpr int timestampingFlags =
      SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RX_SOFTWARE |
      SOF_TIMESTAMPING_SOFTWARE | SOF_TIMESTAMPING_SYS_HARDWARE |
      SOF_TIMESTAMPING_RAW_HARDWARE;
  const int enableTimestamping = timestampingFlags;
  if (setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPING, &enableTimestamping,
                 sizeof(enableTimestamping)) == 0) {
    timestampingEnabled = true;
  }
#endif
  if (!timestampingEnabled && !softwareTimestampEnabled) {
    const auto status = SystemError("IEC61850启用二层内核时间戳");
    Close();
    return status;
  }
  timestampingEnabled_ = timestampingEnabled;
  sockaddr_ll address{};
  address.sll_family = AF_PACKET;
  address.sll_protocol = htons(ETH_P_ALL);
  address.sll_ifindex = ifIndex_;
  if (bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
    const auto status = SystemError("IEC61850绑定二层网卡");
    Close();
    return status;
  }
  ifreq hardwareAddress{};
  std::copy(name.begin(), name.end(), hardwareAddress.ifr_name);
  if (ioctl(fd_, SIOCGIFHWADDR, &hardwareAddress) < 0) {
    const auto status = SystemError("IEC61850获取网卡本机MAC");
    Close();
    return status;
  }
  std::copy_n(reinterpret_cast<const std::uint8_t*>(
                  hardwareAddress.ifr_hwaddr.sa_data),
              localMac_.size(), localMac_.begin());
  if ((filter.destinationMac[0] & 0x01) != 0) {
    packet_mreq membership{};
    membership.mr_ifindex = ifIndex_;
    membership.mr_type = PACKET_MR_MULTICAST;
    membership.mr_alen = filter.destinationMac.size();
    std::copy(filter.destinationMac.begin(), filter.destinationMac.end(),
              membership.mr_address);
    if (setsockopt(fd_, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &membership,
                   sizeof(membership)) < 0) {
      const auto status = SystemError("IEC61850加入二层组播");
      Close();
      return status;
    }
  }
  filter_ = filter;
  interfaceName_ = std::move(name);
  return grpc::Status::OK;
}

void RawEthernetSocket::Close() noexcept {
  if (fd_ >= 0) {
    close(fd_);
  }
  fd_ = -1;
  ifIndex_ = 0;
  interfaceName_.clear();
  localMac_.fill(0);
  timestampingEnabled_ = false;
}

bool RawEthernetSocket::IsOpen() const noexcept { return fd_ >= 0; }

grpc::Status RawEthernetSocket::Receive(std::span<std::uint8_t> storage,
                                        RawEthernetFrameView* frame) {
  if (frame == nullptr || storage.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850二层接收缓冲参数无效");
  }
  *frame = {};
  if (fd_ < 0) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850二层网卡尚未打开");
  }
  iovec io{};
  io.iov_base = storage.data();
  io.iov_len = storage.size();
  std::array<std::uint8_t, CMSG_SPACE(sizeof(timespec) * 3) +
                               CMSG_SPACE(sizeof(timespec))>
      control{};
  msghdr message{};
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  message.msg_control = control.data();
  message.msg_controllen = control.size();
  const auto received = recvmsg(fd_, &message, MSG_DONTWAIT | MSG_TRUNC);
  if (received < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return grpc::Status::OK;
    }
    return SystemError("IEC61850接收二层报文");
  }
  if (static_cast<std::size_t>(received) > storage.size()) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "IEC61850二层报文超过预分配接收缓冲");
  }
  if ((message.msg_flags & MSG_CTRUNC) != 0) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "IEC61850二层时间戳控制消息被截断");
  }
  const auto status = DecodeRawEthernetFrame(
      std::span<const std::uint8_t>(storage.data(),
                                    static_cast<std::size_t>(received)),
      filter_, frame);
  if (!status.ok() || frame->payload.empty()) {
    return status;
  }
  bool timestampDecoded = false;
#ifdef SO_TIMESTAMPING
  if (timestampingEnabled_) {
    for (auto* controlMessage = CMSG_FIRSTHDR(&message);
         controlMessage != nullptr;
         controlMessage = CMSG_NXTHDR(&message, controlMessage)) {
      if (controlMessage->cmsg_level != SOL_SOCKET ||
          controlMessage->cmsg_type != SO_TIMESTAMPING ||
          controlMessage->cmsg_len < CMSG_LEN(sizeof(timespec) * 3)) {
        continue;
      }
      std::array<timespec, 3> timestamps{};
      std::memcpy(timestamps.data(), CMSG_DATA(controlMessage),
                  sizeof(timestamps));
      // [1]是经系统时钟转换的硬件时间戳，[2]是原始PHC时间戳；硬件值只
      // 进入独立字段，不能覆盖与CLOCK_REALTIME兼容的软件时间戳。
      for (const auto index : {std::size_t{1}, std::size_t{2}}) {
        if (!ConvertTimestamp(timestamps[index],
                              &frame->hardwareTimestampNs) ||
            frame->hardwareTimestampNs <= 0) {
          continue;
        }
        frame->hardwareTimestampValid = true;
        frame->timestampSource = RawEthernetTimestampSource::HARDWARE;
        timestampDecoded = true;
        break;
      }
      if (ConvertTimestamp(timestamps[0], &frame->kernelTimestampNs) &&
          frame->kernelTimestampNs > 0) {
        if (!timestampDecoded) {
          frame->timestampSource = RawEthernetTimestampSource::SOFTWARE;
        }
        timestampDecoded = true;
      }
      if (timestampDecoded) {
        break;
      }
    }
  }
#endif
 #ifdef SO_TIMESTAMPNS
  if (frame->kernelTimestampNs <= 0) {
    for (auto* controlMessage = CMSG_FIRSTHDR(&message);
         controlMessage != nullptr;
         controlMessage = CMSG_NXTHDR(&message, controlMessage)) {
      if (controlMessage->cmsg_level != SOL_SOCKET ||
          controlMessage->cmsg_type != SO_TIMESTAMPNS ||
          controlMessage->cmsg_len < CMSG_LEN(sizeof(timespec))) {
        continue;
      }
      timespec timestamp{};
      std::memcpy(&timestamp, CMSG_DATA(controlMessage), sizeof(timestamp));
      if (ConvertTimestamp(timestamp, &frame->kernelTimestampNs)) {
        frame->timestampSource = RawEthernetTimestampSource::SOFTWARE;
        break;
      }
    }
  }
 #endif
  return status;
}

grpc::Status RawEthernetSocket::Send(
    std::span<const std::uint8_t> ethernetFrame) {
  if (fd_ < 0) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850二层网卡尚未打开");
  }
  if (ethernetFrame.size() < ETH_HLEN) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850二层发送报文必须包含完整Ethernet头");
  }
  sockaddr_ll address{};
  address.sll_family = AF_PACKET;
  address.sll_ifindex = ifIndex_;
  address.sll_halen = ETH_ALEN;
  std::copy_n(ethernetFrame.data(), ETH_ALEN, address.sll_addr);
  const auto sent = sendto(fd_, ethernetFrame.data(), ethernetFrame.size(),
                           MSG_DONTWAIT,
                           reinterpret_cast<const sockaddr*>(&address),
                           sizeof(address));
  if (sent < 0) {
    return SystemError("IEC61850发送二层报文");
  }
  if (static_cast<std::size_t>(sent) != ethernetFrame.size()) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "IEC61850二层报文未完整发送");
  }
  return grpc::Status::OK;
}

std::string RawEthernetSocket::interfaceName() const { return interfaceName_; }

std::array<std::uint8_t, 6> RawEthernetSocket::localMac() const noexcept {
  return localMac_;
}

std::optional<std::array<std::uint8_t, 6>> ParseRawMac(
    std::string_view text) {
  std::array<std::uint8_t, 6> result{};
  std::size_t output = 0;
  std::uint8_t highNibble = 0;
  bool haveHighNibble = false;
  bool lastWasSeparator = false;
  char separator = '\0';
  for (const auto character : text) {
    if (character == ':' || character == '-') {
      if (haveHighNibble || output == 0 || lastWasSeparator ||
          (separator != '\0' && separator != character)) {
        return std::nullopt;
      }
      separator = character;
      lastWasSeparator = true;
      continue;
    }
    std::uint8_t nibble = 0;
    if (character >= '0' && character <= '9') {
      nibble = static_cast<std::uint8_t>(character - '0');
    } else if (character >= 'a' && character <= 'f') {
      nibble = static_cast<std::uint8_t>(character - 'a' + 10);
    } else if (character >= 'A' && character <= 'F') {
      nibble = static_cast<std::uint8_t>(character - 'A' + 10);
    } else {
      return std::nullopt;
    }
    if (!haveHighNibble) {
      highNibble = static_cast<std::uint8_t>(nibble << 4);
      haveHighNibble = true;
    } else {
      if (output >= result.size()) {
        return std::nullopt;
      }
      result[output++] = static_cast<std::uint8_t>(highNibble | nibble);
      haveHighNibble = false;
    }
    lastWasSeparator = false;
  }
  return !haveHighNibble && !lastWasSeparator && output == result.size()
             ? std::optional<std::array<std::uint8_t, 6>>(result)
             : std::nullopt;
}

}  // namespace IEC61850
