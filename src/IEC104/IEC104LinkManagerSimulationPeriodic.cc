#include <chrono>
#include <utility>

#include "IEC104LibInfo.h"
#include "IEC104LinkManager.h"
#include "Logger.h"
#include "ThreadUtil.hpp"

namespace IEC104 {

bool LinkManager::simulationPeriodicReadyLocked(const LinkRuntime &link) const {
  return link.simulationPeriodic.enabled() && isSlaveStation(link.config) &&
      !link.simulationValues.empty() && link.state == IEC104Proto::LINK_STATE_RUNNING &&
      link.transport && link.transport->IsDataTransferActive();
}

void LinkManager::fillSimulationPeriodicStatusLocked(const LinkRuntime &link, IEC104Proto::SimulationPeriodicStatus *out) const {
  out->Clear();
  out->set_conn_name(link.config.conn_name());
  out->set_enabled(link.simulationPeriodic.enabled());
  out->set_interval_seconds(link.simulationPeriodic.intervalSeconds());
  out->set_running(simulationPeriodicReadyLocked(link));
  out->set_last_sent_ms(link.simulationPeriodic.lastSentMs());
}

grpc::Status LinkManager::SetSimulationPeriodicReport(const IEC104Proto::SimulationPeriodicReportRequest &request, IEC104Proto::SimulationPeriodicStatus *out) {
  auto status = validateConnName(request.conn_name());
  if (!status.ok()) {
    return status;
  }
  if (!out || request.interval_seconds() < 1 || request.interval_seconds() > 3600) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "周期上报间隔必须为 1～3600 秒且响应不能为空");
  }
  // 串行化线程创建与停止；后台循环只使用 mu_，停机等待时不占用 mu_。
  std::lock_guard lifecycleLock(simulationLifecycleMu_);
  std::lock_guard lock(mu_);
  auto it = linksByName_.find(request.conn_name());
  if (it == linksByName_.end()) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "IEC104 连接不存在");
  }
  auto &link = it->second;
  if (!isSlaveStation(link.config)) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模拟周期上报仅支持 IEC104 从站链路");
  }
  if (request.enabled() && link.simulationValues.empty()) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "启用周期上报前请先生成或保存模拟快照");
  }
  const bool changed = link.simulationPeriodic.Configure(
      request.enabled(), request.interval_seconds(), detail::SimulationPeriodicSchedule::Clock::now());
  if (request.enabled() && !simulationPeriodicThread_.joinable()) {
    simulationPeriodicThread_ = ModuleManager::StartModuleThread(
        IEC104LibInfo.LIB_NAME, [this](std::stop_token token) { runSimulationPeriodic(token); });
  }
  fillSimulationPeriodicStatusLocked(link, out);
  LOG_INFO("IEC104 模拟周期上报设置: conn_name={}, 启用={}, 间隔秒={}, 配置变化={}, 具备发送条件={}", request.conn_name(), out->enabled(), out->interval_seconds(), changed, out->running());
  return grpc::Status::OK;
}

grpc::Status LinkManager::GetSimulationPeriodicStatus(const std::string &connName, IEC104Proto::SimulationPeriodicStatus *out) const {
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }
  if (!out) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "周期上报状态响应不能为空");
  }
  std::lock_guard lock(mu_);
  auto it = linksByName_.find(connName);
  if (it == linksByName_.end()) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "IEC104 连接不存在");
  }
  fillSimulationPeriodicStatusLocked(it->second, out);
  return grpc::Status::OK;
}

void LinkManager::runSimulationPeriodic(std::stop_token stopToken) {
  LOG_INFO("IEC104 模拟周期上报调度线程已启动");
  std::unique_lock lock(mu_);
  while (!stopToken.stop_requested()) {
    const auto now = detail::SimulationPeriodicSchedule::Clock::now();
    for (auto &[connName, link] : linksByName_) {
      if (!link.simulationPeriodic.ConsumeDue(now, simulationPeriodicReadyLocked(link))) {
        continue;
      }
      const auto status = sendSimulationValuesLocked(connName, link, true);
      if (!status.ok()) {
        LOG_WARNING("IEC104 模拟周期快照未提交发送: conn_name={}, 原因={}", connName, status.error_message());
        continue;
      }
      const auto sentMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
      link.simulationPeriodic.MarkSent(sentMs);
      LOG_INFO("IEC104 模拟周期快照已提交发送: conn_name={}, 时间毫秒={}", connName, sentMs);
    }
    simulationPeriodicCv_.wait_for(lock, stopToken, std::chrono::milliseconds(100), [] { return false; });
  }
  LOG_INFO("IEC104 模拟周期上报调度线程已退出");
}

void LinkManager::StopSimulationPeriodicReports() {
  std::lock_guard lifecycleLock(simulationLifecycleMu_);
  std::jthread thread;
  {
    std::lock_guard lock(mu_);
    for (auto &[connName, link] : linksByName_) {
      link.simulationPeriodic = detail::SimulationPeriodicSchedule{};
    }
    thread = std::move(simulationPeriodicThread_);
  }
  if (thread.joinable()) {
    thread.request_stop();
    thread.join();
    LOG_INFO("IEC104 模块周期上报任务已清理");
  }
}

}  // namespace IEC104
