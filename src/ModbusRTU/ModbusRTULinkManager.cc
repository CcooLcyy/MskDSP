#include "ModbusRTULinkManager.h"

#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Logger.h"
#include "ModbusRTULibInfo.h"
#include "ThreadUtil.hpp"

namespace ModbusRTU {
namespace {
constexpr uint32_t kDefaultBaudRate = 9600;
constexpr uint32_t kDefaultDataBits = 8;
constexpr uint32_t kDefaultReadTimeoutMs = 1000;
constexpr uint32_t kDefaultPollIntervalMs = 1000;
constexpr uint16_t kMaxReadCoilsQuantity = 2000;
constexpr uint16_t kMaxReadHoldingRegistersQuantity = 125;
constexpr uint8_t kFunctionReadCoils = 0x01;
constexpr uint8_t kFunctionReadHoldingRegisters = 0x03;
constexpr uint8_t kExceptionIllegalFunction = 0x01;
constexpr uint8_t kExceptionIllegalDataAddress = 0x02;
constexpr uint8_t kExceptionIllegalDataValue = 0x03;
constexpr uint8_t kExceptionSlaveDeviceFailure = 0x04;

uint16_t swapWordBytes(uint16_t value) {
  return static_cast<uint16_t>((value << 8) | (value >> 8));
}

uint32_t decodeUint32(uint16_t first,
                      uint16_t second,
                      ModbusRTUProto::WordOrder wordOrder,
                      ModbusRTUProto::ByteOrder byteOrder) {
  if (byteOrder == ModbusRTUProto::BYTE_ORDER_BA) {
    first = swapWordBytes(first);
    second = swapWordBytes(second);
  }
  if (wordOrder == ModbusRTUProto::WORD_ORDER_LH) {
    std::swap(first, second);
  }
  return (static_cast<uint32_t>(first) << 16) | static_cast<uint32_t>(second);
}

std::array<uint16_t, 2> encodeUint32(uint32_t value,
                                     ModbusRTUProto::WordOrder wordOrder,
                                     ModbusRTUProto::ByteOrder byteOrder) {
  uint16_t high = static_cast<uint16_t>((value >> 16) & 0xFFFF);
  uint16_t low = static_cast<uint16_t>(value & 0xFFFF);
  uint16_t first = (wordOrder == ModbusRTUProto::WORD_ORDER_LH) ? low : high;
  uint16_t second = (wordOrder == ModbusRTUProto::WORD_ORDER_LH) ? high : low;
  if (byteOrder == ModbusRTUProto::BYTE_ORDER_BA) {
    first = swapWordBytes(first);
    second = swapWordBytes(second);
  }
  return {first, second};
}

bool shouldReport(double value, double deadband, const std::optional<double>& last) {
  if (deadband <= 0 || !last.has_value()) {
    return true;
  }
  return std::fabs(value - last.value()) >= deadband;
}

grpc::Status makeNotFound(const std::string& connName) {
  return grpc::Status(grpc::StatusCode::NOT_FOUND, std::format("未找到链路: {}", connName));
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
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_name 不能为空");
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::normalizeLinkConfig(const ModbusRTUProto::LinkConfig& config, ModbusRTUProto::LinkConfig* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  if (config.conn_name().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_name 不能为空");
  }
  if (!config.has_serial()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "serial 配置不能为空");
  }
  *out = config;
  auto* serial = out->mutable_serial();
  if (serial->device().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "serial.device 不能为空");
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
  if (out->mode() == ModbusRTUProto::LINK_MODE_UNSPECIFIED) {
    out->set_mode(ModbusRTUProto::LINK_MODE_MASTER);
  }

  if (serial->data_bits() < 5 || serial->data_bits() > 8) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "serial.data_bits 必须在 [5,8] 范围内");
  }
  if (serial->parity() != ModbusRTUProto::PARITY_NONE &&
      serial->parity() != ModbusRTUProto::PARITY_ODD &&
      serial->parity() != ModbusRTUProto::PARITY_EVEN) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "serial.parity 非法");
  }
  if (serial->stop_bits() != ModbusRTUProto::STOP_BITS_ONE &&
      serial->stop_bits() != ModbusRTUProto::STOP_BITS_TWO) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "serial.stop_bits 非法");
  }
  if (serial->read_timeout_ms() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "serial.read_timeout_ms 不能为空");
  }
  if (out->poll_interval_ms() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "poll_interval_ms 不能为空");
  }
  if (out->address_base() != ModbusRTUProto::ADDRESS_BASE_ZERO &&
      out->address_base() != ModbusRTUProto::ADDRESS_BASE_ONE) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "address_base 非法");
  }
  if (out->mode() != ModbusRTUProto::LINK_MODE_MASTER &&
      out->mode() != ModbusRTUProto::LINK_MODE_SLAVE) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "link mode 非法");
  }
  if (out->slave_id() == 0 || out->slave_id() > 247) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "slave_id 必须在 [1,247] 范围内");
  }
  if (out->has_read_plan()) {
    auto *plan = out->mutable_read_plan();
    if (plan->mode() == ModbusRTUProto::READ_PLAN_MODE_UNSPECIFIED) {
      plan->set_mode(plan->blocks_size() > 0 ? ModbusRTUProto::READ_PLAN_MODE_EXPLICIT
                                             : ModbusRTUProto::READ_PLAN_MODE_POINT);
    }
    if (plan->mode() == ModbusRTUProto::READ_PLAN_MODE_EXPLICIT) {
      if (out->mode() != ModbusRTUProto::LINK_MODE_MASTER) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "read_plan 仅支持主站模式");
      }
      if (plan->blocks_size() == 0) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "read_plan.blocks 不能为空");
      }
      for (const auto &block : plan->blocks()) {
        if (block.function() == ModbusRTUProto::FUNCTION_UNSPECIFIED) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "read_plan.blocks.function 不能为空");
        }
        if (block.function() != ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "read_plan 仅支持读保持寄存器(0x03)");
        }
        if (block.quantity() == 0) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "read_plan.blocks.quantity 不能为空");
        }
        if (block.quantity() > kMaxReadHoldingRegistersQuantity) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "read_plan.blocks.quantity 超出上限(125)");
        }
        uint32_t start = block.start();
        if (out->address_base() == ModbusRTUProto::ADDRESS_BASE_ONE) {
          if (start == 0) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "read_plan.blocks.start 不能为 0（address_base=ONE）");
          }
          start -= 1;
        }
        if (start > 0xFFFFu || start + block.quantity() - 1 > 0xFFFFu) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "read_plan.blocks 超出地址范围");
        }
      }
    } else if (plan->mode() == ModbusRTUProto::READ_PLAN_MODE_POINT) {
      if (plan->blocks_size() > 0) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "read_plan.mode=POINT 时 blocks 必须为空");
      }
    } else {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "read_plan.mode 非法");
    }
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
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  out->Clear();
  *out->mutable_config() = link.config;
  out->set_conn_id(link.connId);
  out->set_state(link.state);
  out->set_last_error(link.lastError);
  return grpc::Status::OK;
}

