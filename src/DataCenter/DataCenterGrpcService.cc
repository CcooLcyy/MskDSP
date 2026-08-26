#include "DataCenterGrpcService.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include "DataCenterCore.h"
#include "DataCenterStateStore.h"
#include "Logger.h"

namespace DataCenter {
namespace {
constexpr size_t kRouteSampleLimit = 3;
constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

std::string formatBytesHexPreview(const std::string& bytes, size_t maxLen = 32) {
  std::ostringstream oss;
  oss << std::uppercase << std::hex << std::setfill('0');
  const auto total = bytes.size();
  const auto count = std::min(total, maxLen);
  for (size_t i = 0; i < count; ++i) {
    if (i > 0) {
      oss << ' ';
    }
    const auto byte = static_cast<unsigned char>(bytes[i]);
    oss << std::setw(2) << static_cast<unsigned>(byte);
  }
  if (total > maxLen) {
    oss << " ...";
  }
  return oss.str();
}

const char* qualityToString(DataCenterProto::Quality quality) {
  switch (quality) {
    case DataCenterProto::QUALITY_GOOD:
      return "良好";
    case DataCenterProto::QUALITY_BAD:
      return "异常";
    case DataCenterProto::QUALITY_UNCERTAIN:
      return "不确定";
    case DataCenterProto::QUALITY_UNSPECIFIED:
    default:
      return "未指定";
  }
}

std::string formatPointValue(const DataCenterProto::PointValue& value) {
  switch (value.kind_case()) {
    case DataCenterProto::PointValue::kBoolValue:
      return std::string("类型=布尔, 值=") + (value.bool_value() ? "真" : "假");
    case DataCenterProto::PointValue::kIntValue:
      return "类型=整数, 值=" + std::to_string(value.int_value());
    case DataCenterProto::PointValue::kDoubleValue:
      return "类型=双精度, 值=" + std::to_string(value.double_value());
    case DataCenterProto::PointValue::kStringValue:
      return "类型=字符串, 长度=" +
             std::to_string(value.string_value().size()) +
             ", 十六进制=" + formatBytesHexPreview(value.string_value());
    case DataCenterProto::PointValue::kBytesValue: {
      const auto& bytes = value.bytes_value();
      return "类型=字节串, 长度=" + std::to_string(bytes.size()) +
             ", 十六进制=" + formatBytesHexPreview(bytes);
    }
    case DataCenterProto::PointValue::KIND_NOT_SET:
    default:
      return "类型=未设置";
  }
}

std::string contextPeer(grpc::ServerContext* context) {
  if (context == nullptr) {
    return "未知调用方";
  }
  auto peer = context->peer();
  if (peer.empty()) {
    return "未知调用方";
  }
  return peer;
}

std::string buildModuleUnixSocketAddress(const std::string& moduleName) {
  auto dir = std::filesystem::path("./socket");
  std::error_code ec;
  auto absDir = std::filesystem::absolute(dir, ec);
  if (ec) {
    absDir = dir;
  }
  auto sockPath = absDir / (moduleName + ".sock");
  return "unix:" + sockPath.string();
}

void appendHash(uint64_t* hash, const std::string& value) {
  if (hash == nullptr) {
    return;
  }
  for (const auto ch : value) {
    *hash ^= static_cast<unsigned char>(ch);
    *hash *= kFnvPrime;
  }
  *hash ^= 0xff;
  *hash *= kFnvPrime;
}

void appendHash(uint64_t* hash, uint32_t value) {
  appendHash(hash, std::to_string(value));
}

std::string formatHash(uint64_t hash) {
  std::ostringstream oss;
  oss << std::uppercase << std::hex << std::setfill('0') << std::setw(16) << hash;
  return oss.str();
}

std::string formatEndpointForLog(const DataCenterProto::Endpoint& endpoint) {
  std::ostringstream oss;
  oss << "{module_name=" << endpoint.module_name()
      << ", conn_name=" << endpoint.conn_name()
      << ", conn_id=" << endpoint.conn_id()
      << ", tag=" << endpoint.tag() << "}";
  return oss.str();
}

std::string formatRouteForLog(const DataCenterProto::Route& route) {
  return "src=" + formatEndpointForLog(route.src()) + " -> dst=" + formatEndpointForLog(route.dst());
}

void appendRouteHash(uint64_t* hash, const DataCenterProto::Route& route) {
  appendHash(hash, route.src().module_name());
  appendHash(hash, route.src().conn_name());
  appendHash(hash, route.src().conn_id());
  appendHash(hash, route.src().tag());
  appendHash(hash, route.dst().module_name());
  appendHash(hash, route.dst().conn_name());
  appendHash(hash, route.dst().conn_id());
  appendHash(hash, route.dst().tag());
}

struct RouteSummary {
  int total{0};
  int business{0};
  std::string hash;
  std::string sample;
};

template <typename Routes>
RouteSummary summarizeRoutes(const Routes& routes) {
  RouteSummary summary;
  summary.total = static_cast<int>(routes.size());
  uint64_t hash = kFnvOffset;
  std::ostringstream sample;
  int index = 0;
  for (const auto& route : routes) {
    appendRouteHash(&hash, route);
    if (index < static_cast<int>(kRouteSampleLimit)) {
      if (index > 0) {
        sample << " | ";
      }
      sample << formatRouteForLog(route);
    }
    ++index;
  }
  summary.business = summary.total;
  summary.hash = formatHash(hash);
  if (summary.total == 0) {
    summary.sample = "空";
  } else {
    if (summary.total > static_cast<int>(kRouteSampleLimit)) {
      sample << " | ... 共" << summary.total << "条";
    }
    summary.sample = sample.str();
  }
  return summary;
}

struct RouteFileState {
  std::string path;
  std::string exists{"未知"};
  std::string size{"未知"};
  std::string writeTimeTicks{"未知"};
  std::string hash{"未知"};
  std::string error;
  bool existsValue{false};
  bool sizeKnown{false};
  std::uintmax_t sizeValue{0};
};

void appendFileStateError(RouteFileState* state, const char* action, const std::error_code& ec) {
  if (!state->error.empty()) {
    state->error += "; ";
  }
  state->error += action;
  state->error += "失败: ";
  state->error += ec.message();
}

std::string fileHashForLog(const std::filesystem::path& path, std::error_code& ec) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs.is_open()) {
    ec = std::make_error_code(std::errc::no_such_file_or_directory);
    return "未知";
  }

  uint64_t hash = kFnvOffset;
  char buffer[4096];
  while (ifs) {
    ifs.read(buffer, sizeof(buffer));
    const auto count = ifs.gcount();
    for (std::streamsize i = 0; i < count; ++i) {
      hash ^= static_cast<unsigned char>(buffer[i]);
      hash *= kFnvPrime;
    }
  }
  if (ifs.bad()) {
    ec = std::make_error_code(std::errc::io_error);
    return "未知";
  }
  ec.clear();
  return formatHash(hash);
}

