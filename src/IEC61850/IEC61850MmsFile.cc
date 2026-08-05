#include "IEC61850MmsFile.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <string_view>

#include "IEC61850MmsExchange.h"
#include "IEC61850MmsIsoSession.h"
#include "IEC61850MmsService.h"
#include "Logger.h"

namespace IEC61850 {
namespace {

constexpr std::size_t kFilePduBufferSize = 64 * 1024;
constexpr std::size_t kFileDirectoryEntryLimit = 65536;
constexpr std::uint64_t kDefaultFileMaxBytes = 64ull * 1024ull * 1024ull;

grpc::Status Invalid(std::string_view reason) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                      std::format("IEC61850 MMS文件参数无效: {}", reason));
}

grpc::Status DataLoss(std::string_view reason) {
  return grpc::Status(grpc::StatusCode::DATA_LOSS,
                      std::format("IEC61850 MMS文件响应无效: {}", reason));
}

grpc::Status AdvanceInvokeId(std::uint32_t* invokeId) {
  if (invokeId == nullptr || *invokeId == 0) {
    return Invalid("invokeID未初始化");
  }
  *invokeId = *invokeId == std::numeric_limits<std::uint32_t>::max()
                  ? 1
                  : *invokeId + 1;
  return grpc::Status::OK;
}

grpc::Status CheckDeadline(std::chrono::steady_clock::time_point deadline,
                           const std::function<bool()>& isCancelled) {
  if (isCancelled && isCancelled()) {
    return grpc::Status(grpc::StatusCode::CANCELLED,
                        "IEC61850 MMS文件操作已取消");
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS文件操作超时");
  }
  return grpc::Status::OK;
}

std::chrono::milliseconds Remaining(
    std::chrono::steady_clock::time_point deadline) {
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  return std::max(std::chrono::milliseconds(1), remaining);
}

bool IsParentComponent(std::string_view component) {
  return component == "..";
}

}  // namespace

grpc::Status MmsFileClient::ValidateRemoteFileName(
    std::string_view remoteFile) {
  if (remoteFile.empty() || remoteFile.size() > 1024 ||
      remoteFile.find('\0') != std::string_view::npos ||
      (remoteFile.size() > 1 && remoteFile[1] == ':')) {
    return Invalid("远端文件名为空、过长或包含非法根名称");
  }
  const bool hasLeadingSeparator = remoteFile.front() == '/' ||
                                   remoteFile.front() == '\\';
  const bool hasTrailingSeparator = remoteFile.back() == '/' ||
                                    remoteFile.back() == '\\';
  const auto isSeparator = [](char value) {
    return value == '/' || value == '\\';
  };
  if ((hasLeadingSeparator && remoteFile.size() > 1 &&
       isSeparator(remoteFile[1])) ||
      (hasTrailingSeparator && remoteFile.size() > 1 &&
       isSeparator(remoteFile[remoteFile.size() - 2]))) {
    return Invalid("远端文件名包含空路径组件");
  }
  const auto contentBegin = hasLeadingSeparator ? 1U : 0U;
  const auto contentEnd = hasTrailingSeparator ? remoteFile.size() - 1
                                                : remoteFile.size();
  if (contentBegin >= contentEnd) {
    return Invalid("远端文件名不得为空");
  }
  std::size_t begin = contentBegin;
  while (begin < contentEnd) {
    const auto end = remoteFile.find_first_of("/\\", begin);
    const auto componentEnd = end == std::string_view::npos || end > contentEnd
                                  ? contentEnd
                                  : end;
    const auto component = remoteFile.substr(begin, componentEnd - begin);
    if (IsParentComponent(component)) {
      return Invalid("远端文件名包含父目录穿越");
    }
    if (component.empty()) {
      return Invalid("远端文件名包含空路径组件");
    }
    if (componentEnd == contentEnd) {
      break;
    }
    begin = componentEnd + 1;
    if (begin >= contentEnd) {
      break;
    }
  }
  return grpc::Status::OK;
}

