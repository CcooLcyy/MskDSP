#include "IEC104TcpLink.h"

#include <chrono>
#include <format>
#include <optional>
#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include "IEC104TcpSession.h"

namespace IEC104 {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace {
constexpr std::chrono::milliseconds kDefaultReconnectDelay{1000};

grpc::Status validateLinkConfig(const IEC104Proto::LinkConfig& config) {
  if (config.conn_name().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_name is required");
  }
  if (config.role() == IEC104Proto::ROLE_SERVER) {
    if (config.local().port() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "local.port is required for server role");
    }
  } else if (config.role() == IEC104Proto::ROLE_CLIENT) {
    if (config.remote().ip().empty() || config.remote().port() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "remote.ip/port is required for client role");
    }
  } else {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "role is required");
  }
  return grpc::Status::OK;
}

grpc::Status parseEndpoint(const IEC104Proto::Endpoint& ep, bool allowAny, tcp::endpoint* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
  }
  if (ep.port() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "port is required");
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
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::format("invalid ip: {}", ep.ip()));
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
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "link already running");
  }

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
      acceptor_.reset();
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("acceptor open failed: {}", ec.message()));
    }
    acceptor_->set_option(tcp::acceptor::reuse_address(true), ec);
    if (ec) {
      acceptor_.reset();
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("acceptor set_option failed: {}", ec.message()));
    }
    acceptor_->bind(endpoint, ec);
    if (ec) {
      acceptor_.reset();
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("acceptor bind failed: {}", ec.message()));
    }
    acceptor_->listen(tcp::acceptor::max_listen_connections, ec);
    if (ec) {
      acceptor_.reset();
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("acceptor listen failed: {}", ec.message()));
    }
  }

  thread_ = std::jthread([this](std::stop_token st) { run(st); });
  return grpc::Status::OK;
}

void TcpLink::Stop() {
  if (!thread_.joinable()) {
    return;
  }
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

void TcpLink::SendMeasuredValue(uint32_t ioa, double value, uint8_t quality, uint8_t cause) {
  auto s = session();
  if (!s) {
    return;
  }
  s->SendMeasuredValue(ioa, value, quality, cause);
}

void TcpLink::SetMeasuredValueCallback(MeasuredValueCallback cb) {
  std::lock_guard<std::mutex> lock(mu_);
  onMeasuredValue_ = std::move(cb);
  if (session_) {
    session_->SetMeasuredValueCallback(onMeasuredValue_);
  }
}

void TcpLink::SetInterrogationSnapshotProvider(SnapshotProvider provider) {
  std::lock_guard<std::mutex> lock(mu_);
  interrogationSnapshotProvider_ = std::move(provider);
  if (session_) {
    session_->SetInterrogationSnapshotProvider(interrogationSnapshotProvider_);
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
      return;
    }

    auto newSession = std::make_shared<TcpSession>(io_, config_, false);
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (session_) {
        session_->Stop();
      }
      session_ = newSession;
      session_->SetMeasuredValueCallback(onMeasuredValue_);
      session_->SetInterrogationSnapshotProvider(interrogationSnapshotProvider_);
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
      scheduleReconnect(kDefaultReconnectDelay);
      return;
    }
    auto sock = std::make_shared<tcp::socket>(io_);
    asio::async_connect(*sock, results, [this, sock](const boost::system::error_code& ec, const tcp::endpoint&) {
      if (ec) {
        scheduleReconnect(kDefaultReconnectDelay);
        return;
      }

      auto newSession = std::make_shared<TcpSession>(io_, config_, true);
      newSession->SetClosedCallback([this]() { scheduleReconnect(kDefaultReconnectDelay); });
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (session_) {
          session_->Stop();
        }
        session_ = newSession;
        session_->SetMeasuredValueCallback(onMeasuredValue_);
        session_->SetInterrogationSnapshotProvider(interrogationSnapshotProvider_);
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
