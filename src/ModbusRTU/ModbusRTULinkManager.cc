#include "ModbusRTULinkManager.h"

#include <chrono>
#include <format>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Logger.h"

namespace ModbusRTU {
namespace {
constexpr uint32_t kDefaultBaudRate = 9600;
constexpr uint32_t kDefaultDataBits = 8;
constexpr uint32_t kDefaultReadTimeoutMs = 1000;
constexpr uint32_t kDefaultPollIntervalMs = 1000;

grpc::Status makeNotFound(const std::string& connName) {
  return grpc::Status(grpc::StatusCode::NOT_FOUND, std::format("link not found: {}", connName));
}
}  // namespace

LinkManager::LinkManager(std::string moduleName) :
  dataCenter_(std::move(moduleName)) {}

void LinkManager::setDataCenterServerAddress(std::string address) {
  dataCenter_.setServerAddress(std::move(address));
}

void LinkManager::setDataCenterStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub) {
  dataCenter_.setStub(std::move(stub));
}

size_t LinkManager::SerialKeyHash::operator()(const SerialKey& key) const {
  std::hash<std::string> strHash;
  size_t seed = strHash(key.device);
  seed ^= static_cast<size_t>(key.baudRate) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  seed ^= static_cast<size_t>(key.dataBits) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  seed ^= static_cast<size_t>(key.parity) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  seed ^= static_cast<size_t>(key.stopBits) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  seed ^= static_cast<size_t>(key.readTimeoutMs) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  return seed;
}

grpc::Status LinkManager::validateConnName(const std::string& connName) {
  if (connName.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_name is required");
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::normalizeLinkConfig(const ModbusRTUProto::LinkConfig& config, ModbusRTUProto::LinkConfig* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
  }
  if (config.conn_name().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_name is required");
  }
  if (!config.has_serial()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "serial config is required");
  }
  *out = config;
  auto* serial = out->mutable_serial();
  if (serial->device().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "serial.device is required");
  }
  if (serial->baud_rate() == 0) {
    serial->set_baud_rate(kDefaultBaudRate);
  }
  if (serial->data_bits() == 0) {
    serial->set_data_bits(kDefaultDataBits);
  }
  if (serial->parity() == ModbusRTUProto::PARITY_UNSPECIFIED) {
    serial->set_parity(ModbusRTUProto::PARITY_NONE);
  }
  if (serial->stop_bits() == ModbusRTUProto::STOP_BITS_UNSPECIFIED) {
    serial->set_stop_bits(ModbusRTUProto::STOP_BITS_ONE);
  }
  if (serial->read_timeout_ms() == 0) {
    serial->set_read_timeout_ms(kDefaultReadTimeoutMs);
  }
  if (out->poll_interval_ms() == 0) {
    out->set_poll_interval_ms(kDefaultPollIntervalMs);
  }
  if (out->address_base() == ModbusRTUProto::ADDRESS_BASE_UNSPECIFIED) {
    out->set_address_base(ModbusRTUProto::ADDRESS_BASE_ZERO);
  }

  if (serial->data_bits() < 5 || serial->data_bits() > 8) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "serial.data_bits must be in [5,8]");
  }
  if (serial->parity() != ModbusRTUProto::PARITY_NONE &&
      serial->parity() != ModbusRTUProto::PARITY_ODD &&
      serial->parity() != ModbusRTUProto::PARITY_EVEN) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "serial.parity is invalid");
  }
  if (serial->stop_bits() != ModbusRTUProto::STOP_BITS_ONE &&
      serial->stop_bits() != ModbusRTUProto::STOP_BITS_TWO) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "serial.stop_bits is invalid");
  }
  if (serial->read_timeout_ms() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "serial.read_timeout_ms is required");
  }
  if (out->poll_interval_ms() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "poll_interval_ms is required");
  }
  if (out->address_base() != ModbusRTUProto::ADDRESS_BASE_ZERO &&
      out->address_base() != ModbusRTUProto::ADDRESS_BASE_ONE) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "address_base is invalid");
  }
  if (out->slave_id() == 0 || out->slave_id() > 247) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "slave_id must be in [1,247]");
  }
  return grpc::Status::OK;
}

LinkManager::SerialKey LinkManager::makeSerialKey(const ModbusRTUProto::SerialConfig& serial) {
  SerialKey key;
  key.device = serial.device();
  key.baudRate = serial.baud_rate();
  key.dataBits = serial.data_bits();
  key.parity = serial.parity();
  key.stopBits = serial.stop_bits();
  key.readTimeoutMs = serial.read_timeout_ms();
  return key;
}

