#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "IEC104SoeStore.h"

namespace {
class ScopedSoeTempDir {
public:
  ScopedSoeTempDir() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::current_path() / ("iec104_soe_store_test_" + std::to_string(now));
    std::filesystem::create_directories(path_);
  }

  ~ScopedSoeTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};
}  // 匿名命名空间结束

// 验证：每条连接独立保留最近 8000 条 SOE，第 8001 条只覆盖本连接最旧记录。
TEST(IEC104SoeStoreTest, RetainsLatestEightThousandEventsPerConnection) {
  ScopedSoeTempDir dir;
  IEC104::IEC104SoeStore store(dir.path() / "config.db");

  IEC104::SoeAppendResult appendResult;
  for (uint32_t i = 0; i < IEC104::IEC104SoeStore::kCapacityPerConnection + 1; ++i) {
    ASSERT_TRUE(store.Append("channel-a", 100 + i, (i % 2) != 0, 1700000000000LL + i,
                             static_cast<uint8_t>(i & 0xFF), &appendResult)
                    .ok());
  }
  EXPECT_EQ(appendResult.record.eventSequence, IEC104::IEC104SoeStore::kCapacityPerConnection + 1);
  EXPECT_EQ(appendResult.overwrittenCount, 1u);
  EXPECT_EQ(appendResult.overwrittenUnacknowledgedCount, 1u);

  ASSERT_TRUE(store.Append("channel-b", 7, true, 1700000001000LL, 0x80, &appendResult).ok());
  EXPECT_EQ(appendResult.record.eventSequence, 1u);
  EXPECT_EQ(appendResult.overwrittenCount, 0u);

  std::vector<IEC104::SoeRecord> channelA;
  ASSERT_TRUE(store.LoadRecent("channel-a", &channelA).ok());
  ASSERT_EQ(channelA.size(), IEC104::IEC104SoeStore::kCapacityPerConnection);
  EXPECT_EQ(channelA.front().eventSequence, 2u);
  EXPECT_EQ(channelA.front().ioa, 101u);
  EXPECT_EQ(channelA.back().eventSequence, IEC104::IEC104SoeStore::kCapacityPerConnection + 1);

  std::vector<IEC104::SoeRecord> channelB;
  ASSERT_TRUE(store.LoadRecent("channel-b", &channelB).ok());
  ASSERT_EQ(channelB.size(), 1u);
  EXPECT_EQ(channelB.front().eventSequence, 1u);
  EXPECT_EQ(channelB.front().ioa, 7u);
}

// 验证：确认状态、字段和顺序在重新打开 SQLite 后保持，已确认事件不再进入补传查询。
TEST(IEC104SoeStoreTest, PersistsAcknowledgementAndEventFieldsAcrossRestart) {
  ScopedSoeTempDir dir;
  const auto databasePath = dir.path() / "config.db";

  {
    IEC104::IEC104SoeStore store(databasePath);
    IEC104::SoeAppendResult result;
    ASSERT_TRUE(store.Append("channel", 11, false, 1700000000001LL, 0x00, &result).ok());
    ASSERT_TRUE(store.Append("channel", 12, true, 1700000000002LL, 0x80, &result).ok());
    ASSERT_TRUE(store.Append("channel", 13, false, 1700000000003LL, 0x40, &result).ok());

    const std::array<uint64_t, 2> acknowledged{1, 3};
    ASSERT_TRUE(store.MarkAcknowledged("channel", acknowledged).ok());
  }

  IEC104::IEC104SoeStore restored(databasePath);
  std::vector<IEC104::SoeRecord> history;
  ASSERT_TRUE(restored.LoadRecent("channel", &history).ok());
  ASSERT_EQ(history.size(), 3u);
  EXPECT_TRUE(history[0].acknowledged);
  EXPECT_FALSE(history[1].acknowledged);
  EXPECT_TRUE(history[2].acknowledged);
  EXPECT_EQ(history[1].ioa, 12u);
  EXPECT_TRUE(history[1].state);
  EXPECT_EQ(history[1].tsMs, 1700000000002LL);
  EXPECT_EQ(history[1].quality, 0x80);

  std::vector<IEC104::SoeRecord> pending;
  ASSERT_TRUE(restored.LoadUnacknowledged("channel", &pending).ok());
  ASSERT_EQ(pending.size(), 1u);
  EXPECT_EQ(pending.front().eventSequence, 2u);

  IEC104::SoeAppendResult continued;
  ASSERT_TRUE(restored.Append("channel", 14, true, 1700000000004LL, 0, &continued).ok());
  EXPECT_EQ(continued.record.eventSequence, 4u);
}