grpc::Status LinkManager::ensureSerialCompatibleLocked(
    const SerialKey& key, const std::string& connName, ModbusRTUProto::LinkMode mode) const {
  for (const auto& [name, link] : linksByName_) {
    if (name == connName) {
      continue;
    }
    if (link.serialKey.device != key.device) {
      continue;
    }
    if (!(link.serialKey == key)) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "串口配置与现有链路冲突");
    }
    if (link.config.mode() != mode) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "串口链路模式与现有链路冲突");
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

grpc::Status LinkManager::startSlaveLink(const std::string& connName,
                                         const ModbusRTUProto::LinkConfig& config,
                                         const PointTable& pointTable,
                                         uint32_t connId,
                                         const SerialKey& serialKey,
                                         std::shared_ptr<SerialBus> bus) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(connName);
  if (it == linksByName_.end()) {
    return makeNotFound(connName);
  }
  if (it->second.state == ModbusRTUProto::LINK_STATE_PENDING_DELETE) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路处于待删除状态");
  }
  if (it->second.state == ModbusRTUProto::LINK_STATE_RUNNING) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路已在运行");
  }

  auto& slaveBus = slaveBuses_[serialKey];
  if (!slaveBus.bus) {
    slaveBus.bus = bus;
  }
  const auto slaveId = static_cast<uint8_t>(config.slave_id());
  if (slaveBus.linksBySlaveId.contains(slaveId)) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "该串口上的 slave_id 已在运行");
  }
  auto snapshot = std::make_shared<SlaveLinkSnapshot>();
  snapshot->connName = connName;
  snapshot->config = config;
  snapshot->connId = connId;
  snapshot->pointTable = pointTable;
  slaveBus.linksBySlaveId.emplace(slaveId, std::move(snapshot));

  if (!slaveBus.worker.joinable()) {
    slaveBus.worker = ModuleManager::StartModuleThread(
        ModbusRTULibInfo.LIB_NAME,
        [this, serialKey, bus](std::stop_token stopToken) { slaveLoop(serialKey, bus, stopToken); });
  }

  it->second.bus = bus;
  it->second.state = ModbusRTUProto::LINK_STATE_RUNNING;
  it->second.lastError.clear();
  return grpc::Status::OK;
}

grpc::Status LinkManager::stopSlaveLink(const std::string& connName,
                                        const ModbusRTUProto::LinkConfig& config,
                                        const SerialKey& serialKey,
                                        std::shared_ptr<SerialBus> bus) {
  std::jthread worker;
  std::shared_ptr<SerialBus> releasedBus;
  bool shouldStopWorker = false;
  bool removed = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = slaveBuses_.find(serialKey);
    if (it != slaveBuses_.end()) {
      const auto slaveId = static_cast<uint8_t>(config.slave_id());
      removed = (it->second.linksBySlaveId.erase(slaveId) > 0);
      if (it->second.linksBySlaveId.empty()) {
        worker = std::move(it->second.worker);
        slaveBuses_.erase(it);
        shouldStopWorker = true;
      }
    }
    if (bus) {
      releasedBus = releaseBusLocked(serialKey);
    }
  }

  if (!removed) {
    LOG_DEBUG("ModbusRTU 从站响应未找到: conn_name={}", connName);
  }
  if (shouldStopWorker && worker.joinable()) {
    worker.request_stop();
    worker.join();
  }

  if (releasedBus) {
    releasedBus->Close();
  }

  return grpc::Status::OK;
}