grpc::Status LinkManager::fillLinkInfoLocked(const LinkRuntime& link, ModbusRTUProto::LinkInfo* out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
  }
  out->Clear();
  *out->mutable_config() = link.config;
  out->set_conn_id(link.connId);
  out->set_state(link.state);
  out->set_last_error(link.lastError);
  return grpc::Status::OK;
}

grpc::Status LinkManager::ensureSerialCompatibleLocked(const SerialKey& key, const std::string& connName) const {
  for (const auto& [name, link] : linksByName_) {
    if (name == connName) {
      continue;
    }
    if (link.serialKey.device == key.device && !(link.serialKey == key)) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "serial config conflicts with existing link");
    }
  }
  return grpc::Status::OK;
}

std::shared_ptr<SerialBus> LinkManager::acquireBusLocked(const SerialKey& key, const ModbusRTUProto::SerialConfig& serial) {
  auto it = buses_.find(key);
  if (it != buses_.end()) {
    it->second.refCount += 1;
    return it->second.bus;
  }
  auto bus = std::make_shared<SerialBus>(serial);
  BusEntry entry;
  entry.bus = bus;
  entry.refCount = 1;
  buses_.emplace(key, std::move(entry));
  return bus;
}

std::shared_ptr<SerialBus> LinkManager::releaseBusLocked(const SerialKey& key) {
  auto it = buses_.find(key);
  if (it == buses_.end()) {
    return nullptr;
  }
  if (it->second.refCount > 0) {
    it->second.refCount -= 1;
  }
  if (it->second.refCount == 0) {
    auto bus = it->second.bus;
    buses_.erase(it);
    return bus;
  }
  return nullptr;
}

grpc::Status LinkManager::UpsertLink(const ModbusRTUProto::UpsertLinkRequest& request, ModbusRTUProto::LinkInfo* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
  }
  ModbusRTUProto::LinkConfig normalized;
  auto status = normalizeLinkConfig(request.config(), &normalized);
  if (!status.ok()) {
    return status;
  }

  const auto connName = normalized.conn_name();
  const auto serialKey = makeSerialKey(normalized.serial());

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it != linksByName_.end()) {
      if (request.create_only()) {
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name already exists");
      }
      if (it->second.state == ModbusRTUProto::LINK_STATE_RUNNING) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "stop link before updating config");
      }
      if (it->second.state == ModbusRTUProto::LINK_STATE_PENDING_DELETE) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "link is pending delete");
      }
      status = ensureSerialCompatibleLocked(serialKey, connName);
      if (!status.ok()) {
        return status;
      }

      it->second.config = normalized;
      it->second.serialKey = serialKey;
      it->second.lastError.clear();
      return fillLinkInfoLocked(it->second, out);
    }

    if (pendingCreateByName_.contains(connName)) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name already exists");
    }
    status = ensureSerialCompatibleLocked(serialKey, connName);
    if (!status.ok()) {
      return status;
    }
    pendingCreateByName_.insert(connName);
  }

  auto rollbackPendingCreate = [this, &connName]() {
    std::lock_guard<std::mutex> lock(mu_);
    pendingCreateByName_.erase(connName);
  };

  if (request.create_only()) {
    bool exists = false;
    status = dataCenter_.ConnectionExists(connName, &exists);
    if (!status.ok()) {
      rollbackPendingCreate();
      LOG_ERROR("ModbusRTU 查询 DataCenter 连接失败: conn_name={}, 原因={}", connName, status.error_message());
      return status;
    }
    if (exists) {
      rollbackPendingCreate();
      LOG_WARNING("ModbusRTU 创建连接失败: conn_name={} 已存在于 DataCenter", connName);
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name already exists");
    }
  }

  DataCenterProto::ConnectionInfo connInfo;
  status = dataCenter_.GetOrCreateConnection(connName, &connInfo);
  if (!status.ok()) {
    rollbackPendingCreate();
    LOG_ERROR("ModbusRTU 获取 DataCenter 连接失败: conn_name={}, 原因={}", connName, status.error_message());
    return status;
  }
  if (connInfo.conn_id() == 0) {
    rollbackPendingCreate();
    LOG_ERROR("ModbusRTU 获取到的 DataCenter conn_id 无效: conn_name={}", connName);
    return grpc::Status(grpc::StatusCode::INTERNAL, "DataCenter returned conn_id=0");
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    pendingCreateByName_.erase(connName);
    LinkRuntime link;
    link.config = normalized;
    link.serialKey = serialKey;
    link.connId = connInfo.conn_id();
    link.state = ModbusRTUProto::LINK_STATE_STOPPED;
    link.lastError.clear();
    auto [it, inserted] = linksByName_.emplace(connName, std::move(link));
    if (!inserted) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name already exists");
    }
    return fillLinkInfoLocked(it->second, out);
  }
}

