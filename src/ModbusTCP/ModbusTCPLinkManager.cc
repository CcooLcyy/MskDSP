#include "ModbusTCPLinkManager.h"

#include <chrono>
#include <concepts>
#include <format>
#include <limits>
#include <optional>
#include <unordered_map>
#include <utility>

#include "Logger.h"
#include "ModbusTCPLibInfo.h"
#include "mskdsp/Decimal20.hpp"

namespace ModbusTCP {
namespace {
constexpr uint32_t kDefaultPort = 502;
constexpr uint32_t kDefaultConnectTimeoutMs = 3000;
constexpr uint32_t kDefaultRequestTimeoutMs = 3000;
constexpr uint32_t kDefaultPollIntervalMs = 1000;

using mskdsp::numeric::Decimal20;
using mskdsp::numeric::DecimalError;
using mskdsp::numeric::RoundingMode;

bool isReadRegister(ModbusRTUProto::FunctionCode f) { return f == ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS || f == ModbusRTUProto::FUNCTION_READ_INPUT_REGISTERS; }
bool isWrite(ModbusRTUProto::FunctionCode f) { return f == ModbusRTUProto::FUNCTION_WRITE_SINGLE_COIL || f == ModbusRTUProto::FUNCTION_WRITE_SINGLE_REGISTER || f == ModbusRTUProto::FUNCTION_WRITE_MULTIPLE_REGISTERS; }
uint16_t swapBytes(uint16_t v) { return static_cast<uint16_t>((v << 8) | (v >> 8)); }

std::expected<Decimal20, DecimalError> toDecimal(
    const DataCenterProto::PointValue& value) {
  switch (value.kind_case()) {
    case DataCenterProto::PointValue::kDoubleValue:
      return Decimal20::FromDouble(value.double_value());
    case DataCenterProto::PointValue::kIntValue:
      return Decimal20::FromInt64(value.int_value());
    case DataCenterProto::PointValue::kBoolValue:
      return Decimal20::FromInt64(value.bool_value() ? 1 : 0);
    case DataCenterProto::PointValue::kDecimalValue:
      return Decimal20::Parse(value.decimal_value());
    default:
      return std::unexpected(DecimalError::kInvalidFormat);
  }
}

std::expected<Decimal20, DecimalError> engineeringFromRaw(
    int64_t raw, const ModbusRTU::PointTable::Point& point) {
  auto rawDecimal = Decimal20::FromInt64(raw);
  if (!rawDecimal.has_value()) {
    return std::unexpected(rawDecimal.error());
  }
  return mskdsp::numeric::ApplyEngineering(
      *rawDecimal, point.scale, point.offset);
}

template <std::integral Integer>
std::expected<Integer, DecimalError> quantizeRegister(
    const Decimal20& raw) {
  const auto minimum = Decimal20::FromInt64(
      static_cast<int64_t>(std::numeric_limits<Integer>::min()));
  const auto maximum = Decimal20::FromInt64(
      static_cast<int64_t>(std::numeric_limits<Integer>::max()));
  if (!minimum.has_value() || !maximum.has_value() ||
      raw < *minimum || raw > *maximum) {
    return std::unexpected(DecimalError::kIntegerOverflow);
  }
  return raw.ToInteger<Integer>(RoundingMode::kHalfAwayFromZero);
}

uint32_t decode32(uint16_t a, uint16_t b, ModbusRTUProto::WordOrder word, ModbusRTUProto::ByteOrder byte) { if (byte == ModbusRTUProto::BYTE_ORDER_BA) { a = swapBytes(a); b = swapBytes(b); } if (word == ModbusRTUProto::WORD_ORDER_LH) std::swap(a, b); return (static_cast<uint32_t>(a) << 16) | b; }
grpc::Status notFound(const std::string& name) { return {grpc::StatusCode::NOT_FOUND, std::format("未找到 ModbusTCP 链路: {}", name)}; }
}

LinkManager::LinkManager(std::filesystem::path db) : dataCenter_(ModbusTCPLibInfo.LIB_NAME), linkStore_(db), pointTableStore_(std::move(db)) {}
LinkManager::~LinkManager() { std::vector<std::string> names; { std::lock_guard lock(mu_); for (const auto& [name, _] : links_) names.push_back(name); } for (const auto& name : names) (void)StopLink(name); }
void LinkManager::setDataCenterStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub) { dataCenter_.setStub(std::move(stub)); }