RouteFileState inspectRouteFileState(const std::filesystem::path& path) {
  RouteFileState state;
  state.path = path.string();
  std::error_code ec;
  const bool exists = std::filesystem::exists(path, ec);
  if (ec) {
    appendFileStateError(&state, "检查存在性", ec);
    return state;
  }
  state.exists = exists ? "true" : "false";
  state.existsValue = exists;
  if (!exists) {
    return state;
  }

  ec.clear();
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    appendFileStateError(&state, "获取大小", ec);
  } else {
    state.size = std::to_string(size);
    state.sizeKnown = true;
    state.sizeValue = size;
  }

  ec.clear();
  const auto mtime = std::filesystem::last_write_time(path, ec);
  if (ec) {
    appendFileStateError(&state, "获取修改时间", ec);
  } else {
    state.writeTimeTicks = std::to_string(mtime.time_since_epoch().count());
  }

  ec.clear();
  auto hash = fileHashForLog(path, ec);
  if (ec) {
    appendFileStateError(&state, "计算hash", ec);
  } else {
    state.hash = hash;
  }
  return state;
}

bool isExistingFileWithSize(const RouteFileState& state, std::uintmax_t size) {
  return state.existsValue && state.sizeKnown && state.sizeValue == size;
}

bool containsTraceWarning(const std::string& message) {
  return message.rfind("告警:", 0) == 0;
}

void logStateStoreTrace(const std::string& source, const RouteSummary& summary, const std::string& message) {
  if (containsTraceWarning(message)) {
    LOG_WARNING("DataCenter 状态持久化详细流程: 来源={}, routes_size={}, 业务路由数={}, 路由hash={}, {}",
                source, summary.total, summary.business, summary.hash, message);
    return;
  }
  LOG_INFO("DataCenter 状态持久化详细流程: 来源={}, routes_size={}, 业务路由数={}, 路由hash={}, {}",
           source, summary.total, summary.business, summary.hash, message);
}

void logStateStoreFiles(const char* phase, const DataCenterStateStore& stateStore, const RouteSummary& summary, const std::string& source = {}) {
  const auto databaseFile = inspectRouteFileState(stateStore.databasePath());
  LOG_INFO("DataCenter 状态持久化数据库状态: 阶段={}, 来源={}, routes_size={}, 业务路由数={}, 路由hash={}, 数据库路径={}, 数据库存在={}, 数据库大小={}, 数据库修改时间ticks={}, 数据库hash={}, 数据库状态错误={}",
           phase, source, summary.total, summary.business, summary.hash,
           databaseFile.path, databaseFile.exists, databaseFile.size, databaseFile.writeTimeTicks, databaseFile.hash, databaseFile.error);
}

RouteSummary unknownRouteSummary() {
  RouteSummary summary;
  summary.total = -1;
  summary.business = -1;
  summary.hash = "未知";
  summary.sample = "未知";
  return summary;
}
}  // namespace

struct DataCenterGrpcServiceImpl::Impl {
  struct Subscriber {
    uint64_t id{};
    uint32_t connId{};
    std::unordered_set<std::string> tagsFilter;
    uint64_t dropped{0};
    std::chrono::steady_clock::time_point lastDropLog{};

    std::mutex mu;
    std::condition_variable cv;
    std::deque<DataCenterProto::PointUpdate> queue;
    bool closed{false};
  };

  struct Delivery {
    const DataCenterProto::PointUpdate* update{nullptr};
    std::vector<std::shared_ptr<Subscriber>> subscribers;
  };

  static constexpr size_t kMaxQueueSize = 10000;
  static constexpr auto kSubscriberWaitTimeout = std::chrono::milliseconds(200);
  static constexpr auto kDropLogInterval = std::chrono::seconds(5);

  std::mutex mu;
  DataCenterCore core;
  DataCenterStateStore stateStore;

  uint64_t nextSubscriberId{0};
  std::unordered_map<uint32_t, std::unordered_map<uint64_t, std::shared_ptr<Subscriber>>> subscribersByConn;

  std::vector<std::shared_ptr<Subscriber>> matchSubscribersLocked(uint32_t dstConnId, const std::string& dstTag) {
    std::vector<std::shared_ptr<Subscriber>> result;
    auto it = subscribersByConn.find(dstConnId);
    if (it == subscribersByConn.end()) {
      return result;
    }

    result.reserve(it->second.size());
    for (const auto& [_, sub] : it->second) {
      if (!sub) {
        continue;
      }
      if (!sub->tagsFilter.empty() && !sub->tagsFilter.contains(dstTag)) {
        continue;
      }
      result.emplace_back(sub);
    }
    return result;
  }

  static void enqueue(const std::shared_ptr<Subscriber>& sub, const DataCenterProto::PointUpdate& update) {
    bool shouldLogDrop = false;
    uint64_t droppedTotal = 0;
    uint32_t connId = 0;
    size_t queueSize = 0;
    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(sub->mu);
      if (sub->closed) {
        return;
      }
      size_t droppedNow = 0;
      while (sub->queue.size() >= kMaxQueueSize) {
        sub->queue.pop_front();
        ++droppedNow;
      }
      if (droppedNow > 0) {
        sub->dropped += droppedNow;
        if (now - sub->lastDropLog >= kDropLogInterval) {
          sub->lastDropLog = now;
          shouldLogDrop = true;
          droppedTotal = sub->dropped;
          connId = sub->connId;
        }
      }
      sub->queue.emplace_back(update);
      if (shouldLogDrop) {
        queueSize = sub->queue.size();
      }
    }
    if (shouldLogDrop) {
      LOG_WARNING("DataCenter 订阅队列丢弃消息: conn_id={}, 已丢弃总数={}, 队列长度={}",
                  connId, droppedTotal, queueSize);
    }
    sub->cv.notify_one();
  }

  void removeSubscriber(uint32_t connId, uint64_t id) {
    std::lock_guard<std::mutex> lock(mu);
    auto it = subscribersByConn.find(connId);
    if (it == subscribersByConn.end()) {
      return;
    }
    it->second.erase(id);
    if (it->second.empty()) {
      subscribersByConn.erase(it);
    }
  }

  void closeSubscribersLocked(uint32_t connId) {
    auto it = subscribersByConn.find(connId);
    if (it == subscribersByConn.end()) {
      return;
    }

    for (const auto &[_, sub] : it->second) {
      if (!sub) {
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(sub->mu);
        sub->closed = true;
      }
      sub->cv.notify_all();
    }
    subscribersByConn.erase(it);
  }

  DataCenterProto::DataCenterState dumpStateLocked() const {
    DataCenterProto::DataCenterState state;
    *state.mutable_connections() = core.DumpConnectionsConfig();
    *state.mutable_conn_tags() = core.DumpConnTagsConfig();
    *state.mutable_routes() = core.DumpRoutesConfig();
    return state;
  }

  grpc::Status saveStateLocked(const std::string& source) {
    auto state = dumpStateLocked();
    const auto summary = summarizeRoutes(state.routes().routes());
    LOG_INFO("DataCenter 准备保存完整状态: 来源={}, connections={}, conn_tags={}, routes_size={}, 业务路由数={}, 路由hash={}, 路由样本={}",
             source, state.connections().conns_size(), state.conn_tags().conn_tags_size(),
             summary.total, summary.business, summary.hash, summary.sample);
    if (summary.total == 0) {
      LOG_WARNING("DataCenter 准备保存完整状态且路由为空: 来源={}, connections={}, conn_tags={}, routes_size=0, 业务路由数=0, 路由hash={}",
                  source, state.connections().conns_size(), state.conn_tags().conn_tags_size(), summary.hash);
    }
    logStateStoreFiles("状态落盘前", stateStore, summary, source);
    auto status = stateStore.Save(state, [source, summary](const std::string& message) {
      logStateStoreTrace(source, summary, message);
    });
    if (!status.ok()) {
      LOG_ERROR("DataCenter 完整状态落盘失败: 来源={}, connections={}, conn_tags={}, routes_size={}, 业务路由数={}, 路由hash={}, 原因={}",
                source, state.connections().conns_size(), state.conn_tags().conn_tags_size(),
                summary.total, summary.business, summary.hash, status.error_message());
      logStateStoreFiles("状态落盘失败后", stateStore, summary, source);
    } else {
      logStateStoreFiles("状态落盘后", stateStore, summary, source);
      const auto databaseFile = inspectRouteFileState(stateStore.databasePath());
      if (isExistingFileWithSize(databaseFile, 0)) {
        LOG_WARNING("DataCenter 完整状态落盘后数据库文件大小为0: 来源={}, connections={}, conn_tags={}, routes_size={}, 业务路由数={}, 路由hash={}, 数据库路径={}",
                    source, state.connections().conns_size(), state.conn_tags().conn_tags_size(),
                    summary.total, summary.business, summary.hash, databaseFile.path);
      }
    }
    return status;
  }
};