grpc::Status MmsFileClient::ValidateLocalFileName(
    std::string_view localFile) {
  if (localFile.empty() || localFile.size() > 4096 ||
      localFile.find('\0') != std::string_view::npos) {
    return Invalid("本地文件名为空、过长或包含非法字符");
  }
  const std::filesystem::path path{std::string(localFile)};
  if (path.has_root_name() && path.root_name().string().size() > 1 &&
      path.root_name().string()[1] == ':') {
    return Invalid("本地文件名包含不支持的驱动器根名称");
  }
  if (localFile.back() == '/' || localFile.back() == '\\') {
    return Invalid("本地文件名必须指向普通文件");
  }
  for (const auto& component : path) {
    if (component == "..") {
      return Invalid("本地文件名不得包含父目录穿越");
    }
  }
  if (path.filename().empty() || path.filename() == "." ||
      path.filename() == "..") {
    return Invalid("本地文件名必须指向普通文件");
  }
  std::error_code error;
  if (std::filesystem::is_directory(path, error)) {
    return Invalid("本地文件名不得指向目录");
  }
  return grpc::Status::OK;
}

grpc::Status MmsFileClient::ReadDirectory(
    MmsTransport& transport, std::uint32_t* nextInvokeId,
    const MmsFileDirectoryRequest& request, MmsFileDirectoryResponse* response,
    std::chrono::milliseconds timeout,
    const MmsFileUnconfirmedHandler& onUnconfirmed,
    const std::function<bool()>& isCancelled,
    const MmsRequestSentHandler& requestSent) const {
  if (nextInvokeId == nullptr || response == nullptr) {
    return Invalid("文件目录输出参数为空");
  }
  if (!request.fileSpecification.empty()) {
    auto status = ValidateRemoteFileName(request.fileSpecification);
    if (!status.ok()) {
      return status;
    }
  }
  if (request.continueAfter.has_value()) {
    auto status = ValidateRemoteFileName(*request.continueAfter);
    if (!status.ok()) {
      return status;
    }
  }
  if (timeout <= std::chrono::milliseconds::zero()) {
    return Invalid("文件目录超时参数无效");
  }
  *response = {};
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  MmsFileDirectoryRequest page = request;
  for (;;) {
    auto status = CheckDeadline(deadline, isCancelled);
    if (!status.ok()) {
      return status;
    }
    if (response->entries.size() >= kFileDirectoryEntryLimit) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "MMS文件目录条目超过下位机上限");
    }
    std::array<std::uint8_t, kFilePduBufferSize> requestBuffer{};
    std::size_t requestSize = 0;
    status = EncodeMmsFileDirectoryRequest(*nextInvokeId, page, requestBuffer,
                                            &requestSize);
    if (!status.ok()) {
      return status;
    }
    std::vector<std::uint8_t> received;
    status = ExchangeMmsConfirmedRequest(
        transport, std::span<const std::uint8_t>(requestBuffer.data(),
                                                   requestSize),
        &received, onUnconfirmed, Remaining(deadline), isCancelled,
        requestSent);
    if (!status.ok()) {
      return status;
    }
    MmsFileDirectoryResponse decoded;
    status = DecodeMmsFileDirectoryResponse(received, *nextInvokeId, &decoded);
    if (!status.ok()) {
      return status;
    }
    if (decoded.entries.empty() && decoded.moreFollows) {
      return DataLoss("目录分页为空且moreFollows仍为true");
    }
    if (decoded.entries.size() >
        kFileDirectoryEntryLimit - response->entries.size()) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "MMS文件目录累计条目超过下位机上限");
    }
    response->entries.insert(response->entries.end(), decoded.entries.begin(),
                             decoded.entries.end());
    response->moreFollows = decoded.moreFollows;
    LOG_INFO("IEC61850 MMS读取文件目录: 页条目={}, 累计条目={}, 是否还有后续={}",
             decoded.entries.size(), response->entries.size(),
             decoded.moreFollows ? "是" : "否");
    status = AdvanceInvokeId(nextInvokeId);
    if (!status.ok() || !decoded.moreFollows) {
      return status;
    }
    const auto& last = decoded.entries.back().fileName;
    if (last.empty() || (page.continueAfter.has_value() &&
                         *page.continueAfter == last)) {
      return DataLoss("目录分页continueAfter未前进");
    }
    page.continueAfter = last;
  }
}

