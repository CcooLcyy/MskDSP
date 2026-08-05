#include <cstdint>
#include <chrono>
#include <thread>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "IEC61850MmsSettingGroup.h"

namespace {

IEC61850::MmsObjectName Object(std::string identifier) {
  IEC61850::MmsObjectName object;
  object.type = IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC;
  object.domain = "IED1LD0";
  object.identifier = std::move(identifier);
  return object;
}

IEC61850::MmsSettingGroupPlan Plan() {
  IEC61850::MmsSettingGroupPlan plan;
  plan.numberOfGroups = Object("SGCB$NumOfSG");
  plan.activeGroup = Object("SGCB$ActSG");
  plan.editGroup = Object("SGCB$EditSG");
  plan.confirmEdit = Object("SGCB$CnfEdit");
  plan.maxGroups = 16;
  return plan;
}

IEC61850::MmsReadResponse StatusResponse(std::uint32_t number,
                                         std::uint32_t active,
                                         std::uint32_t edit, bool confirmed) {
  IEC61850::MmsReadResponse response;
  for (const auto [value, boolean] :
       std::vector<std::pair<std::uint32_t, bool>>{{number, false},
                                                    {active, false},
                                                    {edit, false}}) {
    auto& item = response.items.emplace_back();
    item.success = true;
    if (boolean) {
      EXPECT_TRUE(IEC61850::EncodeMmsDataBoolean(true, &item.encodedData).ok());
    } else {
      EXPECT_TRUE(IEC61850::EncodeMmsDataSigned(value, &item.encodedData).ok());
    }
  }
  auto& confirm = response.items.emplace_back();
  confirm.success = true;
  EXPECT_TRUE(
      IEC61850::EncodeMmsDataBoolean(confirmed, &confirm.encodedData).ok());
  return response;
}

std::vector<IEC61850::MmsObjectName> OnlineObjects(
    const IEC61850::MmsSettingGroupPlan& plan) {
  return {plan.numberOfGroups, plan.activeGroup, plan.editGroup,
          plan.confirmEdit};
}

}  // namespace

