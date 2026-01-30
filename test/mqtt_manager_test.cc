#include "gtest/gtest.h"

#include <boost/json.hpp>

#include "MQTTJsonPath.hpp"
#include "MQTTTopicMatcher.hpp"

// 验证主题过滤匹配支持 + 与 # 通配符。
TEST(MqttTopicMatcherTest, MatchTopicFilterSupportsWildcards) {
  EXPECT_TRUE(MQTTManager::MatchTopicFilter("#", "a/b/c"));
  EXPECT_TRUE(MQTTManager::MatchTopicFilter("a/+/c", "a/b/c"));
  EXPECT_FALSE(MQTTManager::MatchTopicFilter("a/+/c", "a/b/d"));
  EXPECT_TRUE(MQTTManager::MatchTopicFilter("a/#", "a/b/c"));
  EXPECT_TRUE(MQTTManager::MatchTopicFilter("a/b", "a/b"));
  EXPECT_FALSE(MQTTManager::MatchTopicFilter("a/b", "a/b/c"));
}

// 验证嵌套路径能够正确解析并提取字段值。
TEST(MqttJsonPathTest, ExtractNestedField) {
  boost::system::error_code ec;
  const auto json = boost::json::parse(
      R"({"data":{"items":[{"id":"a1"},{"id":"b2"}]}})", ec);
  ASSERT_FALSE(ec);

  std::vector<MQTTManager::JsonPath::Segment> segments;
  std::string error;
  ASSERT_TRUE(MQTTManager::JsonPath::parsePath("data.items[1].id", &segments, &error));

  const boost::json::value* value = nullptr;
  ASSERT_TRUE(MQTTManager::JsonPath::extractValue(json, segments, &value, &error));
  ASSERT_TRUE(value->is_string());
  EXPECT_EQ(value->as_string(), "b2");
}

// 验证根数组路径解析与字段提取。
TEST(MqttJsonPathTest, ExtractFromRootArray) {
  boost::system::error_code ec;
  const auto json = boost::json::parse(
      R"([{"id":"x"},{"id":"y"}])", ec);
  ASSERT_FALSE(ec);

  std::vector<MQTTManager::JsonPath::Segment> segments;
  std::string error;
  ASSERT_TRUE(MQTTManager::JsonPath::parsePath("[0].id", &segments, &error));

  const boost::json::value* value = nullptr;
  ASSERT_TRUE(MQTTManager::JsonPath::extractValue(json, segments, &value, &error));
  ASSERT_TRUE(value->is_string());
  EXPECT_EQ(value->as_string(), "x");
}

// 验证非法路径会解析失败。
TEST(MqttJsonPathTest, RejectsInvalidPath) {
  std::vector<MQTTManager::JsonPath::Segment> segments;
  std::string error;
  EXPECT_FALSE(MQTTManager::JsonPath::parsePath("", &segments, &error));
  EXPECT_FALSE(MQTTManager::JsonPath::parsePath("data..id", &segments, &error));
  EXPECT_FALSE(MQTTManager::JsonPath::parsePath("data[", &segments, &error));
}

// 验证路径存在但字段缺失时返回失败。
TEST(MqttJsonPathTest, MissingFieldReturnsFalse) {
  boost::system::error_code ec;
  const auto json = boost::json::parse(
      R"({"data":{"id":"v1"}})", ec);
  ASSERT_FALSE(ec);

  std::vector<MQTTManager::JsonPath::Segment> segments;
  std::string error;
  ASSERT_TRUE(MQTTManager::JsonPath::parsePath("data.missing", &segments, &error));

  const boost::json::value* value = nullptr;
  EXPECT_FALSE(MQTTManager::JsonPath::extractValue(json, segments, &value, &error));
}