grpc::Status MmsFileClient::Download(
    MmsTransport& transport, std::uint32_t* nextInvokeId,
    const MmsFileDownloadRequest& request, std::chrono::milliseconds timeout,
    const MmsFileUnconfirmedHandler& onUnconfirmed,
    const std::function<bool()>& isCancelled,
    const MmsRequestSentHandler& requestSent) const {
  if (nextInvokeId == nullptr) {
    return Invalid("文件下载invokeID输出参数为空");
  }
  const auto remoteFile = request.remoteFile.empty() ? request.remoteFileName
                                                      : request.remoteFile;
  const auto localFile = request.localFile.empty() ? request.localPath
                                                    : request.localFile;
  auto status = ValidateRemoteFileName(remoteFile);
  if (!status.ok()) {
    return status;
  }
  status = ValidateLocalFileName(localFile);
  if (!status.ok()) {
    return status;
  }
  const auto maxBytes = request.maxBytes == 0 ? kDefaultFileMaxBytes
                                             : request.maxBytes;
  if (timeout <= std::chrono::milliseconds::zero()) {
    return Invalid("文件下载超时参数无效");
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::array<std::uint8_t, kFilePduBufferSize> requestBuffer{};
  std::size_t requestSize = 0;
  status = EncodeMmsFileOpenRequest(*nextInvokeId, remoteFile, 0,
                                    requestBuffer, &requestSize);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> received;
  status = ExchangeMmsConfirmedRequest(
      transport, std::span<const std::uint8_t>(requestBuffer.data(),
                                                 requestSize),
      &received, onUnconfirmed, Remaining(deadline), isCancelled, requestSent);
  if (!status.ok()) {
    return status;
  }
  MmsFileOpenResponse opened;
  status = DecodeMmsFileOpenResponse(received, *nextInvokeId, &opened);
  if (!status.ok()) {
    return status;
  }
  const auto closeRemote = [&]() {
    std::array<std::uint8_t, kFilePduBufferSize> closeBuffer{};
    std::size_t closeSize = 0;
    auto closeStatus = EncodeMmsFileCloseRequest(
        *nextInvokeId, opened.frsmId, closeBuffer, &closeSize);
    if (!closeStatus.ok()) {
      return closeStatus;
    }
    std::vector<std::uint8_t> closeResponse;
    closeStatus = ExchangeMmsConfirmedRequest(
        transport,
        std::span<const std::uint8_t>(closeBuffer.data(), closeSize),
        &closeResponse, onUnconfirmed, Remaining(deadline), {});
    if (closeStatus.ok()) {
      MmsConfirmedPduView closePdu;
      closeStatus = DecodeMmsConfirmedResponse(closeResponse, &closePdu);
      if (closeStatus.ok() &&
          (closePdu.invokeId != *nextInvokeId || closePdu.serviceTag != 74 ||
           !closePdu.serviceValue.empty())) {
        closeStatus = DataLoss("FileClose响应服务选择或内容错误");
      }
    }
    if (closeStatus.ok()) {
      closeStatus = AdvanceInvokeId(nextInvokeId);
    }
    return closeStatus;
  };
  if (opened.fileSize > maxBytes) {
    (void)closeRemote();
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "MMS远端文件超过本地下载上限");
  }
  status = AdvanceInvokeId(nextInvokeId);
  if (!status.ok()) {
    return status;
  }

  const std::filesystem::path target(localFile);
  const auto temp = target.string() + ".iec61850.part";
  std::ofstream file(temp, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    (void)closeRemote();
    return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                        "MMS本地临时文件无法打开");
  }
  std::uint64_t total = 0;
  bool completed = false;
  status = grpc::Status::OK;
  while (!completed) {
    status = CheckDeadline(deadline, isCancelled);
    if (!status.ok()) {
      break;
    }
    status = EncodeMmsFileReadRequest(*nextInvokeId, opened.frsmId,
                                      requestBuffer, &requestSize);
    if (!status.ok()) {
      break;
    }
    received.clear();
    status = ExchangeMmsConfirmedRequest(
        transport, std::span<const std::uint8_t>(requestBuffer.data(),
                                                   requestSize),
        &received, onUnconfirmed, Remaining(deadline), isCancelled,
        requestSent);
    if (!status.ok()) {
      break;
    }
    MmsFileReadResponse part;
    status = DecodeMmsFileReadResponse(received, *nextInvokeId, &part);
    if (!status.ok()) {
      break;
    }
    if (part.data.empty() && part.moreFollows) {
      status = DataLoss("FileRead返回空分片但仍声明存在后续内容");
      break;
    }
    if (part.data.size() > maxBytes - total) {
      status = grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                            "MMS文件累计大小超过本地下载上限");
      break;
    }
    file.write(reinterpret_cast<const char*>(part.data.data()),
               static_cast<std::streamsize>(part.data.size()));
    if (!file.good()) {
      status = grpc::Status(grpc::StatusCode::INTERNAL,
                            "MMS本地文件写入失败");
      break;
    }
    total += part.data.size();
    completed = !part.moreFollows;
    LOG_DEBUG("IEC61850 MMS读取文件分段: 文件={}, 分段字节={}, 累计字节={}, 是否还有后续={}",
              remoteFile, part.data.size(), total,
              part.moreFollows ? "是" : "否");
    status = AdvanceInvokeId(nextInvokeId);
    if (!status.ok()) {
      break;
    }
  }
  file.close();

  // 无论读取是否成功，都尽力释放服务端FRSM句柄。
  const auto closeStatus = closeRemote();
  if (!status.ok()) {
    std::error_code error;
    std::filesystem::remove(temp, error);
    return status;
  }
  if (!closeStatus.ok()) {
    std::error_code error;
    std::filesystem::remove(temp, error);
    return closeStatus;
  }
  if (total != opened.fileSize) {
    std::error_code error;
    std::filesystem::remove(temp, error);
    return DataLoss("FileRead累计长度与FileOpen文件属性不一致");
  }
  std::error_code error;
  std::filesystem::rename(temp, target, error);
  if (error) {
    std::filesystem::remove(temp, error);
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "MMS下载文件原子替换失败");
  }
  LOG_INFO("IEC61850 MMS文件下载完成: 远端文件={}, 本地文件={}, 字节数={}",
           remoteFile, localFile, total);
  return grpc::Status::OK;
}