DataCenterGrpcServiceImpl::DataCenterGrpcServiceImpl() :
  impl_(std::make_unique<DataCenterGrpcServiceImpl::Impl>()) {
  DataCenterProto::DataCenterState state;
  auto loadSummary = unknownRouteSummary();
  logStateStoreFiles("启动模块 DataCenter 加载完整状态前", impl_->stateStore, loadSummary, "启动模块 DataCenter 加载完整状态");
  auto status = impl_->stateStore.Load(&state, [loadSummary](const std::string& message) {
    logStateStoreTrace("启动模块 DataCenter 加载完整状态", loadSummary, message);
  });
  if (!status.ok()) {
    LOG_ERROR("DataCenter 完整状态加载失败: {}", status.error_message());
    return;
  }

  const auto summary = summarizeRoutes(state.routes().routes());
  logStateStoreFiles("启动模块 DataCenter 加载完整状态后", impl_->stateStore, summary, "启动模块 DataCenter 加载完整状态");
  if (state.connections().conns_size() == 0 && state.connections().next_conn_id() == 0 &&
      state.conn_tags().conn_tags_size() == 0 && summary.total == 0) {
    LOG_INFO("DataCenter 完整状态为空，启动模块 DataCenter 将以空状态运行");
    return;
  }

  status = impl_->core.ReplaceConnectionsConfig(state.connections());
  if (!status.ok()) {
    LOG_ERROR("DataCenter 完整状态中的连接注册表应用失败: {}", status.error_message());
    return;
  }
  status = impl_->core.ReplaceConnTagsConfig(state.conn_tags());
  if (!status.ok()) {
    LOG_ERROR("DataCenter 完整状态中的连接标签注册表应用失败: {}", status.error_message());
    return;
  }
  status = impl_->core.ReplaceRoutesConfig(state.routes());
  if (!status.ok()) {
    LOG_ERROR("DataCenter 完整状态中的路由应用失败: {}", status.error_message());
    return;
  }

  if (summary.total == 0) {
    LOG_WARNING("DataCenter 完整状态中的路由为空: connections={}, conn_tags={}, routes_size=0, 路由hash={}",
                state.connections().conns_size(), state.conn_tags().conn_tags_size(), summary.hash);
  }
  LOG_INFO("DataCenter 已加载完整状态: connections={}, conn_tags={}, routes_size={}, 业务路由数={}, 路由hash={}, 路由样本={}",
           state.connections().conns_size(), state.conn_tags().conn_tags_size(),
           summary.total, summary.business, summary.hash, summary.sample);
}
DataCenterGrpcServiceImpl::~DataCenterGrpcServiceImpl() = default;