// 验证SGCB组号必须从1开始，并拒绝超过配置上限的请求。
TEST(IEC61850MmsSettingGroupTest, RejectsInvalidGroupBeforeNetwork) {
  std::size_t reads = 0;
  std::size_t writes = 0;
  IEC61850::MmsSettingGroupClient client(
      [&reads](const auto&, auto*) {
        ++reads;
        return grpc::Status::OK;
      },
      [&writes](const auto&, auto*) {
        ++writes;
        return grpc::Status::OK;
      });

  EXPECT_EQ(client.Select(Plan(), 0).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(client.Select(Plan(), 17).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(reads, 0u);
  EXPECT_EQ(writes, 0u);
}

// 验证读取SGCB状态时保持NumOfSG、ActSG、EditSG和CnfEdit顺序。
TEST(IEC61850MmsSettingGroupTest, ReadsStatusInStableOrder) {
  const auto expected = StatusResponse(4, 2, 2, false);
  std::vector<std::string> names;
  IEC61850::MmsSettingGroupClient client(
      [&expected, &names](const IEC61850::MmsReadRequest& request,
                          IEC61850::MmsReadResponse* response) {
        for (const auto& variable : request.variables) {
          names.push_back(variable.identifier);
        }
        *response = expected;
        return grpc::Status::OK;
      },
      {});

  IEC61850::MmsSettingGroupStatus status;
  ASSERT_TRUE(client.ReadStatus(Plan(), &status).ok());
  EXPECT_EQ(status.numberOfGroups, 4u);
  EXPECT_EQ(status.activeGroup, 2u);
  EXPECT_EQ(status.editGroup, 2u);
  EXPECT_FALSE(status.confirmEdit);
  ASSERT_EQ(names.size(), 4u);
  EXPECT_EQ(names[0], "SGCB$NumOfSG");
  EXPECT_EQ(names[1], "SGCB$ActSG");
  EXPECT_EQ(names[2], "SGCB$EditSG");
  EXPECT_EQ(names[3], "SGCB$CnfEdit");
}

// 验证AR502H选择流程先清除旧CnfEdit，再写入EditSG。
TEST(IEC61850MmsSettingGroupTest, SelectCancelsOldEditBeforeWritingGroup) {
  std::vector<std::string> writes;
  const auto response = StatusResponse(4, 2, 0, false);
  IEC61850::MmsSettingGroupClient client(
      [response](const auto&, IEC61850::MmsReadResponse* output) {
        *output = response;
        return grpc::Status::OK;
      },
      [&writes](const IEC61850::MmsWriteRequest& request, auto* response) {
        writes.push_back(request.items.front().variable.identifier);
        response->items.resize(1);
        response->items.front().success = true;
        return grpc::Status::OK;
      });

  ASSERT_TRUE(client.Select(Plan(), 3).ok());
  ASSERT_EQ(writes.size(), 2u);
  EXPECT_EQ(writes[0], "SGCB$CnfEdit");
  EXPECT_EQ(writes[1], "SGCB$EditSG");
}

// 验证激活流程只允许激活当前编辑组，并按确认后ActSG的顺序写入。
TEST(IEC61850MmsSettingGroupTest, ActivateConfirmsThenWritesActiveGroup) {
  std::vector<std::string> writes;
  const auto response = StatusResponse(4, 2, 3, false);
  IEC61850::MmsSettingGroupClient client(
      [response](const auto&, IEC61850::MmsReadResponse* output) {
        *output = response;
        return grpc::Status::OK;
      },
      [&writes](const IEC61850::MmsWriteRequest& request, auto* response) {
        writes.push_back(request.items.front().variable.identifier);
        response->items.resize(1);
        response->items.front().success = true;
        return grpc::Status::OK;
      });

  ASSERT_TRUE(client.Activate(Plan(), 3).ok());
  ASSERT_EQ(writes.size(), 2u);
  EXPECT_EQ(writes[0], "SGCB$CnfEdit");
  EXPECT_EQ(writes[1], "SGCB$ActSG");
}

// 验证在线NameList缺少对象时不宣称SGCB读写能力。
TEST(IEC61850MmsSettingGroupTest, DiscoversMissingOnlineCapability) {
  const auto plan = Plan();
  auto online = OnlineObjects(plan);
  online.pop_back();
  IEC61850::MmsSettingGroupCapabilities capabilities;
  ASSERT_TRUE(IEC61850::MmsSettingGroupClient::DiscoverCapabilities(
                  plan, online, &capabilities)
                  .ok());
  EXPECT_TRUE(capabilities.discovered);
  EXPECT_FALSE(capabilities.supportsRead);
  EXPECT_FALSE(capabilities.supportsWrite);
  EXPECT_FALSE(capabilities.supportsConfirm);
}

// 验证NameList返回短标识符时也能完成SGCB对象匹配。
TEST(IEC61850MmsSettingGroupTest, DiscoversCapabilityFromNameListIdentifiers) {
  const auto plan = Plan();
  const std::vector<std::string> identifiers = {
      "SGCB$NumOfSG", "IED1LD0SGCB$ActSG", "SGCB$EditSG", "SGCB$CnfEdit"};
  IEC61850::MmsSettingGroupCapabilities capabilities;
  ASSERT_TRUE(IEC61850::MmsSettingGroupClient::DiscoverCapabilities(
                  plan, identifiers, &capabilities)
                  .ok());
  EXPECT_TRUE(capabilities.supportsRead);
  EXPECT_TRUE(capabilities.supportsWrite);
}

// 验证已发现但未协商Write时，在进入网络队列前拒绝SGCB写操作。
TEST(IEC61850MmsSettingGroupTest, RejectsUnnegotiatedWrite) {
  auto plan = Plan();
  IEC61850::MmsSettingGroupCapabilities capabilities;
  capabilities.discovered = true;
  capabilities.supportsRead = true;
  capabilities.supportsWrite = false;
  capabilities.supportsSelect = false;
  capabilities.supportsConfirm = false;
  capabilities.supportsCancel = false;
  capabilities.supportsActivate = false;
  plan.capabilities = capabilities;
  std::size_t writes = 0;
  IEC61850::MmsSettingGroupClient client(
      [](const auto&, IEC61850::MmsReadResponse* response) {
        *response = StatusResponse(4, 2, 2, false);
        return grpc::Status::OK;
      },
      [&writes](const auto&, IEC61850::MmsWriteResponse*) {
        ++writes;
        return grpc::Status::OK;
      });
  EXPECT_EQ(client.ConfirmEdit(plan).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(writes, 0u);
}

// 验证上层权限策略拒绝写入，不把越权请求送入MMS串行队列。
TEST(IEC61850MmsSettingGroupTest, RejectsWriteWithoutPermission) {
  auto plan = Plan();
  IEC61850::MmsSettingGroupCapabilities capabilities;
  capabilities.discovered = true;
  capabilities.supportsRead = true;
  capabilities.supportsWrite = true;
  capabilities.supportsConfirm = true;
  capabilities.writePermission = false;
  plan.capabilities = capabilities;
  std::size_t writes = 0;
  IEC61850::MmsSettingGroupClient client(
      {}, [&writes](const auto&, IEC61850::MmsWriteResponse*) {
        ++writes;
        return grpc::Status::OK;
      });
  EXPECT_EQ(client.ConfirmEdit(plan).error_code(),
            grpc::StatusCode::PERMISSION_DENIED);
  EXPECT_EQ(writes, 0u);
}

// 验证在线操作超过统一截止时间时返回超时并不伪造状态。
TEST(IEC61850MmsSettingGroupTest, RejectsOperationAfterDeadline) {
  auto plan = Plan();
  IEC61850::MmsSettingGroupCapabilities capabilities;
  capabilities.discovered = true;
  capabilities.supportsRead = true;
  capabilities.timeout = std::chrono::milliseconds(1);
  plan.capabilities = capabilities;
  IEC61850::MmsSettingGroupClient client(
      [](const auto&, IEC61850::MmsReadResponse* response) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        *response = StatusResponse(4, 2, 2, false);
        return grpc::Status::OK;
      },
      {});
  IEC61850::MmsSettingGroupStatus status;
  EXPECT_EQ(client.ReadStatus(plan, &status).error_code(),
            grpc::StatusCode::DEADLINE_EXCEEDED);
  EXPECT_EQ(status.state, IEC61850::MmsSettingGroupState::UNKNOWN);
}

// 验证EditSG写入失败且回滚再次失败时报告不确定状态，禁止继续激活。
TEST(IEC61850MmsSettingGroupTest, ReportsIndeterminateWhenRollbackFails) {
  const auto response = StatusResponse(4, 2, 2, false);
  auto plan = Plan();
  std::size_t writes = 0;
  IEC61850::MmsSettingGroupClient client(
      [response](const auto&, IEC61850::MmsReadResponse* output) {
        *output = response;
        return grpc::Status::OK;
      },
      [&writes](const auto&, IEC61850::MmsWriteResponse* output) {
        ++writes;
        if (output != nullptr) {
          output->items.resize(1);
          output->items.front().success = true;
        }
        if (writes == 2 || writes == 4) {
          return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                              "模拟连接中断");
        }
        return grpc::Status::OK;
      });
  EXPECT_EQ(client.Select(plan, 3).error_code(), grpc::StatusCode::ABORTED);
  EXPECT_EQ(writes, 4u);
}

// 验证定值多变量写入先读取快照，部分失败后恢复原始值。
TEST(IEC61850MmsSettingGroupTest, WritesValuesAndRollsBackPartialFailure) {
  const auto plan = Plan();
  const auto first = Object("PTOC$StrVal");
  const auto second = Object("PTOC$OpDlTmms");
  std::size_t reads = 0;
  std::size_t writes = 0;
  std::vector<std::string> rollbackNames;
  IEC61850::MmsSettingGroupClient client(
      [&reads](const IEC61850::MmsReadRequest& request,
               IEC61850::MmsReadResponse* response) {
        ++reads;
        response->items.clear();
        for (std::size_t index = 0; index < request.variables.size(); ++index) {
          auto& item = response->items.emplace_back();
          item.success = true;
          EXPECT_TRUE(IEC61850::EncodeMmsDataSigned(
                          static_cast<std::int64_t>(index + 1),
                          &item.encodedData)
                          .ok());
        }
        return grpc::Status::OK;
      },
      [&writes, &rollbackNames](const IEC61850::MmsWriteRequest& request,
                                IEC61850::MmsWriteResponse* response) {
        ++writes;
        response->items.resize(request.items.size());
        for (auto& item : response->items) {
          item.success = true;
        }
        if (writes == 1) {
          response->items.back().success = false;
        } else {
          for (const auto& item : request.items) {
            rollbackNames.push_back(item.variable.identifier);
          }
        }
        return grpc::Status::OK;
      });
  std::vector<IEC61850::MmsWriteRequestItem> values(2);
  values[0].variable = first;
  values[1].variable = second;
  ASSERT_TRUE(IEC61850::EncodeMmsDataSigned(10, &values[0].encodedData).ok());
  ASSERT_TRUE(IEC61850::EncodeMmsDataSigned(20, &values[1].encodedData).ok());
  IEC61850::MmsWriteResponse response;
  EXPECT_EQ(client.WriteValues(plan, values, &response).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(reads, 1u);
  EXPECT_EQ(writes, 2u);
  ASSERT_EQ(rollbackNames.size(), 2u);
  EXPECT_EQ(rollbackNames[0], first.identifier);
  EXPECT_EQ(rollbackNames[1], second.identifier);
}