grpc::Status MmsFileClient::Upload(
    const MmsFileUploadRequest& request, std::chrono::milliseconds timeout,
    const MmsCancellationPredicate& isCancelled) const {
  if (!exchange_ || nextInvokeId_ == nullptr) {
    return Invalid("文件上传事务参数未配置");
  }
  const auto remote = request.remoteFile;
  auto status = ValidateRemoteFileName(remote);
  if (!status.ok()) return status;
  status = ValidateLocalFileName(request.localFile);
  if (!status.ok()) return status;
  if (timeout <= std::chrono::milliseconds::zero() ||
      request.timeout <= std::chrono::milliseconds::zero() ||
      request.segmentBytes == 0 ||
      request.segmentBytes > 64 * 1024) {
    return Invalid("文件上传超时或分片大小参数无效");
  }
  timeout = std::min(timeout, request.timeout);
  const auto maxBytes = request.maxBytes == 0 ? kDefaultFileMaxBytes
                                             : request.maxBytes;
  std::error_code error;
  const auto fileSize = std::filesystem::file_size(request.localFile, error);
  if (error || !std::filesystem::is_regular_file(request.localFile, error)) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND,
                        "MMS本地上传文件不存在或不是普通文件");
  }
  if (fileSize > maxBytes) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "MMS本地上传文件超过大小上限");
  }
  const auto cancelled = [&]() {
    return (isCancelled && isCancelled()) ||
           (request.cancellation &&
            request.cancellation->load(std::memory_order_acquire));
  };
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  status = CheckDeadline(deadline, cancelled);
  if (!status.ok()) return status;
  std::array<std::uint8_t, kFilePduBufferSize> buffer{};
  std::size_t encodedSize = 0;
  MmsInitiateDownloadRequest initiate;
  initiate.domain = request.remoteDomain.empty() ? "" : request.remoteDomain;
  initiate.fileName = remote;
  initiate.fileSize = fileSize;
  if (initiate.domain.empty()) {
    initiate.domain = "VMD";
  }
  status = EncodeMmsInitiateDownloadRequest(*nextInvokeId_, initiate, buffer,
                                            &encodedSize);
  if (!status.ok()) return status;
  std::vector<std::uint8_t> received;
  status = exchange_(std::span<const std::uint8_t>(buffer.data(), encodedSize),
                     &received, Remaining(deadline), cancelled, [] { return true; });
  if (!status.ok()) return status;
  MmsInitiateDownloadResponse started;
  status = DecodeMmsInitiateDownloadResponse(received, *nextInvokeId_, &started);
  if (!status.ok()) return status;
  const auto terminate = [&]() {
    if (started.frsmId < 0) return grpc::Status::OK;
    std::size_t terminateSize = 0;
    auto terminateStatus = EncodeMmsTerminateDownloadRequest(
        *nextInvokeId_, {started.frsmId}, buffer, &terminateSize);
    if (!terminateStatus.ok()) return terminateStatus;
    std::vector<std::uint8_t> terminateResponse;
    terminateStatus = exchange_(
        std::span<const std::uint8_t>(buffer.data(), terminateSize),
        &terminateResponse, Remaining(deadline), {}, [] { return true; });
    if (!terminateStatus.ok()) return terminateStatus;
    MmsConfirmedPduView pdu;
    terminateStatus = DecodeMmsConfirmedResponse(terminateResponse, &pdu);
    if (terminateStatus.ok() &&
        (pdu.invokeId != *nextInvokeId_ || pdu.serviceTag != 28 ||
         !pdu.serviceValue.empty())) {
      terminateStatus = DataLoss("TerminateDownload响应服务选择或内容错误");
    }
    if (terminateStatus.ok()) terminateStatus = AdvanceInvokeId(nextInvokeId_);
    return terminateStatus;
  };
  status = CheckDeadline(deadline, cancelled);
  if (!status.ok()) {
    const auto terminateStatus = terminate();
    if (!terminateStatus.ok()) {
      return grpc::Status(grpc::StatusCode::ABORTED,
                          "MMS文件上传取消时TerminateDownload释放失败");
    }
    return status;
  }
  status = AdvanceInvokeId(nextInvokeId_);
  if (!status.ok()) return status;
  std::ifstream file(request.localFile, std::ios::binary);
  if (!file.is_open()) {
    (void)terminate();
    return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                        "MMS本地上传文件无法打开");
  }
  std::vector<std::uint8_t> chunk(request.segmentBytes);
  std::uint64_t sent = 0;
  while (sent < fileSize) {
    status = CheckDeadline(deadline, cancelled);
    if (!status.ok()) break;
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(request.segmentBytes, fileSize - sent));
    file.read(reinterpret_cast<char*>(chunk.data()),
              static_cast<std::streamsize>(count));
    if (file.gcount() != static_cast<std::streamsize>(count)) {
      status = grpc::Status(grpc::StatusCode::INTERNAL,
                            "MMS本地上传文件读取失败");
      break;
    }
    MmsDownloadSegmentRequest segment;
    segment.frsmId = started.frsmId;
    segment.data.assign(chunk.begin(), chunk.begin() + count);
    status = EncodeMmsDownloadSegmentRequest(*nextInvokeId_, segment, buffer,
                                              &encodedSize);
    if (!status.ok()) break;
    received.clear();
    status = exchange_(std::span<const std::uint8_t>(buffer.data(), encodedSize),
                       &received, Remaining(deadline), cancelled,
                       [] { return true; });
    if (!status.ok()) break;
    status = CheckDeadline(deadline, cancelled);
    if (!status.ok()) break;
    MmsConfirmedPduView pdu;
    status = DecodeMmsConfirmedResponse(received, &pdu);
    if (!status.ok() || pdu.invokeId != *nextInvokeId_ ||
        pdu.serviceTag != 27 || !pdu.serviceValue.empty()) {
      status = DataLoss("DownloadSegment响应服务选择或内容错误");
      break;
    }
    sent += count;
    status = AdvanceInvokeId(nextInvokeId_);
    if (!status.ok()) break;
  }
  file.close();
  const auto terminateStatus = terminate();
  if (!status.ok()) return status;
  if (!terminateStatus.ok()) return terminateStatus;
  LOG_INFO("IEC61850 MMS文件上传完成: 本地文件={}, 远端文件={}, 字节数={}",
           request.localFile, request.remoteFile, sent);
  return grpc::Status::OK;
}