// 验证：链路改名会迁移 SOE 与序号进度，链路删除会清理历史和序号进度。
TEST(IEC104SoeStoreTest, RenamesAndDeletesConnectionHistory) {
  ScopedSoeTempDir dir;
  IEC104::IEC104SoeStore store(dir.path() / "config.db");
  IEC104::SoeAppendResult result;
  ASSERT_TRUE(store.Append("old-name", 1, true, 1700000000000LL, 0, &result).ok());

  ASSERT_TRUE(store.RenameConnection("old-name", "new-name").ok());
  std::vector<IEC104::SoeRecord> history;
  ASSERT_TRUE(store.LoadRecent("old-name", &history).ok());
  EXPECT_TRUE(history.empty());
  ASSERT_TRUE(store.LoadRecent("new-name", &history).ok());
  ASSERT_EQ(history.size(), 1u);
  EXPECT_EQ(history.front().connName, "new-name");

  ASSERT_TRUE(store.Append("new-name", 2, false, 1700000000001LL, 0, &result).ok());
  EXPECT_EQ(result.record.eventSequence, 2u);
  ASSERT_TRUE(store.DeleteConnection("new-name").ok());
  ASSERT_TRUE(store.LoadRecent("new-name", &history).ok());
  EXPECT_TRUE(history.empty());

  ASSERT_TRUE(store.Append("new-name", 3, true, 1700000000002LL, 0, &result).ok());
  EXPECT_EQ(result.record.eventSequence, 1u);
}

// 验证：SOE 查询支持时间、IOA、确认状态筛选及向更早事件翻页，并返回正确统计值。
TEST(IEC104SoeStoreTest, QueriesHistoryWithFiltersAndCursor) {
  ScopedSoeTempDir dir;
  IEC104::IEC104SoeStore store(dir.path() / "config.db");
  IEC104::SoeAppendResult result;
  ASSERT_TRUE(store.Append("channel", 10, false, 1000, 0x01, &result).ok());
  ASSERT_TRUE(store.Append("channel", 11, true, 2000, 0x02, &result).ok());
  ASSERT_TRUE(store.Append("channel", 10, true, 3000, 0x03, &result).ok());
  ASSERT_TRUE(store.Append("channel", 12, false, 4000, 0x04, &result).ok());
  const std::array<uint64_t, 2> acknowledged{2, 4};
  ASSERT_TRUE(store.MarkAcknowledged("channel", acknowledged).ok());

  IEC104::SoeQueryOptions options;
  options.pageSize = 2;
  IEC104::SoeQueryResult page;
  ASSERT_TRUE(store.Query("channel", options, &page).ok());
  ASSERT_EQ(page.records.size(), 2u);
  EXPECT_EQ(page.records[0].eventSequence, 4u);
  EXPECT_EQ(page.records[1].eventSequence, 3u);
  EXPECT_TRUE(page.hasMore);
  ASSERT_TRUE(page.nextEventSequence.has_value());
  EXPECT_EQ(page.nextEventSequence.value(), 3u);
  EXPECT_EQ(page.totalCount, 4u);
  EXPECT_EQ(page.unacknowledgedCount, 2u);

  options.beforeEventSequence = page.nextEventSequence;
  ASSERT_TRUE(store.Query("channel", options, &page).ok());
  ASSERT_EQ(page.records.size(), 2u);
  EXPECT_EQ(page.records[0].eventSequence, 2u);
  EXPECT_EQ(page.records[1].eventSequence, 1u);
  EXPECT_FALSE(page.hasMore);
  EXPECT_FALSE(page.nextEventSequence.has_value());

  options = {};
  options.ioa = 10;
  options.startTsMs = 1500;
  options.endTsMs = 3500;
  options.acknowledgedFilter = IEC104::SoeAcknowledgedFilter::kUnacknowledged;
  ASSERT_TRUE(store.Query("channel", options, &page).ok());
  ASSERT_EQ(page.records.size(), 1u);
  EXPECT_EQ(page.records.front().eventSequence, 3u);
  EXPECT_EQ(page.totalCount, 1u);
  EXPECT_EQ(page.unacknowledgedCount, 1u);
}

// 验证：SOE 查询拒绝空连接名、非法时间范围、非法页大小和零游标。
TEST(IEC104SoeStoreTest, RejectsInvalidQueryOptions) {
  ScopedSoeTempDir dir;
  IEC104::IEC104SoeStore store(dir.path() / "config.db");
  IEC104::SoeQueryOptions options;
  IEC104::SoeQueryResult result;

  EXPECT_EQ(store.Query("", options, &result).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  options.startTsMs = 20;
  options.endTsMs = 10;
  EXPECT_EQ(store.Query("channel", options, &result).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  options = {};
  options.pageSize = IEC104::IEC104SoeStore::kCapacityPerConnection + 1;
  EXPECT_EQ(store.Query("channel", options, &result).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  options = {};
  options.beforeEventSequence = 0;
  EXPECT_EQ(store.Query("channel", options, &result).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}
