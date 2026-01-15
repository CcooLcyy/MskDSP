#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "AGCGroupManager.h"
#include "DataCenter_mock.grpc.pb.h"

namespace {
using AGC::GroupManager;

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

AGCProto::UpsertGroupRequest MakeGroupReq(const char* groupName) {
  AGCProto::UpsertGroupRequest req;
  req.set_create_only(true);
  auto* cfg = req.mutable_config();
  cfg->set_group_name(groupName);
  cfg->mutable_p_cmd()->mutable_signal()->set_tag("P_CMD");
  cfg->mutable_p_cmd()->mutable_signal()->set_unit("kW");
  cfg->mutable_p_cmd()->set_mode(AGCProto::VALUE_MODE_ABSOLUTE);

  cfg->mutable_outputs()->mutable_p_total_meas()->set_tag("P_TOTAL");
  cfg->mutable_outputs()->mutable_p_total_meas()->set_unit("kW");

  auto* inv1 = cfg->add_members();
  inv1->set_member_name("inv-1");
  inv1->set_controllable(true);
  inv1->set_capacity_kw(50);
  inv1->set_weight(50);
  inv1->mutable_p_meas()->set_tag("INV1_P_MEAS");
  inv1->mutable_p_meas()->set_unit("kW");
  inv1->mutable_p_set()->mutable_signal()->set_tag("INV1_P_SET");
  inv1->mutable_p_set()->mutable_signal()->set_unit("kW");
  inv1->mutable_p_set()->set_mode(AGCProto::VALUE_MODE_ABSOLUTE);

  auto* inv2 = cfg->add_members();
  inv2->set_member_name("inv-2");
  inv2->set_controllable(true);
  inv2->set_capacity_kw(100);
  inv2->set_weight(100);
  inv2->mutable_p_meas()->set_tag("INV2_P_MEAS");
  inv2->mutable_p_meas()->set_unit("kW");
  inv2->mutable_p_set()->mutable_signal()->set_tag("INV2_P_SET");
  inv2->mutable_p_set()->mutable_signal()->set_unit("kW");
  inv2->mutable_p_set()->set_mode(AGCProto::VALUE_MODE_ABSOLUTE);

  return req;
}
}  // namespace

// 验证：create_only UpsertGroup 会向 DataCenter 取/建 conn_id，并回填到 GroupInfo。
TEST(AgcGroupManagerTest, UpsertGroupCreateOnlyReturnsConnId) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-1");

  AGCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  EXPECT_NE(info.conn_id(), 0u);
  EXPECT_EQ(info.state(), AGCProto::GROUP_STATE_STOPPED);
  EXPECT_EQ(info.config().group_name(), "g-1");
  EXPECT_TRUE(state.HasConnection("AGC", "g-1"));
}

// 验证：当 DataCenter 已存在相同 (module_name, conn_name) 时，create_only UpsertGroup 返回 ALREADY_EXISTS。
TEST(AgcGroupManagerTest, UpsertGroupCreateOnlyRejectsWhenDataCenterAlreadyHasKey) {
  FakeDataCenterState state;
  state.AddConnection(42, "AGC", "dup");
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("dup");

  AGCProto::GroupInfo info;
  auto st = mgr.UpsertGroup(req, &info);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}

// 验证：DeleteGroup 会调用 DataCenter.DeleteConnection，并移除本地 group 配置。
TEST(AgcGroupManagerTest, DeleteGroupCallsDataCenterDeleteAndRemovesLocal) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-del");

  AGCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  ASSERT_TRUE(state.HasConnection("AGC", "g-del"));

  ASSERT_TRUE(mgr.DeleteGroup("g-del").ok());
  EXPECT_FALSE(state.HasConnection("AGC", "g-del"));

  AGCProto::GroupInfo got;
  auto st = mgr.GetGroup("g-del", &got);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}

// 验证：当 DataCenter 删除失败时，DeleteGroup 标记 PENDING_DELETE 且保留本地配置以便重试。
TEST(AgcGroupManagerTest, DeleteGroupFailureMarksPendingDeleteAndKeepsLocal) {
  FakeDataCenterState state;
  state.FailDeleteForConnName("g-fail");
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-fail");

  AGCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());

  auto st = mgr.DeleteGroup("g-fail");
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);

  AGCProto::GroupInfo got;
  ASSERT_TRUE(mgr.GetGroup("g-fail", &got).ok());
  EXPECT_EQ(got.state(), AGCProto::GROUP_STATE_PENDING_DELETE);
  EXPECT_FALSE(got.last_error().empty());
}

// 验证：当 ValueSpec 使用 BASE_TAG 时，UpsertGroup 会把 base_tag 一并注册到 DataCenter 点表。
TEST(AgcGroupManagerTest, UpsertGroupRegistersBaseTagToDataCenterPointTable) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  EXPECT_CALL(*stub, UpsertPointTable(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*, const DataCenterProto::UpsertPointTableRequest& req, DataCenterProto::Empty*) {
        EXPECT_NE(req.conn_id(), 0u);
        EXPECT_TRUE(req.replace());

        std::unordered_set<std::string> tags;
        for (const auto& t : req.tags()) {
          tags.emplace(t);
        }
        EXPECT_TRUE(tags.contains("P_CMD"));
        EXPECT_TRUE(tags.contains("P_BASE"));
        EXPECT_TRUE(tags.contains("INV1_P_MEAS"));
        EXPECT_TRUE(tags.contains("INV1_P_SET"));
        EXPECT_TRUE(tags.contains("INV2_P_MEAS"));
        EXPECT_TRUE(tags.contains("INV2_P_SET"));
        EXPECT_TRUE(tags.contains("P_TOTAL"));
        return grpc::Status::OK;
      }));

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-base");
  req.mutable_config()->mutable_p_cmd()->set_mode(AGCProto::VALUE_MODE_DELTA);
  req.mutable_config()->mutable_p_cmd()->set_delta_base(AGCProto::DELTA_BASE_BASE_TAG);
  req.mutable_config()->mutable_p_cmd()->set_base_tag("P_BASE");

  AGCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
}
