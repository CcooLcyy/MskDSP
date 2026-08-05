#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <grpcpp/support/status.h>

namespace IEC61850 {

// MMS GetNameList使用的基本对象类编号。
enum class MmsObjectClass : std::uint16_t {
  NAMED_VARIABLE = 0,
  NAMED_VARIABLE_LIST = 1,
  NAMED_TYPE = 2,
  DOMAIN = 8,
};

enum class MmsObjectScopeType : std::uint8_t {
  VMD_SPECIFIC = 0,
  DOMAIN_SPECIFIC = 1,
  AA_SPECIFIC = 2,
};

struct MmsObjectScope {
  MmsObjectScopeType type = MmsObjectScopeType::VMD_SPECIFIC;
  std::string domain;
};

struct MmsGetNameListRequest {
  MmsObjectClass objectClass = MmsObjectClass::DOMAIN;
  MmsObjectScope scope;
  std::optional<std::string> continueAfter;
};

struct MmsGetNameListResponse {
  std::vector<std::string> identifiers;
  // MMS省略该字段时使用协议定义的默认值true。
  bool moreFollows = true;
};

// MMS Identify响应中的远端厂商、型号和版本信息。
struct MmsIdentifyResponse {
  std::string vendorName;
  std::string modelName;
  std::string revision;
};

// MMS FileDirectory/FileOpen共用的文件属性。时间保留服务端
// GeneralizedTime原始文本，避免在协议层引入本地时区或历法转换误差。
struct MmsFileAttributes {
  bool sizePresent = false;
  std::uint64_t size = 0;
  bool lastModifiedPresent = false;
  std::string lastModified;
};

struct MmsFileDirectoryEntry {
  std::string fileName;
  MmsFileAttributes attributes;
  // 兼容早期调用方的扁平字段，编解码时与attributes保持同步。
  std::uint64_t fileSize = 0;
  bool modifiedTimePresent = false;
  std::string modifiedTime;
};

struct MmsFileDirectoryRequest {
  // 空字符串表示查询服务端默认文件空间。
  std::string fileSpecification;
  std::optional<std::string> continueAfter;
};

struct MmsFileDirectoryResponse {
  std::vector<MmsFileDirectoryEntry> entries;
  // FileDirectory省略该字段时使用协议定义的默认值false。
  bool moreFollows = false;
};

struct MmsFileOpenRequest {
  std::string fileName;
  std::uint32_t initialPosition = 0;
};

struct MmsFileReadRequest {
  std::int32_t frsmId = 0;
};

struct MmsFileCloseRequest {
  std::int32_t frsmId = 0;
};

struct MmsFileOpenResponse {
  std::int32_t frsmId = 0;
  MmsFileAttributes attributes;
  // 兼容扁平字段。
  std::uint64_t fileSize = 0;
  bool modifiedTimePresent = false;
  std::string modifiedTime;
};

struct MmsFileReadResponse {
  std::vector<std::uint8_t> data;
  std::vector<std::uint8_t> fileData;
  // FileRead省略moreFollows时，协议默认值为true。
  bool moreFollows = true;
};

enum class MmsObjectNameType : std::uint8_t {
  VMD_SPECIFIC = 0,
  DOMAIN_SPECIFIC = 1,
  AA_SPECIFIC = 2,
};

struct MmsObjectName {
  MmsObjectNameType type = MmsObjectNameType::VMD_SPECIFIC;
  std::string domain;
  std::string identifier;

