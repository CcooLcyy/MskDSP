#include "IEC61850MmsServer.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using namespace IEC61850;

template <typename Encoder>
std::vector<std::uint8_t> Encode(Encoder&& encoder) {
  std::array<std::uint8_t, 65536> buffer{};
  std::size_t size = 0;
  EXPECT_TRUE(encoder(std::span<std::uint8_t>(buffer), &size).ok());
  return {buffer.begin(), buffer.begin() + size};
}

MmsObjectName Object(std::string identifier) {
  return MmsObjectName{MmsObjectNameType::DOMAIN_SPECIFIC, "IED1LD0",
                       std::move(identifier)};
}

TEST(IEC61850MmsServerTest, NameListContinuationStartsAfterRequestedName) {
  MmsServerModel model;
  for (int index = 0; index < 257; ++index) {
    model.domains.push_back("D" + std::to_string(index));
  }
  MmsServer server(std::move(model));

  MmsGetNameListRequest firstRequest;
  firstRequest.objectClass = MmsObjectClass::DOMAIN;
  firstRequest.scope.type = MmsObjectScopeType::VMD_SPECIFIC;
  const auto first = Encode([&](auto output, auto* size) {
    return EncodeMmsGetNameListRequest(1, firstRequest, output, size);
  });
  std::vector<std::uint8_t> response;
  const auto firstStatus = server.HandleConfirmed(first, &response);
  ASSERT_TRUE(firstStatus.ok()) << firstStatus.error_message();
  MmsGetNameListResponse firstResponse;
  ASSERT_TRUE(DecodeMmsGetNameListResponse(response, 1, &firstResponse).ok());
  ASSERT_EQ(firstResponse.identifiers.size(), 256U);
  ASSERT_TRUE(firstResponse.moreFollows);

  MmsGetNameListRequest secondRequest = firstRequest;
  secondRequest.continueAfter = firstResponse.identifiers.back();
  const auto second = Encode([&](auto output, auto* size) {
    return EncodeMmsGetNameListRequest(2, secondRequest, output, size);
  });
  ASSERT_TRUE(server.HandleConfirmed(second, &response).ok());
  MmsGetNameListResponse secondResponse;
  ASSERT_TRUE(DecodeMmsGetNameListResponse(response, 2, &secondResponse).ok());
  ASSERT_EQ(secondResponse.identifiers, (std::vector<std::string>{"D256"}));
  EXPECT_FALSE(secondResponse.moreFollows);
}

TEST(IEC61850MmsServerTest, ReadWriteJournalAndDirectoryCallbacks) {
  MmsServerModel model;
  model.domains = {"IED1"};
  model.namedVariables = {"X1"};
  model.read = [](const MmsReadRequest& request, MmsReadResponse* response) {
    if (request.variables.size() != 1U || response == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Read请求无效");
    }
    MmsReadResponseItem item;
    item.success = true;
    auto status = EncodeMmsDataBoolean(true, &item.encodedData);
    if (!status.ok()) return status;
    response->items = {std::move(item)};
    return grpc::Status::OK;
  };
  model.write = [](const MmsWriteRequest& request, MmsWriteResponse* response) {
    if (request.items.size() != 1U || response == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Write请求无效");
    }
    response->items = {MmsWriteResponseItem{true, 0}};
    return grpc::Status::OK;
  };
  model.journalRead = [](const MmsJournalReadRequest* request,
                         MmsJournalReadResponse* response) {
    if (request == nullptr || response == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Journal请求无效");
    }
    MmsJournalEntry entry;
    entry.entryId = {0x01};
    entry.occurrenceTimeMs = 1700000000000LL;
    entry.kind = MmsJournalEntryKind::SOE;
    entry.eventCondition = "Trip";
    response->entries = {std::move(entry)};
    response->moreFollows = false;
    return grpc::Status::OK;
  };
  model.fileDirectory = [](const MmsFileDirectoryRequest* request,
                           MmsFileDirectoryResponse* response) {
    if (request == nullptr || response == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "目录请求无效");
    }
    MmsFileDirectoryEntry entry;
    entry.fileName = "/COMTRADE/trip.cfg";
    entry.attributes.sizePresent = true;
    entry.attributes.size = 12;
    response->entries = {std::move(entry)};
    return grpc::Status::OK;
  };
  MmsServer server(std::move(model));
  std::vector<std::uint8_t> response;

  MmsReadRequest readRequest;
  readRequest.variables = {Object("X1")};
  auto request = Encode([&](auto output, auto* size) {
    return EncodeMmsReadRequest(10, readRequest, output, size);
  });
  auto writeHandleStatus = server.HandleConfirmed(request, &response);
  ASSERT_TRUE(writeHandleStatus.ok()) << writeHandleStatus.error_message();
  MmsReadResponse readResponse;
  ASSERT_TRUE(DecodeMmsReadResponse(response, 10, &readResponse).ok());
  ASSERT_EQ(readResponse.items.size(), 1U);
  EXPECT_TRUE(readResponse.items.front().success);

  MmsWriteRequest writeRequest;
  MmsWriteRequestItem writeItem;
  writeItem.variable = Object("X1");
  ASSERT_TRUE(EncodeMmsDataBoolean(false, &writeItem.encodedData).ok());
  writeRequest.items = {std::move(writeItem)};
  request = Encode([&](auto output, auto* size) {
    return EncodeMmsWriteRequest(11, writeRequest, output, size);
  });
  auto handleStatus = server.HandleConfirmed(request, &response);
  ASSERT_TRUE(handleStatus.ok()) << handleStatus.error_message();
  MmsWriteResponse writeResponse;
  ASSERT_TRUE(DecodeMmsWriteResponse(response, 11, &writeResponse).ok());
  ASSERT_EQ(writeResponse.items.size(), 1U);
  EXPECT_TRUE(writeResponse.items.front().success);

  MmsJournalReadRequest journalRequest;
  journalRequest.journal = Object("EventLog");
  request = Encode([&](auto output, auto* size) {
    return EncodeMmsJournalReadRequest(12, journalRequest, output, size);
  });
  ASSERT_TRUE(server.HandleConfirmed(request, &response).ok());
  MmsJournalReadResponse journalResponse;
  ASSERT_TRUE(DecodeMmsJournalReadResponse(response, 12, &journalResponse).ok());
  ASSERT_EQ(journalResponse.entries.size(), 1U);
  EXPECT_EQ(journalResponse.entries.front().kind, MmsJournalEntryKind::SOE);

  MmsFileDirectoryRequest directoryRequest;
  directoryRequest.fileSpecification = "/COMTRADE/";
  request = Encode([&](auto output, auto* size) {
    return EncodeMmsFileDirectoryRequest(13, directoryRequest, output, size);
  });
  ASSERT_TRUE(server.HandleConfirmed(request, &response).ok());
  MmsFileDirectoryResponse directoryResponse;
  ASSERT_TRUE(
      DecodeMmsFileDirectoryResponse(response, 13, &directoryResponse).ok());
  ASSERT_EQ(directoryResponse.entries.size(), 1U);
  EXPECT_EQ(directoryResponse.entries.front().fileName, "/COMTRADE/trip.cfg");
}