grpc::Status LinkManager::GetLink(const std::string& connName, ModbusRTUProto::LinkInfo* out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
  }
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(connName);
  if (it == linksByName_.end()) {
    return makeNotFound(connName);
  }
  return fillLinkInfoLocked(it->second, out);
}

grpc::Status LinkManager::ListLinks(ModbusRTUProto::ListLinksResponse* out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
  }
  std::lock_guard<std::mutex> lock(mu_);
  out->Clear();
  for (const auto& [_, link] : linksByName_) {
    auto* elem = out->add_links();
    fillLinkInfoLocked(link, elem);
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::StartLink(const std::string& connName) {
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }

  ModbusRTUProto::LinkConfig config;
  PointTable pointTable;
  uint32_t connId = 0;
  SerialKey serialKey;
  std::shared_ptr<SerialBus> bus;

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      return makeNotFound(connName);
    }
    auto& link = it->second;
    if (link.state == ModbusRTUProto::LINK_STATE_PENDING_DELETE) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "link is pending delete");
    }
    if (link.state == ModbusRTUProto::LINK_STATE_RUNNING) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "link already running");
    }
    config = link.config;
    pointTable = link.pointTable;
    connId = link.connId;
    serialKey = link.serialKey;
  }

  if (config.address_base() == ModbusRTUProto::ADDRESS_BASE_ONE) {
    for (const auto& point : pointTable.Points()) {
      if (point.address == 0) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "address_base=ONE requires address >= 1");
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    bus = acquireBusLocked(serialKey, config.serial());
  }
  status = bus->Open();
  if (!status.ok()) {
    std::lock_guard<std::mutex> lock(mu_);
    auto released = releaseBusLocked(serialKey);
    if (released) {
      released->Close();
    }
    updateLastError(connName, status.error_message());
    return status;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      auto released = releaseBusLocked(serialKey);
      if (released) {
        released->Close();
      }
      return makeNotFound(connName);
    }
    auto& link = it->second;
    if (link.state == ModbusRTUProto::LINK_STATE_PENDING_DELETE) {
      auto released = releaseBusLocked(serialKey);
      if (released) {
        released->Close();
      }
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "link is pending delete");
    }
    link.bus = bus;
    link.state = ModbusRTUProto::LINK_STATE_RUNNING;
    link.lastError.clear();
    link.pollThread = std::jthread(
        [this, connName, connId, config, pointTable, bus](std::stop_token stopToken) {
          pollLoop(connName, connId, config, pointTable, bus, stopToken);
        });
  }

  LOG_INFO("ModbusRTU 已启动轮询: conn_name={}, slave_id={}, device={}", connName, config.slave_id(), config.serial().device());
  return grpc::Status::OK;
}

grpc::Status LinkManager::StopLink(const std::string& connName) {
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }

  std::jthread pollThread;
  SerialKey serialKey;
  std::shared_ptr<SerialBus> bus;
  bool pendingDelete = false;

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      return makeNotFound(connName);
    }
    pendingDelete = (it->second.state == ModbusRTUProto::LINK_STATE_PENDING_DELETE);
    pollThread = std::move(it->second.pollThread);
    serialKey = it->second.serialKey;
    bus = it->second.bus;
    it->second.bus.reset();
    it->second.state = pendingDelete ? ModbusRTUProto::LINK_STATE_PENDING_DELETE : ModbusRTUProto::LINK_STATE_STOPPED;
  }

  if (pollThread.joinable()) {
    pollThread.request_stop();
  }

  if (bus) {
    std::shared_ptr<SerialBus> released;
    {
      std::lock_guard<std::mutex> lock(mu_);
      released = releaseBusLocked(serialKey);
    }
    if (released) {
      released->Close();
    }
  }

  LOG_INFO("ModbusRTU 已停止轮询: conn_name={}", connName);
  return grpc::Status::OK;
}

grpc::Status LinkManager::DeleteLink(const std::string& connName) {
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }

  status = StopLink(connName);
  if (!status.ok() && status.error_code() != grpc::StatusCode::NOT_FOUND) {
    return status;
  }

  grpc::Status dc = dataCenter_.DeleteConnection(connName);
  if (!dc.ok() && dc.error_code() != grpc::StatusCode::NOT_FOUND) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it != linksByName_.end()) {
      it->second.state = ModbusRTUProto::LINK_STATE_PENDING_DELETE;
      it->second.lastError = dc.error_message();
    }
    return dc;
  }

  std::lock_guard<std::mutex> lock(mu_);
  linksByName_.erase(connName);
  return grpc::Status::OK;
}

