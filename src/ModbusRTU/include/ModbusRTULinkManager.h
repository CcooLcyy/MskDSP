#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>

#include "ModbusRTU.pb.h"
#include "ModbusRTUDataCenterClient.h"
#include "ModbusRTUPointTable.h"
#include "ModbusRTUSerialBus.h"

namespace ModbusRTU {

class LinkManager {
public:
  explicit LinkManager(std::string moduleName);

  void setDataCenterServerAddress(std::string address);
  void setDataCenterStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub);

  grpc::Status UpsertLink(const ModbusRTUProto::UpsertLinkRequest& request, ModbusRTUProto::LinkInfo* out);
  grpc::Status GetLink(const std::string& connName, ModbusRTUProto::LinkInfo* out) const;
  grpc::Status ListLinks(ModbusRTUProto::ListLinksResponse* out) const;
  grpc::Status StartLink(const std::string& connName);
  grpc::Status StopLink(const std::string& connName);
  grpc::Status DeleteLink(const std::string& connName);

  grpc::Status UpsertPointTable(const ModbusRTUProto::UpsertPointTableRequest& request);
  grpc::Status GetPointTable(const std::string& connName, ModbusRTUProto::PointTable* out) const;

private:
  struct SerialKey {
    std::string device;
    uint32_t baudRate = 0;
    uint32_t dataBits = 0;
    ModbusRTUProto::Parity parity = ModbusRTUProto::PARITY_UNSPECIFIED;
    ModbusRTUProto::StopBits stopBits = ModbusRTUProto::STOP_BITS_UNSPECIFIED;
    uint32_t readTimeoutMs = 0;

    bool operator==(const SerialKey& other) const {
      return device == other.device &&
          baudRate == other.baudRate &&
          dataBits == other.dataBits &&
          parity == other.parity &&
          stopBits == other.stopBits &&
          readTimeoutMs == other.readTimeoutMs;
    }
  };

  struct SerialKeyHash {
    size_t operator()(const SerialKey& key) const;
  };

  struct BusEntry {
    std::shared_ptr<SerialBus> bus;
    size_t refCount = 0;
  };

  struct LinkRuntime {
    ModbusRTUProto::LinkConfig config;
    SerialKey serialKey;
    uint32_t connId = 0;
    ModbusRTUProto::LinkState state = ModbusRTUProto::LINK_STATE_STOPPED;
    std::string lastError;
    PointTable pointTable;
    std::shared_ptr<SerialBus> bus;
    std::jthread pollThread;
  };

  static grpc::Status validateConnName(const std::string& connName);
  static grpc::Status normalizeLinkConfig(const ModbusRTUProto::LinkConfig& config, ModbusRTUProto::LinkConfig* out);
  static SerialKey makeSerialKey(const ModbusRTUProto::SerialConfig& serial);

  grpc::Status fillLinkInfoLocked(const LinkRuntime& link, ModbusRTUProto::LinkInfo* out) const;
  grpc::Status ensureSerialCompatibleLocked(const SerialKey& key, const std::string& connName) const;
  std::shared_ptr<SerialBus> acquireBusLocked(const SerialKey& key, const ModbusRTUProto::SerialConfig& serial);
  std::shared_ptr<SerialBus> releaseBusLocked(const SerialKey& key);

  void pollLoop(std::string connName,
                uint32_t connId,
                ModbusRTUProto::LinkConfig config,
                PointTable pointTable,
                std::shared_ptr<SerialBus> bus,
                std::stop_token stopToken);
  void updateLastError(const std::string& connName, const std::string& error);

  mutable std::mutex mu_;
  std::unordered_map<std::string, LinkRuntime> linksByName_;
  std::unordered_map<SerialKey, BusEntry, SerialKeyHash> buses_;
  std::unordered_set<std::string> pendingCreateByName_;
  DataCenterClient dataCenter_;
};

}  // namespace ModbusRTU
