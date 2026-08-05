#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <grpcpp/support/status.h>

namespace IEC61850 {

// MMS使用的单个ISO-on-TCP通道参数。
struct MmsTransportEndpoint {
  std::string interfaceName;
  std::string localIp;
  std::string remoteIp;
  std::uint16_t remotePort = 102;
  std::uint32_t connectTimeoutMs = 3000;
  std::uint32_t ioTimeoutMs = 1000;
};

// 编码COTP连接请求；输出包含完整TPKT头。
grpc::Status EncodeCotpConnectionRequest(
    std::span<std::uint8_t> output, std::size_t* outputSize,
    std::uint16_t sourceReference = 1);

// 校验服务端COTP连接确认；输入必须是完整TPKT帧。
grpc::Status ValidateCotpConnectionConfirm(
    std::span<const std::uint8_t> frame);

// 编码COTP数据TPDU；输出包含完整TPKT头。
grpc::Status EncodeCotpData(std::span<const std::uint8_t> payload,
                            std::span<std::uint8_t> output,
                            std::size_t* outputSize);

// 将一个MMS载荷按当前下位机固定TPDU上限编码为多个完整TPKT/COTP数据帧。
grpc::Status EncodeCotpDataSegments(
    std::span<const std::uint8_t> payload,
    std::vector<std::vector<std::uint8_t>>* frames);

// 解码单个COTP数据段，并返回EOT标志；EOT=false表示后续仍有数据段。
grpc::Status DecodeCotpDataFrame(std::span<const std::uint8_t> frame,
                                 std::span<std::uint8_t> payload,
                                 std::size_t* payloadSize, bool* endOfTransport);

// 校验一份完整TPKT/COTP数据帧并复制MMS载荷。
grpc::Status DecodeCotpData(std::span<const std::uint8_t> frame,
                            std::span<std::uint8_t> payload,
                            std::size_t* payloadSize);

// MMS会话依赖的最小传输契约；测试可注入有界脚本化报文，生产环境使用TCP实现。
class MmsTransport {
public:
  virtual ~MmsTransport() = default;

  // timeoutMs为0时使用端点默认的TCP/COTP建链预算；非0时是本次TCP/COTP
  // 建链剩余预算。关联工作器可以把Session总截止时间的剩余值传入，
  // 传输实现不得因进入COTP下一阶段而重新计时。
  virtual grpc::Status Connect(const MmsTransportEndpoint& endpoint,
                               std::uint32_t timeoutMs = 0) = 0;
  virtual grpc::Status Send(std::span<const std::uint8_t> payload) = 0;
  // timeoutMs为0时使用传输层默认I/O超时；实现必须将其作为本次完整发送的预算。
  virtual grpc::Status Send(std::span<const std::uint8_t> payload,
                             std::uint32_t timeoutMs) = 0;
  virtual grpc::Status Receive(std::vector<std::uint8_t>* payload,
                               std::uint32_t timeoutMs = 0) = 0;
  virtual void Close() noexcept = 0;
  virtual bool IsConnected() const noexcept = 0;
};

// 只负责TCP、TPKT和COTP，不解析ACSE或MMS ASN.1。
class MmsTcpTransport final : public MmsTransport {
public:
  MmsTcpTransport() = default;
  ~MmsTcpTransport() override;

  MmsTcpTransport(const MmsTcpTransport&) = delete;
  MmsTcpTransport& operator=(const MmsTcpTransport&) = delete;

  grpc::Status Connect(const MmsTransportEndpoint& endpoint,
                       std::uint32_t timeoutMs = 0) override;
  grpc::Status Send(std::span<const std::uint8_t> payload) override;
  grpc::Status Send(std::span<const std::uint8_t> payload,
                    std::uint32_t timeoutMs) override;
  grpc::Status Receive(std::vector<std::uint8_t>* payload,
                       std::uint32_t timeoutMs = 0) override;
  void Close() noexcept override;
  bool IsConnected() const noexcept override;

private:
  int fd_ = -1;
  std::uint32_t ioTimeoutMs_ = 1000;
  std::string interfaceName_;
  std::vector<std::uint8_t> receivePayload_;
};

}  // namespace IEC61850