grpc::Status DataCenterGrpcServiceImpl::UpsertConnection(grpc::ServerContext* context, const DataCenterProto::UpsertConnectionRequest* request, DataCenterProto::Empty*) {
  if (request == nullptr) {
    LOG_ERROR("DataCenter UpsertConnection 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  const auto& conn = request->conn();
  std::lock_guard<std::mutex> lock(impl_->mu);
  auto status = impl_->core.UpsertConnection(*request);
  if (!status.ok()) {
    LOG_ERROR("DataCenter 更新连接失败: module_name={}, conn_name={}, conn_id={}, 原因={}",
              conn.module_name(), conn.conn_name(), conn.conn_id(), status.error_message());
    return status;
  }
  status = impl_->saveStateLocked("更新连接接口 调用方=" + contextPeer(context) + ", module_name=" + conn.module_name() +
                                  ", conn_name=" + conn.conn_name() + ", conn_id=" + std::to_string(conn.conn_id()));
  if (!status.ok()) {
    LOG_ERROR("DataCenter 更新连接落盘失败: module_name={}, conn_name={}, conn_id={}, 原因={}",
              conn.module_name(), conn.conn_name(), conn.conn_id(), status.error_message());
    return status;
  }
  LOG_INFO("DataCenter 已更新连接: module_name={}, conn_name={}, conn_id={}",
           conn.module_name(), conn.conn_name(), conn.conn_id());
  return grpc::Status::OK;
}

grpc::Status DataCenterGrpcServiceImpl::ListConnections(grpc::ServerContext*, const DataCenterProto::Empty*, DataCenterProto::ListConnectionsResponse* response) {
  if (response == nullptr) {
    LOG_ERROR("DataCenter ListConnections 响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  *response = impl_->core.ListConnections();
  return grpc::Status::OK;
}

grpc::Status DataCenterGrpcServiceImpl::GetOrCreateConnection(grpc::ServerContext* context, const DataCenterProto::GetOrCreateConnectionRequest* request, DataCenterProto::ConnectionInfo* response) {
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("DataCenter GetOrCreateConnection 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  const auto& key = request->key();

  std::lock_guard<std::mutex> lock(impl_->mu);
  auto status = impl_->core.GetOrCreateConnection(*request, response);
  if (!status.ok()) {
    LOG_ERROR("DataCenter 获取/创建连接失败: module_name={}, conn_name={}, 原因={}",
              key.module_name(), key.conn_name(), status.error_message());
    return status;
  }
  status = impl_->saveStateLocked("获取/创建连接接口 调用方=" + contextPeer(context) + ", module_name=" + key.module_name() +
                                  ", conn_name=" + key.conn_name() + ", conn_id=" + std::to_string(response->conn_id()));
  if (!status.ok()) {
    LOG_ERROR("DataCenter 获取/创建连接落盘失败: module_name={}, conn_name={}, 原因={}",
              key.module_name(), key.conn_name(), status.error_message());
    return status;
  }
  LOG_INFO("DataCenter 已获取/创建连接: module_name={}, conn_name={}, conn_id={}",
           response->module_name(), response->conn_name(), response->conn_id());
  return grpc::Status::OK;
}

grpc::Status DataCenterGrpcServiceImpl::RenameConnection(grpc::ServerContext* context, const DataCenterProto::RenameConnectionRequest* request, DataCenterProto::ConnectionInfo* response) {
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("DataCenter RenameConnection 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  const auto& oldKey = request->old_key();
  const auto& newKey = request->new_key();

  std::lock_guard<std::mutex> lock(impl_->mu);
  auto status = impl_->core.RenameConnection(*request, response);
  if (!status.ok()) {
    LOG_ERROR("DataCenter 重命名连接失败: old=({}, {}), new=({}, {}), 原因={}",
              oldKey.module_name(), oldKey.conn_name(), newKey.module_name(), newKey.conn_name(), status.error_message());
    return status;
  }
  status = impl_->saveStateLocked("重命名连接接口 调用方=" + contextPeer(context) + ", old=(" + oldKey.module_name() + ", " + oldKey.conn_name() +
                                  "), new=(" + newKey.module_name() + ", " + newKey.conn_name() + "), conn_id=" +
                                  std::to_string(response->conn_id()));
  if (!status.ok()) {
    LOG_ERROR("DataCenter 重命名连接落盘失败: old=({}, {}), new=({}, {}), 原因={}",
              oldKey.module_name(), oldKey.conn_name(), newKey.module_name(), newKey.conn_name(), status.error_message());
    return status;
  }
  LOG_INFO("DataCenter 已重命名连接: old=({}, {}), new=({}, {}), conn_id={}",
           oldKey.module_name(), oldKey.conn_name(), newKey.module_name(), newKey.conn_name(), response->conn_id());
  return grpc::Status::OK;
}

grpc::Status DataCenterGrpcServiceImpl::DeleteConnection(grpc::ServerContext* context, const DataCenterProto::DeleteConnectionRequest* request, DataCenterProto::Empty*) {
  const auto peer = contextPeer(context);
  if (request == nullptr) {
    LOG_ERROR("DataCenter DeleteConnection 请求为空: 调用方={}", peer);
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }

  std::lock_guard<std::mutex> lock(impl_->mu);
  const auto beforeSummary = summarizeRoutes(impl_->core.DumpRoutesConfig().routes());
  LOG_INFO("DataCenter 收到删除连接请求: 调用方={}, module_name={}, conn_name={}, 写入前总路由数={}, 写入前业务路由数={}, 写入前路由hash={}",
           peer, request->key().module_name(), request->key().conn_name(),
           beforeSummary.total, beforeSummary.business, beforeSummary.hash);

  DataCenterProto::ConnectionInfo conn;
  auto status = impl_->core.GetConnectionByKey(request->key(), &conn);
  if (!status.ok()) {
    const auto& key = request->key();
    LOG_ERROR("DataCenter 删除连接失败: 调用方={}, module_name={}, conn_name={}, 原因={}",
              peer, key.module_name(), key.conn_name(), status.error_message());
    return status;
  }

  status = impl_->core.DeleteConnection(*request);
  if (!status.ok()) {
    LOG_ERROR("DataCenter 删除连接失败: 调用方={}, module_name={}, conn_name={}, conn_id={}, 原因={}",
              peer, conn.module_name(), conn.conn_name(), conn.conn_id(), status.error_message());
    return status;
  }

  impl_->closeSubscribersLocked(conn.conn_id());

  status = impl_->saveStateLocked("删除连接接口 调用方=" + peer + ", module_name=" + conn.module_name() +
                                  ", conn_name=" + conn.conn_name() + ", conn_id=" + std::to_string(conn.conn_id()));
  if (!status.ok()) {
    LOG_ERROR("DataCenter 删除连接落盘失败: 调用方={}, module_name={}, conn_name={}, conn_id={}, 原因={}",
              peer, conn.module_name(), conn.conn_name(), conn.conn_id(), status.error_message());
    return status;
  }

  const auto afterSummary = summarizeRoutes(impl_->core.DumpRoutesConfig().routes());
  if (beforeSummary.total > 0 && afterSummary.total == 0) {
    LOG_WARNING("DataCenter 删除连接后路由被清空: 调用方={}, module_name={}, conn_name={}, conn_id={}, 写入前总路由数={}, 写入后总路由数={}, 写入前业务路由数={}, 写入后业务路由数={}, 写入前路由hash={}, 写入后路由hash={}",
                peer, conn.module_name(), conn.conn_name(), conn.conn_id(),
                beforeSummary.total, afterSummary.total,
                beforeSummary.business, afterSummary.business, beforeSummary.hash, afterSummary.hash);
  }
  LOG_INFO("DataCenter 已删除连接: 调用方={}, module_name={}, conn_name={}, conn_id={}, 写入前总路由数={}, 删除后总路由数={}, 写入前业务路由数={}, 删除后业务路由数={}, 写入前路由hash={}, 删除后路由hash={}",
           peer, conn.module_name(), conn.conn_name(), conn.conn_id(),
           beforeSummary.total, afterSummary.total,
           beforeSummary.business, afterSummary.business, beforeSummary.hash, afterSummary.hash);
  return grpc::Status::OK;
}

grpc::Status DataCenterGrpcServiceImpl::UpsertConnTags(grpc::ServerContext* context, const DataCenterProto::UpsertConnTagsRequest* request, DataCenterProto::Empty*) {
  if (request == nullptr) {
    LOG_ERROR("DataCenter UpsertConnTags 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  auto status = impl_->core.UpsertConnTags(*request);
  if (!status.ok()) {
    LOG_ERROR("DataCenter 更新连接标签注册表失败: conn_id={}, 标签数={}, replace={}, 原因={}",
              request->conn_id(), request->tags_size(), request->replace(), status.error_message());
    return status;
  }
  status = impl_->saveStateLocked("更新连接标签注册表接口 调用方=" + contextPeer(context) +
                                  ", conn_id=" + std::to_string(request->conn_id()) +
                                  ", replace=" + (request->replace() ? std::string("true") : std::string("false")));
  if (!status.ok()) {
    LOG_ERROR("DataCenter 连接标签注册表落盘失败: conn_id={}, 标签数={}, replace={}, 原因={}",
              request->conn_id(), request->tags_size(), request->replace(), status.error_message());
    return status;
  }
  LOG_INFO("DataCenter 已更新连接标签注册表: conn_id={}, 标签数={}, replace={}",
           request->conn_id(), request->tags_size(), request->replace());
  return grpc::Status::OK;
}

grpc::Status DataCenterGrpcServiceImpl::GetConnTags(grpc::ServerContext*, const DataCenterProto::GetConnTagsRequest* request, DataCenterProto::ConnTags* response) {
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("DataCenter GetConnTags 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->core.GetConnTags(request->conn_id(), response);
}

grpc::Status DataCenterGrpcServiceImpl::UpsertRoutes(grpc::ServerContext* context, const DataCenterProto::UpsertRoutesRequest* request, DataCenterProto::Empty*) {
  const auto peer = contextPeer(context);
  if (request == nullptr) {
    LOG_ERROR("DataCenter UpsertRoutes 请求为空: 调用方={}", peer);
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  const auto requestSummary = summarizeRoutes(request->routes());
  const auto beforeSummary = summarizeRoutes(impl_->core.DumpRoutesConfig().routes());
  LOG_INFO("DataCenter 收到更新路由请求: 调用方={}, routes={}, replace={}, 请求业务路由数={}, 请求路由hash={}, 请求路由样本={}, 写入前总路由数={}, 写入前业务路由数={}, 写入前路由hash={}",
           peer, requestSummary.total, request->replace(),
           requestSummary.business, requestSummary.hash, requestSummary.sample,
           beforeSummary.total, beforeSummary.business, beforeSummary.hash);
  if (request->replace() && request->routes_size() == 0) {
    LOG_WARNING("DataCenter 收到清空全部路由请求: 调用方={}, routes=0, replace=true, 写入前总路由数={}, 写入前业务路由数={}, 写入前路由hash={}",
                peer, beforeSummary.total, beforeSummary.business, beforeSummary.hash);
  }
  auto status = impl_->core.UpsertRoutes(*request);
  if (!status.ok()) {
    LOG_ERROR("DataCenter 更新路由失败: 调用方={}, routes={}, replace={}, 原因={}",
              peer, request->routes_size(), request->replace(), status.error_message());
    return status;
  }
  status = impl_->saveStateLocked("更新路由接口 调用方=" + peer + ", routes=" + std::to_string(request->routes_size()) +
                                  ", replace=" + (request->replace() ? std::string("true") : std::string("false")));
  if (!status.ok()) {
    LOG_ERROR("DataCenter 路由落盘失败: 调用方={}, routes={}, replace={}, 原因={}",
              peer, request->routes_size(), request->replace(), status.error_message());
    return status;
  }
  const auto afterSummary = summarizeRoutes(impl_->core.DumpRoutesConfig().routes());
  if (beforeSummary.total > 0 && afterSummary.total == 0) {
    LOG_WARNING("DataCenter 更新路由后路由被清空: 调用方={}, routes={}, replace={}, 请求路由hash={}, 写入前总路由数={}, 写入后总路由数={}, 写入前业务路由数={}, 写入后业务路由数={}, 写入前路由hash={}, 写入后路由hash={}",
                peer, requestSummary.total, request->replace(), requestSummary.hash,
                beforeSummary.total, afterSummary.total,
                beforeSummary.business, afterSummary.business, beforeSummary.hash, afterSummary.hash);
  }
  LOG_INFO("DataCenter 已更新路由并按稳定连接主键归一化: 调用方={}, routes={}, replace={}, 请求业务路由数={}, 请求路由hash={}, 写入前总路由数={}, 写入后总路由数={}, 写入前业务路由数={}, 写入后业务路由数={}, 写入前路由hash={}, 写入后路由hash={}",
           peer, requestSummary.total, request->replace(),
           requestSummary.business, requestSummary.hash,
           beforeSummary.total, afterSummary.total,
           beforeSummary.business, afterSummary.business, beforeSummary.hash, afterSummary.hash);
  return grpc::Status::OK;
}

grpc::Status DataCenterGrpcServiceImpl::DeleteRoutes(grpc::ServerContext* context, const DataCenterProto::DeleteRoutesRequest* request, DataCenterProto::Empty*) {
  const auto peer = contextPeer(context);
  if (request == nullptr) {
    LOG_ERROR("DataCenter DeleteRoutes 请求为空: 调用方={}", peer);
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  const auto requestSummary = summarizeRoutes(request->routes());
  const auto beforeSummary = summarizeRoutes(impl_->core.DumpRoutesConfig().routes());
  LOG_INFO("DataCenter 收到删除路由请求: 调用方={}, routes={}, 请求业务路由数={}, 请求路由hash={}, 请求路由样本={}, 写入前总路由数={}, 写入前业务路由数={}, 写入前路由hash={}",
           peer, requestSummary.total,
           requestSummary.business, requestSummary.hash, requestSummary.sample,
           beforeSummary.total, beforeSummary.business, beforeSummary.hash);
  auto status = impl_->core.DeleteRoutes(*request);
  if (!status.ok()) {
    LOG_ERROR("DataCenter 删除路由失败: 调用方={}, routes={}, 原因={}",
              peer, request->routes_size(), status.error_message());
    return status;
  }
  status = impl_->saveStateLocked("删除路由接口 调用方=" + peer + ", routes=" + std::to_string(request->routes_size()));
  if (!status.ok()) {
    LOG_ERROR("DataCenter 删除路由落盘失败: 调用方={}, routes={}, 原因={}",
              peer, request->routes_size(), status.error_message());
    return status;
  }
  const auto afterSummary = summarizeRoutes(impl_->core.DumpRoutesConfig().routes());
  if (beforeSummary.total > 0 && afterSummary.total == 0) {
    LOG_WARNING("DataCenter 删除路由后路由被清空: 调用方={}, routes={}, 请求路由hash={}, 写入前总路由数={}, 写入后总路由数={}, 写入前业务路由数={}, 写入后业务路由数={}, 写入前路由hash={}, 写入后路由hash={}",
                peer, requestSummary.total, requestSummary.hash,
                beforeSummary.total, afterSummary.total,
                beforeSummary.business, afterSummary.business, beforeSummary.hash, afterSummary.hash);
  }
  LOG_INFO("DataCenter 已删除稳定连接主键匹配的路由: 调用方={}, routes={}, 请求业务路由数={}, 请求路由hash={}, 写入前总路由数={}, 写入后总路由数={}, 写入前业务路由数={}, 写入后业务路由数={}, 写入前路由hash={}, 写入后路由hash={}",
           peer, requestSummary.total,
           requestSummary.business, requestSummary.hash,
           beforeSummary.total, afterSummary.total,
           beforeSummary.business, afterSummary.business, beforeSummary.hash, afterSummary.hash);
  return grpc::Status::OK;
}

grpc::Status DataCenterGrpcServiceImpl::ListRoutes(grpc::ServerContext*, const DataCenterProto::ListRoutesRequest* request, DataCenterProto::ListRoutesResponse* response) {
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("DataCenter ListRoutes 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  *response = impl_->core.ListRoutes(*request);
  return grpc::Status::OK;
}

grpc::Status DataCenterGrpcServiceImpl::Publish(grpc::ServerContext*, const DataCenterProto::PublishRequest* request, DataCenterProto::Empty*) {
  if (request == nullptr) {
    LOG_ERROR("DataCenter Publish 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }

  LOG_DEBUG("DataCenter 收到点值发布: src_conn_id={}, src_tag={}, {}, 质量={}({}), 请求时间戳={}",
           request->conn_id(), request->tag(), formatPointValue(request->value()),
           static_cast<int>(request->quality()), qualityToString(request->quality()), request->ts_ms());

  std::vector<DataCenterProto::PointUpdate> updates;
  std::vector<Impl::Delivery> deliveries;
  size_t updateCount = 0;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto status = impl_->core.Publish(*request, &updates);
    if (!status.ok()) {
      LOG_ERROR("DataCenter 点值发布失败: src_conn_id={}, src_tag={}, {}, 质量={}({}), 请求时间戳={}, 原因={}",
                request->conn_id(), request->tag(), formatPointValue(request->value()),
                static_cast<int>(request->quality()), qualityToString(request->quality()), request->ts_ms(),
                status.error_message());
      return status;
    }
    updateCount = updates.size();
    if (updateCount == 0) {
      LOG_DEBUG("DataCenter 点值发布未匹配路由: src_conn_id={}, src_tag={}, {}, 质量={}({}), 请求时间戳={}",
               request->conn_id(), request->tag(), formatPointValue(request->value()),
               static_cast<int>(request->quality()), qualityToString(request->quality()), request->ts_ms());
    }
    deliveries.reserve(updates.size());
    for (const auto& update : updates) {
      auto subs = impl_->matchSubscribersLocked(update.dst_conn_id(), update.dst_tag());
      if (subs.empty()) {
        LOG_DEBUG("DataCenter 点值已路由但无订阅者: src_conn_id={}, src_tag={}, dst_conn_id={}, dst_tag={}, {}, 质量={}({}), 时间戳={}",
                 update.src_conn_id(), update.src_tag(), update.dst_conn_id(), update.dst_tag(),
                 formatPointValue(update.value()), static_cast<int>(update.quality()),
                 qualityToString(update.quality()), update.ts_ms());
        continue;
      }
      LOG_DEBUG("DataCenter 点值转发: src_conn_id={}, src_tag={}, dst_conn_id={}, dst_tag={}, {}, 质量={}({}), 时间戳={}, 订阅者数={}",
               update.src_conn_id(), update.src_tag(), update.dst_conn_id(), update.dst_tag(),
               formatPointValue(update.value()), static_cast<int>(update.quality()),
               qualityToString(update.quality()), update.ts_ms(), subs.size());
      deliveries.emplace_back(Impl::Delivery{.update = &update, .subscribers = std::move(subs)});
    }
  }

  for (const auto& delivery : deliveries) {
    for (const auto& sub : delivery.subscribers) {
      Impl::enqueue(sub, *delivery.update);
    }
  }
  LOG_DEBUG("DataCenter 发布: conn_id={}, tag={}, 更新数={}, 投递数={}",
            request->conn_id(), request->tag(), updateCount, deliveries.size());
  return grpc::Status::OK;
}

grpc::Status DataCenterGrpcServiceImpl::ExecuteCommand(
    grpc::ServerContext* context,
    const DataCenterProto::ExecuteCommandRequest* request,
    DataCenterProto::ExecuteCommandResponse* response) {
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("DataCenter ExecuteCommand 请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }

  const auto now = std::chrono::system_clock::now();
  const auto upstreamDeadline = context == nullptr
                                    ? std::chrono::system_clock::time_point::max()
                                    : context->deadline();
  if (upstreamDeadline <= now) {
    LOG_WARNING("DataCenter 同步命令在路由前已超过上游截止时间");
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "DataCenter 同步命令已超过上游截止时间");
  }
  if (context != nullptr && context->IsCancelled()) {
    LOG_WARNING("DataCenter 同步命令在路由前已取消");
    return grpc::Status(grpc::StatusCode::CANCELLED,
                        "DataCenter 同步命令已取消");
  }

  LOG_INFO("DataCenter 收到同步命令: src={}, {}, 质量={}({}), 请求时间戳={}, request_id={}, timeout_ms={}",
           formatEndpointForLog(request->src()),
           formatPointValue(request->value()),
           static_cast<int>(request->quality()),
           qualityToString(request->quality()),
           request->ts_ms(),
           request->request_id(),
           request->timeout_ms());

  DataCenterProto::ExecuteCommandResponse routeResp;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto status = impl_->core.ResolveCommandRoute(*request, &routeResp);
    if (!status.ok()) {
      LOG_ERROR("DataCenter 同步命令路由解析失败: src={}, 原因={}", formatEndpointForLog(request->src()), status.error_message());
      return status;
    }
  }

  // 路由查询可能耗时；即使没有进入目标模块，也不能在调用方已经取消后
  // 把此前得到的无路由/歧义结果作为正常同步命令结果返回。
  const auto afterRoute = std::chrono::system_clock::now();
  if (upstreamDeadline <= afterRoute) {
    LOG_WARNING("DataCenter 同步命令在路由解析后已超过上游截止时间");
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "DataCenter 同步命令已超过上游截止时间");
  }
  if (context != nullptr && context->IsCancelled()) {
    LOG_WARNING("DataCenter 同步命令在路由解析后已取消");
    return grpc::Status(grpc::StatusCode::CANCELLED,
                        "DataCenter 同步命令已取消");
  }

  if (routeResp.status() != DataCenterProto::COMMAND_STATUS_UNSPECIFIED) {
    *response = routeResp;
    LOG_WARNING("DataCenter 同步命令未进入目标模块: src={}, status={}, reason={}",
                formatEndpointForLog(request->src()),
                static_cast<int>(response->status()),
                response->reason());
    return grpc::Status::OK;
  }

  const auto beforeTargetNow = std::chrono::system_clock::now();
  if (upstreamDeadline <= beforeTargetNow) {
    LOG_WARNING("DataCenter 同步命令在调用目标模块前已超过上游截止时间: dst={}",
                formatEndpointForLog(routeResp.dst()));
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "DataCenter 同步命令已超过上游截止时间");
  }
  if (context != nullptr && context->IsCancelled()) {
    LOG_WARNING("DataCenter 同步命令在调用目标模块前已取消: dst={}",
                formatEndpointForLog(routeResp.dst()));
    return grpc::Status(grpc::StatusCode::CANCELLED,
                        "DataCenter 同步命令已取消");
  }

  auto targetReq = *request;
  *targetReq.mutable_dst() = routeResp.dst();
  auto address = buildModuleUnixSocketAddress(routeResp.dst().module_name());
  auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
  auto stub = DataCenterProto::CommandExecutor::NewStub(channel);

  grpc::ClientContext targetContext;
  const auto timeoutMs = request->timeout_ms() == 0 ? 1500u : request->timeout_ms();
  const auto requestDeadline = std::chrono::system_clock::now() +
                               std::chrono::milliseconds(timeoutMs);
  const auto targetDeadline = std::min(upstreamDeadline, requestDeadline);
  targetContext.set_deadline(targetDeadline);

  // 将上游取消传播到目标模块，确保 IEC61850 控制不会在调用方退出后继续执行。
  std::atomic_bool upstreamCancelled{false};
  std::jthread cancellationWatcher;
  if (context != nullptr) {
    cancellationWatcher = std::jthread(
        [context, &targetContext, &upstreamCancelled](std::stop_token stopToken) {
          while (!stopToken.stop_requested()) {
            if (context->IsCancelled()) {
              upstreamCancelled.store(true, std::memory_order_release);
              targetContext.TryCancel();
              return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        });
  }

  DataCenterProto::ExecuteCommandResponse targetResp;
  auto status = stub->ExecuteCommand(&targetContext, targetReq, &targetResp);
  cancellationWatcher.request_stop();
  if (cancellationWatcher.joinable()) {
    cancellationWatcher.join();
  }

  const auto completionNow = std::chrono::system_clock::now();
  if (upstreamDeadline <= completionNow) {
    LOG_WARNING("DataCenter 同步命令在目标模块返回前已超过上游截止时间: dst={}",
                formatEndpointForLog(routeResp.dst()));
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "DataCenter 同步命令已超过上游截止时间");
  }
  if (upstreamCancelled.load(std::memory_order_acquire) ||
      (context != nullptr && context->IsCancelled())) {
    LOG_WARNING("DataCenter 同步命令在目标模块返回前已取消: dst={}",
                formatEndpointForLog(routeResp.dst()));
    return grpc::Status(grpc::StatusCode::CANCELLED,
                        "DataCenter 同步命令已取消");
  }

  if (!status.ok()) {
    response->Clear();
    *response->mutable_dst() = routeResp.dst();
    if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
      response->set_status(DataCenterProto::COMMAND_TIMEOUT);
      response->set_reason("目标模块同步命令执行超时");
    } else if (status.error_code() == grpc::StatusCode::UNIMPLEMENTED ||
               status.error_code() == grpc::StatusCode::UNAVAILABLE) {
      response->set_status(DataCenterProto::COMMAND_TARGET_UNAVAILABLE);
      response->set_reason("目标模块同步命令服务不可用: " + status.error_message());
    } else {
      response->set_status(DataCenterProto::COMMAND_INTERNAL_ERROR);
      response->set_reason("目标模块同步命令执行失败: " + status.error_message());
    }
    LOG_ERROR("DataCenter 同步命令调用目标模块失败: dst={}, 地址={}, grpc_code={}, 原因={}",
              formatEndpointForLog(routeResp.dst()),
              address,
              static_cast<int>(status.error_code()),
              status.error_message());
    return grpc::Status::OK;
  }

  if (!targetResp.has_dst()) {
    *targetResp.mutable_dst() = routeResp.dst();
  }
  if (targetResp.status() == DataCenterProto::COMMAND_STATUS_UNSPECIFIED) {
    targetResp.set_status(DataCenterProto::COMMAND_INTERNAL_ERROR);
    targetResp.set_reason("目标模块返回空命令状态");
  }

  if (targetResp.status() == DataCenterProto::COMMAND_ACCEPTED) {
    if (upstreamCancelled.load(std::memory_order_acquire) ||
        (context != nullptr && context->IsCancelled())) {
      LOG_WARNING("DataCenter 同步命令已取消，不写入已接受命令缓存: dst={}",
                  formatEndpointForLog(routeResp.dst()));
      return grpc::Status(grpc::StatusCode::CANCELLED,
                          "DataCenter 同步命令已取消");
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto storeStatus = impl_->core.StoreAcceptedCommand(targetReq, routeResp.dst());
    if (!storeStatus.ok()) {
      targetResp.set_status(DataCenterProto::COMMAND_INTERNAL_ERROR);
      targetResp.set_reason("同步命令已执行但写入最新值缓存失败: " + storeStatus.error_message());
      LOG_ERROR("DataCenter 同步命令写入最新值缓存失败: dst={}, 原因={}",
                formatEndpointForLog(routeResp.dst()),
                storeStatus.error_message());
    }
  }

  *response = targetResp;
  LOG_INFO("DataCenter 同步命令完成: src={}, dst={}, status={}, reject_code={}, reason={}",
           formatEndpointForLog(request->src()),
           formatEndpointForLog(response->dst()),
           static_cast<int>(response->status()),
           static_cast<int>(response->reject_code()),
           response->reason());
  return grpc::Status::OK;
}

grpc::Status DataCenterGrpcServiceImpl::BatchPublish(grpc::ServerContext*, const DataCenterProto::BatchPublishRequest* request, DataCenterProto::Empty*) {
  if (request == nullptr) {
    LOG_ERROR("DataCenter BatchPublish 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }

  LOG_DEBUG("DataCenter 收到批量点值发布: 点数={}", request->points_size());

  struct SrcKey {
    uint32_t connId{};
    std::string tag;
    bool operator==(const SrcKey& other) const { return connId == other.connId && tag == other.tag; }
  };
  struct SrcKeyHash {
    size_t operator()(const SrcKey& key) const noexcept {
      size_t h1 = std::hash<uint32_t>{}(key.connId);
      size_t h2 = std::hash<std::string>{}(key.tag);
      return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
  };

  std::vector<DataCenterProto::PointUpdate> updates;
  std::vector<Impl::Delivery> deliveries;
  size_t updateCount = 0;
  std::unordered_map<SrcKey, size_t, SrcKeyHash> updateCountBySrc;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto status = impl_->core.BatchPublish(*request, &updates);
    if (!status.ok()) {
      LOG_ERROR("DataCenter 批量点值发布失败: 点数={}, 原因={}", request->points_size(), status.error_message());
      return status;
    }
    updateCount = updates.size();
    updateCountBySrc.reserve(updates.size());
    deliveries.reserve(updates.size());
    for (const auto& update : updates) {
      ++updateCountBySrc[SrcKey{update.src_conn_id(), update.src_tag()}];
      auto subs = impl_->matchSubscribersLocked(update.dst_conn_id(), update.dst_tag());
      if (subs.empty()) {
        LOG_DEBUG("DataCenter 点值已路由但无订阅者: src_conn_id={}, src_tag={}, dst_conn_id={}, dst_tag={}, {}, 质量={}({}), 时间戳={}",
                 update.src_conn_id(), update.src_tag(), update.dst_conn_id(), update.dst_tag(),
                 formatPointValue(update.value()), static_cast<int>(update.quality()),
                 qualityToString(update.quality()), update.ts_ms());
        continue;
      }
      LOG_DEBUG("DataCenter 点值转发: src_conn_id={}, src_tag={}, dst_conn_id={}, dst_tag={}, {}, 质量={}({}), 时间戳={}, 订阅者数={}",
               update.src_conn_id(), update.src_tag(), update.dst_conn_id(), update.dst_tag(),
               formatPointValue(update.value()), static_cast<int>(update.quality()),
               qualityToString(update.quality()), update.ts_ms(), subs.size());
      deliveries.emplace_back(Impl::Delivery{.update = &update, .subscribers = std::move(subs)});
    }
  }

  size_t noRouteCount = 0;
  for (const auto& point : request->points()) {
    SrcKey key{point.conn_id(), point.tag()};
    if (!updateCountBySrc.contains(key)) {
      ++noRouteCount;
      LOG_DEBUG("DataCenter 点值发布未匹配路由: src_conn_id={}, src_tag={}, {}, 质量={}({}), 请求时间戳={}",
               point.conn_id(), point.tag(), formatPointValue(point.value()),
               static_cast<int>(point.quality()), qualityToString(point.quality()), point.ts_ms());
    }
  }

  for (const auto& delivery : deliveries) {
    for (const auto& sub : delivery.subscribers) {
      Impl::enqueue(sub, *delivery.update);
    }
  }
  LOG_INFO("DataCenter 批量发布完成: 点数={}, 更新数={}, 投递数={}, 未匹配路由点数={}",
           request->points_size(), updateCount, deliveries.size(), noRouteCount);
  return grpc::Status::OK;
}

grpc::Status DataCenterGrpcServiceImpl::GetSourceLatest(
    grpc::ServerContext*,
    const DataCenterProto::GetSourceLatestRequest* request,
    DataCenterProto::GetSourceLatestResponse* response) {
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("DataCenter GetSourceLatest 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }

  std::lock_guard<std::mutex> lock(impl_->mu);
  auto status = impl_->core.GetSourceLatest(*request, response);
  if (!status.ok()) {
    LOG_ERROR("DataCenter 获取源端最新值失败: conn_id={}, tags={}, 原因={}",
              request->conn_id(), request->tags_size(), status.error_message());
    return status;
  }
  LOG_DEBUG("DataCenter 已获取源端最新值: conn_id={}, tags={}, updates={}",
            request->conn_id(), request->tags_size(), response->updates_size());
  return grpc::Status::OK;
}

grpc::Status DataCenterGrpcServiceImpl::GetLatest(grpc::ServerContext*, const DataCenterProto::GetLatestRequest* request, DataCenterProto::GetLatestResponse* response) {
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("DataCenter GetLatest 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->core.GetLatest(*request, response);
}

grpc::Status DataCenterGrpcServiceImpl::GetThroughputSnapshot(
    grpc::ServerContext*, const DataCenterProto::Empty* request, DataCenterProto::ThroughputSnapshot* response) {
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("DataCenter GetThroughputSnapshot 请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  *response = impl_->core.GetThroughputSnapshot();
  LOG_DEBUG("DataCenter 已返回吞吐量快照: samples={}, current_points_per_second={}, peak_points_per_second={}",
            response->samples_size(), response->current_points_per_second(), response->peak_points_per_second());
  return grpc::Status::OK;
}

grpc::Status DataCenterGrpcServiceImpl::Subscribe(grpc::ServerContext* context, const DataCenterProto::SubscribeRequest* request, grpc::ServerWriter<DataCenterProto::PointUpdate>* writer) {
  if (context == nullptr || request == nullptr || writer == nullptr) {
    LOG_ERROR("DataCenter Subscribe 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "context/request/writer 为空");
  }
  if (request->conn_id() == 0) {
    LOG_ERROR("DataCenter Subscribe 参数缺失: conn_id");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id 不能为空");
  }

  auto subscriber = std::make_shared<Impl::Subscriber>();
  subscriber->connId = request->conn_id();
  for (const auto& tag : request->tags()) {
    if (tag.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tags 包含空字符串");
    }
    subscriber->tagsFilter.emplace(tag);
  }

  std::vector<DataCenterProto::PointUpdate> snapshot;
  const auto tagsCount = request->tags_size();
  const auto snapshotEnabled = request->snapshot();
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    subscriber->id = ++impl_->nextSubscriberId;
    impl_->subscribersByConn[subscriber->connId].emplace(subscriber->id, subscriber);

    if (request->snapshot()) {
      DataCenterProto::GetLatestRequest latestReq;
      latestReq.set_conn_id(subscriber->connId);
      for (const auto& tag : request->tags()) {
        latestReq.add_tags(tag);
      }
      DataCenterProto::GetLatestResponse latestResp;
      auto status = impl_->core.GetLatest(latestReq, &latestResp);
      if (!status.ok()) {
        auto it = impl_->subscribersByConn.find(subscriber->connId);
        if (it != impl_->subscribersByConn.end()) {
          it->second.erase(subscriber->id);
          if (it->second.empty()) {
            impl_->subscribersByConn.erase(it);
          }
        }
        LOG_ERROR("DataCenter Subscribe 获取快照失败: conn_id={}, subscriber_id={}, 原因={}",
                  subscriber->connId, subscriber->id, status.error_message());
        return status;
      }
      snapshot.reserve(static_cast<size_t>(latestResp.updates_size()));
      for (const auto& update : latestResp.updates()) {
        snapshot.emplace_back(update);
      }
    }
  }
  LOG_INFO("DataCenter Subscribe 开始: conn_id={}, tags={}, snapshot={}, subscriber_id={}",
           subscriber->connId, tagsCount, snapshotEnabled, subscriber->id);

  auto markClosed = [&subscriber]() {
    {
      std::lock_guard<std::mutex> lock(subscriber->mu);
      subscriber->closed = true;
    }
    subscriber->cv.notify_all();
  };

  for (const auto& update : snapshot) {
    if (context->IsCancelled()) {
      break;
    }
    if (!writer->Write(update)) {
      break;
    }
  }

  while (!context->IsCancelled()) {
    DataCenterProto::PointUpdate update;
    {
      std::unique_lock<std::mutex> lock(subscriber->mu);
      subscriber->cv.wait_for(lock, Impl::kSubscriberWaitTimeout, [&subscriber]() { return subscriber->closed || !subscriber->queue.empty(); });
      if (subscriber->closed) {
        break;
      }
      if (subscriber->queue.empty()) {
        continue;
      }
      update = std::move(subscriber->queue.front());
      subscriber->queue.pop_front();
    }
    if (!writer->Write(update)) {
      break;
    }
  }

  markClosed();
  impl_->removeSubscriber(subscriber->connId, subscriber->id);
  uint64_t dropped = 0;
  {
    std::lock_guard<std::mutex> lock(subscriber->mu);
    dropped = subscriber->dropped;
  }
  LOG_INFO("DataCenter Subscribe 结束: conn_id={}, subscriber_id={}, dropped={}",
           subscriber->connId, subscriber->id, dropped);
  return grpc::Status::OK;
}
}  // namespace DataCenter