grpc::Status MmsFileClient::DeleteFile(
    const MmsFileMutationRequest& request, std::chrono::milliseconds timeout,
    const MmsCancellationPredicate& isCancelled) const {
  if (!exchange_ || nextInvokeId_ == nullptr) return Invalid("文件删除事务参数未配置");
  auto status = ValidateRemoteFileName(request.fileName);
  if (!status.ok()) return status;
  if (timeout <= std::chrono::milliseconds::zero()) return Invalid("文件删除超时参数无效");
  const auto cancelled = [&]() {
    return (isCancelled && isCancelled()) ||
           (request.cancellation && request.cancellation->load());
  };
  std::array<std::uint8_t, kFilePduBufferSize> buffer{};
  std::size_t size = 0;
  status = EncodeMmsFileDeleteRequest(*nextInvokeId_, request.fileName, buffer,
                                      &size);
  if (!status.ok()) return status;
  std::vector<std::uint8_t> response;
  status = exchange_(std::span<const std::uint8_t>(buffer.data(), size),
                     &response, timeout, cancelled, [] { return true; });
  if (!status.ok()) return status;
  MmsConfirmedPduView pdu;
  status = DecodeMmsConfirmedResponse(response, &pdu);
  if (!status.ok() || pdu.invokeId != *nextInvokeId_ || pdu.serviceTag != 76 ||
      !pdu.serviceValue.empty()) return DataLoss("FileDelete响应无效");
  return AdvanceInvokeId(nextInvokeId_);
}

