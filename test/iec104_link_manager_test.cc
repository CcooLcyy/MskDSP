#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "DataCenter_mock.grpc.pb.h"
#include "IEC104LinkManager.h"

namespace {
using IEC104::LinkManager;

using ::testing::_;
using ::testing::Invoke;

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

private:
  mutable std::mutex mu_;
  uint32_t nextConnId_ = 1;
  std::unordered_map<ConnKey, DataCenterProto::ConnectionInfo, ConnKeyHash> conns_;
  std::unordered_set<std::string> failDeleteConnNames_;
};

std::shared_ptr<DataCenterProto::MockDataCenterServiceStub> MakeStub(FakeDataCenterState* state) {
  auto stub = std::make_shared<DataCenterProto::MockDataCenterServiceStub>();

  ON_CALL(*stub, ListConnections(_, _, _))
      .WillByDefault(Invoke([state](grpc::ClientContext*, const DataCenterProto::Empty&, DataCenterProto::ListConnectionsResponse* resp) {
        return state->ListConnections(resp);
      }));

  ON_CALL(*stub, GetOrCreateConnection(_, _, _))
      .WillByDefault(Invoke([state](grpc::ClientContext*, const DataCenterProto::GetOrCreateConnectionRequest& req, DataCenterProto::ConnectionInfo* resp) {
        return state->GetOrCreateConnection(req, resp);
      }));

  ON_CALL(*stub, DeleteConnection(_, _, _))
      .WillByDefault(Invoke([state](grpc::ClientContext*, const DataCenterProto::DeleteConnectionRequest& req, DataCenterProto::Empty*) {
        return state->DeleteConnection(req);
      }));

  ON_CALL(*stub, UpsertPointTable(_, _, _))
      .WillByDefault(Invoke([](grpc::ClientContext*, const DataCenterProto::UpsertPointTableRequest& req, DataCenterProto::Empty*) {
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

  return stub;
}

IEC104Proto::UpsertLinkRequest MakeClientLinkReq(const char* connName) {
  IEC104Proto::UpsertLinkRequest req;
  auto* cfg = req.mutable_config();
  cfg->set_conn_name(connName);
  cfg->set_role(IEC104Proto::ROLE_CLIENT);
  cfg->mutable_remote()->set_ip("127.0.0.1");
  cfg->mutable_remote()->set_port(2404);
  cfg->set_ca(1);
  cfg->set_oa(1);
  req.set_create_only(true);
  return req;
}
}  // namespace

TEST(IEC104LinkManagerTest, UpsertLinkCreateOnlyReturnsConnId) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);
  auto req = MakeClientLinkReq("conn-1");

  IEC104Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());
  EXPECT_NE(info.conn_id(), 0u);
  EXPECT_EQ(info.state(), IEC104Proto::LINK_STATE_STOPPED);
  EXPECT_EQ(info.config().conn_name(), "conn-1");
  EXPECT_TRUE(state.HasConnection("IEC104", "conn-1"));
}

TEST(IEC104LinkManagerTest, UpsertLinkCreateOnlyRejectsWhenDataCenterAlreadyHasKey) {
  FakeDataCenterState state;
  state.AddConnection(42, "IEC104", "dup");
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);
  auto req = MakeClientLinkReq("dup");

  IEC104Proto::LinkInfo info;
  auto st = mgr.UpsertLink(req, &info);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}

TEST(IEC104LinkManagerTest, DeleteLinkCallsDataCenterDeleteAndRemovesLocal) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);
  auto req = MakeClientLinkReq("conn-del");

  IEC104Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());
  ASSERT_TRUE(state.HasConnection("IEC104", "conn-del"));

  ASSERT_TRUE(mgr.DeleteLink("conn-del").ok());
  EXPECT_FALSE(state.HasConnection("IEC104", "conn-del"));

  IEC104Proto::LinkInfo got;
  auto st = mgr.GetLink("conn-del", &got);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}

TEST(IEC104LinkManagerTest, DeleteLinkFailureMarksPendingDeleteAndKeepsLocal) {
  FakeDataCenterState state;
  state.FailDeleteForConnName("conn-fail");
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);
  auto req = MakeClientLinkReq("conn-fail");

  IEC104Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());

  auto st = mgr.DeleteLink("conn-fail");
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);

  IEC104Proto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-fail", &got).ok());
  EXPECT_EQ(got.state(), IEC104Proto::LINK_STATE_PENDING_DELETE);
}

TEST(IEC104LinkManagerTest, DeleteLinkTreatsDataCenterNotFoundAsSuccess) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);
  auto req = MakeClientLinkReq("conn-nf");

  IEC104Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());
  ASSERT_TRUE(state.HasConnection("IEC104", "conn-nf"));

  state.RemoveConnection("IEC104", "conn-nf");
  ASSERT_FALSE(state.HasConnection("IEC104", "conn-nf"));

  ASSERT_TRUE(mgr.DeleteLink("conn-nf").ok());
  IEC104Proto::LinkInfo got;
  auto st = mgr.GetLink("conn-nf", &got);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}

