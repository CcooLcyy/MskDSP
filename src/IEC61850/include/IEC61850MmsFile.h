#pragma once

#include <chrono>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/support/status.h>

#include "IEC61850MmsService.h"
#include "IEC61850MmsExchange.h"
#include "IEC61850MmsTransport.h"

namespace IEC61850 {

// AR502H实际业务使用的远端文件下载请求。localFile可以是调用方指定的
// 相对或绝对普通目标文件，协议层不会根据远端文件名拼接本地路径。
struct MmsFileDownloadRequest {
  std::string remoteFile;
  std::string localFile;
  // 与工作器及历史调用方兼容的字段名。
  std::string remoteFileName;
  std::string localPath;
  std::uint64_t maxBytes = 64ull * 1024ull * 1024ull;
  std::chrono::milliseconds timeout = std::chrono::seconds(5);
  std::shared_ptr<std::atomic_bool> cancellation;
};

struct MmsFileDownloadResult {
  std::uint64_t bytesWritten = 0;
  std::string localPath;
};

struct MmsFileUploadRequest {
  std::string localFile;
  std::string remoteFile;
  std::string remoteDomain;
  std::uint64_t maxBytes = 64ull * 1024ull * 1024ull;
  std::size_t segmentBytes = 32 * 1024;
  std::chrono::milliseconds timeout = std::chrono::seconds(5);
  std::shared_ptr<std::atomic_bool> cancellation;
};

struct MmsFileMutationRequest {
  std::string fileName;
  std::string newFileName;
  std::chrono::milliseconds timeout = std::chrono::seconds(5);
  std::shared_ptr<std::atomic_bool> cancellation;
};

using MmsFileExchange = std::function<grpc::Status(
    std::span<const std::uint8_t>, std::vector<std::uint8_t>*,
    std::chrono::milliseconds, const MmsCancellationPredicate&,
    const MmsRequestSentHandler&)>;

using MmsFileUnconfirmedHandler =
    std::function<void(std::span<const std::uint8_t>)>;

// MMS文件服务事务适配器。它不拥有传输对象，调用期间必须由工作器保证
// 同一个MMS通道没有其它确认服务并发访问。
class MmsFileClient {
public:
  MmsFileClient() = default;
  MmsFileClient(MmsFileExchange exchange, std::uint32_t* nextInvokeId)
      : exchange_(std::move(exchange)), nextInvokeId_(nextInvokeId) {}

  grpc::Status ListDirectory(
      const MmsFileDirectoryRequest& request,
      std::vector<MmsFileDirectoryEntry>* entries, std::size_t maxEntries,
      std::chrono::milliseconds timeout,
      const MmsCancellationPredicate& isCancelled = {}) const;

  grpc::Status ObtainFile(
      const MmsFileDownloadRequest& request, MmsFileDownloadResult* result,
      const MmsCancellationPredicate& isCancelled = {}) const;

  grpc::Status Upload(
      const MmsFileUploadRequest& request,
      std::chrono::milliseconds timeout = std::chrono::seconds(5),
      const MmsCancellationPredicate& isCancelled = {}) const;
  grpc::Status DeleteFile(
      const MmsFileMutationRequest& request,
      std::chrono::milliseconds timeout = std::chrono::seconds(5),
      const MmsCancellationPredicate& isCancelled = {}) const;
  grpc::Status RenameFile(
      const MmsFileMutationRequest& request,
      std::chrono::milliseconds timeout = std::chrono::seconds(5),
      const MmsCancellationPredicate& isCancelled = {}) const;

  grpc::Status ReadDirectory(
      MmsTransport& transport, std::uint32_t* nextInvokeId,
      const MmsFileDirectoryRequest& request, MmsFileDirectoryResponse* response,
      std::chrono::milliseconds timeout = std::chrono::seconds(5),
      const MmsFileUnconfirmedHandler& onUnconfirmed = {},
      const std::function<bool()>& isCancelled = {},
      const MmsRequestSentHandler& requestSent = {}) const;

  grpc::Status Download(
      MmsTransport& transport, std::uint32_t* nextInvokeId,
      const MmsFileDownloadRequest& request,
      std::chrono::milliseconds timeout = std::chrono::seconds(5),
      const MmsFileUnconfirmedHandler& onUnconfirmed = {},
      const std::function<bool()>& isCancelled = {},
      const MmsRequestSentHandler& requestSent = {}) const;

  static grpc::Status ValidateRemoteFileName(std::string_view remoteFile);
  static grpc::Status ValidateLocalFileName(std::string_view localFile);

private:
  MmsFileExchange exchange_;
  std::uint32_t* nextInvokeId_ = nullptr;
};

}  // namespace IEC61850
