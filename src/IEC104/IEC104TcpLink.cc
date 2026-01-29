#include "IEC104TcpLink.h"

#include <chrono>
#include <format>
#include <optional>
#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include "IEC104TcpSession.h"
#include "Logger.h"
#include "IEC104LibInfo.h"
#include "ThreadUtil.hpp"

namespace IEC104 {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace {
constexpr std::chrono::milliseconds kDefaultReconnectDelay{1000};

grpc::Status validateLinkConfig(const IEC104Proto::LinkConfig& config) {
  if (config.conn_name().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_name 不能为空");
  }
  if (config.role() == IEC104Proto::ROLE_SERVER) {
    if (config.local().port() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "role=ROLE_SERVER 时 local.port 不能为空");
    }
  } else if (config.role() == IEC104Proto::ROLE_CLIENT) {
    if (config.remote().ip().empty() || config.remote().port() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "role=ROLE_CLIENT 时 remote.ip/port 不能为空");
    }
  } else {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "role 不能为空");
  }
  return grpc::Status::OK;
}

grpc::Status parseEndpoint(const IEC104Proto::Endpoint& ep, bool allowAny, tcp::endpoint* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  if (ep.port() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "port 不能为空");
  }

  if (ep.ip().empty() || ep.ip() == "0.0.0.0") {
    if (allowAny) {
      *out = tcp::endpoint(asio::ip::address_v4::any(), ep.port());
      return grpc::Status::OK;
    }
    *out = tcp::endpoint(asio::ip::address_v4::loopback(), ep.port());
    return grpc::Status::OK;
  }

  boost::system::error_code ec;
  auto addr = asio::ip::make_address(ep.ip(), ec);
  if (ec) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::format("ip 非法: {}", ep.ip()));
  }
  *out = tcp::endpoint(addr, ep.port());
  return grpc::Status::OK;
}
}  // namespace

TcpLink::TcpLink(IEC104Proto::LinkConfig config) : config_(normalizeConfig(config)) {}

TcpLink::~TcpLink() {
  Stop();
}

grpc::Status TcpLink::Start() {
  auto status = validateLinkConfig(config_);
  if (!status.ok()) {
    return status;
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (thread_.joinable()) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路已在运行");
  }

  LOG_INFO("IEC104 链路启动: conn_name={}, 角色={}",
           config_.conn_name(),
           config_.role() == IEC104Proto::ROLE_CLIENT ? "客户端" : "服务端");

  io_.restart();
  acceptor_.reset();
  resolver_.reset();
  reconnectTimer_.reset();

  if (config_.role() == IEC104Proto::ROLE_SERVER) {
    tcp::endpoint endpoint;
    status = parseEndpoint(config_.local(), true, &endpoint);
    if (!status.ok()) {
      return status;
    }

    acceptor_.emplace(io_);
    boost::system::error_code ec;
    acceptor_->open(endpoint.protocol(), ec);
    if (ec) {
      LOG_ERROR("IEC104 监听器打开失败: conn_name={}, 错误={}", config_.conn_name(), ec.message());
      acceptor_.reset();
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("acceptor 打开失败: {}", ec.message()));
    }
    acceptor_->set_option(tcp::acceptor::reuse_address(true), ec);
    if (ec) {
      LOG_ERROR("IEC104 监听器设置参数失败: conn_name={}, 错误={}", config_.conn_name(), ec.message());
      acceptor_.reset();
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("acceptor 设置参数失败: {}", ec.message()));
    }
    acceptor_->bind(endpoint, ec);
    if (ec) {
      LOG_ERROR("IEC104 监听器绑定失败: conn_name={}, 错误={}", config_.conn_name(), ec.message());
      acceptor_.reset();
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("acceptor 绑定失败: {}", ec.message()));
    }
    acceptor_->listen(tcp::acceptor::max_listen_connections, ec);
    if (ec) {
      LOG_ERROR("IEC104 监听器监听失败: conn_name={}, 错误={}", config_.conn_name(), ec.message());
      acceptor_.reset();
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("acceptor 监听失败: {}", ec.message()));
    }
    LOG_INFO("IEC104 开始监听: conn_name={}, 本地={}:{}", config_.conn_name(), endpoint.address().to_string(), endpoint.port());
  }

  thread_ = ModuleManager::StartModuleThread(
      IEC104LibInfo.LIB_NAME,
      [this](std::stop_token st) { run(st); });
  return grpc::Status::OK;
}

