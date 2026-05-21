#include "DataCenterGrpcService.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "DataCenterConnectionStore.h"
#include "DataCenterCore.h"
#include "DataCenterConnTagsStore.h"
#include "DataCenterRouteStore.h"
#include "Logger.h"

namespace DataCenter {
namespace {
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
      return "类型=字符串, 值=\"" + value.string_value() + "\"";
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
  DataCenterConnectionStore connectionStore;
  DataCenterConnTagsStore connTagsStore;
  DataCenterRouteStore routeStore;

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

  grpc::Status saveConnectionsLocked() {
    auto config = core.DumpConnectionsConfig();
    auto status = connectionStore.Save(config);
    if (!status.ok()) {
      LOG_INFO("DataCenter 连接注册表落盘失败: {}", status.error_message());
    }
    return status;
  }

  grpc::Status saveConnTagsLocked() {
    auto config = core.DumpConnTagsConfig();
    auto status = connTagsStore.Save(config);
    if (!status.ok()) {
      LOG_INFO("DataCenter 连接标签注册表落盘失败: {}", status.error_message());
    }
    return status;
  }

  grpc::Status saveRoutesLocked() {
    auto config = core.DumpRoutesConfig();
    auto status = routeStore.Save(config);
    if (!status.ok()) {
      LOG_INFO("DataCenter 路由落盘失败: {}", status.error_message());
    }
    return status;
  }
};

DataCenterGrpcServiceImpl::DataCenterGrpcServiceImpl() :
  impl_(std::make_unique<DataCenterGrpcServiceImpl::Impl>()) {
  {
    DataCenterProto::ConnectionsConfig config;
    auto status = impl_->connectionStore.Load(&config);
    if (!status.ok()) {
      LOG_INFO("DataCenter 连接注册表加载失败: {}", status.error_message());
    } else if (config.conns_size() > 0 || config.next_conn_id() != 0) {
      status = impl_->core.ReplaceConnectionsConfig(config);
      if (!status.ok()) {
        LOG_INFO("DataCenter 连接注册表应用失败: {}", status.error_message());
      } else {
        const auto count = config.conns_size();
        LOG_INFO("DataCenter 已加载连接注册表: {} 条", count);
      }
    }
  }

  {
    DataCenterProto::ConnTagsConfig config;
    auto status = impl_->connTagsStore.Load(&config);
    if (!status.ok()) {
      LOG_INFO("DataCenter 连接标签注册表加载失败: {}", status.error_message());
    } else if (config.conn_tags_size() > 0) {
      status = impl_->core.ReplaceConnTagsConfig(config);
      if (!status.ok()) {
        LOG_INFO("DataCenter 连接标签注册表应用失败: {}", status.error_message());
      } else {
        const auto count = config.conn_tags_size();
        LOG_INFO("DataCenter 已加载连接标签注册表配置: {} 个连接", count);
      }
    }
  }

  {
    DataCenterProto::RoutesConfig config;
    auto status = impl_->routeStore.Load(&config);
    if (!status.ok()) {
      LOG_ERROR("DataCenter 路由加载失败: {}", status.error_message());
    } else if (config.routes_size() > 0) {
      status = impl_->core.ReplaceRoutesConfig(config);
      if (!status.ok()) {
        LOG_INFO("DataCenter 路由应用失败: {}", status.error_message());
      } else {
        const auto count = config.routes_size();
        LOG_INFO("DataCenter 已加载路由配置: {} 条", count);
      }
    }
  }
}
DataCenterGrpcServiceImpl::~DataCenterGrpcServiceImpl() = default;

grpc::Status DataCenterGrpcServiceImpl::UpsertConnection(grpc::ServerContext*, const DataCenterProto::UpsertConnectionRequest* request, DataCenterProto::Empty*) {
  if (request == nullptr) {
    LOG_ERROR("DataCenter UpsertConnection 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  auto status = impl_->core.UpsertConnection(*request);
  if (!status.ok()) {
    const auto& conn = request->conn();
    LOG_ERROR("DataCenter 更新连接失败: module_name={}, conn_name={}, conn_id={}, 原因={}",
              conn.module_name(), conn.conn_name(), conn.conn_id(), status.error_message());
    return status;
  }
  status = impl_->saveConnectionsLocked();
  if (!status.ok()) {
    const auto& conn = request->conn();
    LOG_ERROR("DataCenter 更新连接落盘失败: module_name={}, conn_name={}, conn_id={}, 原因={}",
              conn.module_name(), conn.conn_name(), conn.conn_id(), status.error_message());
    return status;
  }
  const auto& conn = request->conn();
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

grpc::Status DataCenterGrpcServiceImpl::GetOrCreateConnection(grpc::ServerContext*, const DataCenterProto::GetOrCreateConnectionRequest* request, DataCenterProto::ConnectionInfo* response) {
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("DataCenter GetOrCreateConnection 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }

  std::lock_guard<std::mutex> lock(impl_->mu);
  auto status = impl_->core.GetOrCreateConnection(*request, response);
  if (!status.ok()) {
    const auto& key = request->key();
    LOG_ERROR("DataCenter 获取/创建连接失败: module_name={}, conn_name={}, 原因={}",
              key.module_name(), key.conn_name(), status.error_message());
    return status;
  }
  status = impl_->saveConnectionsLocked();
  if (!status.ok()) {
    const auto& key = request->key();
    LOG_ERROR("DataCenter 获取/创建连接落盘失败: module_name={}, conn_name={}, 原因={}",
              key.module_name(), key.conn_name(), status.error_message());
    return status;
  }
  LOG_INFO("DataCenter 已获取/创建连接: module_name={}, conn_name={}, conn_id={}",
           response->module_name(), response->conn_name(), response->conn_id());
  return grpc::Status::OK;
}

grpc::Status DataCenterGrpcServiceImpl::RenameConnection(grpc::ServerContext*, const DataCenterProto::RenameConnectionRequest* request, DataCenterProto::ConnectionInfo* response) {
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("DataCenter RenameConnection 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }

  std::lock_guard<std::mutex> lock(impl_->mu);
  auto status = impl_->core.RenameConnection(*request, response);
  if (!status.ok()) {
    const auto& oldKey = request->old_key();
    const auto& newKey = request->new_key();
    LOG_ERROR("DataCenter 重命名连接失败: old=({}, {}), new=({}, {}), 原因={}",
              oldKey.module_name(), oldKey.conn_name(), newKey.module_name(), newKey.conn_name(), status.error_message());
    return status;
  }
  status = impl_->saveConnectionsLocked();
  if (!status.ok()) {
    const auto& oldKey = request->old_key();
    const auto& newKey = request->new_key();
    LOG_ERROR("DataCenter 重命名连接落盘失败: old=({}, {}), new=({}, {}), 原因={}",
              oldKey.module_name(), oldKey.conn_name(), newKey.module_name(), newKey.conn_name(), status.error_message());
    return status;
  }
  const auto& oldKey = request->old_key();
  const auto& newKey = request->new_key();
  LOG_INFO("DataCenter 已重命名连接: old=({}, {}), new=({}, {}), conn_id={}",
           oldKey.module_name(), oldKey.conn_name(), newKey.module_name(), newKey.conn_name(), response->conn_id());
  return grpc::Status::OK;
}

grpc::Status DataCenterGrpcServiceImpl::DeleteConnection(grpc::ServerContext*, const DataCenterProto::DeleteConnectionRequest* request, DataCenterProto::Empty*) {
  if (request == nullptr) {
    LOG_ERROR("DataCenter DeleteConnection 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }

  std::lock_guard<std::mutex> lock(impl_->mu);

  DataCenterProto::ConnectionInfo conn;
  auto status = impl_->core.GetConnectionByKey(request->key(), &conn);
  if (!status.ok()) {
    const auto& key = request->key();
    LOG_ERROR("DataCenter 删除连接失败: module_name={}, conn_name={}, 原因={}",
              key.module_name(), key.conn_name(), status.error_message());
    return status;
  }

  status = impl_->core.DeleteConnection(*request);
  if (!status.ok()) {
    LOG_ERROR("DataCenter 删除连接失败: module_name={}, conn_name={}, conn_id={}, 原因={}",
              conn.module_name(), conn.conn_name(), conn.conn_id(), status.error_message());
    return status;
  }

  impl_->closeSubscribersLocked(conn.conn_id());

  status = impl_->saveConnectionsLocked();
  if (!status.ok()) {
    LOG_ERROR("DataCenter 删除连接落盘失败: module_name={}, conn_name={}, conn_id={}, 原因={}",
              conn.module_name(), conn.conn_name(), conn.conn_id(), status.error_message());
    return status;
  }

  status = impl_->saveConnTagsLocked();
  if (!status.ok()) {
    LOG_ERROR("DataCenter 删除连接标签注册表落盘失败: module_name={}, conn_name={}, conn_id={}, 原因={}",
              conn.module_name(), conn.conn_name(), conn.conn_id(), status.error_message());
    return status;
  }

  status = impl_->saveRoutesLocked();
  if (!status.ok()) {
    LOG_ERROR("DataCenter 删除连接路由落盘失败: module_name={}, conn_name={}, conn_id={}, 原因={}",
              conn.module_name(), conn.conn_name(), conn.conn_id(), status.error_message());
    return status;
  }
  LOG_INFO("DataCenter 已删除连接: module_name={}, conn_name={}, conn_id={}",
           conn.module_name(), conn.conn_name(), conn.conn_id());
  return grpc::Status::OK;
}

grpc::Status DataCenterGrpcServiceImpl::UpsertConnTags(grpc::ServerContext*, const DataCenterProto::UpsertConnTagsRequest* request, DataCenterProto::Empty*) {
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
  status = impl_->saveConnTagsLocked();
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
  auto status = impl_->core.UpsertRoutes(*request);
  if (!status.ok()) {
    LOG_ERROR("DataCenter 更新路由失败: 调用方={}, routes={}, replace={}, 原因={}",
              peer, request->routes_size(), request->replace(), status.error_message());
    return status;
  }
  status = impl_->saveRoutesLocked();
  if (!status.ok()) {
    LOG_ERROR("DataCenter 路由落盘失败: 调用方={}, routes={}, replace={}, 原因={}",
              peer, request->routes_size(), request->replace(), status.error_message());
    return status;
  }
  const auto totalRoutes = impl_->core.DumpRoutesConfig().routes_size();
  LOG_INFO("DataCenter 已更新路由并按稳定连接主键归一化: 调用方={}, routes={}, replace={}, 总路由数={}",
           peer, request->routes_size(), request->replace(), totalRoutes);
  return grpc::Status::OK;
}

grpc::Status DataCenterGrpcServiceImpl::DeleteRoutes(grpc::ServerContext* context, const DataCenterProto::DeleteRoutesRequest* request, DataCenterProto::Empty*) {
  const auto peer = contextPeer(context);
  if (request == nullptr) {
    LOG_ERROR("DataCenter DeleteRoutes 请求为空: 调用方={}", peer);
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  auto status = impl_->core.DeleteRoutes(*request);
  if (!status.ok()) {
    LOG_ERROR("DataCenter 删除路由失败: 调用方={}, routes={}, 原因={}",
              peer, request->routes_size(), status.error_message());
    return status;
  }
  status = impl_->saveRoutesLocked();
  if (!status.ok()) {
    LOG_ERROR("DataCenter 删除路由落盘失败: 调用方={}, routes={}, 原因={}",
              peer, request->routes_size(), status.error_message());
    return status;
  }
  const auto totalRoutes = impl_->core.DumpRoutesConfig().routes_size();
  LOG_INFO("DataCenter 已删除稳定连接主键匹配的路由: 调用方={}, routes={}, 总路由数={}",
           peer, request->routes_size(), totalRoutes);
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

  LOG_INFO("DataCenter 收到点值发布: src_conn_id={}, src_tag={}, {}, 质量={}({}), 请求时间戳={}",
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
      LOG_INFO("DataCenter 点值发布未匹配路由: src_conn_id={}, src_tag={}, {}, 质量={}({}), 请求时间戳={}",
               request->conn_id(), request->tag(), formatPointValue(request->value()),
               static_cast<int>(request->quality()), qualityToString(request->quality()), request->ts_ms());
    }
    deliveries.reserve(updates.size());
    for (const auto& update : updates) {
      auto subs = impl_->matchSubscribersLocked(update.dst_conn_id(), update.dst_tag());
      if (subs.empty()) {
        LOG_INFO("DataCenter 点值已路由但无订阅者: src_conn_id={}, src_tag={}, dst_conn_id={}, dst_tag={}, {}, 质量={}({}), 时间戳={}",
                 update.src_conn_id(), update.src_tag(), update.dst_conn_id(), update.dst_tag(),
                 formatPointValue(update.value()), static_cast<int>(update.quality()),
                 qualityToString(update.quality()), update.ts_ms());
        continue;
      }
      LOG_INFO("DataCenter 点值转发: src_conn_id={}, src_tag={}, dst_conn_id={}, dst_tag={}, {}, 质量={}({}), 时间戳={}, 订阅者数={}",
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

grpc::Status DataCenterGrpcServiceImpl::BatchPublish(grpc::ServerContext*, const DataCenterProto::BatchPublishRequest* request, DataCenterProto::Empty*) {
  if (request == nullptr) {
    LOG_ERROR("DataCenter BatchPublish 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }

  LOG_INFO("DataCenter 收到批量点值发布: 点数={}", request->points_size());

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
        LOG_INFO("DataCenter 点值已路由但无订阅者: src_conn_id={}, src_tag={}, dst_conn_id={}, dst_tag={}, {}, 质量={}({}), 时间戳={}",
                 update.src_conn_id(), update.src_tag(), update.dst_conn_id(), update.dst_tag(),
                 formatPointValue(update.value()), static_cast<int>(update.quality()),
                 qualityToString(update.quality()), update.ts_ms());
        continue;
      }
      LOG_INFO("DataCenter 点值转发: src_conn_id={}, src_tag={}, dst_conn_id={}, dst_tag={}, {}, 质量={}({}), 时间戳={}, 订阅者数={}",
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
      LOG_INFO("DataCenter 点值发布未匹配路由: src_conn_id={}, src_tag={}, {}, 质量={}({}), 请求时间戳={}",
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

grpc::Status DataCenterGrpcServiceImpl::GetLatest(grpc::ServerContext*, const DataCenterProto::GetLatestRequest* request, DataCenterProto::GetLatestResponse* response) {
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("DataCenter GetLatest 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->core.GetLatest(*request, response);
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