grpc::Status LinkManager::normalize(const ModbusTCPProto::LinkConfig& in, ModbusTCPProto::LinkConfig* out) {
  if (!out) return {grpc::StatusCode::INVALID_ARGUMENT, "out 为空"};
  if (in.conn_name().empty()) return {grpc::StatusCode::INVALID_ARGUMENT, "conn_name 不能为空"};
  if (!in.has_tcp()) return {grpc::StatusCode::INVALID_ARGUMENT, "tcp 配置不能为空"};
  *out = in;
  auto* tcp = out->mutable_tcp();
  if (tcp->host().empty()) return {grpc::StatusCode::INVALID_ARGUMENT, "tcp.host 不能为空"};
  if (tcp->port() == 0) tcp->set_port(kDefaultPort);
  if (tcp->port() > 65535) return {grpc::StatusCode::INVALID_ARGUMENT, "tcp.port 必须在 [1,65535] 范围内"};
  if (tcp->unit_id() == 0) tcp->set_unit_id(1);
  if (tcp->unit_id() > 247) return {grpc::StatusCode::INVALID_ARGUMENT, "tcp.unit_id 必须在 [1,247] 范围内"};
  if (tcp->connect_timeout_ms() == 0) tcp->set_connect_timeout_ms(kDefaultConnectTimeoutMs);
  if (tcp->request_timeout_ms() == 0) tcp->set_request_timeout_ms(kDefaultRequestTimeoutMs);
  if (out->poll_interval_ms() == 0) out->set_poll_interval_ms(kDefaultPollIntervalMs);
  if (out->address_base() == ModbusRTUProto::ADDRESS_BASE_UNSPECIFIED) out->set_address_base(ModbusRTUProto::ADDRESS_BASE_ZERO);
  if (out->address_base() != ModbusRTUProto::ADDRESS_BASE_ZERO && out->address_base() != ModbusRTUProto::ADDRESS_BASE_ONE) return {grpc::StatusCode::INVALID_ARGUMENT, "address_base 非法"};
  if (out->poll_interval_ms() == 0) return {grpc::StatusCode::INVALID_ARGUMENT, "poll_interval_ms 不能为空"};
  return grpc::Status::OK;
}

grpc::Status LinkManager::fillInfoLocked(const Link& link, ModbusTCPProto::LinkInfo* out) const { if (!out) return {grpc::StatusCode::INVALID_ARGUMENT, "out 为空"}; out->Clear(); *out->mutable_config() = link.config; out->set_conn_id(link.connId); out->set_state(link.state); out->set_last_error(link.lastError); return grpc::Status::OK; }
grpc::Status LinkManager::saveLinksLocked() { ModbusTCPProto::LinksConfig c; for (const auto& [_, link] : links_) { auto* p=c.add_links(); *p->mutable_config()=link.config; p->set_conn_id(link.connId); p->set_pending_delete(link.state == ModbusTCPProto::LINK_STATE_PENDING_DELETE); } return linkStore_.Save(c); }
grpc::Status LinkManager::savePointTablesLocked() {
  ModbusTCPProto::PointTablesConfig config;
  for (const auto& [name, link] : links_) {
    if (!link.pointTableConfigured) {
      continue;
    }
    auto* table = config.add_point_tables();
    table->set_conn_name(name);
    ModbusRTUProto::PointTable normalized;
    link.pointTable.ToProto(name, &normalized);
    for (const auto& point : normalized.points()) {
      *table->add_points() = point;
    }
  }
  return pointTableStore_.Save(config);
}

