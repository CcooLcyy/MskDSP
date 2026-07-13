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

struct PointValue {
  uint32_t ioa = 0;
  IEC104Proto::PointType type = IEC104Proto::POINT_TYPE_UNSPECIFIED;
  bool boolValue = false;
  double doubleValue = 0.0;
  uint8_t quality = 0;
  int64_t tsMs = 0;
};

struct CommandValue {
  uint32_t ioa = 0;
  IEC104Proto::PointType type = IEC104Proto::POINT_TYPE_UNSPECIFIED;
  bool boolValue = false;
  double doubleValue = 0.0;
};

struct CommandResult {
  bool accepted = true;
  std::string reason;
};

class TcpLink {
public:
  using PointValueCallback = std::function<void(const PointValue&)>;
  using SnapshotProvider = std::function<std::vector<PointValue>()>;
  using TimeSyncCallback = std::function<void(int64_t)>;
  using CommandCallback = std::function<CommandResult(const CommandValue&)>;

  explicit TcpLink(IEC104Proto::LinkConfig config);
  ~TcpLink();

  TcpLink(const TcpLink&) = delete;
  TcpLink& operator=(const TcpLink&) = delete;

  grpc::Status Start();
  void Stop();
  bool IsRunning() const;

  void SendPointValue(const PointValue& value, uint8_t cause);
  void SendTimeSync(int64_t tsMs);
  void SendSingleCommand(uint32_t ioa, bool value, bool useSelect);
  void SendSetpointCommand(uint32_t ioa, double value);
  void SetPointValueCallback(PointValueCallback cb);
  void SetInterrogationSnapshotProvider(SnapshotProvider provider);
  void SetTimeSyncCallback(TimeSyncCallback cb);
  void SetCommandCallback(CommandCallback cb);

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
  PointValueCallback onPointValue_;
  SnapshotProvider interrogationSnapshotProvider_;
  TimeSyncCallback onTimeSync_;
  CommandCallback onCommand_;
};

}  // namespace IEC104
