#pragma once

#include <gmock/gmock.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>

#include "DataCenter_mock.grpc.pb.h"

struct ConnKey {
  std::string module;
  std::string conn;

  bool operator==(const ConnKey& other) const { return module == other.module && conn == other.conn; }
};

struct ConnKeyHash {
  std::size_t operator()(const ConnKey& k) const noexcept {
    std::hash<std::string> h;
    return h(k.module) ^ (h(k.conn) << 1);
  }
};

class FakeDataCenterState {
public:
  static constexpr auto kSubscriptionIdleTimeout = std::chrono::milliseconds(200);

  struct SubscriptionState {
    explicit SubscriptionState(uint32_t connIdIn, std::vector<std::string> tagsIn) :
        connId(connIdIn),
        tags(std::move(tagsIn)) {}

    bool Matches(const std::string& tag) const {
      return tags.empty() || std::find(tags.begin(), tags.end(), tag) != tags.end();
    }

    uint32_t connId{0};
    std::vector<std::string> tags;
    mutable std::mutex mu;
    std::condition_variable cv;
    std::deque<DataCenterProto::PointUpdate> queue;
    bool closed{false};
  };

  class FakePointUpdateReader : public grpc::ClientReaderInterface<DataCenterProto::PointUpdate> {
  public:
    explicit FakePointUpdateReader(std::shared_ptr<SubscriptionState> subscription)
        : subscription_(std::move(subscription)) {}

    bool Read(DataCenterProto::PointUpdate* msg) override {
      if (msg == nullptr) {
        return false;
      }
      std::unique_lock<std::mutex> lock(subscription_->mu);
      while (subscription_->queue.empty() && !subscription_->closed) {
        if (subscription_->cv.wait_for(lock, kSubscriptionIdleTimeout) == std::cv_status::timeout) {
          subscription_->closed = true;
          return false;
        }
      }
      if (subscription_->queue.empty()) {
        return false;
      }
      *msg = std::move(subscription_->queue.front());
      subscription_->queue.pop_front();
      return true;
    }

    bool NextMessageSize(uint32_t* sz) override {
      if (sz == nullptr) {
        return false;
      }
      std::lock_guard<std::mutex> lock(subscription_->mu);
      if (subscription_->queue.empty()) {
        *sz = 0;
        return false;
      }
      *sz = static_cast<uint32_t>(subscription_->queue.front().ByteSizeLong());
      return true;
    }

    void WaitForInitialMetadata() override {}

    grpc::Status Finish() override {
      std::lock_guard<std::mutex> lock(subscription_->mu);
      subscription_->closed = true;
      subscription_->cv.notify_all();
      return grpc::Status::OK;
    }

  private:
    std::shared_ptr<SubscriptionState> subscription_;
  };

  void AddConnection(uint32_t connId, std::string module, std::string conn) {
    std::lock_guard<std::mutex> lock(mu_);
    ConnKey key{.module = std::move(module), .conn = std::move(conn)};
    DataCenterProto::ConnectionInfo info;
    info.set_conn_id(connId);
    info.set_module_name(key.module);
    info.set_conn_name(key.conn);
    conns_[key] = info;
  }

  void RemoveConnection(const std::string& module, const std::string& conn) {
    std::lock_guard<std::mutex> lock(mu_);
    conns_.erase(ConnKey{module, conn});
  }

  void SetNextConnId(uint32_t nextConnId) {
    std::lock_guard<std::mutex> lock(mu_);
    nextConnId_ = (nextConnId == 0) ? 1 : nextConnId;
  }

  void FailDeleteForConnName(std::string connName) {
    std::lock_guard<std::mutex> lock(mu_);
    failDeleteConnNames_.emplace(std::move(connName));
  }

  void FailPublishForTag(std::string tag) {
    std::lock_guard<std::mutex> lock(mu_);
    failPublishTags_.emplace(std::move(tag));
  }

  void FailGetLatestForConn(uint32_t connId) {
    std::lock_guard<std::mutex> lock(mu_);
    failGetLatestConnIds_.emplace(connId);
  }

  bool HasConnection(const std::string& module, const std::string& conn) const {
    std::lock_guard<std::mutex> lock(mu_);
    return conns_.contains(ConnKey{module, conn});
  }