void LinkManager::LoadPersistedConfig() {
  ModbusTCPProto::LinksConfig links; if (auto s=linkStore_.Load(&links); !s.ok()) { LOG_ERROR("ModbusTCP 恢复链路配置失败: {}", s.error_message()); return; }
  ModbusTCPProto::PointTablesConfig tables; if (auto s=pointTableStore_.Load(&tables); !s.ok()) LOG_WARNING("ModbusTCP 恢复点表失败: {}", s.error_message());
  std::unordered_map<std::string, uint32_t> refreshedConnIds;
  for (const auto& item : links.links()) {
    if (!item.has_config() || item.config().conn_name().empty()) continue;
    DataCenterProto::ConnectionInfo info;
    auto status = dataCenter_.GetOrCreateConnection(item.config().conn_name(), &info);
    if (status.ok() && info.conn_id() != 0) refreshedConnIds[item.config().conn_name()] = info.conn_id();
    else LOG_WARNING("ModbusTCP 恢复链路时确认 DataCenter 连接失败: conn_name={}, 原因={}", item.config().conn_name(), status.error_message());
  }
  std::vector<std::string> autoStartNames;
  {
    std::lock_guard lock(mu_);
    for (const auto& item : links.links()) {
      if (!item.has_config() || item.config().conn_name().empty() || item.conn_id() == 0) continue;
      ModbusTCPProto::LinkConfig normalized;
      if (!normalize(item.config(), &normalized).ok()) {
        LOG_WARNING("ModbusTCP 跳过非法持久化链路: conn_name={}", item.config().conn_name());
        continue;
      }
      Link link;
      link.config = std::move(normalized);
      link.connId = refreshedConnIds.contains(link.config.conn_name()) ? refreshedConnIds[link.config.conn_name()] : item.conn_id();
      link.state = item.pending_delete() ? ModbusTCPProto::LINK_STATE_PENDING_DELETE
                                         : ModbusTCPProto::LINK_STATE_STOPPED;
      links_[link.config.conn_name()] = std::move(link);
    }
    for (const auto& table : tables.point_tables()) {
      auto it = links_.find(table.conn_name());
      if (it == links_.end()) continue;
      if (it->second.pointTable.Upsert(table.points(), true).ok()) {
        it->second.pointTableConfigured = true;
        if (it->second.state == ModbusTCPProto::LINK_STATE_STOPPED) autoStartNames.push_back(table.conn_name());
      }
    }
    LOG_INFO("ModbusTCP 已恢复配置: 链路数={}, 点表数={}", links_.size(), tables.point_tables_size());
  }
  for (const auto& name : autoStartNames) {
    auto status = StartLink(name);
    if (!status.ok()) LOG_WARNING("ModbusTCP 恢复后自动启动失败: conn_name={}, 原因={}", name, status.error_message());
  }
}

grpc::Status LinkManager::UpsertLink(const ModbusTCPProto::UpsertLinkRequest& request, ModbusTCPProto::LinkInfo* out) {
  ModbusTCPProto::LinkConfig normalized; auto status=normalize(request.config(), &normalized); if (!status.ok()) return status;
  DataCenterProto::ConnectionInfo connection; status=dataCenter_.GetOrCreateConnection(normalized.conn_name(), &connection); if (!status.ok()) return status;
  std::lock_guard lock(mu_); auto it=links_.find(normalized.conn_name()); if (it!=links_.end() && request.create_only()) return {grpc::StatusCode::ALREADY_EXISTS,"ModbusTCP 链路已存在"}; if (it!=links_.end() && it->second.state==ModbusTCPProto::LINK_STATE_RUNNING) return {grpc::StatusCode::FAILED_PRECONDITION,"运行中的链路不能修改"}; Link& link=links_[normalized.conn_name()]; link.config=normalized; link.connId=connection.conn_id(); link.state=ModbusTCPProto::LINK_STATE_STOPPED; link.lastError.clear(); status=saveLinksLocked(); if (!status.ok()) return status; LOG_INFO("ModbusTCP 链路配置已更新: conn_name={}, host={}, port={}, unit_id={}", normalized.conn_name(), normalized.tcp().host(), normalized.tcp().port(), normalized.tcp().unit_id()); return fillInfoLocked(link,out);
}

