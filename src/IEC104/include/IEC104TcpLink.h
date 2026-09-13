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

struct SoeEvent {
  uint64_t eventSequence = 0;
  PointValue value;
};

struct CommandValue {
  uint32_t ioa = 0;
  IEC104Proto::PointType type = IEC104Proto::POINT_TYPE_UNSPECIFIED;
  IEC104Proto::RemoteControlType remoteControlType =
      IEC104Proto::REMOTE_CONTROL_TYPE_SINGLE;
  uint8_t controlValue = 0;
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
  using SoeReplayProvider = std::function<std::vector<SoeEvent>()>;
  using SoeAcknowledgedCallback = std::function<void(const std::vector<uint64_t>&)>;
  // 返回 true 表示对时已被业务接受，false 表示应回负确认。
  using TimeSyncCallback = std::function<bool(int64_t)>;
  using CommandCallback = std::function<CommandResult(const CommandValue&)>;
  using CommandExecutionModeCallback =
      std::function<std::optional<IEC104Proto::CommandExecutionMode>(uint32_t)>;
  using ConnectionStateCallback = std::function<void(IEC104Proto::ConnectionState)>;

  explicit TcpLink(IEC104Proto::LinkConfig config);
  ~TcpLink();

  TcpLink(const TcpLink&) = delete;
  TcpLink& operator=(const TcpLink&) = delete;

  grpc::Status Start();
  void Stop();
  bool IsRunning() const;
  IEC104Proto::ConnectionState ConnectionState() const;

  void SendPointValue(const PointValue& value, uint8_t cause);
  void SendSoe(const SoeEvent& event);
  void SendTimeSync(int64_t tsMs);
  void SendSingleCommand(uint32_t ioa, bool value, bool useSelect);
  void SendRemoteControl(uint32_t ioa,
                         IEC104Proto::RemoteControlType type,
                         uint8_t value,
                         IEC104Proto::CommandExecutionMode mode);
  void SendSetpointCommand(uint32_t ioa, double value);
  void SetPointValueCallback(PointValueCallback cb);
  void SetInterrogationSnapshotProvider(SnapshotProvider provider);
  void SetSoeReplayProvider(SoeReplayProvider provider);
  void SetSoeAcknowledgedCallback(SoeAcknowledgedCallback cb);
  void SetTimeSyncCallback(TimeSyncCallback cb);
  void SetCommandCallback(CommandCallback cb);
  void SetCommandExecutionModeCallback(CommandExecutionModeCallback cb);
  void SetConnectionStateCallback(ConnectionStateCallback cb);

private:
  void run(std::stop_token st);

  void startAccept();
  void startConnect();
  void scheduleReconnect(std::chrono::milliseconds delay);
  void handleSessionConnectionState(const std::shared_ptr<class TcpSession>& session,
                                    IEC104Proto::ConnectionState state);
  void setConnectionState(IEC104Proto::ConnectionState state);

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
  IEC104Proto::ConnectionState connectionState_ = IEC104Proto::CONNECTION_STATE_DISCONNECTED;
  PointValueCallback onPointValue_;
  SnapshotProvider interrogationSnapshotProvider_;
  SoeReplayProvider soeReplayProvider_;
  SoeAcknowledgedCallback onSoeAcknowledged_;
  TimeSyncCallback onTimeSync_;
  CommandCallback onCommand_;
  CommandExecutionModeCallback onCommandExecutionMode_;
  ConnectionStateCallback onConnectionState_;
};

}  // namespace IEC104