  size_t GetPublishCount(uint32_t connId, const std::string& tag) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto connIt = publishCountByConnId_.find(connId);
    if (connIt == publishCountByConnId_.end()) {
      return 0;
    }
    auto tagIt = connIt->second.find(tag);
    if (tagIt == connIt->second.end()) {
      return 0;
    }
    return tagIt->second;
  }

  size_t GetSubscriptionCount(uint32_t connId) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = subscriptionsByConnId_.find(connId);
    if (it == subscriptionsByConnId_.end()) {
      return 0;
    }
    size_t active = 0;
    for (const auto& weak : it->second) {
      if (!weak.expired()) {
        ++active;
      }
    }
    return active;
  }

  grpc::Status ListConnections(DataCenterProto::ListConnectionsResponse* response) const {
    if (response == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "response is null");
    }
    std::lock_guard<std::mutex> lock(mu_);
    response->Clear();
    for (const auto& [_, info] : conns_) {
      *response->add_conns() = info;
    }
    return grpc::Status::OK;
  }

  grpc::Status GetOrCreateConnection(
      const DataCenterProto::GetOrCreateConnectionRequest& request, DataCenterProto::ConnectionInfo* response) {
    if (response == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "response is null");
    }
    if (!request.has_key() || request.key().module_name().empty() || request.key().conn_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "key.module_name/conn_name is required");
    }

    std::lock_guard<std::mutex> lock(mu_);
    ConnKey key{request.key().module_name(), request.key().conn_name()};
    auto it = conns_.find(key);
    if (it != conns_.end()) {
      response->CopyFrom(it->second);
      return grpc::Status::OK;
    }

    auto connId = nextConnId_++;
    DataCenterProto::ConnectionInfo info;
    info.set_conn_id(connId);
    info.set_module_name(key.module);
    info.set_conn_name(key.conn);
    conns_[key] = info;
    response->CopyFrom(info);
    return grpc::Status::OK;
  }

  grpc::Status RenameConnection(const DataCenterProto::RenameConnectionRequest& request,
                                DataCenterProto::ConnectionInfo* response) {
    if (response == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
    }
    if (!request.has_old_key() || request.old_key().module_name().empty() || request.old_key().conn_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "old_key.module_name/conn_name 不能为空");
    }
    if (!request.has_new_key() || request.new_key().module_name().empty() || request.new_key().conn_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "new_key.module_name/conn_name 不能为空");
    }

    std::lock_guard<std::mutex> lock(mu_);
    ConnKey oldKey{request.old_key().module_name(), request.old_key().conn_name()};
    auto it = conns_.find(oldKey);
    if (it == conns_.end()) {
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接不存在");
    }

    ConnKey newKey{request.new_key().module_name(), request.new_key().conn_name()};
    if (oldKey == newKey) {
      response->CopyFrom(it->second);
      return grpc::Status::OK;
    }

    auto newIt = conns_.find(newKey);
    if (newIt != conns_.end() && newIt->second.conn_id() != it->second.conn_id()) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "连接已存在");
    }

    auto info = it->second;
    conns_.erase(it);
    info.set_module_name(newKey.module);
    info.set_conn_name(newKey.conn);
    conns_[newKey] = info;
    response->CopyFrom(info);
    return grpc::Status::OK;
  }

  grpc::Status DeleteConnection(const DataCenterProto::DeleteConnectionRequest& request) {
    if (!request.has_key() || request.key().module_name().empty() || request.key().conn_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "key.module_name/conn_name is required");
    }

    std::lock_guard<std::mutex> lock(mu_);
    if (failDeleteConnNames_.contains(request.key().conn_name())) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "forced delete failure");
    }

    ConnKey key{request.key().module_name(), request.key().conn_name()};
    auto it = conns_.find(key);
    if (it == conns_.end()) {
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "connection not found");
    }
    conns_.erase(it);
    return grpc::Status::OK;
  }

  grpc::Status UpsertRoutes(const DataCenterProto::UpsertRoutesRequest& request) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::unordered_map<uint32_t, ConnKey> keysByConnId;
    for (const auto& [key, conn] : conns_) {
      keysByConnId.emplace(conn.conn_id(), key);
    }
    const auto resolveEndpoint = [&](const DataCenterProto::Endpoint& endpoint,
                                     const char* direction) -> grpc::Status {
      if (endpoint.tag().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tag 不能为空");
      }
      if (endpoint.module_name().empty() != endpoint.conn_name().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::string(direction) + " 稳定连接主键不完整");
      }
      const bool hasStableKey = !endpoint.module_name().empty() && !endpoint.conn_name().empty();
      if (!hasStableKey) {
        if (endpoint.conn_id() == 0) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id 不能为空");
        }
        if (!keysByConnId.contains(endpoint.conn_id())) {
          return grpc::Status(grpc::StatusCode::NOT_FOUND, std::string(direction) + " conn_id 未找到");
        }
        return grpc::Status::OK;
      }

      if (endpoint.conn_id() == 0) {
        return grpc::Status::OK;
      }
      auto connIt = keysByConnId.find(endpoint.conn_id());
      if (connIt != keysByConnId.end() &&
          (connIt->second.module != endpoint.module_name() || connIt->second.conn != endpoint.conn_name())) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::string(direction) + " conn_id 与稳定连接主键不匹配");
      }
      return grpc::Status::OK;
    };
    for (const auto& route : request.routes()) {
      if (!route.has_src() || !route.has_dst()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "route src/dst is required");
      }
      auto status = resolveEndpoint(route.src(), "src");
      if (!status.ok()) {
        return status;
      }
      status = resolveEndpoint(route.dst(), "dst");
      if (!status.ok()) {
        return status;
      }
    }
    return grpc::Status::OK;
  }

  grpc::Status Publish(const DataCenterProto::PublishRequest& request) {
    if (request.conn_id() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id 不能为空");
    }
    if (request.tag().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tag 不能为空");
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (failPublishTags_.contains(request.tag())) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "强制发布失败");
      }
    }

    DataCenterProto::PointUpdate update;
    update.set_src_conn_id(request.conn_id());
    update.set_dst_conn_id(request.conn_id());
    update.set_src_tag(request.tag());
    update.set_dst_tag(request.tag());
    update.mutable_value()->CopyFrom(request.value());
    update.set_ts_ms(request.ts_ms());
    update.set_quality(request.quality());

    std::vector<std::shared_ptr<SubscriptionState>> subscriptions;
    {
      std::lock_guard<std::mutex> lock(mu_);
      latestByConnId_[request.conn_id()][request.tag()] = update;
      publishCountByConnId_[request.conn_id()][request.tag()] += 1;
      auto& watchers = subscriptionsByConnId_[request.conn_id()];
      for (auto it = watchers.begin(); it != watchers.end();) {
        if (auto sub = it->lock()) {
          subscriptions.push_back(std::move(sub));
          ++it;
        } else {
          it = watchers.erase(it);
        }
      }
    }
    for (const auto& sub : subscriptions) {
      if (!sub->Matches(request.tag())) {
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(sub->mu);
        if (sub->closed) {
          continue;
        }
        sub->queue.push_back(update);
      }
      sub->cv.notify_one();
    }
    return grpc::Status::OK;
  }

  grpc::Status GetLatest(const DataCenterProto::GetLatestRequest& request,
                         DataCenterProto::GetLatestResponse* response) const {
    if (response == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "response 为空");
    }
    if (request.conn_id() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id 不能为空");
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (failGetLatestConnIds_.contains(request.conn_id())) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "强制获取最新值失败");
      }
    }
    response->Clear();
    std::lock_guard<std::mutex> lock(mu_);
    auto it = latestByConnId_.find(request.conn_id());
    if (it == latestByConnId_.end()) {
      return grpc::Status::OK;
    }
    if (request.tags().empty()) {
      for (const auto& [_, update] : it->second) {
        *response->add_updates() = update;
      }
      return grpc::Status::OK;
    }
    for (const auto& tag : request.tags()) {
      auto tagIt = it->second.find(tag);
      if (tagIt != it->second.end()) {
        *response->add_updates() = tagIt->second;
      }
    }
    return grpc::Status::OK;
  }

  std::unique_ptr<grpc::ClientReaderInterface<DataCenterProto::PointUpdate>> Subscribe(
      const DataCenterProto::SubscribeRequest& request) const {
    if (request.conn_id() == 0) {
      return nullptr;
    }
    auto subscription = std::make_shared<SubscriptionState>(
        request.conn_id(),
        std::vector<std::string>(request.tags().begin(), request.tags().end()));
    if (request.snapshot()) {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = latestByConnId_.find(request.conn_id());
      if (it != latestByConnId_.end()) {
        if (request.tags().empty()) {
          for (const auto& [_, update] : it->second) {
            subscription->queue.push_back(update);
          }
        } else {
          for (const auto& tag : request.tags()) {
            auto tagIt = it->second.find(tag);
            if (tagIt != it->second.end()) {
              subscription->queue.push_back(tagIt->second);
            }
          }
        }
      }
      subscriptionsByConnId_[request.conn_id()].push_back(subscription);
    }
    if (!request.snapshot()) {
      std::lock_guard<std::mutex> lock(mu_);
      subscriptionsByConnId_[request.conn_id()].push_back(subscription);
    }
    return std::make_unique<FakePointUpdateReader>(std::move(subscription));
  }

