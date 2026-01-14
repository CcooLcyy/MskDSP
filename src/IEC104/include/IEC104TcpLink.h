#pragma once

#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <grpcpp/support/status.h>

#include "IEC104.pb.h"

namespace IEC104 {

struct MeasuredValue {
  uint32_t ioa = 0;
  double value = 0.0;
  uint8_t quality = 0;
};

class TcpLink {
public:
  using MeasuredValueCallback = std::function<void(const MeasuredValue&)>;
  using SnapshotProvider = std::function<std::vector<MeasuredValue>()>;

  explicit TcpLink(IEC104Proto::LinkConfig config);
  ~TcpLink();

  TcpLink(const TcpLink&) = delete;
  TcpLink& operator=(const TcpLink&) = delete;

  grpc::Status Start();
  void Stop();
  bool IsRunning() const;

  void SendMeasuredValue(uint32_t ioa, double value, uint8_t quality, uint8_t cause);
  void SetMeasuredValueCallback(MeasuredValueCallback cb);
  void SetInterrogationSnapshotProvider(SnapshotProvider provider);

private:
  void run(std::stop_token st);

  void startAccept();
  void startConnect();
  void scheduleReconnect(std::chrono::milliseconds delay);

  void setSession(std::shared_ptr<class TcpSession> session);
  std::shared_ptr<class TcpSession> session() const;

  static IEC104Proto::LinkConfig normalizeConfig(const IEC104Proto::LinkConfig& in);

  IEC104Proto::LinkConfig config_;
  mutable std::mutex mu_;
  std::jthread thread_;

  boost::asio::io_context io_;
  std::optional<boost::asio::ip::tcp::acceptor> acceptor_;
  std::optional<boost::asio::ip::tcp::resolver> resolver_;
  std::optional<boost::asio::steady_timer> reconnectTimer_;

  std::shared_ptr<TcpSession> session_;
  MeasuredValueCallback onMeasuredValue_;
  SnapshotProvider interrogationSnapshotProvider_;
};

}  // namespace IEC104