void TcpLink::Stop() {
  if (!thread_.joinable()) {
    return;
  }
  LOG_INFO("IEC104 链路停止: conn_name={}", config_.conn_name());
  thread_.request_stop();
  thread_.join();

  std::lock_guard<std::mutex> lock(mu_);
  session_.reset();
  acceptor_.reset();
  resolver_.reset();
  reconnectTimer_.reset();
}

bool TcpLink::IsRunning() const {
  std::lock_guard<std::mutex> lock(mu_);
  return thread_.joinable();
}

void TcpLink::SendPointValue(const PointValue& value, uint8_t cause) {
  auto s = session();
  if (!s) {
    return;
  }
  s->SendPointValue(value, cause);
}

void TcpLink::SendTimeSync(int64_t tsMs) {
  auto s = session();
  if (!s) {
    return;
  }
  s->SendTimeSync(tsMs);
}

void TcpLink::SendSingleCommand(uint32_t ioa, bool value, bool useSelect) {
  auto s = session();
  if (!s) {
    return;
  }
  s->SendSingleCommand(ioa, value, useSelect);
}

void TcpLink::SendSetpointCommand(uint32_t ioa, double value) {
  auto s = session();
  if (!s) {
    return;
  }
  s->SendSetpointCommand(ioa, value);
}

void TcpLink::SetPointValueCallback(PointValueCallback cb) {
  std::lock_guard<std::mutex> lock(mu_);
  onPointValue_ = std::move(cb);
  if (session_) {
    session_->SetPointValueCallback(onPointValue_);
  }
}

void TcpLink::SetInterrogationSnapshotProvider(SnapshotProvider provider) {
  std::lock_guard<std::mutex> lock(mu_);
  interrogationSnapshotProvider_ = std::move(provider);
  if (session_) {
    session_->SetInterrogationSnapshotProvider(interrogationSnapshotProvider_);
  }
}

void TcpLink::SetTimeSyncCallback(TimeSyncCallback cb) {
  std::lock_guard<std::mutex> lock(mu_);
  onTimeSync_ = std::move(cb);
  if (session_) {
    session_->SetTimeSyncCallback(onTimeSync_);
  }
}

void TcpLink::SetCommandCallback(CommandCallback cb) {
  std::lock_guard<std::mutex> lock(mu_);
  onCommand_ = std::move(cb);
  if (session_) {
    session_->SetCommandCallback(onCommand_);
  }
}

void TcpLink::run(std::stop_token st) {
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard(io_.get_executor());
  std::stop_callback cb(st, [this]() {
    boost::system::error_code ec;
    if (acceptor_) {
      acceptor_->close(ec);
    }
    if (reconnectTimer_) {
      reconnectTimer_->cancel();
    }
    io_.stop();
  });

  if (config_.role() == IEC104Proto::ROLE_SERVER) {
    if (!acceptor_) {
      return;
    }
    startAccept();
  } else if (config_.role() == IEC104Proto::ROLE_CLIENT) {
    resolver_.emplace(io_);
    startConnect();
  }

  io_.run();
}

void TcpLink::startAccept() {
  if (!acceptor_) {
    return;
  }
  acceptor_->async_accept([this](const boost::system::error_code& ec, tcp::socket socket) {
    if (ec) {
      if (ec != boost::asio::error::operation_aborted) {
        LOG_WARNING("IEC104 接收连接失败: conn_name={}, 错误={}", config_.conn_name(), ec.message());
        startAccept();
      }
      return;
    }

    boost::system::error_code remoteEc;
    auto remote = socket.remote_endpoint(remoteEc);
    if (remoteEc) {
      LOG_INFO("IEC104 已接受连接: conn_name={}, 远端未知, 错误={}",
               config_.conn_name(),
               remoteEc.message());
    } else {
      LOG_INFO("IEC104 已接受连接: conn_name={}, 远端={}:{}",
               config_.conn_name(),
               remote.address().to_string(),
               remote.port());
    }
    auto newSession = std::make_shared<TcpSession>(io_, config_, false);
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (session_) {
        session_->Stop();
      }
      session_ = newSession;
      session_->SetPointValueCallback(onPointValue_);
      session_->SetInterrogationSnapshotProvider(interrogationSnapshotProvider_);
      session_->SetTimeSyncCallback(onTimeSync_);
      session_->SetCommandCallback(onCommand_);
    }
    newSession->Start(std::move(socket));

    startAccept();
  });
}