private:
  mutable std::mutex mu_;
  uint32_t nextConnId_ = 1;
  std::unordered_map<ConnKey, DataCenterProto::ConnectionInfo, ConnKeyHash> conns_;
  std::unordered_set<std::string> failDeleteConnNames_;
  std::unordered_set<std::string> failPublishTags_;
  std::unordered_set<uint32_t> failGetLatestConnIds_;
  std::unordered_map<uint32_t, std::unordered_map<std::string, DataCenterProto::PointUpdate>> latestByConnId_;
  std::unordered_map<uint32_t, std::unordered_map<std::string, size_t>> publishCountByConnId_;
  mutable std::unordered_map<uint32_t, std::vector<std::weak_ptr<SubscriptionState>>> subscriptionsByConnId_;
};

inline std::shared_ptr<DataCenterProto::MockDataCenterServiceStub> MakeStub(FakeDataCenterState* state) {
  auto stub = std::make_shared<DataCenterProto::MockDataCenterServiceStub>();

  ON_CALL(*stub, ListConnections(::testing::_, ::testing::_, ::testing::_))
      .WillByDefault(::testing::Invoke([state](grpc::ClientContext*, const DataCenterProto::Empty&, DataCenterProto::ListConnectionsResponse* resp) {
        return state->ListConnections(resp);
      }));

  ON_CALL(*stub, GetOrCreateConnection(::testing::_, ::testing::_, ::testing::_))
      .WillByDefault(::testing::Invoke([state](grpc::ClientContext*, const DataCenterProto::GetOrCreateConnectionRequest& req, DataCenterProto::ConnectionInfo* resp) {
        return state->GetOrCreateConnection(req, resp);
      }));

  ON_CALL(*stub, RenameConnection(::testing::_, ::testing::_, ::testing::_))
      .WillByDefault(::testing::Invoke([state](grpc::ClientContext*,
                                               const DataCenterProto::RenameConnectionRequest& req,
                                               DataCenterProto::ConnectionInfo* resp) {
        return state->RenameConnection(req, resp);
      }));

  ON_CALL(*stub, DeleteConnection(::testing::_, ::testing::_, ::testing::_))
      .WillByDefault(::testing::Invoke([state](grpc::ClientContext*, const DataCenterProto::DeleteConnectionRequest& req, DataCenterProto::Empty*) {
        return state->DeleteConnection(req);
      }));

  ON_CALL(*stub, UpsertConnTags(::testing::_, ::testing::_, ::testing::_))
      .WillByDefault(::testing::Invoke([](grpc::ClientContext*, const DataCenterProto::UpsertConnTagsRequest& req, DataCenterProto::Empty*) {
        if (req.conn_id() == 0) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id is required");
        }
        for (const auto& tag : req.tags()) {
          if (tag.empty()) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tags contains empty string");
          }
        }
        return grpc::Status::OK;
      }));

  ON_CALL(*stub, UpsertRoutes(::testing::_, ::testing::_, ::testing::_))
      .WillByDefault(::testing::Invoke([state](grpc::ClientContext*, const DataCenterProto::UpsertRoutesRequest& req, DataCenterProto::Empty*) {
        return state->UpsertRoutes(req);
      }));

  ON_CALL(*stub, Publish(::testing::_, ::testing::_, ::testing::_))
      .WillByDefault(::testing::Invoke([state](grpc::ClientContext*, const DataCenterProto::PublishRequest& req, DataCenterProto::Empty*) {
        return state->Publish(req);
      }));

  ON_CALL(*stub, GetLatest(::testing::_, ::testing::_, ::testing::_))
      .WillByDefault(::testing::Invoke([state](grpc::ClientContext*, const DataCenterProto::GetLatestRequest& req, DataCenterProto::GetLatestResponse* resp) {
        return state->GetLatest(req, resp);
      }));

  ON_CALL(*stub, SubscribeRaw(::testing::_, ::testing::_))
      .WillByDefault(::testing::Invoke([state](grpc::ClientContext*, const DataCenterProto::SubscribeRequest& req) {
        return state->Subscribe(req).release();
      }));

  return stub;
}