grpc::Status LinkManager::UpsertPointTable(const ModbusRTUProto::UpsertPointTableRequest& request) {
  auto status = validateConnName(request.conn_name());
  if (!status.ok()) {
    return status;
  }

  uint32_t connId = 0;
  PointTable current;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(request.conn_name());
    if (it == linksByName_.end()) {
      return makeNotFound(request.conn_name());
    }
    if (it->second.state == ModbusRTUProto::LINK_STATE_RUNNING) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "stop link before updating point table");
    }
    if (it->second.state == ModbusRTUProto::LINK_STATE_PENDING_DELETE) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "link is pending delete");
    }
    connId = it->second.connId;
    current = it->second.pointTable;
  }

  PointTable next = current;
  status = next.Upsert(request.points(), request.replace());
  if (!status.ok()) {
    return status;
  }

  auto tags = next.Tags();
  status = dataCenter_.UpsertPointTable(connId, tags, true);
  if (!status.ok()) {
    return status;
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(request.conn_name());
  if (it == linksByName_.end()) {
    return makeNotFound(request.conn_name());
  }
  it->second.pointTable = std::move(next);
  return grpc::Status::OK;
}

grpc::Status LinkManager::GetPointTable(const std::string& connName, ModbusRTUProto::PointTable* out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
  }
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(connName);
  if (it == linksByName_.end()) {
    return makeNotFound(connName);
  }
  it->second.pointTable.ToProto(connName, out);
  return grpc::Status::OK;
}

void LinkManager::pollLoop(std::string connName,
                           uint32_t connId,
                           ModbusRTUProto::LinkConfig config,
                           PointTable pointTable,
                           std::shared_ptr<SerialBus> bus,
                           std::stop_token stopToken) {
  const auto interval = std::chrono::milliseconds(config.poll_interval_ms());
  const auto points = pointTable.Points();
  LOG_INFO("ModbusRTU 轮询开始: conn_name={}, points={}, interval={}ms", connName, points.size(), config.poll_interval_ms());

  while (!stopToken.stop_requested()) {
    for (const auto& point : points) {
      if (stopToken.stop_requested()) {
        break;
      }

      uint32_t address = point.address;
      if (config.address_base() == ModbusRTUProto::ADDRESS_BASE_ONE) {
        if (address == 0) {
          updateLastError(connName, "address_base=ONE but address is 0");
          LOG_WARNING("ModbusRTU 点表地址非法: conn_name={}, tag={}, address=0", connName, point.tag);
          continue;
        }
        address -= 1;
      }

      grpc::Status status;
      if (point.function == ModbusRTUProto::FUNCTION_READ_COILS) {
        bool value = false;
        status = bus->ReadCoil(static_cast<uint8_t>(config.slave_id()), static_cast<uint16_t>(address), &value);
        if (status.ok()) {
          auto dc = dataCenter_.PublishBool(connId, point.tag, value, DataCenterProto::QUALITY_GOOD, 0);
          if (!dc.ok()) {
            updateLastError(connName, dc.error_message());
            LOG_ERROR("ModbusRTU 发布点值失败: conn_name={}, tag={}, 原因={}", connName, point.tag, dc.error_message());
          }
        }
      } else if (point.function == ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS) {
        uint16_t value = 0;
        status = bus->ReadHoldingRegister(static_cast<uint8_t>(config.slave_id()), static_cast<uint16_t>(address), &value);
        if (status.ok()) {
          auto dc = dataCenter_.PublishUInt16(connId, point.tag, value, DataCenterProto::QUALITY_GOOD, 0);
          if (!dc.ok()) {
            updateLastError(connName, dc.error_message());
            LOG_ERROR("ModbusRTU 发布点值失败: conn_name={}, tag={}, 原因={}", connName, point.tag, dc.error_message());
          }
        }
      } else {
        status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "unsupported function");
      }

      if (!status.ok()) {
        updateLastError(connName, status.error_message());
        LOG_WARNING("ModbusRTU 轮询点失败: conn_name={}, tag={}, 原因={}", connName, point.tag, status.error_message());
      }
    }

    if (stopToken.stop_requested()) {
      break;
    }
    std::this_thread::sleep_for(interval);
  }

  LOG_INFO("ModbusRTU 轮询结束: conn_name={}", connName);
}

void LinkManager::updateLastError(const std::string& connName, const std::string& error) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(connName);
  if (it == linksByName_.end()) {
    return;
  }
  it->second.lastError = error;
}

}  // namespace ModbusRTU