void TcpLink::startConnect() {
  if (!resolver_) {
    return;
  }
  if (thread_.get_stop_token().stop_requested()) {
    return;
  }

  auto host = config_.remote().ip();
  auto port = std::to_string(config_.remote().port());

  resolver_->async_resolve(host, port, [this](const boost::system::error_code& ec, tcp::resolver::results_type results) {
    if (ec) {
      LOG_WARNING("IEC104 解析远端地址失败: conn_name={}, 远端={}:{}, 错误={}",
                  config_.conn_name(),
                  config_.remote().ip(),
                  config_.remote().port(),
                  ec.message());
      scheduleReconnect(kDefaultReconnectDelay);
      return;
    }
    auto sock = std::make_shared<tcp::socket>(io_);
    asio::async_connect(*sock, results, [this, sock](const boost::system::error_code& ec, const tcp::endpoint&) {
      if (ec) {
        LOG_WARNING("IEC104 连接远端失败: conn_name={}, 远端={}:{}, 错误={}",
                    config_.conn_name(),
                    config_.remote().ip(),
                    config_.remote().port(),
                    ec.message());
        scheduleReconnect(kDefaultReconnectDelay);
        return;
      }

      boost::system::error_code localEc;
      boost::system::error_code remoteEc;
      auto local = sock->local_endpoint(localEc);
      auto remote = sock->remote_endpoint(remoteEc);
      if (localEc || remoteEc) {
        LOG_INFO("IEC104 连接已建立: conn_name={}, 本地未知, 远端未知, 本地错误={}, 远端错误={}",
                 config_.conn_name(),
                 localEc.message(),
                 remoteEc.message());
      } else {
        LOG_INFO("IEC104 连接已建立: conn_name={}, 本地={}:{}, 远端={}:{}",
                 config_.conn_name(),
                 local.address().to_string(),
                 local.port(),
                 remote.address().to_string(),
                 remote.port());
      }
      auto newSession = std::make_shared<TcpSession>(io_, config_, true);
      newSession->SetClosedCallback([this]() { scheduleReconnect(kDefaultReconnectDelay); });
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (session_) {
          session_->Stop();
        }
      session_ = newSession;
      session_->SetPointValueCallback(onPointValue_);
      session_->SetInterrogationSnapshotProvider(interrogationSnapshotProvider_);
      session_->SetTimeSyncCallback(onTimeSync_);
      session_->SetCommandCallback(onCommand_);
    }
      newSession->Start(std::move(*sock));
    });
  });
}

void TcpLink::scheduleReconnect(std::chrono::milliseconds delay) {
  if (config_.role() != IEC104Proto::ROLE_CLIENT) {
    return;
  }
  if (thread_.get_stop_token().stop_requested()) {
    return;
  }
  if (!reconnectTimer_) {
    reconnectTimer_.emplace(io_);
  }
  reconnectTimer_->expires_after(delay);
  LOG_DEBUG("IEC104 计划重连: conn_name={}, delay_ms={}", config_.conn_name(), delay.count());
  reconnectTimer_->async_wait([this](const boost::system::error_code& ec) {
    if (ec) {
      return;
    }
    startConnect();
  });
}

void TcpLink::setSession(std::shared_ptr<TcpSession> session) {
  std::lock_guard<std::mutex> lock(mu_);
  session_ = std::move(session);
}

std::shared_ptr<TcpSession> TcpLink::session() const {
  std::lock_guard<std::mutex> lock(mu_);
  return session_;
}

IEC104Proto::LinkConfig TcpLink::normalizeConfig(const IEC104Proto::LinkConfig& in) {
  IEC104Proto::LinkConfig out(in);
  if (out.apci().k() == 0) {
    out.mutable_apci()->set_k(12);
  }
  if (out.apci().w() == 0) {
    out.mutable_apci()->set_w(8);
  }
  if (out.apci().t0() == 0) {
    out.mutable_apci()->set_t0(30);
  }
  if (out.apci().t1() == 0) {
    out.mutable_apci()->set_t1(15);
  }
  if (out.apci().t2() == 0) {
    out.mutable_apci()->set_t2(10);
  }
  if (out.apci().t3() == 0) {
    out.mutable_apci()->set_t3(20);
  }
  return out;
}

}  // namespace IEC104