TEST(IEC61850MmsServerTest, DynamicNamedVariableListLifecycle) {
  MmsServerModel model;
  MmsServer server(std::move(model));
  std::vector<std::uint8_t> response;
  MmsNamedVariableListDefinition definition;
  definition.listName = Object("RuntimeDS");
  definition.variables = {Object("X1"), Object("X2")};

  auto request = Encode([&](auto output, auto* size) {
    return EncodeMmsDefineNamedVariableListRequest(20, definition, output, size);
  });
  ASSERT_TRUE(server.HandleConfirmed(request, &response).ok());
  MmsConfirmedPduView pdu;
  ASSERT_TRUE(DecodeMmsConfirmedResponse(response, &pdu).ok());
  EXPECT_EQ(pdu.serviceTag, 11U);

  request = Encode([&](auto output, auto* size) {
    return EncodeMmsGetNamedVariableListAttributesRequest(21, definition.listName,
                                                            output, size);
  });
  ASSERT_TRUE(server.HandleConfirmed(request, &response).ok());
  MmsGetNamedVariableListAttributesResponse attributes;
  ASSERT_TRUE(DecodeMmsGetNamedVariableListAttributesResponse(
                   response, 21, &attributes)
                   .ok());
  EXPECT_EQ(attributes.variables, definition.variables);

  request = Encode([&](auto output, auto* size) {
    return EncodeMmsDeleteNamedVariableListRequest(22, definition.listName,
                                                    output, size);
  });
  ASSERT_TRUE(server.HandleConfirmed(request, &response).ok());
  EXPECT_EQ(DecodeMmsConfirmedResponse(response, &pdu).error_code(),
            grpc::StatusCode::OK);
  request = Encode([&](auto output, auto* size) {
    return EncodeMmsGetNamedVariableListAttributesRequest(23, definition.listName,
                                                            output, size);
  });
  EXPECT_EQ(server.HandleConfirmed(request, &response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

TEST(IEC61850MmsServerTest, MissingDataModelIsRejected) {
  MmsServer server;
  MmsReadRequest readRequest;
  readRequest.variables = {Object("X1")};
  auto request = Encode([&](auto output, auto* size) {
    return EncodeMmsReadRequest(30, readRequest, output, size);
  });
  std::vector<std::uint8_t> response;
  const auto status = server.HandleConfirmed(request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNIMPLEMENTED);
  EXPECT_TRUE(response.empty());
}

TEST(IEC61850MmsServerTest, RejectsUnsupportedServiceAndMalformedRequest) {
  MmsServer server;
  std::vector<std::uint8_t> response;

  const auto unsupported = Encode([&](auto output, auto* size) {
    return EncodeMmsConfirmedRequest(31, 99, {}, output, size);
  });
  ASSERT_FALSE(unsupported.empty());
  auto status = server.HandleConfirmed(unsupported, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNIMPLEMENTED);
  EXPECT_TRUE(response.empty());

  const std::array<std::uint8_t, 3> malformed{0xa0, 0x01, 0x00};
  response.clear();
  status = server.HandleConfirmed(malformed, &response);
  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(response.empty());
}

TEST(IEC61850MmsServerTest, InitiateNegotiatesOnlyInstalledServices) {
  MmsServerModel model;
  model.read = [](const MmsReadRequest&, MmsReadResponse*) {
    return grpc::Status::OK;
  };
  model.journalRead = [](const MmsJournalReadRequest*,
                         MmsJournalReadResponse*) {
    return grpc::Status::OK;
  };
  model.parameterSupport.size = 2;
  model.parameterSupport.unusedBits = 5;
  model.parameterSupport.bytes[0] = 0x80;
  MmsServer server(std::move(model));

  MmsInitiateRequest initiate;
  initiate.proposedParameterSupport.size = 2;
  initiate.proposedParameterSupport.unusedBits = 5;
  initiate.proposedParameterSupport.bytes[0] = 0xff;
  initiate.proposedParameterSupport.bytes[1] = 0xe0;
  initiate.proposedServiceSupport.size = 11;
  initiate.proposedServiceSupport.unusedBits = 3;
  initiate.proposedServiceSupport.bytes.fill(0xff);
  initiate.proposedServiceSupport.bytes[10] = 0xf8;
  std::array<std::uint8_t, 65536> requestBuffer{};
  std::size_t requestSize = 0;
  const auto requestStatus = EncodeMmsInitiateRequest(
      initiate, requestBuffer, &requestSize);
  ASSERT_TRUE(requestStatus.ok()) << requestStatus.error_message();
  const std::vector<std::uint8_t> request(requestBuffer.begin(),
                                          requestBuffer.begin() + requestSize);
  std::vector<std::uint8_t> response;
  ASSERT_TRUE(server.HandleInitiate(request, &response).ok());
  MmsInitiateResponse negotiated;
  ASSERT_TRUE(DecodeMmsInitiateResponse(response, &negotiated).ok());
  ASSERT_EQ(negotiated.negotiatedServiceSupport.size, 11U);
  EXPECT_EQ(negotiated.negotiatedServiceSupport.bytes[0], 0x48U);
  EXPECT_EQ(negotiated.negotiatedServiceSupport.bytes[1], 0x1cU);
  EXPECT_EQ(negotiated.negotiatedServiceSupport.bytes[8], 0x40U);
  EXPECT_EQ(negotiated.negotiatedServiceSupport.bytes[9], 0x00U);
  EXPECT_EQ(negotiated.negotiatedParameterSupport.bytes[0], 0x80U);
}

TEST(IEC61850MmsServerTest, ConcurrentRequestsAndModelReloadAreIsolated) {
  std::atomic_uint readCount = 0;
  MmsServerModel model;
  model.read = [&readCount](const MmsReadRequest& request,
                            MmsReadResponse* response) {
    if (request.variables.size() != 1U || response == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Read请求无效");
    }
    ++readCount;
    MmsReadResponseItem item;
    item.success = true;
    auto status = EncodeMmsDataUnsigned(7, &item.encodedData);
    if (!status.ok()) return status;
    response->items = {std::move(item)};
    return grpc::Status::OK;
  };
  MmsServer server(model);
  MmsReadRequest readRequest;
  readRequest.variables = {Object("X1")};
  const auto request = Encode([&](auto output, auto* size) {
    return EncodeMmsReadRequest(40, readRequest, output, size);
  });

  std::atomic_bool failed = false;
  std::vector<std::thread> threads;
  for (int threadIndex = 0; threadIndex < 8; ++threadIndex) {
    threads.emplace_back([&] {
      for (int iteration = 0; iteration < 50; ++iteration) {
        std::vector<std::uint8_t> response;
        MmsReadResponse decoded;
        if (!server.HandleConfirmed(request, &response).ok() ||
            !DecodeMmsReadResponse(response, 40, &decoded).ok() ||
            decoded.items.size() != 1U || !decoded.items.front().success) {
          failed.store(true, std::memory_order_release);
          return;
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  EXPECT_FALSE(failed.load(std::memory_order_acquire));
  EXPECT_EQ(readCount.load(), 400U);

  server.SetModel({});
  std::vector<std::uint8_t> response;
  EXPECT_EQ(server.HandleConfirmed(request, &response).error_code(),
            grpc::StatusCode::UNIMPLEMENTED);
  server.SetModel(std::move(model));
  EXPECT_TRUE(server.HandleConfirmed(request, &response).ok());
  EXPECT_EQ(readCount.load(), 401U);
}

}  // namespace