void LinkManager::slaveLoop(SerialKey serialKey, std::shared_ptr<SerialBus> bus, std::stop_token stopToken) {
  LOG_INFO("ModbusRTU 从站监听启动: device={}", serialKey.device);
  while (!stopToken.stop_requested()) {
    SerialBus::RtuRequest request;
    auto status = bus->ReadRequest(&request);
    if (!status.ok()) {
      if (status.error_code() != grpc::StatusCode::DEADLINE_EXCEEDED) {
        LOG_WARNING("ModbusRTU 从站读取请求失败: device={}, 原因={}", serialKey.device, status.error_message());
      }
      continue;
    }

    if (stopToken.stop_requested()) {
      break;
    }

    if (request.slaveId == 0) {
      LOG_DEBUG("ModbusRTU 从站忽略广播请求: device={}", serialKey.device);
      continue;
    }

    std::shared_ptr<SlaveLinkSnapshot> link;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto busIt = slaveBuses_.find(serialKey);
      if (busIt == slaveBuses_.end()) {
        continue;
      }
      auto it = busIt->second.linksBySlaveId.find(request.slaveId);
      if (it == busIt->second.linksBySlaveId.end()) {
        LOG_DEBUG("ModbusRTU 未匹配到从站地址: device={}, slave_id={}", serialKey.device, request.slaveId);
        continue;
      }
      link = it->second;
    }

    auto sendException = [&](uint8_t exceptionCode, const char* reason) {
      std::vector<uint8_t> resp;
      resp.reserve(5);
      resp.push_back(request.slaveId);
      resp.push_back(static_cast<uint8_t>(request.function | 0x80));
      resp.push_back(exceptionCode);
      SerialBus::appendCrc(&resp);
      auto sendStatus = bus->WriteFrame(resp);
      if (!sendStatus.ok()) {
        LOG_ERROR("ModbusRTU 从站异常响应发送失败: conn_name={}, 原因={}", link->connName, sendStatus.error_message());
        updateLastError(link->connName, sendStatus.error_message());
      } else {
        LOG_WARNING("ModbusRTU 从站返回异常: conn_name={}, 功能码=0x{:02X}, 异常码=0x{:02X}, 原因={}",
                    link->connName,
                    static_cast<unsigned int>(request.function),
                    static_cast<unsigned int>(exceptionCode),
                    reason);
        updateLastError(link->connName, reason);
      }
    };

    if (request.function == kFunctionReadCoils) {
      if (request.quantity == 0 || request.quantity > kMaxReadCoilsQuantity) {
        sendException(kExceptionIllegalDataValue, "线圈数量非法");
        continue;
      }
      if (static_cast<uint32_t>(request.address) + request.quantity - 1 > 0xFFFF) {
        sendException(kExceptionIllegalDataAddress, "线圈地址超出范围");
        continue;
      }

      struct CoilSlot {
        bool hasPoint = false;
        std::string tag;
        std::optional<bool> defaultValue;
      };
      std::vector<CoilSlot> slots;
      slots.resize(request.quantity);
      std::vector<std::string> tags;
      tags.reserve(request.quantity);

      bool addressOverflow = false;
      for (uint16_t i = 0; i < request.quantity; ++i) {
        uint32_t reqAddr = static_cast<uint32_t>(request.address) + i;
        uint32_t lookupAddr = reqAddr;
        if (link->config.address_base() == ModbusRTUProto::ADDRESS_BASE_ONE) {
          if (reqAddr == 0xFFFF) {
            addressOverflow = true;
            break;
          }
          lookupAddr = reqAddr + 1;
        }
        auto point = link->pointTable.FindByAddress(ModbusRTUProto::FUNCTION_READ_COILS, lookupAddr);
        if (!point.has_value()) {
          continue;
        }
        slots[i].hasPoint = true;
        slots[i].tag = point->tag;
        slots[i].defaultValue = point->defaultBool;
        tags.push_back(point->tag);
      }

      if (addressOverflow) {
        sendException(kExceptionIllegalDataAddress, "线圈地址溢出");
        continue;
      }

      std::unordered_map<std::string, std::optional<bool>> valuesByTag;
      bool dcOk = true;
      if (!tags.empty()) {
        DataCenterProto::GetLatestResponse resp;
        auto dcStatus = dataCenter_.GetLatest(link->connId, tags, &resp);
        if (!dcStatus.ok()) {
          dcOk = false;
          LOG_WARNING("ModbusRTU 从站获取 DataCenter 最新值失败: conn_name={}, 原因={}",
                      link->connName, dcStatus.error_message());
          updateLastError(link->connName, dcStatus.error_message());
        } else {
          for (const auto& update : resp.updates()) {
            if (update.value().kind_case() == DataCenterProto::PointValue::kBoolValue) {
              valuesByTag[update.dst_tag()] = update.value().bool_value();
            } else {
              valuesByTag[update.dst_tag()] = std::nullopt;
              LOG_WARNING("ModbusRTU 从站点值类型不匹配: conn_name={}, tag={}",
                          link->connName, update.dst_tag());
            }
          }
        }
      }

      const size_t byteCount = (static_cast<size_t>(request.quantity) + 7) / 8;
      std::vector<uint8_t> coilBytes(byteCount, 0);
      bool missingValue = false;

      for (uint16_t i = 0; i < request.quantity; ++i) {
        bool value = false;
        if (!slots[i].hasPoint) {
          value = false;
        } else if (dcOk) {
          auto it = valuesByTag.find(slots[i].tag);
          if (it != valuesByTag.end() && it->second.has_value()) {
            value = it->second.value();
          } else if (slots[i].defaultValue.has_value()) {
            value = slots[i].defaultValue.value();
          } else {
            missingValue = true;
          }
        } else {
          if (slots[i].defaultValue.has_value()) {
            value = slots[i].defaultValue.value();
          } else {
            missingValue = true;
          }
        }

        if (missingValue) {
          break;
        }
        if (value) {
          coilBytes[static_cast<size_t>(i / 8)] |= static_cast<uint8_t>(1u << (i % 8));
        }
      }

      if (missingValue) {
        sendException(kExceptionSlaveDeviceFailure, "线圈值缺失");
        continue;
      }

      std::vector<uint8_t> response;
      response.reserve(3 + coilBytes.size() + 2);
      response.push_back(request.slaveId);
      response.push_back(kFunctionReadCoils);
      response.push_back(static_cast<uint8_t>(coilBytes.size()));
      response.insert(response.end(), coilBytes.begin(), coilBytes.end());
      SerialBus::appendCrc(&response);

      auto sendStatus = bus->WriteFrame(response);
      if (!sendStatus.ok()) {
        LOG_ERROR("ModbusRTU 从站响应发送失败: conn_name={}, 原因={}", link->connName, sendStatus.error_message());
        updateLastError(link->connName, sendStatus.error_message());
      }
      continue;
    }

    if (request.function == kFunctionReadHoldingRegisters) {
      if (request.quantity == 0 || request.quantity > kMaxReadHoldingRegistersQuantity) {
        sendException(kExceptionIllegalDataValue, "保持寄存器数量非法");
        continue;
      }
      if (static_cast<uint32_t>(request.address) + request.quantity - 1 > 0xFFFF) {
        sendException(kExceptionIllegalDataAddress, "保持寄存器地址超出范围");
        continue;
      }

      struct RegisterSlot {
        bool hasPoint = false;
        std::string tag;
        ModbusRTUProto::DataType type = ModbusRTUProto::DATA_TYPE_UNSPECIFIED;
        uint32_t wordIndex = 0;
      };

      struct PointMeta {
        ModbusRTUProto::DataType type = ModbusRTUProto::DATA_TYPE_UNSPECIFIED;
        uint32_t regCount = 1;
        ModbusRTUProto::WordOrder wordOrder = ModbusRTUProto::WORD_ORDER_HL;
        ModbusRTUProto::ByteOrder byteOrder = ModbusRTUProto::BYTE_ORDER_AB;
        double scale = 1.0;
        double offset = 0.0;
        std::optional<uint32_t> defaultValue;
      };

      std::vector<RegisterSlot> slots(request.quantity);
      std::unordered_map<std::string, PointMeta> metaByTag;
      metaByTag.reserve(request.quantity);

      bool addressOverflow = false;
      for (uint16_t i = 0; i < request.quantity; ++i) {
        uint32_t reqAddr = static_cast<uint32_t>(request.address) + i;
        uint32_t lookupAddr = reqAddr;
        if (link->config.address_base() == ModbusRTUProto::ADDRESS_BASE_ONE) {
          if (reqAddr == 0xFFFF) {
            addressOverflow = true;
            break;
          }
          lookupAddr = reqAddr + 1;
        }
        auto point = link->pointTable.FindRegisterByAddress(ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS, lookupAddr);
        if (!point) {
          continue;
        }
        slots[i].hasPoint = true;
        slots[i].tag = point->point.tag;
        slots[i].type = point->point.type;
        slots[i].wordIndex = point->wordIndex;
        if (metaByTag.find(point->point.tag) == metaByTag.end()) {
          PointMeta meta;
          meta.type = point->point.type;
          meta.regCount = point->point.regCount;
          meta.wordOrder = point->point.wordOrder;
          meta.byteOrder = point->point.byteOrder;
          meta.scale = point->point.scale;
          meta.offset = point->point.offset;
          if (point->point.defaultUInt16.has_value()) {
            meta.defaultValue = point->point.defaultUInt16.value();
          } else if (point->point.defaultUInt32.has_value()) {
            meta.defaultValue = point->point.defaultUInt32.value();
          }
          metaByTag.emplace(point->point.tag, meta);
        }
      }

      if (addressOverflow) {
        sendException(kExceptionIllegalDataAddress, "保持寄存器地址溢出");
        continue;
      }

      std::vector<std::string> tags;
      tags.reserve(metaByTag.size());
      for (const auto& [tag, _] : metaByTag) {
        tags.push_back(tag);
      }

      std::unordered_map<std::string, std::optional<double>> valuesByTag;
      bool dcOk = true;
      if (!tags.empty()) {
        DataCenterProto::GetLatestResponse resp;
        auto dcStatus = dataCenter_.GetLatest(link->connId, tags, &resp);
        if (!dcStatus.ok()) {
          dcOk = false;
          LOG_WARNING("ModbusRTU 从站获取 DataCenter 最新值失败: conn_name={}, 原因={}",
                      link->connName, dcStatus.error_message());
          updateLastError(link->connName, dcStatus.error_message());
        } else {
          for (const auto& update : resp.updates()) {
            if (update.value().kind_case() == DataCenterProto::PointValue::kDoubleValue) {
              valuesByTag[update.dst_tag()] = update.value().double_value();
            } else if (update.value().kind_case() == DataCenterProto::PointValue::kIntValue) {
              valuesByTag[update.dst_tag()] = static_cast<double>(update.value().int_value());
            } else {
              valuesByTag[update.dst_tag()] = std::nullopt;
              LOG_WARNING("ModbusRTU 从站点值类型不匹配: conn_name={}, tag={}",
                          link->connName, update.dst_tag());
            }
          }
        }
      }

      std::unordered_map<std::string, std::optional<uint32_t>> rawByTag;
      rawByTag.reserve(metaByTag.size());
      bool missingValue = false;

      for (const auto& [tag, meta] : metaByTag) {
        uint32_t value = 0;
        bool hasValue = false;
        if (dcOk) {
          auto it = valuesByTag.find(tag);
          if (it != valuesByTag.end() && it->second.has_value()) {
            const double engValue = it->second.value();
            const double scale = meta.scale == 0.0 ? 1.0 : meta.scale;
            const double rawValue = (engValue - meta.offset) / scale;
            if (!std::isfinite(rawValue)) {
              LOG_WARNING("ModbusRTU 从站点值无法反向缩放: conn_name={}, tag={}, value={}",
                          link->connName, tag, engValue);
              missingValue = true;
            } else {
              long long rounded = std::llround(rawValue);
              const unsigned long long maxValue =
                  meta.type == ModbusRTUProto::DATA_TYPE_UINT32 ? 0xFFFFFFFFull : 0xFFFFull;
              if (rounded < 0) {
                LOG_WARNING("ModbusRTU 从站点值超出范围已截断: conn_name={}, tag={}, raw={}",
                            link->connName, tag, rounded);
                rounded = 0;
              } else if (static_cast<unsigned long long>(rounded) > maxValue) {
                LOG_WARNING("ModbusRTU 从站点值超出范围已截断: conn_name={}, tag={}, raw={}",
                            link->connName, tag, rounded);
                rounded = static_cast<long long>(maxValue);
              }
              value = static_cast<uint32_t>(rounded);
              hasValue = true;
            }
          }
        }
        if (!hasValue && meta.defaultValue.has_value()) {
          value = meta.defaultValue.value();
          hasValue = true;
        }
        if (!hasValue) {
          missingValue = true;
        } else {
          rawByTag[tag] = value;
        }
        if (missingValue) {
          break;
        }
      }

      if (missingValue) {
        sendException(kExceptionSlaveDeviceFailure, "保持寄存器值缺失");
        continue;
      }

      std::unordered_map<std::string, std::array<uint16_t, 2>> wordsByTag;
      wordsByTag.reserve(metaByTag.size());
      for (const auto& [tag, meta] : metaByTag) {
        if (meta.type != ModbusRTUProto::DATA_TYPE_UINT32) {
          continue;
        }
        auto rawIt = rawByTag.find(tag);
        if (rawIt == rawByTag.end() || !rawIt->second.has_value()) {
          missingValue = true;
          break;
        }
        auto words = encodeUint32(rawIt->second.value(), meta.wordOrder, meta.byteOrder);
        wordsByTag.emplace(tag, words);
        LOG_DEBUG("ModbusRTU 从站32位寄存器编码: conn_name={}, tag={}, raw={}, reg0={}, reg1={}, word_order={}, byte_order={}",
                  link->connName,
                  tag,
                  rawIt->second.value(),
                  words[0],
                  words[1],
                  static_cast<int>(meta.wordOrder),
                  static_cast<int>(meta.byteOrder));
      }

      if (missingValue) {
        sendException(kExceptionSlaveDeviceFailure, "保持寄存器值缺失");
        continue;
      }

      const size_t byteCount = static_cast<size_t>(request.quantity) * 2;
      std::vector<uint8_t> registerBytes(byteCount, 0);
      for (uint16_t i = 0; i < request.quantity; ++i) {
        uint16_t value = 0;
        if (!slots[i].hasPoint) {
          value = 0;
        } else {
          auto rawIt = rawByTag.find(slots[i].tag);
          if (rawIt == rawByTag.end() || !rawIt->second.has_value()) {
            missingValue = true;
          } else if (slots[i].type == ModbusRTUProto::DATA_TYPE_UINT32) {
            auto wordsIt = wordsByTag.find(slots[i].tag);
            if (wordsIt == wordsByTag.end() || slots[i].wordIndex >= wordsIt->second.size()) {
              missingValue = true;
            } else {
              value = wordsIt->second[slots[i].wordIndex];
            }
          } else {
            value = static_cast<uint16_t>(rawIt->second.value());
            auto metaIt = metaByTag.find(slots[i].tag);
            if (metaIt != metaByTag.end() && metaIt->second.byteOrder == ModbusRTUProto::BYTE_ORDER_BA) {
              value = swapWordBytes(value);
            }
          }
        }
        if (missingValue) {
          break;
        }
        const auto offset = static_cast<size_t>(i) * 2;
        registerBytes[offset] = static_cast<uint8_t>((value >> 8) & 0xFF);
        registerBytes[offset + 1] = static_cast<uint8_t>(value & 0xFF);
      }

      if (missingValue) {
        sendException(kExceptionSlaveDeviceFailure, "保持寄存器值缺失");
        continue;
      }

      std::vector<uint8_t> response;
      response.reserve(3 + registerBytes.size() + 2);
      response.push_back(request.slaveId);
      response.push_back(kFunctionReadHoldingRegisters);
      response.push_back(static_cast<uint8_t>(registerBytes.size()));
      response.insert(response.end(), registerBytes.begin(), registerBytes.end());
      SerialBus::appendCrc(&response);

      auto sendStatus = bus->WriteFrame(response);
      if (!sendStatus.ok()) {
        LOG_ERROR("ModbusRTU 从站响应发送失败: conn_name={}, 原因={}", link->connName, sendStatus.error_message());
        updateLastError(link->connName, sendStatus.error_message());
      }
      continue;
    }

    sendException(kExceptionIllegalFunction, "不支持的功能码");
  }

  LOG_INFO("ModbusRTU 从站监听结束: device={}", serialKey.device);
}

