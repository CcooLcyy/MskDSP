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
  // REFUSE用于ACSE阶段失败：装置在AARQ被拒时回REFUSE而不是ABORT。
  REFUSE = 0x0c,
  ABORT = 0x19,
};

// 会话SPDU中用户数据的只读视图；底层内存由输入报文持有。
struct IsoSessionPduView {
  IsoSessionPduType type = IsoSessionPduType::ABORT;
  std::span<const std::uint8_t> userData;
};

// 会话层S-SEL说明：本模块按实测被装置接受的CONNECT发送33/34会话选择子；
// 装置的ACCEPT不带这两个参数，因此解码侧只校验会话要求内的版本号子项。

// IEC 61850不区分表示层P-SEL，缺省与实测被装置接受的CONNECT一致。
// 注意必须是4字节：下位机A/B抓包（ab2.pcap）中5字节P-SEL会被装置回Session ABORT。
inline constexpr std::array<std::uint8_t, 4> kDefaultPresentationSelector{
    0x00, 0x00, 0x00, 0x01};

// ACSE的表示层上下文标识；AARQ/AARE以PDV-list形式承载时必须带该编号。
inline constexpr std::uint32_t kAcsePresentationContextId = 1;

// MMS的表示层上下文标识；AARQ/AARE与P-DATA必须使用同一个值。
inline constexpr std::uint32_t kMmsPresentationContextId = 3;

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

// 表示层CP（Connect Presentation，BER标签0x31）的解析结果；底层内存由输入报文持有。
struct PresentationCpView {
  std::array<std::uint8_t, 5> callingPresentationSelector{};
  std::size_t callingPresentationSelectorSize = 0;
  std::array<std::uint8_t, 5> calledPresentationSelector{};
  // 装置的ACCEPT只回带[3]应答选择子（responding-presentation-selector），
  // 此时它记录在calledPresentationSelector，callingPresentationSelectorSize为0。
  std::size_t calledPresentationSelectorSize = 0;
  // CP内user-data承载的ACSE用户数据；不带CP的旧格式为空。
  std::span<const std::uint8_t> userData;
};

// 编码表示层CP；userData通常是ACSE AARQ/AARE报文。
grpc::Status EncodeMmsPresentationCp(
    std::span<const std::uint8_t> userData,
    std::span<const std::uint8_t> callingPresentationSelector,
    std::span<const std::uint8_t> calledPresentationSelector,
    std::span<std::uint8_t> output, std::size_t* outputSize);

// 解析表示层CP并取出其user-data；用于服务端和模拟器兼容带CP的客户端。
grpc::Status DecodeMmsPresentationCp(std::span<const std::uint8_t> input,
                                     PresentationCpView* cp);

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

// 解析AARQ，供协议模拟器和后续服务端测试使用。服务端与模拟器需同时兼容
// 带表示层CP和旧的不带CP两种客户端，因此两个解码入口都接受这两种封装。
grpc::Status DecodeMmsAarq(std::span<const std::uint8_t> input,
                           MmsAareView* result);

// 编码/解析ACSE AARQ/AARE本体（不含表示层CP）。
grpc::Status EncodeMmsAcsePdu(std::uint8_t applicationTag,
                              std::span<const std::uint32_t> applicationContextOid,
                              std::uint32_t result, bool includeResult,
                              std::span<const std::uint8_t> mmsPdu,
                              std::span<std::uint8_t> output,
                              std::size_t* outputSize);

grpc::Status DecodeMmsAcsePdu(std::span<const std::uint8_t> input,
                              std::uint8_t expectedTag, MmsAareView* result);

// 编码/解析已建立关联后的P-DATA-TF。输出会话数据可直接交给
// EncodeIsoSessionData；MMS PDU仍由调用方提供和拥有。
grpc::Status EncodeMmsPresentationData(std::span<const std::uint8_t> mmsPdu,
                                        std::span<std::uint8_t> output,
                                        std::size_t* outputSize);

grpc::Status DecodeMmsPresentationData(std::span<const std::uint8_t> input,
                                        std::span<const std::uint8_t>* mmsPdu);

}  // namespace IEC61850