grpc::Status LinkManager::RenameLink(const ModbusTCPProto::RenameLinkRequest& request, ModbusTCPProto::LinkInfo* out) { if(request.old_conn_name().empty()||request.new_conn_name().empty()) return {grpc::StatusCode::INVALID_ARGUMENT,"连接名不能为空"}; std::lock_guard lock(mu_); auto it=links_.find(request.old_conn_name()); if(it==links_.end())return notFound(request.old_conn_name()); if(links_.contains(request.new_conn_name()))return {grpc::StatusCode::ALREADY_EXISTS,"新连接名已存在"}; if(it->second.state==ModbusTCPProto::LINK_STATE_RUNNING)return {grpc::StatusCode::FAILED_PRECONDITION,"运行中的链路不能改名"}; DataCenterProto::ConnectionInfo info; auto status=dataCenter_.RenameConnection(request.old_conn_name(),request.new_conn_name(),&info); if(!status.ok())return status; Link link=std::move(it->second); links_.erase(it); link.config.set_conn_name(request.new_conn_name()); links_[request.new_conn_name()]=std::move(link); status=saveLinksLocked(); if(!status.ok())return status; (void)savePointTablesLocked(); return fillInfoLocked(links_.at(request.new_conn_name()),out); }
grpc::Status LinkManager::GetLink(const std::string& name, ModbusTCPProto::LinkInfo* out) const { if(name.empty())return {grpc::StatusCode::INVALID_ARGUMENT,"conn_name 不能为空"}; std::lock_guard lock(mu_); auto it=links_.find(name); if(it==links_.end())return notFound(name); return fillInfoLocked(it->second,out); }
grpc::Status LinkManager::ListLinks(ModbusTCPProto::ListLinksResponse* out) const { if(!out)return {grpc::StatusCode::INVALID_ARGUMENT,"out 为空"}; std::lock_guard lock(mu_); out->Clear(); for(const auto& [_,link]:links_) fillInfoLocked(link,out->add_links()); return grpc::Status::OK; }
grpc::Status LinkManager::DeleteLink(const std::string& name) {
  auto s = StopLink(name);
  if (!s.ok() && s.error_code() != grpc::StatusCode::NOT_FOUND) return s;
  std::lock_guard lock(mu_);
  auto it = links_.find(name);
  if (it == links_.end()) return notFound(name);
  s = dataCenter_.DeleteConnection(name);
  if (!s.ok() && s.error_code() != grpc::StatusCode::NOT_FOUND) {
    it->second.state = ModbusTCPProto::LINK_STATE_PENDING_DELETE;
    (void)saveLinksLocked();
    return s;
  }
  links_.erase(it);
  s = saveLinksLocked();
  if (!s.ok()) return s;
  return savePointTablesLocked();
}

grpc::Status LinkManager::StartLink(const std::string& name) { ModbusTCPProto::LinkConfig config; ModbusRTU::PointTable table; uint32_t id=0; { std::lock_guard lock(mu_); auto it=links_.find(name); if(it==links_.end())return notFound(name); if(it->second.state==ModbusTCPProto::LINK_STATE_RUNNING)return grpc::Status::OK; if(it->second.state==ModbusTCPProto::LINK_STATE_PENDING_DELETE)return {grpc::StatusCode::FAILED_PRECONDITION,"链路处于待删除状态"}; if(!it->second.pointTableConfigured)return {grpc::StatusCode::FAILED_PRECONDITION,"点表尚未配置"}; config=it->second.config; table=it->second.pointTable; id=it->second.connId; }
  auto bus=std::make_shared<TcpBus>(config.tcp()); auto status=bus->Open(); if(!status.ok()){std::lock_guard lock(mu_);links_[name].lastError=status.error_message();return status;}
  std::lock_guard lock(mu_); auto it=links_.find(name); if(it==links_.end()){bus->Close();return notFound(name);} it->second.bus=bus; it->second.state=ModbusTCPProto::LINK_STATE_RUNNING; it->second.lastError.clear(); it->second.pollThread=std::jthread([this,name,id,config,table,bus](std::stop_token stop){pollLoop(name,id,config,table,bus,stop);}); LOG_INFO("ModbusTCP 链路功能已启动: conn_name={}",name); return grpc::Status::OK;
}
grpc::Status LinkManager::StopLink(const std::string& name) {
  std::shared_ptr<TcpBus> bus;
  std::jthread pollThread;
  {
    std::lock_guard lock(mu_);
    auto it = links_.find(name);
    if (it == links_.end()) return notFound(name);
    if (it->second.state != ModbusTCPProto::LINK_STATE_RUNNING) {
      it->second.state = it->second.state == ModbusTCPProto::LINK_STATE_PENDING_DELETE
                             ? it->second.state
                             : ModbusTCPProto::LINK_STATE_STOPPED;
      return grpc::Status::OK;
    }
    it->second.pollThread.request_stop();
    pollThread = std::move(it->second.pollThread);
    bus = std::move(it->second.bus);
    it->second.state = ModbusTCPProto::LINK_STATE_STOPPED;
  }
  if (pollThread.joinable()) pollThread.join();
  if (bus) bus->Close();
  LOG_INFO("ModbusTCP 链路功能已停止: conn_name={}", name);
  return grpc::Status::OK;
}

