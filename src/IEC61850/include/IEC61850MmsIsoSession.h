#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <grpcpp/support/status.h>

namespace IEC61850 {

// ISO 8327会话层当前使用的SPDU类型。
enum class IsoSessionPduType : std::uint8_t {
  CONNECT = 0x0d,
  ACCEPT = 0x0e,
  DATA = 0x01,
  FINISH = 0x09,
  DISCONNECT = 0x0a,
  ABORT = 0x19,
};

// 会话SPDU中用户数据的只读视图；底层内存由输入报文持有。
struct IsoSessionPduView {
  IsoSessionPduType type = IsoSessionPduType::ABORT;
  std::span<const std::uint8_t> userData;
};

grpc::Status EncodeIsoSessionConnect(
    std::span<const std::uint8_t> presentationData,
    std::span<std::uint8_t> output, std::size_t* outputSize);

grpc::Status EncodeIsoSessionAccept(
    std::span<const std::uint8_t> presentationData,
    std::span<std::uint8_t> output, std::size_t* outputSize);

grpc::Status EncodeIsoSessionData(std::span<const std::uint8_t> presentationData,
                                  std::span<std::uint8_t> output,
                                  std::size_t* outputSize);

grpc::Status EncodeIsoSessionFinish(
    std::span<const std::uint8_t> presentationData,
    std::span<std::uint8_t> output, std::size_t* outputSize);

grpc::Status EncodeIsoSessionDisconnect(
    std::span<const std::uint8_t> presentationData,
    std::span<std::uint8_t> output, std::size_t* outputSize);

grpc::Status EncodeIsoSessionAbort(
    std::span<const std::uint8_t> presentationData,
    std::span<std::uint8_t> output, std::size_t* outputSize);

grpc::Status DecodeIsoSessionPdu(std::span<const std::uint8_t> input,
                                 IsoSessionPduView* pdu);

// MMS ACSE关联结果；0表示accepted。
struct MmsAareView {
  std::uint32_t result = 0;
  std::array<std::uint32_t, 16> applicationContextOid{};
  std::size_t applicationContextOidSize = 0;
  std::span<const std::uint8_t> mmsPdu;
};

// 在AARQ/AARE的user-information中使用的MMS抽象语法OID。
inline constexpr std::array<std::uint32_t, 5> kMmsAbstractSyntaxOid{
    1, 0, 9506, 2, 1};

inline constexpr std::array<std::uint32_t, 5> kMmsApplicationContextOid{
    1, 0, 9506, 2, 3};

// 编码一个IEC 61850常用的AARQ；mmsPdu通常是MMS InitiateRequest。
grpc::Status EncodeMmsAarq(
    std::span<const std::uint32_t> applicationContextOid,
    std::span<const std::uint8_t> mmsPdu, std::span<std::uint8_t> output,
    std::size_t* outputSize);

// 编码AARE；result=0表示接受，mmsPdu通常是MMS InitiateResponse。
grpc::Status EncodeMmsAare(
    std::span<const std::uint32_t> applicationContextOid,
    std::uint32_t result, std::span<const std::uint8_t> mmsPdu,
    std::span<std::uint8_t> output, std::size_t* outputSize);

// 解析AARE并校验其application-context-name和user-information结构。
grpc::Status DecodeMmsAare(std::span<const std::uint8_t> input,
                           MmsAareView* result);

// 解析AARQ，供协议模拟器和后续服务端测试使用。
grpc::Status DecodeMmsAarq(std::span<const std::uint8_t> input,
                           MmsAareView* result);

// 编码/解析已建立关联后的P-DATA-TF。输出会话数据可直接交给
// EncodeIsoSessionData；MMS PDU仍由调用方提供和拥有。
grpc::Status EncodeMmsPresentationData(std::span<const std::uint8_t> mmsPdu,
                                        std::span<std::uint8_t> output,
                                        std::size_t* outputSize);

grpc::Status DecodeMmsPresentationData(std::span<const std::uint8_t> input,
                                        std::span<const std::uint8_t>* mmsPdu);

}  // namespace IEC61850
