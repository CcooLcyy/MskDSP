#pragma once

#include <functional>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <grpcpp/support/status.h>

#include "IEC61850MmsService.h"
#include "IEC61850MmsPdu.h"

namespace IEC61850 {

// 运行在现有Confirmed-PDU/BER层上的MMS最小服务端模型。该类不拥有TCP
// 监听器；COTP/Session线程收到MMS报文后应把报文串行交给HandleConfirmed。
struct MmsServerModel {
  // 置零时由当前已安装的回调和服务端模型自动生成能力位；非零时作为
  // 显式能力上限，并始终与客户端Initiate提议求交集。
  MmsBitString serviceSupport;
  MmsBitString parameterSupport;
  std::vector<std::string> domains;
  std::vector<std::string> namedVariables;
  std::unordered_map<std::string, std::vector<MmsObjectName>> namedVariableLists;
  MmsFileDirectoryRequest fileDirectoryRequest;
  std::function<grpc::Status(const MmsReadRequest&, MmsReadResponse*)> read;
  std::function<grpc::Status(const MmsWriteRequest&, MmsWriteResponse*)> write;
  std::function<grpc::Status(const MmsFileDirectoryRequest*,
                             MmsFileDirectoryResponse*)>
      fileDirectory;
  std::function<grpc::Status(const MmsJournalReadRequest*,
                             MmsJournalReadResponse*)>
      journalRead;
};

class MmsServer {
public:
  explicit MmsServer(MmsServerModel model = {});

  MmsServer(const MmsServer&) = delete;
  MmsServer& operator=(const MmsServer&) = delete;

  // 处理一个完整Confirmed-RequestPDU并返回完整Confirmed-ResponsePDU。
  // 调用方负责在COTP/Session层保证同一会话的invokeID串行性。
  grpc::Status HandleConfirmed(std::span<const std::uint8_t> request,
                               std::vector<std::uint8_t>* response);

  // 处理已经由ACSE/Presentation交付的InitiateRequestPDU。TCP/COTP和
  // Session状态仍由调用方维护；本方法只负责能力协商和有界BER响应。
  grpc::Status HandleInitiate(std::span<const std::uint8_t> request,
                              std::vector<std::uint8_t>* response);

  void SetModel(MmsServerModel model);

private:
  grpc::Status HandleNameList(const MmsConfirmedPduView& pdu,
                              std::span<const std::uint8_t> request,
                              std::vector<std::uint8_t>* response);
  grpc::Status HandleRead(const MmsConfirmedPduView& pdu,
                          std::span<const std::uint8_t> request,
                          std::vector<std::uint8_t>* response);
  grpc::Status HandleWrite(const MmsConfirmedPduView& pdu,
                           std::span<const std::uint8_t> request,
                           std::vector<std::uint8_t>* response);
  grpc::Status HandleNamedVariableList(const MmsConfirmedPduView& pdu,
                                       std::span<const std::uint8_t> request,
                                       std::vector<std::uint8_t>* response);
  grpc::Status EncodeNameListResponse(
      std::uint32_t invokeId, const std::vector<std::string>& identifiers,
      bool moreFollows, std::vector<std::uint8_t>* response) const;
  grpc::Status EncodeNamedVariableListAttributesResponse(
      std::uint32_t invokeId, const std::vector<MmsObjectName>& variables,
      std::vector<std::uint8_t>* response) const;
  MmsBitString SupportedServicesLocked() const;

  mutable std::mutex mutex_;
  MmsServerModel model_;
};

}  // namespace IEC61850