grpc::Status MmsFileClient::RenameFile(
    const MmsFileMutationRequest& request, std::chrono::milliseconds timeout,
    const MmsCancellationPredicate& isCancelled) const {
  if (!exchange_ || nextInvokeId_ == nullptr) return Invalid("文件改名事务参数未配置");
  auto status = ValidateRemoteFileName(request.fileName);
  if (!status.ok()) return status;
  status = ValidateRemoteFileName(request.newFileName);
  if (!status.ok()) return status;
  if (timeout <= std::chrono::milliseconds::zero()) return Invalid("文件改名超时参数无效");
  const auto cancelled = [&]() {
    return (isCancelled && isCancelled()) ||
           (request.cancellation && request.cancellation->load());
  };
  std::array<std::uint8_t, kFilePduBufferSize> buffer{};
  std::size_t size = 0;
  status = EncodeMmsFileRenameRequest(*nextInvokeId_, request.fileName,
                                      request.newFileName, buffer, &size);
  if (!status.ok()) return status;
  std::vector<std::uint8_t> response;
  status = exchange_(std::span<const std::uint8_t>(buffer.data(), size),
                     &response, timeout, cancelled, [] { return true; });
  if (!status.ok()) return status;
  MmsConfirmedPduView pdu;
  status = DecodeMmsConfirmedResponse(response, &pdu);
  if (!status.ok() || pdu.invokeId != *nextInvokeId_ || pdu.serviceTag != 75 ||
      !pdu.serviceValue.empty()) return DataLoss("FileRename响应无效");
  return AdvanceInvokeId(nextInvokeId_);
}