  bool operator==(const MmsObjectName&) const = default;
};

// MMS Journal中的结构化历史事件/告警/SOE条目。entryId用于分页续读，
// occurrenceTimeMs统一保存为Unix毫秒；变量值保留完整MMS Data选择。
enum class MmsJournalEntryKind : std::uint8_t {
  UNKNOWN = 0,
  EVENT = 1,
  ALARM = 2,
  SOE = 3,
  ANNOTATION = 4,
};

struct MmsJournalVariable {
  std::string tag;
  std::vector<std::uint8_t> encodedData;
  std::uint32_t reasonCode = 0;
};

struct MmsJournalEntry {
  std::vector<std::uint8_t> entryId;
  std::vector<std::uint8_t> originatingAe;
  std::int64_t occurrenceTimeMs = 0;
  MmsJournalEntryKind kind = MmsJournalEntryKind::UNKNOWN;
  std::string eventCondition;
  std::int32_t currentState = 0;
  std::string annotation;
  std::vector<MmsJournalVariable> variables;
};

struct MmsJournalStatusRequest {
  MmsObjectName journal;
};

struct MmsJournalStatusResponse {
  std::uint32_t currentEntries = 0;
  bool mmsDeletable = false;
};

struct MmsInitializeJournalRequest {
  MmsObjectName journal;
  std::optional<std::int64_t> limitTimeMs;
  std::vector<std::uint8_t> limitEntryId;
};

struct MmsInitializeJournalResponse {
  std::uint32_t deletedEntries = 0;
};

struct MmsJournalReadRequest {
  MmsObjectName journal;
  std::optional<std::int64_t> startTimeMs;
  std::vector<std::uint8_t> startEntryId;
  std::optional<std::int64_t> endTimeMs;
  std::optional<std::uint32_t> numberOfEntries;
  std::vector<std::string> variableTags;
  std::optional<std::int64_t> startAfterTimeMs;
  std::vector<std::uint8_t> startAfterEntryId;
};

struct MmsJournalReadResponse {
  std::vector<MmsJournalEntry> entries;
  bool moreFollows = false;
};

grpc::Status EncodeMmsJournalStatusRequest(
    std::uint32_t invokeId, const MmsJournalStatusRequest& request,
    std::span<std::uint8_t> output, std::size_t* outputSize);
grpc::Status DecodeMmsJournalStatusRequest(
    std::span<const std::uint8_t> input, std::uint32_t* invokeId,
    MmsJournalStatusRequest* request);
grpc::Status EncodeMmsJournalStatusResponse(
    std::uint32_t invokeId, const MmsJournalStatusResponse& response,
    std::span<std::uint8_t> output, std::size_t* outputSize);
grpc::Status DecodeMmsJournalStatusResponse(
    std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId,
    MmsJournalStatusResponse* response);
grpc::Status EncodeMmsInitializeJournalRequest(
    std::uint32_t invokeId, const MmsInitializeJournalRequest& request,
    std::span<std::uint8_t> output, std::size_t* outputSize);
grpc::Status DecodeMmsInitializeJournalRequest(
    std::span<const std::uint8_t> input, std::uint32_t* invokeId,
    MmsInitializeJournalRequest* request);
grpc::Status EncodeMmsInitializeJournalResponse(
    std::uint32_t invokeId, const MmsInitializeJournalResponse& response,
    std::span<std::uint8_t> output, std::size_t* outputSize);
grpc::Status DecodeMmsInitializeJournalResponse(
    std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId,
    MmsInitializeJournalResponse* response);
grpc::Status EncodeMmsJournalReadRequest(
    std::uint32_t invokeId, const MmsJournalReadRequest& request,
    std::span<std::uint8_t> output, std::size_t* outputSize);
grpc::Status DecodeMmsJournalReadRequest(
    std::span<const std::uint8_t> input, std::uint32_t* invokeId,
    MmsJournalReadRequest* request);
grpc::Status EncodeMmsJournalReadResponse(
    std::uint32_t invokeId, const MmsJournalReadResponse& response,
    std::span<std::uint8_t> output, std::size_t* outputSize);
grpc::Status DecodeMmsJournalReadResponse(
    std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId,
    MmsJournalReadResponse* response);

// 运行时Named Variable List（动态DataSet）管理。
struct MmsNamedVariableListDefinition {
  MmsObjectName listName;
  std::vector<MmsObjectName> variables;
};
grpc::Status EncodeMmsDefineNamedVariableListRequest(
    std::uint32_t invokeId, const MmsNamedVariableListDefinition& request,
    std::span<std::uint8_t> output, std::size_t* outputSize);
grpc::Status DecodeMmsDefineNamedVariableListRequest(
    std::span<const std::uint8_t> input, std::uint32_t* invokeId,
    MmsNamedVariableListDefinition* request);
grpc::Status EncodeMmsDeleteNamedVariableListRequest(
    std::uint32_t invokeId, const MmsObjectName& listName,
    std::span<std::uint8_t> output, std::size_t* outputSize);
grpc::Status DecodeMmsDeleteNamedVariableListRequest(
    std::span<const std::uint8_t> input, std::uint32_t* invokeId,
    MmsObjectName* listName);

// 文件管理扩展。InitiateDownload/DownloadSegment/TerminateDownload用于
// 真正的下载序列（下位机上传文件），不复用ObtainFile读取流程。
struct MmsInitiateDownloadRequest {
  std::string domain;
  std::string fileName;
  std::uint64_t fileSize = 0;
};
struct MmsInitiateDownloadResponse {
  std::int32_t frsmId = 0;
};
struct MmsDownloadSegmentRequest {
  std::int32_t frsmId = 0;
  std::vector<std::uint8_t> data;
};
struct MmsTerminateDownloadRequest {
  std::int32_t frsmId = 0;
};
grpc::Status EncodeMmsInitiateDownloadRequest(
    std::uint32_t invokeId, const MmsInitiateDownloadRequest& request,
    std::span<std::uint8_t> output, std::size_t* outputSize);
grpc::Status DecodeMmsInitiateDownloadRequest(
    std::span<const std::uint8_t> input, std::uint32_t* invokeId,
    MmsInitiateDownloadRequest* request);
grpc::Status DecodeMmsInitiateDownloadResponse(
    std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId,
    MmsInitiateDownloadResponse* response);
grpc::Status EncodeMmsDownloadSegmentRequest(
    std::uint32_t invokeId, const MmsDownloadSegmentRequest& request,
    std::span<std::uint8_t> output, std::size_t* outputSize);
grpc::Status DecodeMmsDownloadSegmentRequest(
    std::span<const std::uint8_t> input, std::uint32_t* invokeId,
    MmsDownloadSegmentRequest* request);
grpc::Status EncodeMmsTerminateDownloadRequest(
    std::uint32_t invokeId, const MmsTerminateDownloadRequest& request,
    std::span<std::uint8_t> output, std::size_t* outputSize);
grpc::Status DecodeMmsTerminateDownloadRequest(
    std::span<const std::uint8_t> input, std::uint32_t* invokeId,
    MmsTerminateDownloadRequest* request);
grpc::Status EncodeMmsFileDeleteRequest(
    std::uint32_t invokeId, std::string_view fileName,
    std::span<std::uint8_t> output, std::size_t* outputSize);
grpc::Status EncodeMmsFileRenameRequest(
    std::uint32_t invokeId, std::string_view currentName,
    std::string_view newName, std::span<std::uint8_t> output,
    std::size_t* outputSize);

// 解码一个完整MMS ObjectName选择；输出只在整份选择校验成功后提交。
grpc::Status EncodeMmsObjectName(const MmsObjectName& name,
                                 std::vector<std::uint8_t>* encoded);
grpc::Status DecodeMmsObjectName(std::span<const std::uint8_t> input,
                                 MmsObjectName* name);

// MMS TypeSpecification的有限树表示；只保留在线目录和数据解码所需字段。
enum class MmsTypeSpecificationKind : std::uint8_t {
  NAMED_TYPE,
  ARRAY,
  STRUCTURE,
  BOOLEAN,
  BIT_STRING,
  INTEGER,
  UNSIGNED,
  FLOATING_POINT,
  OCTET_STRING,
  VISIBLE_STRING,
  UTF8_STRING,
  GENERAL_TIME,
  BINARY_TIME,
  BCD,
  OBJECT_IDENTIFIER,
  MMS_STRING,
  UTC_TIME,
};

struct MmsTypeSpecification {
  struct Component {
    std::string name;
    std::shared_ptr<MmsTypeSpecification> type;
  };