grpc::Status LinkManager::UpsertPointTable(const ModbusTCPProto::UpsertPointTableRequest& request) {
  if (request.conn_name().empty()) return {grpc::StatusCode::INVALID_ARGUMENT, "conn_name 不能为空"};
  bool ready = false;
  {
    std::lock_guard lock(mu_);
    auto it = links_.find(request.conn_name());
    if (it == links_.end()) return notFound(request.conn_name());
    if (it->second.state == ModbusTCPProto::LINK_STATE_RUNNING) return {grpc::StatusCode::FAILED_PRECONDITION, "运行中的链路不能更新点表"};
    ModbusRTU::PointTable next = it->second.pointTable;
    auto status = next.Upsert(request.points(), request.replace());
    if (!status.ok()) return status;
    status = dataCenter_.UpsertConnTags(it->second.connId, next.Tags(), true);
    if (!status.ok()) return status;
    it->second.pointTable = std::move(next);
    it->second.pointTableConfigured = !it->second.pointTable.Points().empty();
    ready = it->second.pointTableConfigured;
    status = savePointTablesLocked();
    if (!status.ok()) return status;
  }
  if (ready) {
    auto status = StartLink(request.conn_name());
    if (!status.ok()) LOG_WARNING("ModbusTCP 点表下发后自动启动失败: conn_name={}, 原因={}", request.conn_name(), status.error_message());
  }
  return grpc::Status::OK;
}
grpc::Status LinkManager::GetPointTable(
    const std::string& name, ModbusTCPProto::PointTable* out) const {
  if (name.empty() || out == nullptr) {
    return {grpc::StatusCode::INVALID_ARGUMENT, "连接名或响应为空"};
  }
  std::lock_guard lock(mu_);
  const auto it = links_.find(name);
  if (it == links_.end()) {
    return notFound(name);
  }
  out->Clear();
  out->set_conn_name(name);
  ModbusRTUProto::PointTable normalized;
  it->second.pointTable.ToProto(name, &normalized);
  for (const auto& point : normalized.points()) {
    *out->add_points() = point;
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::executePointWrite(const Link& link,
                                            const ModbusRTU::PointTable::Point& point,
                                            const DataCenterProto::PointValue& value,
                                            DataCenterProto::ExecuteCommandResponse* response) {
  const auto engineering = toDecimal(value);
  if (!engineering.has_value()) {
    return {grpc::StatusCode::INVALID_ARGUMENT,
            std::format("命令值无法转换为 Decimal20: {}",
                        mskdsp::numeric::DecimalErrorMessage(
                            engineering.error()))};
  }
  uint32_t address = point.address;
  if (link.config.address_base() == ModbusRTUProto::ADDRESS_BASE_ONE) {
    if (address == 0) return {grpc::StatusCode::INVALID_ARGUMENT, "address_base=ONE 但 address 为 0"};
    --address;
  }
  if (address > 65535) return {grpc::StatusCode::OUT_OF_RANGE, "写点地址超出范围"};
  const auto id = static_cast<uint8_t>(link.config.tcp().unit_id());
  grpc::Status status;
  if (point.function == ModbusRTUProto::FUNCTION_WRITE_SINGLE_COIL) {
    status = link.bus->WriteSingleCoil(
        id, static_cast<uint16_t>(address), !engineering->IsZero());
  } else if (point.function == ModbusRTUProto::FUNCTION_WRITE_SINGLE_REGISTER) {
    if (point.type == ModbusRTUProto::DATA_TYPE_BOOL) {
      status = link.bus->WriteSingleRegister(
          id, static_cast<uint16_t>(address),
          engineering->IsZero() ? 0 : 1);
    } else if (point.type == ModbusRTUProto::DATA_TYPE_UINT16 ||
               point.type == ModbusRTUProto::DATA_TYPE_INT16) {
      const auto raw = mskdsp::numeric::ReverseEngineering(
          *engineering, point.scale, point.offset);
      if (!raw.has_value()) {
        return {grpc::StatusCode::INVALID_ARGUMENT, "工程量反向缩放失败"};
      }
      uint16_t word = 0;
      if (point.type == ModbusRTUProto::DATA_TYPE_INT16) {
        const auto integer = quantizeRegister<int16_t>(*raw);
        if (!integer.has_value()) {
          return {grpc::StatusCode::OUT_OF_RANGE,
                  "INT16 写入值超出寄存器范围"};
        }
        word = static_cast<uint16_t>(*integer);
      } else {
        const auto integer = quantizeRegister<uint16_t>(*raw);
        if (!integer.has_value()) {
          return {grpc::StatusCode::OUT_OF_RANGE,
                  "UINT16 写入值超出寄存器范围"};
        }
        word = *integer;
      }
      if (point.byteOrder == ModbusRTUProto::BYTE_ORDER_BA) word = swapBytes(word);
      status = link.bus->WriteSingleRegister(id, static_cast<uint16_t>(address), word);
    } else {
      return {grpc::StatusCode::UNIMPLEMENTED, "写单寄存器仅支持 BOOL、INT16、UINT16"};
    }
  } else if (point.function == ModbusRTUProto::FUNCTION_WRITE_MULTIPLE_REGISTERS) {
    const auto raw = mskdsp::numeric::ReverseEngineering(
        *engineering, point.scale, point.offset);
    if (!raw.has_value()) {
      return {grpc::StatusCode::INVALID_ARGUMENT, "工程量反向缩放失败"};
    }
    std::vector<uint16_t> words;
    if (point.type == ModbusRTUProto::DATA_TYPE_UINT16 || point.type == ModbusRTUProto::DATA_TYPE_INT16) {
      uint16_t word = 0;
      if (point.type == ModbusRTUProto::DATA_TYPE_INT16) {
        const auto integer = quantizeRegister<int16_t>(*raw);
        if (!integer.has_value()) {
          return {grpc::StatusCode::OUT_OF_RANGE,
                  "INT16 写入值超出寄存器范围"};
        }
        word = static_cast<uint16_t>(*integer);
      } else {
        const auto integer = quantizeRegister<uint16_t>(*raw);
        if (!integer.has_value()) {
          return {grpc::StatusCode::OUT_OF_RANGE,
                  "UINT16 写入值超出寄存器范围"};
        }
        word = *integer;
      }
      if (point.byteOrder == ModbusRTUProto::BYTE_ORDER_BA) word = swapBytes(word);
      words.push_back(word);
    } else if (point.type == ModbusRTUProto::DATA_TYPE_UINT32 || point.type == ModbusRTUProto::DATA_TYPE_INT32) {
      uint32_t value32 = 0;
      if (point.type == ModbusRTUProto::DATA_TYPE_INT32) {
        const auto integer = quantizeRegister<int32_t>(*raw);
        if (!integer.has_value()) {
          return {grpc::StatusCode::OUT_OF_RANGE,
                  "INT32 写入值超出 32 位寄存器范围"};
        }
        value32 = static_cast<uint32_t>(*integer);
      } else {
        const auto integer = quantizeRegister<uint32_t>(*raw);
        if (!integer.has_value()) {
          return {grpc::StatusCode::OUT_OF_RANGE,
                  "UINT32 写入值超出 32 位寄存器范围"};
        }
        value32 = *integer;
      }
      uint16_t high = static_cast<uint16_t>(value32 >> 16);
      uint16_t low = static_cast<uint16_t>(value32);
      if (point.byteOrder == ModbusRTUProto::BYTE_ORDER_BA) {
        high = swapBytes(high);
        low = swapBytes(low);
      }
      if (point.wordOrder == ModbusRTUProto::WORD_ORDER_LH) std::swap(high, low);
      words = {high, low};
    } else {
      return {grpc::StatusCode::UNIMPLEMENTED, "写多寄存器类型不支持"};
    }
    status = link.bus->WriteMultipleRegisters(id, static_cast<uint16_t>(address), words);
  } else {
    return {grpc::StatusCode::INVALID_ARGUMENT, "点位不是可写功能码"};
  }
  if (response != nullptr) {
    response->set_status(status.ok() ? DataCenterProto::COMMAND_ACCEPTED : DataCenterProto::COMMAND_TARGET_UNAVAILABLE);
    response->set_reason(status.ok() ? "Modbus TCP 同步写命令已执行" : "Modbus TCP 写入失败: " + status.error_message());
    const auto legacyValue = engineering->ToDouble();
    if (status.ok() && legacyValue.has_value()) {
      response->set_accepted_value(*legacyValue);
    }
    if (status.ok()) {
      response->set_accepted_value_decimal(engineering->ToFixedString());
    }
  }
  return status;
}

grpc::Status LinkManager::ExecuteCommand(const DataCenterProto::ExecuteCommandRequest& request,
                                         DataCenterProto::ExecuteCommandResponse* response) {
  if (response == nullptr) return {grpc::StatusCode::INVALID_ARGUMENT, "response 为空"};
  response->Clear();
  if (!request.has_dst() || request.dst().tag().empty()) return {grpc::StatusCode::INVALID_ARGUMENT, "dst/tag 不能为空"};
  if (!request.dst().module_name().empty() && request.dst().module_name() != ModbusTCPLibInfo.LIB_NAME) {
    return {grpc::StatusCode::INVALID_ARGUMENT, "dst.module_name 不是 ModbusTCP"};
  }
  std::string connName;
  uint32_t connId = 0;
  ModbusTCPProto::LinkConfig config;
  ModbusRTU::PointTable::Point point;
  std::shared_ptr<TcpBus> bus;
  {
    std::lock_guard lock(mu_);
    auto it = links_.end();
    if (!request.dst().conn_name().empty()) it = links_.find(request.dst().conn_name());
    if (it == links_.end() && request.dst().conn_id() != 0) {
      for (auto candidate = links_.begin(); candidate != links_.end(); ++candidate) {
        if (candidate->second.connId == request.dst().conn_id()) {
          it = candidate;
          break;
        }
      }
    }
    if (it == links_.end()) return {grpc::StatusCode::NOT_FOUND, "未找到命令目标链路"};
    if (request.dst().conn_id() != 0 && request.dst().conn_id() != it->second.connId) {
      return {grpc::StatusCode::INVALID_ARGUMENT, "目的连接名称与 conn_id 不一致"};
    }
    if (it->second.state != ModbusTCPProto::LINK_STATE_RUNNING || !it->second.bus) {
      response->set_status(DataCenterProto::COMMAND_TARGET_UNAVAILABLE);
      response->set_reason("ModbusTCP 链路未运行");
      return grpc::Status::OK;
    }
    auto pointOpt = it->second.pointTable.FindByTag(request.dst().tag());
    if (!pointOpt) return {grpc::StatusCode::NOT_FOUND, "未找到目标写点"};
    connName = it->first;
    connId = it->second.connId;
    config = it->second.config;
    point = *pointOpt;
    bus = it->second.bus;
  }
  Link link;
  link.config = std::move(config);
  link.connId = connId;
  link.bus = std::move(bus);
  *response->mutable_dst() = request.dst();
  response->mutable_dst()->set_conn_name(connName);
  response->mutable_dst()->set_conn_id(connId);
  response->mutable_dst()->set_module_name(ModbusTCPLibInfo.LIB_NAME);
  const auto requested = toDecimal(request.value());
  if (!requested.has_value()) {
    response->set_status(DataCenterProto::COMMAND_REJECTED);
    response->set_reject_code(
        DataCenterProto::COMMAND_REJECT_UNSUPPORTED_POINT);
    response->set_reason(std::format(
        "命令值无法转换为 Decimal20: {}",
        mskdsp::numeric::DecimalErrorMessage(requested.error())));
    return grpc::Status::OK;
  }
  const auto legacyRequested = requested->ToDouble();
  if (legacyRequested.has_value()) {
    response->set_requested_value(*legacyRequested);
  }
  response->set_requested_value_decimal(requested->ToFixedString());
  LOG_INFO("ModbusTCP 收到同步写命令: conn_name={}, conn_id={}, tag={}, value={}, request_id={}",
           connName, connId, request.dst().tag(), requested->ToString(),
           request.request_id());
  return executePointWrite(link, point, request.value(), response);
}

void LinkManager::pollLoop(std::string name,
                           uint32_t connId,
                           ModbusTCPProto::LinkConfig config,
                           ModbusRTU::PointTable table,
                           std::shared_ptr<TcpBus> bus,
                           std::stop_token stop) {
  const auto points = table.Points();
  std::unordered_map<std::string, Decimal20> lastReportedByTag;
  lastReportedByTag.reserve(points.size());
  LOG_INFO("ModbusTCP 轮询开始: conn_name={}, points={}", name,
           points.size());
  while (!stop.stop_requested()) {
    for (const auto& point : points) {
      if (stop.stop_requested() || isWrite(point.function)) {
        continue;
      }
      uint32_t address = point.address;
      if (config.address_base() == ModbusRTUProto::ADDRESS_BASE_ONE) {
        if (address == 0) {
          LOG_WARNING("ModbusTCP 点表地址非法: conn_name={}, tag={}, address=0",
                      name, point.tag);
          continue;
        }
        --address;
      }

      const auto id = static_cast<uint8_t>(config.tcp().unit_id());
      grpc::Status status;
      if (point.function == ModbusRTUProto::FUNCTION_READ_COILS) {
        bool value = false;
        status = bus->ReadCoil(id, static_cast<uint16_t>(address), &value);
        if (status.ok()) {
          status = dataCenter_.PublishBool(
              connId, point.tag, value, DataCenterProto::QUALITY_GOOD, 0);
        }
      } else if (isReadRegister(point.function)) {
        const bool inputRegisters =
            point.function ==
            ModbusRTUProto::FUNCTION_READ_INPUT_REGISTERS;
        std::vector<uint16_t> values;
        status = inputRegisters
            ? bus->ReadInputRegisters(
                  id, static_cast<uint16_t>(address),
                  static_cast<uint16_t>(point.regCount), &values)
            : bus->ReadHoldingRegisters(
                  id, static_cast<uint16_t>(address),
                  static_cast<uint16_t>(point.regCount), &values);
        if (status.ok() && values.size() != point.regCount) {
          status = grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                "寄存器响应数量异常");
        }
        if (status.ok() && point.type == ModbusRTUProto::DATA_TYPE_BOOL) {
          uint32_t raw = point.regCount == 2
              ? decode32(values[0], values[1], point.wordOrder,
                         point.byteOrder)
              : values[0];
          if (point.byteOrder == ModbusRTUProto::BYTE_ORDER_BA &&
              point.regCount == 1) {
            raw = swapBytes(static_cast<uint16_t>(raw));
          }
          const bool value =
              ((raw >> point.bitIndex.value_or(0)) & 1u) != 0;
          status = dataCenter_.PublishBool(
              connId, point.tag, value, DataCenterProto::QUALITY_GOOD, 0);
        } else if (status.ok()) {
          int64_t raw = 0;
          if (point.type == ModbusRTUProto::DATA_TYPE_UINT16) {
            raw = point.byteOrder == ModbusRTUProto::BYTE_ORDER_BA
                ? swapBytes(values[0])
                : values[0];
          } else if (point.type == ModbusRTUProto::DATA_TYPE_INT16) {
            const auto word =
                point.byteOrder == ModbusRTUProto::BYTE_ORDER_BA
                ? swapBytes(values[0])
                : values[0];
            raw = static_cast<int16_t>(word);
          } else if (point.type == ModbusRTUProto::DATA_TYPE_UINT32) {
            raw = decode32(values[0], values[1], point.wordOrder,
                           point.byteOrder);
          } else if (point.type == ModbusRTUProto::DATA_TYPE_INT32) {
            raw = static_cast<int32_t>(decode32(
                values[0], values[1], point.wordOrder, point.byteOrder));
          } else {
            status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                  "寄存器点位类型不支持");
          }

          if (status.ok()) {
            const auto engineering = engineeringFromRaw(raw, point);
            if (!engineering.has_value()) {
              status = grpc::Status(
                  grpc::StatusCode::OUT_OF_RANGE,
                  std::format("工程量换算超出 Decimal20 范围: {}",
                              mskdsp::numeric::DecimalErrorMessage(
                                  engineering.error())));
            } else {
              const auto last = lastReportedByTag.find(point.tag);
              if (last != lastReportedByTag.end() &&
                  !mskdsp::numeric::ShouldReport(
                      *engineering, last->second, point.deadband)) {
                LOG_DEBUG("ModbusTCP 死区过滤上报: conn_name={}, tag={}, 当前值={}, 上次值={}, 死区={}",
                          name, point.tag, engineering->ToString(),
                          last->second.ToString(), point.deadband.ToString());
                continue;
              }
              status = dataCenter_.PublishDecimal(
                  connId, point.tag, engineering->ToFixedString(),
                  DataCenterProto::QUALITY_GOOD, 0);
              if (status.ok()) {
                lastReportedByTag[point.tag] = *engineering;
              }
            }
          }
        }
      }
      if (!status.ok()) {
        LOG_WARNING("ModbusTCP 轮询点失败: conn_name={}, tag={}, 原因={}",
                    name, point.tag, status.error_message());
      }
    }
    std::this_thread::sleep_for(
        std::chrono::milliseconds(config.poll_interval_ms()));
  }
  LOG_INFO("ModbusTCP 轮询结束: conn_name={}", name);
}
}