grpc::Status MmsFileClient::ListDirectory(
    const MmsFileDirectoryRequest& request,
    std::vector<MmsFileDirectoryEntry>* entries, std::size_t maxEntries,
    std::chrono::milliseconds timeout,
    const MmsCancellationPredicate& isCancelled) const {
  if (!exchange_ || nextInvokeId_ == nullptr || entries == nullptr) {
    return Invalid("文件目录事务参数未配置");
  }
  if (maxEntries == 0 || maxEntries > kFileDirectoryEntryLimit) {
    return Invalid("文件目录数量上限无效");
  }
  if (!request.fileSpecification.empty()) {
    auto status = ValidateRemoteFileName(request.fileSpecification);
    if (!status.ok()) {
      return status;
    }
  }
  *entries = {};
  MmsFileDirectoryRequest page = request;
  for (;;) {
    if (isCancelled && isCancelled()) {
      return grpc::Status(grpc::StatusCode::CANCELLED,
                          "IEC61850 MMS文件目录操作已取消");
    }
    if (entries->size() >= maxEntries) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "MMS文件目录条目超过调用方上限");
    }
    std::array<std::uint8_t, kFilePduBufferSize> buffer{};
    std::size_t encodedSize = 0;
    auto status = EncodeMmsFileDirectoryRequest(*nextInvokeId_, page, buffer,
                                                &encodedSize);
    if (!status.ok()) {
      return status;
    }
    std::vector<std::uint8_t> response;
    status = exchange_(
        std::span<const std::uint8_t>(buffer.data(), encodedSize), &response,
        timeout, isCancelled, [] { return true; });
    if (!status.ok()) {
      return status;
    }
    MmsFileDirectoryResponse decoded;
    status = DecodeMmsFileDirectoryResponse(response, *nextInvokeId_, &decoded);
    if (!status.ok()) {
      return status;
    }
    if (decoded.entries.empty() && decoded.moreFollows) {
      return DataLoss("目录分页为空且moreFollows仍为true");
    }
    if (decoded.entries.size() > maxEntries - entries->size()) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "MMS文件目录条目超过调用方上限");
    }
    entries->insert(entries->end(), decoded.entries.begin(),
                    decoded.entries.end());
    status = AdvanceInvokeId(nextInvokeId_);
    if (!status.ok() || !decoded.moreFollows) {
      return status;
    }
    const auto& last = decoded.entries.back().fileName;
    if (last.empty() || (page.continueAfter.has_value() &&
                         *page.continueAfter == last)) {
      return DataLoss("目录分页continueAfter未前进");
    }
    page.continueAfter = last;
  }
}