  MmsTypeSpecificationKind kind = MmsTypeSpecificationKind::BOOLEAN;
  // NAMED_TYPE使用typeName；ARRAY使用elementCount；字符串和数值类型使用width。
  std::string typeName;
  std::uint32_t elementCount = 0;
  std::shared_ptr<MmsTypeSpecification> elementType;
  std::uint32_t width = 0;
  std::uint32_t exponentWidth = 0;
  std::uint8_t binaryTimeWidth = 0;
  std::vector<Component> components;
};

struct MmsGetVariableAccessAttributesResponse {
  bool mmsDeletable = false;
  bool addressPresent = false;
  MmsTypeSpecification typeSpecification;
};

struct MmsGetNamedVariableListAttributesResponse {
  bool mmsDeletable = false;
  // 按服务端返回顺序保存DataSet成员对象名。
  std::vector<MmsObjectName> variables;
};

// MMS Read请求中的变量访问项。当前协议层使用listOfVariable形式，
// 每个对象名作为一个独立的VariableSpecification发送，便于后续按点读取。
struct MmsReadRequest {
  std::vector<MmsObjectName> variables;
  // true表示要求服务端在响应中返回SpecificationWithResult结果；
  // MMS默认值为false。
  bool specificationWithResult = false;
};

// MMS Read响应项。encodedData保留完整Data选择TLV，供上层按在线类型解码；
// failure保留完整DataAccessError选择TLV，失败项不会伪造成无效数据。
struct MmsReadResponseItem {
  bool success = false;
  std::vector<std::uint8_t> encodedData;
  std::vector<std::uint8_t> failure;
};

struct MmsReadResponse {
  std::vector<MmsReadResponseItem> items;
};

// MMS Write请求项；encodedData必须是完整的MMS Data选择TLV。
struct MmsWriteRequestItem {
  MmsObjectName variable;
  std::vector<std::uint8_t> encodedData;
};

struct MmsWriteRequest {
  std::vector<MmsWriteRequestItem> items;
};

struct MmsWriteResponseItem {
  bool success = false;
  // 仅失败项使用，取值范围为MMS DataAccessError 0..11。
  std::int64_t failureCode = 0;
};

struct MmsWriteResponse {
  std::vector<MmsWriteResponseItem> items;
};

// Confirmed PDU的服务视图；serviceValue只引用输入报文，不拥有内存。
struct MmsConfirmedPduView {
  std::uint32_t invokeId = 0;
  std::uint32_t serviceTag = 0;
  std::span<const std::uint8_t> serviceValue;
};

// MMS Confirmed-ErrorPDU的有界服务错误视图；附加描述由视图拥有，服务专用信息引用输入报文。
struct MmsConfirmedErrorPduView {
  std::uint32_t invokeId = 0;
  // ErrorClass的选择编号，取值范围为0..11。
  std::uint8_t errorClass = 0;
  std::int64_t errorCode = 0;
  std::optional<std::int64_t> additionalCode;
  std::string additionalDescription;
  std::span<const std::uint8_t> serviceSpecificInformation;
  std::optional<std::int64_t> modifierPosition;
};

grpc::Status EncodeMmsConfirmedRequest(
    std::uint32_t invokeId, std::uint32_t serviceTag,
    std::span<const std::uint8_t> serviceValue,
    std::span<std::uint8_t> output, std::size_t* outputSize);

// 编码原始上下文标签的Confirmed-Request。FileRead/FileClose等MMS服务请求
// 的服务选择本身不是构造类型，不能复用默认的构造标签编码。
grpc::Status EncodeMmsConfirmedPrimitiveRequest(
    std::uint32_t invokeId, std::uint32_t serviceTag,
    std::span<const std::uint8_t> serviceValue,
    std::span<std::uint8_t> output, std::size_t* outputSize);

grpc::Status DecodeMmsConfirmedRequest(std::span<const std::uint8_t> input,
                                        MmsConfirmedPduView* pdu);

grpc::Status EncodeMmsConfirmedResponse(
    std::uint32_t invokeId, std::uint32_t serviceTag,
    std::span<const std::uint8_t> serviceValue,
    std::span<std::uint8_t> output, std::size_t* outputSize);

grpc::Status DecodeMmsConfirmedResponse(std::span<const std::uint8_t> input,
                                         MmsConfirmedPduView* pdu);

// 解码MMS Confirmed-ErrorPDU；只解析服务错误字段，不把远端错误伪装成确认响应。
grpc::Status DecodeMmsConfirmedError(
    std::span<const std::uint8_t> input, MmsConfirmedErrorPduView* pdu);

grpc::Status EncodeMmsIdentifyRequest(std::uint32_t invokeId,
                                      std::span<std::uint8_t> output,
                                      std::size_t* outputSize);

grpc::Status DecodeMmsIdentifyResponse(
    std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId,
    MmsIdentifyResponse* response);

// MMS文件目录、打开、分段读取和关闭服务（FileDirectory=77、
// FileOpen=72、FileRead=73、FileClose=74）。
grpc::Status EncodeMmsFileDirectoryRequest(
    std::uint32_t invokeId, const MmsFileDirectoryRequest& request,
    std::span<std::uint8_t> output, std::size_t* outputSize);
grpc::Status DecodeMmsFileDirectoryResponse(
    std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId,
    MmsFileDirectoryResponse* response);
grpc::Status EncodeMmsFileDirectoryResponse(
    std::uint32_t invokeId, const MmsFileDirectoryResponse& response,
    std::span<std::uint8_t> output, std::size_t* outputSize);
grpc::Status DecodeMmsFileDirectoryRequest(
    std::span<const std::uint8_t> input, std::uint32_t* invokeId,
    MmsFileDirectoryRequest* request);
grpc::Status EncodeMmsFileOpenRequest(
    std::uint32_t invokeId, std::string_view fileName,
    std::uint32_t initialPosition, std::span<std::uint8_t> output,
    std::size_t* outputSize);
grpc::Status EncodeMmsFileOpenRequest(
    std::uint32_t invokeId, const MmsFileOpenRequest& request,
    std::span<std::uint8_t> output, std::size_t* outputSize);
grpc::Status DecodeMmsFileOpenRequest(
    std::span<const std::uint8_t> input, std::uint32_t* invokeId,
    MmsFileOpenRequest* request);
grpc::Status DecodeMmsFileOpenResponse(
    std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId,
    MmsFileOpenResponse* response);
grpc::Status EncodeMmsFileOpenResponse(
    std::uint32_t invokeId, const MmsFileOpenResponse& response,
    std::span<std::uint8_t> output, std::size_t* outputSize);
grpc::Status EncodeMmsFileReadRequest(
    std::uint32_t invokeId, std::int32_t frsmId,
    std::span<std::uint8_t> output, std::size_t* outputSize);
grpc::Status EncodeMmsFileReadRequest(
    std::uint32_t invokeId, const MmsFileReadRequest& request,
    std::span<std::uint8_t> output, std::size_t* outputSize);
grpc::Status DecodeMmsFileReadRequest(
    std::span<const std::uint8_t> input, std::uint32_t* invokeId,
    MmsFileReadRequest* request);
grpc::Status DecodeMmsFileReadResponse(
    std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId,
    MmsFileReadResponse* response);
grpc::Status EncodeMmsFileReadResponse(
    std::uint32_t invokeId, const MmsFileReadResponse& response,
    std::span<std::uint8_t> output, std::size_t* outputSize);
grpc::Status EncodeMmsFileCloseRequest(
    std::uint32_t invokeId, std::int32_t frsmId,
    std::span<std::uint8_t> output, std::size_t* outputSize);
grpc::Status EncodeMmsFileCloseRequest(
    std::uint32_t invokeId, const MmsFileCloseRequest& request,
    std::span<std::uint8_t> output, std::size_t* outputSize);
grpc::Status DecodeMmsFileCloseRequest(
    std::span<const std::uint8_t> input, std::uint32_t* invokeId,
    MmsFileCloseRequest* request);
grpc::Status EncodeMmsFileCloseResponse(
    std::uint32_t invokeId, std::span<std::uint8_t> output,
    std::size_t* outputSize);
grpc::Status DecodeMmsFileCloseResponse(
    std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId);

grpc::Status EncodeMmsGetNameListRequest(
    std::uint32_t invokeId, const MmsGetNameListRequest& request,
    std::span<std::uint8_t> output, std::size_t* outputSize);

grpc::Status DecodeMmsGetNameListRequest(
    std::span<const std::uint8_t> input, std::uint32_t* invokeId,
    MmsGetNameListRequest* request);

grpc::Status DecodeMmsGetNameListResponse(
    std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId,
    MmsGetNameListResponse* response);

// 编码GetVariableAccessAttributes请求。
grpc::Status EncodeMmsGetVariableAccessAttributesRequest(
    std::uint32_t invokeId, const MmsObjectName& objectName,
    std::span<std::uint8_t> output, std::size_t* outputSize);

// 解码GetVariableAccessAttributes响应及其有界TypeSpecification树。
grpc::Status DecodeMmsGetVariableAccessAttributesResponse(
    std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId,
    MmsGetVariableAccessAttributesResponse* response);

// 编码GetNamedVariableListAttributes请求。
grpc::Status EncodeMmsGetNamedVariableListAttributesRequest(
    std::uint32_t invokeId, const MmsObjectName& objectName,
    std::span<std::uint8_t> output, std::size_t* outputSize);

// 解码GetNamedVariableListAttributes响应及有序成员对象名。
grpc::Status DecodeMmsGetNamedVariableListAttributesResponse(
    std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId,
    MmsGetNamedVariableListAttributesResponse* response);

// 编码/解码MMS Read请求。
grpc::Status EncodeMmsReadRequest(
    std::uint32_t invokeId, const MmsReadRequest& request,
    std::span<std::uint8_t> output, std::size_t* outputSize);

grpc::Status DecodeMmsReadRequest(std::span<const std::uint8_t> input,
                                  std::uint32_t* invokeId,
                                  MmsReadRequest* request);

// 编码/解码MMS Read响应。
grpc::Status EncodeMmsReadResponse(
    std::uint32_t invokeId, const MmsReadResponse& response,
    std::span<std::uint8_t> output, std::size_t* outputSize);

grpc::Status DecodeMmsReadResponse(std::span<const std::uint8_t> input,
                                   std::uint32_t expectedInvokeId,
                                   MmsReadResponse* response);

// 编码/解码MMS Write请求和响应；当前只使用listOfVariable访问方式。
grpc::Status EncodeMmsWriteRequest(
    std::uint32_t invokeId, const MmsWriteRequest& request,
    std::span<std::uint8_t> output, std::size_t* outputSize);

// 解码失败时保持invokeId为0且request为空，避免交付部分解析结果。
grpc::Status DecodeMmsWriteRequest(std::span<const std::uint8_t> input,
                                   std::uint32_t* invokeId,
                                   MmsWriteRequest* request);

grpc::Status EncodeMmsWriteResponse(
    std::uint32_t invokeId, const MmsWriteResponse& response,
    std::span<std::uint8_t> output, std::size_t* outputSize);

grpc::Status DecodeMmsWriteResponse(std::span<const std::uint8_t> input,
                                    std::uint32_t expectedInvokeId,
                                    MmsWriteResponse* response);

// 常用MMS Data选择的有界编码辅助函数，供RCB配置写入使用。
grpc::Status EncodeMmsDataBoolean(bool value,
                                  std::vector<std::uint8_t>* encodedData);
grpc::Status EncodeMmsDataSigned(
    std::int64_t value, std::vector<std::uint8_t>* encodedData);
grpc::Status EncodeMmsDataUnsigned(
    std::uint64_t value, std::vector<std::uint8_t>* encodedData);
grpc::Status EncodeMmsDataFloatingPoint(
    double value, std::uint8_t formatWidth,
    std::vector<std::uint8_t>* encodedData);
grpc::Status EncodeMmsDataBinaryTime(
    std::int64_t timestampMs, std::vector<std::uint8_t>* encodedData);
grpc::Status EncodeMmsDataUtcTime(
    std::int64_t timestampMs, bool timeQualityValid,
    std::vector<std::uint8_t>* encodedData);
grpc::Status EncodeMmsDataVisibleString(
    std::string_view value, std::vector<std::uint8_t>* encodedData);
grpc::Status EncodeMmsDataBitString(
    std::uint8_t unusedBits, std::span<const std::uint8_t> payload,
    std::vector<std::uint8_t>* encodedData);

}  // namespace IEC61850
