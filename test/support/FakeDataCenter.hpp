#pragma once

#include <gmock/gmock.h>

#include <cstdint>
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
  class FakePointUpdateReader : public grpc::ClientReaderInterface<DataCenterProto::PointUpdate> {
  public:
    explicit FakePointUpdateReader(std::vector<DataCenterProto::PointUpdate> updates)
        : updates_(std::move(updates)) {}

    bool Read(DataCenterProto::PointUpdate* msg) override {
      if (msg == nullptr) {
        return false;
      }
      if (index_ >= updates_.size()) {
        return false;
      }
      *msg = updates_[index_++];
      return true;
    }

    bool NextMessageSize(uint32_t* sz) override {
      if (sz == nullptr) {
        return false;
      }
      if (index_ >= updates_.size()) {
        *sz = 0;
        return false;
      }
      *sz = static_cast<uint32_t>(updates_[index_].ByteSizeLong());
      return true;
    }

    void WaitForInitialMetadata() override {}

    grpc::Status Finish() override { return grpc::Status::OK; }

  private:
    std::vector<DataCenterProto::PointUpdate> updates_;
    size_t index_{0};
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
    for (const auto& route : request.routes()) {
      if (!route.has_src() || !route.has_dst()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "route src/dst is required");
      }
      if (route.src().conn_id() == 0 || route.dst().conn_id() == 0) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id is required");
      }
      if (route.src().tag().empty() || route.dst().tag().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tag is required");
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

    std::lock_guard<std::mutex> lock(mu_);
    latestByConnId_[request.conn_id()][request.tag()] = update;
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
    std::vector<DataCenterProto::PointUpdate> updates;
    if (request.snapshot()) {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = latestByConnId_.find(request.conn_id());
      if (it != latestByConnId_.end()) {
        if (request.tags().empty()) {
          for (const auto& [_, update] : it->second) {
            updates.push_back(update);
          }
        } else {
          for (const auto& tag : request.tags()) {
            auto tagIt = it->second.find(tag);
            if (tagIt != it->second.end()) {
              updates.push_back(tagIt->second);
            }
          }
        }
      }
    }
    return std::make_unique<FakePointUpdateReader>(std::move(updates));
  }

private:
  mutable std::mutex mu_;
  uint32_t nextConnId_ = 1;
  std::unordered_map<ConnKey, DataCenterProto::ConnectionInfo, ConnKeyHash> conns_;
  std::unordered_set<std::string> failDeleteConnNames_;
  std::unordered_set<std::string> failPublishTags_;
  std::unordered_set<uint32_t> failGetLatestConnIds_;
  std::unordered_map<uint32_t, std::unordered_map<std::string, DataCenterProto::PointUpdate>> latestByConnId_;
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

  ON_CALL(*stub, DeleteConnection(::testing::_, ::testing::_, ::testing::_))
      .WillByDefault(::testing::Invoke([state](grpc::ClientContext*, const DataCenterProto::DeleteConnectionRequest& req, DataCenterProto::Empty*) {
        return state->DeleteConnection(req);
      }));

  ON_CALL(*stub, UpsertPointTable(::testing::_, ::testing::_, ::testing::_))
      .WillByDefault(::testing::Invoke([](grpc::ClientContext*, const DataCenterProto::UpsertPointTableRequest& req, DataCenterProto::Empty*) {
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
