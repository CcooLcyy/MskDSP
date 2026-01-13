#include "DataCenterGrpcService.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "DataCenterConnectionStore.h"
#include "DataCenterCore.h"
#include "DataCenterPointTableStore.h"
#include "DataCenterRouteStore.h"
#include "Logger.h"

namespace DataCenter {
struct DataCenterGrpcServiceImpl::Impl {
  struct Subscriber {
    uint64_t id{};
    uint32_t connId{};
    std::unordered_set<std::string> tagsFilter;

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

  std::mutex mu;
  DataCenterCore core;
  DataCenterConnectionStore connectionStore;
  DataCenterPointTableStore pointTableStore;
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
    {
      std::lock_guard<std::mutex> lock(sub->mu);
      if (sub->closed) {
        return;
      }
      while (sub->queue.size() >= kMaxQueueSize) {
        sub->queue.pop_front();
      }
      sub->queue.emplace_back(update);
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
};

DataCenterGrpcServiceImpl::DataCenterGrpcServiceImpl() :
  impl_(std::make_unique<DataCenterGrpcServiceImpl::Impl>()) {
  {
    DataCenterProto::ConnectionsConfig config;
    auto status = impl_->connectionStore.Load(&config);
    if (!status.ok()) {
      const auto message = status.error_message();
      LOG_INFO("DataCenter 连接注册表加载失败: {}", message);
    } else if (config.conns_size() > 0 || config.next_conn_id() != 0) {
      status = impl_->core.ReplaceConnectionsConfig(config);
      if (!status.ok()) {
        const auto message = status.error_message();
        LOG_INFO("DataCenter 连接注册表应用失败: {}", message);
      } else {
        const auto count = config.conns_size();
        LOG_INFO("DataCenter 已加载连接注册表: {} 条", count);
      }
    }
  }

  {
    DataCenterProto::PointTablesConfig config;
    auto status = impl_->pointTableStore.Load(&config);
    if (!status.ok()) {
      const auto message = status.error_message();
      LOG_INFO("DataCenter 点表加载失败: {}", message);
    } else if (config.point_tables_size() > 0) {
      status = impl_->core.ReplacePointTablesConfig(config);
      if (!status.ok()) {
        const auto message = status.error_message();
        LOG_INFO("DataCenter 点表应用失败: {}", message);
      } else {
        const auto count = config.point_tables_size();
        LOG_INFO("DataCenter 已加载点表配置: {} 个连接", count);
      }
    }
  }

  {
    DataCenterProto::RoutesConfig config;
    auto status = impl_->routeStore.Load(&config);
    if (!status.ok()) {
      const auto message = status.error_message();
      LOG_INFO("DataCenter 路由加载失败: {}", message);
    } else if (config.routes_size() > 0) {
      status = impl_->core.ReplaceRoutesConfig(config);
      if (!status.ok()) {
        const auto message = status.error_message();
        LOG_INFO("DataCenter 路由应用失败: {}", message);
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
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request is null");
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  auto status = impl_->core.UpsertConnection(*request);
  if (!status.ok()) {
    return status;
  }
  auto config = impl_->core.DumpConnectionsConfig();
  status = impl_->connectionStore.Save(config);
  if (!status.ok()) {
    const auto message = status.error_message();
    LOG_INFO("DataCenter 连接注册表落盘失败: {}", message);
  }
  return status;
}

grpc::Status DataCenterGrpcServiceImpl::ListConnections(grpc::ServerContext*, const DataCenterProto::Empty*, DataCenterProto::ListConnectionsResponse* response) {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "response is null");
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  *response = impl_->core.ListConnections();
  return grpc::Status::OK;
}

grpc::Status DataCenterGrpcServiceImpl::GetOrCreateConnection(grpc::ServerContext*, const DataCenterProto::GetOrCreateConnectionRequest* request, DataCenterProto::ConnectionInfo* response) {
  if (request == nullptr || response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request/response is null");
  }

  std::lock_guard<std::mutex> lock(impl_->mu);
  auto status = impl_->core.GetOrCreateConnection(*request, response);
  if (!status.ok()) {
    return status;
  }
  auto config = impl_->core.DumpConnectionsConfig();
  status = impl_->connectionStore.Save(config);
  if (!status.ok()) {
    const auto message = status.error_message();
    LOG_INFO("DataCenter 连接注册表落盘失败: {}", message);
  }
  return status;
}

grpc::Status DataCenterGrpcServiceImpl::RenameConnection(grpc::ServerContext*, const DataCenterProto::RenameConnectionRequest* request, DataCenterProto::ConnectionInfo* response) {
  if (request == nullptr || response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request/response is null");
  }

  std::lock_guard<std::mutex> lock(impl_->mu);
  auto status = impl_->core.RenameConnection(*request, response);
  if (!status.ok()) {
    return status;
  }
  auto config = impl_->core.DumpConnectionsConfig();
  status = impl_->connectionStore.Save(config);
  if (!status.ok()) {
    const auto message = status.error_message();
    LOG_INFO("DataCenter 连接注册表落盘失败: {}", message);
  }
  return status;
}

grpc::Status DataCenterGrpcServiceImpl::DeleteConnection(grpc::ServerContext*, const DataCenterProto::DeleteConnectionRequest* request, DataCenterProto::Empty*) {
  if (request == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request is null");
  }

  std::lock_guard<std::mutex> lock(impl_->mu);

  DataCenterProto::ConnectionInfo conn;
  auto status = impl_->core.GetConnectionByKey(request->key(), &conn);
  if (!status.ok()) {
    return status;
  }

  status = impl_->core.DeleteConnection(*request);
  if (!status.ok()) {
    return status;
  }

  impl_->closeSubscribersLocked(conn.conn_id());

  auto connConfig = impl_->core.DumpConnectionsConfig();
  status = impl_->connectionStore.Save(connConfig);
  if (!status.ok()) {
    const auto message = status.error_message();
    LOG_INFO("DataCenter 连接注册表落盘失败: {}", message);
    return status;
  }

  auto ptConfig = impl_->core.DumpPointTablesConfig();
  status = impl_->pointTableStore.Save(ptConfig);
  if (!status.ok()) {
    const auto message = status.error_message();
    LOG_INFO("DataCenter 点表落盘失败: {}", message);
    return status;
  }

  auto routesConfig = impl_->core.DumpRoutesConfig();
  status = impl_->routeStore.Save(routesConfig);
  if (!status.ok()) {
    const auto message = status.error_message();
    LOG_INFO("DataCenter 路由落盘失败: {}", message);
    return status;
  }
  return grpc::Status::OK;
}

grpc::Status DataCenterGrpcServiceImpl::UpsertPointTable(grpc::ServerContext*, const DataCenterProto::UpsertPointTableRequest* request, DataCenterProto::Empty*) {
  if (request == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request is null");
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  auto status = impl_->core.UpsertPointTable(*request);
  if (!status.ok()) {
    return status;
  }
  auto config = impl_->core.DumpPointTablesConfig();
  status = impl_->pointTableStore.Save(config);
  if (!status.ok()) {
    const auto message = status.error_message();
    LOG_INFO("DataCenter 点表落盘失败: {}", message);
  }
  return status;
}

grpc::Status DataCenterGrpcServiceImpl::GetPointTable(grpc::ServerContext*, const DataCenterProto::GetPointTableRequest* request, DataCenterProto::PointTable* response) {
  if (request == nullptr || response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request/response is null");
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->core.GetPointTable(request->conn_id(), response);
}

grpc::Status DataCenterGrpcServiceImpl::UpsertRoutes(grpc::ServerContext*, const DataCenterProto::UpsertRoutesRequest* request, DataCenterProto::Empty*) {
  if (request == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request is null");
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  auto status = impl_->core.UpsertRoutes(*request);
  if (!status.ok()) {
    return status;
  }
  auto config = impl_->core.DumpRoutesConfig();
  status = impl_->routeStore.Save(config);
  if (!status.ok()) {
    const auto message = status.error_message();
    LOG_INFO("DataCenter 路由落盘失败: {}", message);
  }
  return status;
}

grpc::Status DataCenterGrpcServiceImpl::DeleteRoutes(grpc::ServerContext*, const DataCenterProto::DeleteRoutesRequest* request, DataCenterProto::Empty*) {
  if (request == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request is null");
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  auto status = impl_->core.DeleteRoutes(*request);
  if (!status.ok()) {
    return status;
  }
  auto config = impl_->core.DumpRoutesConfig();
  status = impl_->routeStore.Save(config);
  if (!status.ok()) {
    const auto message = status.error_message();
    LOG_INFO("DataCenter 路由落盘失败: {}", message);
  }
  return status;
}

grpc::Status DataCenterGrpcServiceImpl::ListRoutes(grpc::ServerContext*, const DataCenterProto::ListRoutesRequest* request, DataCenterProto::ListRoutesResponse* response) {
  if (request == nullptr || response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request/response is null");
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  *response = impl_->core.ListRoutes(*request);
  return grpc::Status::OK;
}

grpc::Status DataCenterGrpcServiceImpl::Publish(grpc::ServerContext*, const DataCenterProto::PublishRequest* request, DataCenterProto::Empty*) {
  if (request == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request is null");
  }

  std::vector<DataCenterProto::PointUpdate> updates;
  std::vector<Impl::Delivery> deliveries;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto status = impl_->core.Publish(*request, &updates);
    if (!status.ok()) {
      return status;
    }
    deliveries.reserve(updates.size());
    for (const auto& update : updates) {
      auto subs = impl_->matchSubscribersLocked(update.dst_conn_id(), update.dst_tag());
      if (subs.empty()) {
        continue;
      }
      deliveries.emplace_back(Impl::Delivery{.update = &update, .subscribers = std::move(subs)});
    }
  }

  for (const auto& delivery : deliveries) {
    for (const auto& sub : delivery.subscribers) {
      Impl::enqueue(sub, *delivery.update);
    }
  }
  return grpc::Status::OK;
}

grpc::Status DataCenterGrpcServiceImpl::BatchPublish(grpc::ServerContext*, const DataCenterProto::BatchPublishRequest* request, DataCenterProto::Empty*) {
  if (request == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request is null");
  }

  std::vector<DataCenterProto::PointUpdate> updates;
  std::vector<Impl::Delivery> deliveries;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto status = impl_->core.BatchPublish(*request, &updates);
    if (!status.ok()) {
      return status;
    }
    deliveries.reserve(updates.size());
    for (const auto& update : updates) {
      auto subs = impl_->matchSubscribersLocked(update.dst_conn_id(), update.dst_tag());
      if (subs.empty()) {
        continue;
      }
      deliveries.emplace_back(Impl::Delivery{.update = &update, .subscribers = std::move(subs)});
    }
  }

  for (const auto& delivery : deliveries) {
    for (const auto& sub : delivery.subscribers) {
      Impl::enqueue(sub, *delivery.update);
    }
  }
  return grpc::Status::OK;
}

grpc::Status DataCenterGrpcServiceImpl::GetLatest(grpc::ServerContext*, const DataCenterProto::GetLatestRequest* request, DataCenterProto::GetLatestResponse* response) {
  if (request == nullptr || response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request/response is null");
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->core.GetLatest(*request, response);
}

grpc::Status DataCenterGrpcServiceImpl::Subscribe(grpc::ServerContext* context, const DataCenterProto::SubscribeRequest* request, grpc::ServerWriter<DataCenterProto::PointUpdate>* writer) {
  if (context == nullptr || request == nullptr || writer == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "context/request/writer is null");
  }
  if (request->conn_id() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id is required");
  }

  auto subscriber = std::make_shared<Impl::Subscriber>();
  subscriber->connId = request->conn_id();
  for (const auto& tag : request->tags()) {
    if (tag.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tags contains empty string");
    }
    subscriber->tagsFilter.emplace(tag);
  }

  std::vector<DataCenterProto::PointUpdate> snapshot;
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
        return status;
      }
      snapshot.reserve(static_cast<size_t>(latestResp.updates_size()));
      for (const auto& update : latestResp.updates()) {
        snapshot.emplace_back(update);
      }
    }
  }

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
      subscriber->cv.wait_for(lock, std::chrono::milliseconds(200), [&subscriber]() { return subscriber->closed || !subscriber->queue.empty(); });
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
  return grpc::Status::OK;
}
}  // namespace DataCenter
