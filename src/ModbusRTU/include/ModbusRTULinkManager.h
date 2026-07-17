#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>

#include "MQTTManager.grpc.pb.h"
#include "ModbusRTU.pb.h"
#include "ModbusRTUBus.h"
#include "ModbusRTUDataCenterClient.h"
#include "ModbusRTULinkStore.h"
#include "ModbusRTUMqttClient.h"
#include "ModbusRTUMqttBus.h"
#include "ModbusRTUMqttStore.h"
#include "ModbusRTUPointTable.h"
#include "ModbusRTUPointTableStore.h"
#include "ModbusRTUSerialBus.h"

namespace ModbusRTU {

class LinkManager {
public:
  explicit LinkManager(std::string moduleName,
                       std::filesystem::path configDbPath = std::filesystem::path("./conf/config.db"));

  void LoadPersistedConfig();

  void setDataCenterServerAddress(std::string address);
  void setDataCenterStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub);
  void setMqttStub(std::shared_ptr<MQTTManagerProto::MQTTManagerService::StubInterface> stub);

  grpc::Status UpdateConfig(const ModbusRTUProto::UpdateConfigRequest& request,
                            ModbusRTUProto::UpdateConfigResponse* response);

  grpc::Status UpsertLink(const ModbusRTUProto::UpsertLinkRequest& request, ModbusRTUProto::LinkInfo* out);
  grpc::Status RenameLink(const std::string& oldConnName, const std::string& newConnName, ModbusRTUProto::LinkInfo* out);
  grpc::Status GetLink(const std::string& connName, ModbusRTUProto::LinkInfo* out) const;
  grpc::Status ListLinks(ModbusRTUProto::ListLinksResponse* out) const;
  grpc::Status StartLink(const std::string& connName);
  grpc::Status StopLink(const std::string& connName);
  grpc::Status DeleteLink(const std::string& connName);
  void TryAutoStartReadyLinks(std::string_view trigger);

  grpc::Status UpsertPointTable(const ModbusRTUProto::UpsertPointTableRequest& request);
  grpc::Status GetPointTable(const std::string& connName, ModbusRTUProto::PointTable* out) const;
  grpc::Status ExecuteCommand(const DataCenterProto::ExecuteCommandRequest& request,
                              DataCenterProto::ExecuteCommandResponse* response);

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
    std::shared_ptr<Bus> bus;
    size_t refCount = 0;
  };

  struct CommandGate {
    std::mutex mu;
    std::condition_variable cv;
    bool accepting = false;
    size_t active = 0;
  };

  struct MqttKey {
    std::string serialPort;
    uint32_t baudRate = 0;
    uint32_t dataBits = 0;
    ModbusRTUProto::Parity parity = ModbusRTUProto::PARITY_UNSPECIFIED;
    ModbusRTUProto::StopBits stopBits = ModbusRTUProto::STOP_BITS_UNSPECIFIED;
    uint32_t requestTimeoutMs = 0;
    uint32_t byteTimeoutMs = 0;
    uint32_t frameTimeoutMs = 0;
    uint32_t estSize = 0;

    bool operator==(const MqttKey& other) const {
      return serialPort == other.serialPort &&
          baudRate == other.baudRate &&
          dataBits == other.dataBits &&
          parity == other.parity &&
          stopBits == other.stopBits &&
          requestTimeoutMs == other.requestTimeoutMs &&
          byteTimeoutMs == other.byteTimeoutMs &&
          frameTimeoutMs == other.frameTimeoutMs &&
          estSize == other.estSize;
    }
  };

  struct MqttKeyHash {
    size_t operator()(const MqttKey& key) const;
  };

  struct LinkRuntime {
    ModbusRTUProto::LinkConfig config;
    SerialKey serialKey;
    MqttKey mqttKey;
    uint32_t connId = 0;
    ModbusRTUProto::LinkState state = ModbusRTUProto::LINK_STATE_STOPPED;
    std::string lastError;
    PointTable pointTable;
    bool pointTableConfigured = false;
    std::shared_ptr<Bus> bus;
    std::shared_ptr<CommandGate> commandGate = std::make_shared<CommandGate>();
    std::jthread pollThread;
    std::shared_ptr<grpc::ClientContext> dcCommandContext;
    std::jthread dcCommandThread;
  };

  static grpc::Status validateConnName(const std::string& connName);
  static grpc::Status normalizeLinkConfig(const ModbusRTUProto::LinkConfig& config, ModbusRTUProto::LinkConfig* out);
  static SerialKey makeSerialKey(const ModbusRTUProto::SerialConfig& serial);
  static MqttKey makeMqttKey(const ModbusRTUProto::LinkConfig& config);

  grpc::Status fillLinkInfoLocked(const LinkRuntime& link, ModbusRTUProto::LinkInfo* out) const;
  ModbusRTUProto::LinksConfig dumpLinksConfigLocked() const;
  ModbusRTUProto::PointTablesConfig dumpPointTablesConfigLocked() const;
  grpc::Status saveLinksConfig(const ModbusRTUProto::LinksConfig& config);
  grpc::Status savePointTablesConfig(const ModbusRTUProto::PointTablesConfig& config);
  bool isLinkAutoStartReadyLocked(const LinkRuntime& link, std::string* reason) const;
  grpc::Status maybeAutoStartLink(const std::string& connName, std::string_view trigger);
  void autoStartEligibleLinks(std::string_view trigger);
  grpc::Status ensureSerialCompatibleLocked(const SerialKey& key, const std::string& connName) const;
  grpc::Status ensureMqttCompatibleLocked(const MqttKey& key, const std::string& connName) const;
  std::shared_ptr<Bus> acquireSerialBusLocked(const SerialKey& key, const ModbusRTUProto::SerialConfig& serial);
  std::shared_ptr<Bus> releaseSerialBusLocked(const SerialKey& key);
  std::shared_ptr<Bus> acquireMqttBusLocked(const MqttKey& key, const ModbusRTUProto::LinkConfig& config);
  std::shared_ptr<Bus> releaseMqttBusLocked(const MqttKey& key);
  void startCommandSubscribeLocked(const std::string& connName, LinkRuntime* link);
  void stopCommandSubscribeLocked(LinkRuntime* link);
  grpc::Status executeWriteCommand(const std::string& connName,
                                   const ModbusRTUProto::LinkConfig& config,
                                   const PointTable::Point& point,
                                   std::shared_ptr<Bus> bus,
                                   const DataCenterProto::PointUpdate& update);

  void pollLoop(std::string connName,
                uint32_t connId,
                ModbusRTUProto::LinkConfig config,
                PointTable pointTable,
                std::shared_ptr<Bus> bus,
                std::stop_token stopToken);
  void updateLastError(const std::string& connName, const std::string& error);

  mutable std::mutex mu_;
  std::unordered_map<std::string, LinkRuntime> linksByName_;
  std::unordered_map<SerialKey, BusEntry, SerialKeyHash> buses_;
  std::unordered_map<MqttKey, BusEntry, MqttKeyHash> mqttBuses_;
  std::unordered_set<std::string> pendingCreateByName_;
  DataCenterClient dataCenter_;
  MqttClient mqttClient_;
  ModbusRTUMqttStore mqttStore_;
  ModbusRTULinkStore linkStore_;
  ModbusRTUPointTableStore pointTableStore_;
};

}  // namespace ModbusRTU
