#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>

#include "DataCenter.pb.h"
#include "ModbusRTUDataCenterClient.h"
#include "ModbusRTUPointTable.h"
#include "ModbusTCP.pb.h"
#include "ModbusTCPPointTableStore.h"
#include "ModbusTCPLinkStore.h"
#include "ModbusTCPTcpBus.h"

namespace ModbusTCP {
class LinkManager {
public:
  explicit LinkManager(std::filesystem::path db = "./conf/config.db");
  ~LinkManager();
  void setDataCenterStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub);
  void LoadPersistedConfig();
  grpc::Status UpsertLink(const ModbusTCPProto::UpsertLinkRequest& request, ModbusTCPProto::LinkInfo* out);
  grpc::Status RenameLink(const ModbusTCPProto::RenameLinkRequest& request, ModbusTCPProto::LinkInfo* out);
  grpc::Status GetLink(const std::string& name, ModbusTCPProto::LinkInfo* out) const;
  grpc::Status ListLinks(ModbusTCPProto::ListLinksResponse* out) const;
  grpc::Status DeleteLink(const std::string& name);
  grpc::Status StartLink(const std::string& name);
  grpc::Status StopLink(const std::string& name);
  grpc::Status UpsertPointTable(const ModbusTCPProto::UpsertPointTableRequest& request);
  grpc::Status GetPointTable(const std::string& name, ModbusTCPProto::PointTable* out) const;
  grpc::Status ExecuteCommand(const DataCenterProto::ExecuteCommandRequest& request, DataCenterProto::ExecuteCommandResponse* response);

private:
  struct Link {
    ModbusTCPProto::LinkConfig config;
    uint32_t connId = 0;
    ModbusTCPProto::LinkState state = ModbusTCPProto::LINK_STATE_STOPPED;
    std::string lastError;
    ModbusRTU::PointTable pointTable;
    bool pointTableConfigured = false;
    std::shared_ptr<TcpBus> bus;
    std::jthread pollThread;
  };

  static grpc::Status normalize(const ModbusTCPProto::LinkConfig& in, ModbusTCPProto::LinkConfig* out);
  grpc::Status fillInfoLocked(const Link& link, ModbusTCPProto::LinkInfo* out) const;
  grpc::Status saveLinksLocked();
  grpc::Status savePointTablesLocked();
  void pollLoop(std::string name, uint32_t connId, ModbusTCPProto::LinkConfig config, ModbusRTU::PointTable table, std::shared_ptr<TcpBus> bus, std::stop_token stop);
  grpc::Status executePointWrite(const Link& link, const ModbusRTU::PointTable::Point& point, const DataCenterProto::PointValue& value, DataCenterProto::ExecuteCommandResponse* response);

  mutable std::mutex mu_;
  std::unordered_map<std::string, Link> links_;
  ModbusRTU::DataCenterClient dataCenter_;
  LinkStore linkStore_;
  PointTableStore pointTableStore_;
};
}
