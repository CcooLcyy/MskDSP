#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "IEC61850ProtocolStack.h"
#include "IEC61850RealtimeSignalBus.h"

namespace IEC61850 {

struct SvMathMemberPlan {
  std::uint32_t inputSignalId = 0;
  std::uint32_t rmsSignalId = 0;
};

struct SvMathStreamPlan {
  std::uint32_t streamId = 0;
  // SCL smpRate表示每额定周期采样数；首期额定频率固定为50Hz。
  std::size_t samplesPerCycle = 0;
  std::uint32_t expectedAsduCount = 1;
  double nominalFrequencyHz = 50.0;
  std::vector<SvMathMemberPlan> members;
};

// 在唯一实时消费者线程中按完整样本窗口计算SV派生量。
// 构造后所有成员缓冲区固定，Process不分配内存、不访问外部服务。
class SvMathEngine {
public:
  SvMathEngine(std::vector<SvMathStreamPlan> plans,
               std::uint64_t sessionGeneration);

  SvMathEngine(const SvMathEngine&) = delete;
  SvMathEngine& operator=(const SvMathEngine&) = delete;

  // 输入是现有实时总线中的一个SV原始成员更新；返回写入outputs的数量。
  // nofASDU不为1、质量无效、序列缺口或窗口不足时不产生输出。
  std::size_t Process(const RealtimeSignalUpdate& update,
                      std::int64_t nowNs,
                      std::span<RealtimeSignalUpdate> outputs) noexcept;

  std::size_t streamCount() const noexcept;
  // 返回单次Process可能写入的最大派生量数量，供启动时预分配输出缓冲区。
  std::size_t maxOutputCount() const noexcept;
  std::uint64_t sessionGeneration() const noexcept;

private:
  struct StreamState {
    SvMathStreamPlan plan;
    std::vector<double> pendingValues;
    std::vector<bool> pendingSeen;
    std::vector<double> history;
    bool pendingValid = false;
    std::uint16_t pendingSampleCount = 0;
    bool hasCommittedSample = false;
    std::uint16_t lastSampleCount = 0;
    std::size_t historyWriteIndex = 0;
    std::size_t historyCount = 0;
  };

  std::vector<StreamState> streams_;
  std::uint64_t sessionGeneration_ = 0;
};

}  // namespace IEC61850