grpc::Status MmsFileClient::ObtainFile(
    const MmsFileDownloadRequest& request, MmsFileDownloadResult* result,
    const MmsCancellationPredicate& isCancelled) const {
  if (!exchange_ || nextInvokeId_ == nullptr || result == nullptr) {
    return Invalid("文件下载事务参数未配置");
  }
  *result = {};
  const auto remote = request.remoteFileName.empty() ? request.remoteFile
                                                     : request.remoteFileName;
  const auto local = request.localPath.empty() ? request.localFile
                                               : request.localPath;
  auto status = ValidateRemoteFileName(remote);
  if (!status.ok()) {
    return status;
  }
  status = ValidateLocalFileName(local);
  if (!status.ok()) {
    return status;
  }
  const auto maxBytes = request.maxBytes == 0 ? kDefaultFileMaxBytes
                                             : request.maxBytes;
  const auto cancelled = [&] {
    return (isCancelled && isCancelled()) ||
           (request.cancellation != nullptr &&
            request.cancellation->load(std::memory_order_acquire));
  };
  if (request.timeout <= std::chrono::milliseconds::zero()) {
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS文件下载超时参数无效");
  }
  const auto deadline = std::chrono::steady_clock::now() + request.timeout;
  status = CheckDeadline(deadline, cancelled);
  if (!status.ok()) return status;
  std::array<std::uint8_t, kFilePduBufferSize> buffer{};
  std::size_t encodedSize = 0;
  MmsFileOpenRequest openRequest{remote, 0};
  status = EncodeMmsFileOpenRequest(*nextInvokeId_, openRequest, buffer,
                                    &encodedSize);
  if (!status.ok()) return status;
  std::vector<std::uint8_t> response;
  status = exchange_(std::span<const std::uint8_t>(buffer.data(), encodedSize),
                     &response, Remaining(deadline), cancelled,
                     [] { return true; });
  if (!status.ok()) return status;
  status = CheckDeadline(deadline, cancelled);
  if (!status.ok()) return status;
  MmsFileOpenResponse opened;
  status = DecodeMmsFileOpenResponse(response, *nextInvokeId_, &opened);
  if (!status.ok()) return status;
  status = AdvanceInvokeId(nextInvokeId_);
  if (!status.ok()) return status;

  const auto closeRemote = [&]() {
    MmsFileCloseRequest closeRequest{opened.frsmId};
    auto closeStatus = EncodeMmsFileCloseRequest(*nextInvokeId_, closeRequest,
                                                 buffer, &encodedSize);
    if (!closeStatus.ok()) return closeStatus;
    std::vector<std::uint8_t> closeResponse;
    closeStatus = exchange_(
        std::span<const std::uint8_t>(buffer.data(), encodedSize),
        &closeResponse, Remaining(deadline), {}, [] { return true; });
    if (!closeStatus.ok()) return closeStatus;
    closeStatus = DecodeMmsFileCloseResponse(closeResponse, *nextInvokeId_);
    if (!closeStatus.ok()) return closeStatus;
    return AdvanceInvokeId(nextInvokeId_);
  };
  if (opened.fileSize > maxBytes) {
    const auto closeStatus = closeRemote();
    if (!closeStatus.ok()) {
      return grpc::Status(grpc::StatusCode::ABORTED,
                          "MMS远端文件超限且FileClose释放失败");
    }
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "MMS远端文件超过本地下载上限");
  }
  const auto temp = local + ".iec61850.part";
  std::ofstream file(temp, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    (void)closeRemote();
    return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                        "MMS本地临时文件无法打开");
  }
  std::uint64_t total = 0;
  bool finished = false;
  status = grpc::Status::OK;
  while (!finished) {
    status = CheckDeadline(deadline, cancelled);
    if (!status.ok()) break;
    MmsFileReadRequest readRequest{opened.frsmId};
    status = EncodeMmsFileReadRequest(*nextInvokeId_, readRequest, buffer,
                                      &encodedSize);
    if (!status.ok()) break;
    response.clear();
    status = exchange_(
        std::span<const std::uint8_t>(buffer.data(), encodedSize), &response,
        Remaining(deadline), cancelled, [] { return true; });
    if (!status.ok()) break;
    status = CheckDeadline(deadline, cancelled);
    if (!status.ok()) break;
    MmsFileReadResponse part;
    status = DecodeMmsFileReadResponse(response, *nextInvokeId_, &part);
    if (!status.ok()) break;
    const auto& data = part.fileData.empty() ? part.data : part.fileData;
    if (total > maxBytes || data.size() > maxBytes - total) {
      status = grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                            "MMS文件累计大小超过本地下载上限");
      break;
    }
    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
    if (!file.good()) {
      status = grpc::Status(grpc::StatusCode::INTERNAL,
                            "MMS本地文件写入失败");
      break;
    }
    total += data.size();
    finished = !part.moreFollows;
    status = AdvanceInvokeId(nextInvokeId_);
    if (!status.ok()) break;
  }
  file.close();
  const auto closeStatus = closeRemote();
  std::error_code error;
  if (!closeStatus.ok()) {
    std::filesystem::remove(temp, error);
    return grpc::Status(grpc::StatusCode::ABORTED,
                        "MMS文件下载结束时FileClose释放失败");
  }
  if (!status.ok()) {
    std::filesystem::remove(temp, error);
    return status;
  }
  if (total != opened.fileSize) {
    std::filesystem::remove(temp, error);
    return DataLoss("FileRead累计长度与FileOpen文件属性不一致");
  }
  std::filesystem::rename(temp, local, error);
  if (error) {
    std::filesystem::remove(temp, error);
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "MMS下载文件原子替换失败");
  }
  result->bytesWritten = total;
  result->localPath = local;
  LOG_INFO("IEC61850 MMS文件下载完成: 远端文件={}, 本地文件={}, 字节数={}",
           remote, local, total);
  return grpc::Status::OK;
}

}  // namespace IEC61850