grpc::Status LinkManager::UpsertLink(const ModbusRTUProto::UpsertLinkRequest& request, ModbusRTUProto::LinkInfo* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
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
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name 已存在");
      }
      if (it->second.state == ModbusRTUProto::LINK_STATE_RUNNING) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "更新配置前请先停止链路");
      }
      if (it->second.state == ModbusRTUProto::LINK_STATE_PENDING_DELETE) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路处于待删除状态");
      }
      status = ensureSerialCompatibleLocked(serialKey, connName, normalized.mode());
      if (!status.ok()) {
        return status;
      }

      it->second.config = normalized;
      it->second.serialKey = serialKey;
      it->second.lastError.clear();
      return fillLinkInfoLocked(it->second, out);
    }

    if (pendingCreateByName_.contains(connName)) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name 已存在");
    }
    status = ensureSerialCompatibleLocked(serialKey, connName, normalized.mode());
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
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name 已存在");
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
    return grpc::Status(grpc::StatusCode::INTERNAL, "DataCenter 返回 conn_id=0");
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
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name 已存在");
    }
    return fillLinkInfoLocked(it->second, out);
  }
}

grpc::Status LinkManager::GetLink(const std::string& connName, ModbusRTUProto::LinkInfo* out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
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
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
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
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路处于待删除状态");
    }
    if (link.state == ModbusRTUProto::LINK_STATE_RUNNING) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路已在运行");
    }
    config = link.config;
    pointTable = link.pointTable;
    connId = link.connId;
    serialKey = link.serialKey;
  }

  if (config.address_base() == ModbusRTUProto::ADDRESS_BASE_ONE) {
    for (const auto& point : pointTable.Points()) {
      if (point.address == 0) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "address_base=ONE 要求 address >= 1");
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    bus = acquireBusLocked(serialKey, config.serial());
  }
  status = bus->Open();
  if (!status.ok()) {
    const auto errorMessage = status.error_message();
    const auto device = config.serial().device();
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto released = releaseBusLocked(serialKey);
      if (released) {
        released->Close();
      }
      auto it = linksByName_.find(connName);
      if (it != linksByName_.end()) {
        it->second.lastError = errorMessage;
      }
    }
    LOG_ERROR("ModbusRTU 打开串口失败: conn_name={}, device={}, 原因={}", connName, device, errorMessage);
    return status;
  }

  if (config.mode() == ModbusRTUProto::LINK_MODE_SLAVE) {
    status = startSlaveLink(connName, config, pointTable, connId, serialKey, bus);
    if (!status.ok()) {
      const auto errorMessage = status.error_message();
      const auto device = config.serial().device();
      {
        std::lock_guard<std::mutex> lock(mu_);
        auto released = releaseBusLocked(serialKey);
        if (released) {
          released->Close();
        }
        auto it = linksByName_.find(connName);
        if (it != linksByName_.end()) {
          it->second.lastError = errorMessage;
        }
      }
      LOG_ERROR("ModbusRTU 启动从站响应失败: conn_name={}, device={}, 原因={}", connName, device, errorMessage);
      return status;
    }
    LOG_INFO("ModbusRTU 已启动从站响应: conn_name={}, slave_id={}, device={}",
             connName, config.slave_id(), config.serial().device());
    return grpc::Status::OK;
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
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路处于待删除状态");
    }
    link.bus = bus;
    link.state = ModbusRTUProto::LINK_STATE_RUNNING;
    link.lastError.clear();
    link.pollThread = ModuleManager::StartModuleThread(
        ModbusRTULibInfo.LIB_NAME,
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
  ModbusRTUProto::LinkConfig config;
  ModbusRTUProto::LinkMode mode = ModbusRTUProto::LINK_MODE_UNSPECIFIED;

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      return makeNotFound(connName);
    }
    pendingDelete = (it->second.state == ModbusRTUProto::LINK_STATE_PENDING_DELETE);
    pollThread = std::move(it->second.pollThread);
    config = it->second.config;
    mode = it->second.config.mode();
    serialKey = it->second.serialKey;
    bus = it->second.bus;
    it->second.bus.reset();
    it->second.state = pendingDelete ? ModbusRTUProto::LINK_STATE_PENDING_DELETE : ModbusRTUProto::LINK_STATE_STOPPED;
  }

  if (mode == ModbusRTUProto::LINK_MODE_SLAVE) {
    stopSlaveLink(connName, config, serialKey, bus);
    LOG_INFO("ModbusRTU 已停止从站响应: conn_name={}", connName);
    return grpc::Status::OK;
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
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "更新点表前请先停止链路");
    }
    if (it->second.state == ModbusRTUProto::LINK_STATE_PENDING_DELETE) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路处于待删除状态");
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
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
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
  std::unordered_map<std::string, double> lastReportedByTag;
  lastReportedByTag.reserve(points.size());
  const auto readPlanMode = config.has_read_plan() ? config.read_plan().mode() : ModbusRTUProto::READ_PLAN_MODE_POINT;
  const bool useExplicitPlan = (config.mode() == ModbusRTUProto::LINK_MODE_MASTER &&
                                readPlanMode == ModbusRTUProto::READ_PLAN_MODE_EXPLICIT &&
                                config.read_plan().blocks_size() > 0);
  if (useExplicitPlan) {
    LOG_INFO("ModbusRTU 轮询使用显式抄读方案: conn_name={}, blocks={}, interval={}ms",
             connName,
             config.read_plan().blocks_size(),
             config.poll_interval_ms());
  }
  LOG_INFO("ModbusRTU 轮询开始: conn_name={}, points={}, interval={}ms", connName, points.size(), config.poll_interval_ms());

  if (!useExplicitPlan) {
    while (!stopToken.stop_requested()) {
      for (const auto& point : points) {
        if (stopToken.stop_requested()) {
          break;
        }

        uint32_t address = point.address;
        if (config.address_base() == ModbusRTUProto::ADDRESS_BASE_ONE) {
          if (address == 0) {
            updateLastError(connName, "address_base=ONE 但 address 为 0");
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
          if (point.type == ModbusRTUProto::DATA_TYPE_UINT16) {
            uint16_t value = 0;
            status = bus->ReadHoldingRegister(static_cast<uint8_t>(config.slave_id()),
                                              static_cast<uint16_t>(address),
                                              &value);
            if (status.ok()) {
              if (point.byteOrder == ModbusRTUProto::BYTE_ORDER_BA) {
                value = swapWordBytes(value);
              }
              LOG_DEBUG("ModbusRTU 读取UINT16寄存器: conn_name={}, tag={}, raw={}, byte_order={}",
                        connName,
                        point.tag,
                        value,
                        static_cast<int>(point.byteOrder));
              const double engValue = static_cast<double>(value) * point.scale + point.offset;
              std::optional<double> last;
              auto lastIt = lastReportedByTag.find(point.tag);
              if (lastIt != lastReportedByTag.end()) {
                last = lastIt->second;
              }
              if (!shouldReport(engValue, point.deadband, last)) {
                LOG_DEBUG("ModbusRTU 死区过滤上报: conn_name={}, tag={}, value={}, last={}, 死区={}",
                          connName,
                          point.tag,
                          engValue,
                          last.value(),
                          point.deadband);
                continue;
              }
              auto dc = dataCenter_.PublishDouble(connId, point.tag, engValue, DataCenterProto::QUALITY_GOOD, 0);
              if (!dc.ok()) {
                updateLastError(connName, dc.error_message());
                LOG_ERROR("ModbusRTU 发布点值失败: conn_name={}, tag={}, 原因={}", connName, point.tag, dc.error_message());
              } else {
                lastReportedByTag[point.tag] = engValue;
              }
            }
          } else if (point.type == ModbusRTUProto::DATA_TYPE_UINT32) {
            std::vector<uint16_t> values;
            status = bus->ReadHoldingRegisters(static_cast<uint8_t>(config.slave_id()),
                                               static_cast<uint16_t>(address),
                                               2,
                                               &values);
            if (status.ok()) {
              if (values.size() != 2) {
                status = grpc::Status(grpc::StatusCode::UNAVAILABLE, "保持寄存器响应数量异常");
              } else {
                const uint32_t raw = decodeUint32(values[0], values[1], point.wordOrder, point.byteOrder);
                LOG_DEBUG("ModbusRTU 读取UINT32寄存器: conn_name={}, tag={}, raw={}, reg0={}, reg1={}, word_order={}, byte_order={}",
                          connName,
                          point.tag,
                          raw,
                          values[0],
                          values[1],
                          static_cast<int>(point.wordOrder),
                          static_cast<int>(point.byteOrder));
                const double engValue = static_cast<double>(raw) * point.scale + point.offset;
                std::optional<double> last;
                auto lastIt = lastReportedByTag.find(point.tag);
                if (lastIt != lastReportedByTag.end()) {
                  last = lastIt->second;
                }
                if (!shouldReport(engValue, point.deadband, last)) {
                  LOG_DEBUG("ModbusRTU 死区过滤上报: conn_name={}, tag={}, value={}, last={}, 死区={}",
                            connName,
                            point.tag,
                            engValue,
                            last.value(),
                            point.deadband);
                  continue;
                }
                auto dc = dataCenter_.PublishDouble(connId, point.tag, engValue, DataCenterProto::QUALITY_GOOD, 0);
                if (!dc.ok()) {
                  updateLastError(connName, dc.error_message());
                  LOG_ERROR("ModbusRTU 发布点值失败: conn_name={}, tag={}, 原因={}", connName, point.tag, dc.error_message());
                } else {
                  lastReportedByTag[point.tag] = engValue;
                }
              }
            }
          } else {
            status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "保持寄存器点位类型不支持");
          }
        } else {
          status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "不支持的功能码");
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
    return;
  }

  struct NormalizedPoint {
    std::string tag;
    ModbusRTUProto::FunctionCode function = ModbusRTUProto::FUNCTION_UNSPECIFIED;
    ModbusRTUProto::DataType type = ModbusRTUProto::DATA_TYPE_UNSPECIFIED;
    uint32_t address = 0;
    uint32_t regCount = 1;
    ModbusRTUProto::WordOrder wordOrder = ModbusRTUProto::WORD_ORDER_HL;
    ModbusRTUProto::ByteOrder byteOrder = ModbusRTUProto::BYTE_ORDER_AB;
    double scale = 1.0;
    double offset = 0.0;
    double deadband = 0.0;
  };

  std::vector<NormalizedPoint> holdingPoints;
  std::vector<NormalizedPoint> coilPoints;
  holdingPoints.reserve(points.size());
  coilPoints.reserve(points.size());

  for (const auto& point : points) {
    uint32_t address = point.address;
    if (config.address_base() == ModbusRTUProto::ADDRESS_BASE_ONE) {
      if (address == 0) {
        updateLastError(connName, "address_base=ONE 但 address 为 0");
        LOG_WARNING("ModbusRTU 点表地址非法: conn_name={}, tag={}, address=0", connName, point.tag);
        continue;
      }
      address -= 1;
    }

    NormalizedPoint normalized;
    normalized.tag = point.tag;
    normalized.function = point.function;
    normalized.type = point.type;
    normalized.address = address;
    normalized.regCount = point.regCount;
    normalized.wordOrder = point.wordOrder;
    normalized.byteOrder = point.byteOrder;
    normalized.scale = point.scale;
    normalized.offset = point.offset;
    normalized.deadband = point.deadband;

    if (point.function == ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS) {
      holdingPoints.push_back(std::move(normalized));
    } else if (point.function == ModbusRTUProto::FUNCTION_READ_COILS) {
      coilPoints.push_back(std::move(normalized));
    } else {
      LOG_WARNING("ModbusRTU 点表功能码不支持: conn_name={}, tag={}, function={}",
                  connName,
                  point.tag,
                  static_cast<int>(point.function));
    }
  }

  if (!holdingPoints.empty()) {
    size_t uncoveredCount = 0;
    std::vector<std::string> sampleTags;
    sampleTags.reserve(5);
    for (const auto& point : holdingPoints) {
      bool covered = false;
      for (const auto& block : config.read_plan().blocks()) {
        if (block.function() != ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS) {
          continue;
        }
        uint32_t start = block.start();
        if (config.address_base() == ModbusRTUProto::ADDRESS_BASE_ONE) {
          if (start == 0) {
            continue;
          }
          start -= 1;
        }
        const uint32_t end = start + block.quantity() - 1;
        if (point.address >= start && (point.address + point.regCount - 1) <= end) {
          covered = true;
          break;
        }
      }
      if (!covered) {
        uncoveredCount += 1;
        if (sampleTags.size() < 5) {
          sampleTags.push_back(point.tag);
        }
      }
    }
    if (uncoveredCount > 0) {
      std::string sample;
      for (size_t i = 0; i < sampleTags.size(); ++i) {
        if (i > 0) {
          sample.append(",");
        }
        sample.append(sampleTags[i]);
      }
      LOG_WARNING("ModbusRTU 显式抄读区间未覆盖点表: conn_name={}, 未覆盖点数={}, 示例={}",
                  connName,
                  uncoveredCount,
                  sample);
    }
  }

  while (!stopToken.stop_requested()) {
    std::unordered_set<std::string> processedTags;
    processedTags.reserve(holdingPoints.size());

    for (const auto& block : config.read_plan().blocks()) {
      if (stopToken.stop_requested()) {
        break;
      }
      if (block.function() != ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS) {
        LOG_WARNING("ModbusRTU 显式抄读区间功能码不支持: conn_name={}, function={}",
                    connName,
                    static_cast<int>(block.function()));
        continue;
      }
      if (block.quantity() == 0) {
        updateLastError(connName, "read_plan.blocks.quantity 不能为空");
        LOG_WARNING("ModbusRTU 显式抄读区间数量非法: conn_name={}, quantity=0", connName);
        continue;
      }
      uint32_t start = block.start();
      if (config.address_base() == ModbusRTUProto::ADDRESS_BASE_ONE) {
        if (start == 0) {
          updateLastError(connName, "read_plan.blocks.start 不能为 0（address_base=ONE）");
          LOG_WARNING("ModbusRTU 显式抄读区间起始地址非法: conn_name={}, start=0", connName);
          continue;
        }
        start -= 1;
      }

      LOG_DEBUG("ModbusRTU 轮询区间抄读: conn_name={}, start={}, quantity={}",
                connName,
                start,
                block.quantity());

      std::vector<uint16_t> values;
      auto status = bus->ReadHoldingRegisters(static_cast<uint8_t>(config.slave_id()),
                                              static_cast<uint16_t>(start),
                                              static_cast<uint16_t>(block.quantity()),
                                              &values);
      if (!status.ok()) {
        updateLastError(connName, status.error_message());
        LOG_WARNING("ModbusRTU 轮询区间抄读失败: conn_name={}, start={}, quantity={}, 原因={}",
                    connName,
                    start,
                    block.quantity(),
                    status.error_message());
        continue;
      }
      if (values.size() != block.quantity()) {
        updateLastError(connName, "保持寄存器响应数量异常");
        LOG_WARNING("ModbusRTU 轮询区间响应数量异常: conn_name={}, start={}, quantity={}, 实际={}",
                    connName,
                    start,
                    block.quantity(),
                    values.size());
        continue;
      }

      size_t matchedPoints = 0;
      const uint32_t end = start + block.quantity() - 1;
      for (const auto& point : holdingPoints) {
        if (point.address < start || (point.address + point.regCount - 1) > end) {
          continue;
        }
        if (!processedTags.insert(point.tag).second) {
          LOG_WARNING("ModbusRTU 显式抄读区间重叠: conn_name={}, tag={} 已被重复抄读",
                      connName,
                      point.tag);
          continue;
        }
        const size_t offset = static_cast<size_t>(point.address - start);
        uint32_t raw = 0;
        if (point.type == ModbusRTUProto::DATA_TYPE_UINT16) {
          if (offset >= values.size()) {
            updateLastError(connName, "保持寄存器地址溢出");
            LOG_WARNING("ModbusRTU 轮询区间解码越界: conn_name={}, tag={}, offset={}",
                        connName,
                        point.tag,
                        offset);
            continue;
          }
          uint16_t value = values[offset];
          if (point.byteOrder == ModbusRTUProto::BYTE_ORDER_BA) {
            value = swapWordBytes(value);
          }
          raw = value;
          LOG_DEBUG("ModbusRTU 读取UINT16寄存器: conn_name={}, tag={}, raw={}, byte_order={}",
                    connName,
                    point.tag,
                    value,
                    static_cast<int>(point.byteOrder));
        } else if (point.type == ModbusRTUProto::DATA_TYPE_UINT32) {
          if (offset + 1 >= values.size()) {
            updateLastError(connName, "保持寄存器地址溢出");
            LOG_WARNING("ModbusRTU 轮询区间解码越界: conn_name={}, tag={}, offset={}",
                        connName,
                        point.tag,
                        offset);
            continue;
          }
          const uint16_t first = values[offset];
          const uint16_t second = values[offset + 1];
          raw = decodeUint32(first, second, point.wordOrder, point.byteOrder);
          LOG_DEBUG("ModbusRTU 读取UINT32寄存器: conn_name={}, tag={}, raw={}, reg0={}, reg1={}, word_order={}, byte_order={}",
                    connName,
                    point.tag,
                    raw,
                    first,
                    second,
                    static_cast<int>(point.wordOrder),
                    static_cast<int>(point.byteOrder));
        } else {
          updateLastError(connName, "保持寄存器点位类型不支持");
          LOG_WARNING("ModbusRTU 轮询区间点位类型不支持: conn_name={}, tag={}", connName, point.tag);
          continue;
        }

        const double engValue = static_cast<double>(raw) * point.scale + point.offset;
        std::optional<double> last;
        auto lastIt = lastReportedByTag.find(point.tag);
        if (lastIt != lastReportedByTag.end()) {
          last = lastIt->second;
        }
        if (!shouldReport(engValue, point.deadband, last)) {
          LOG_DEBUG("ModbusRTU 死区过滤上报: conn_name={}, tag={}, value={}, last={}, 死区={}",
                    connName,
                    point.tag,
                    engValue,
                    last.value(),
                    point.deadband);
          continue;
        }
        auto dc = dataCenter_.PublishDouble(connId, point.tag, engValue, DataCenterProto::QUALITY_GOOD, 0);
        if (!dc.ok()) {
          updateLastError(connName, dc.error_message());
          LOG_ERROR("ModbusRTU 发布点值失败: conn_name={}, tag={}, 原因={}", connName, point.tag, dc.error_message());
        } else {
          lastReportedByTag[point.tag] = engValue;
        }
        matchedPoints += 1;
      }

      if (matchedPoints == 0) {
        LOG_DEBUG("ModbusRTU 轮询区间未命中点表: conn_name={}, start={}, quantity={}",
                  connName,
                  start,
                  block.quantity());
      }
    }

    for (const auto& point : coilPoints) {
      if (stopToken.stop_requested()) {
        break;
      }
      bool value = false;
      auto status = bus->ReadCoil(static_cast<uint8_t>(config.slave_id()),
                                  static_cast<uint16_t>(point.address),
                                  &value);
      if (status.ok()) {
        auto dc = dataCenter_.PublishBool(connId, point.tag, value, DataCenterProto::QUALITY_GOOD, 0);
        if (!dc.ok()) {
          updateLastError(connName, dc.error_message());
          LOG_ERROR("ModbusRTU 发布点值失败: conn_name={}, tag={}, 原因={}", connName, point.tag, dc.error_message());
        }
      } else {
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
