#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "IEC61850MmsWorker.h"

namespace {

struct Options {
  std::string remoteIp = "127.0.0.1";
  std::uint16_t remotePort = 102;
  std::uint32_t timeoutMs = 10000;
  bool rcb = false;
  std::string valueType = "BOOLEAN";
};

constexpr std::int64_t kExpectedReportTimestampMs = 1700000000000LL;

std::string Upper(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::toupper(character));
                 });
  return result;
}

bool IsSupportedValueType(std::string_view valueType) {
  const auto type = Upper(valueType);
  return type == "BOOLEAN" || type == "INT32" || type == "INT32U" ||
         type == "FLOAT32" || type == "FLOAT64" || type == "QUALITY" ||
         type == "TIMESTAMP" || type.starts_with("VISSTRING");
}

bool ParseUnsigned(std::string_view text, std::uint32_t maximum,
                   std::uint32_t* output) {
  if (output == nullptr || text.empty()) {
    return false;
  }
  try {
    const auto value = std::stoul(std::string(text));
    if (value > maximum) {
      return false;
    }
    *output = static_cast<std::uint32_t>(value);
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseArguments(int argc, char** argv, Options* options) {
  if (options == nullptr) {
    return false;
  }
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--rcb") {
      options->rcb = true;
      continue;
    }
    if (index + 1 >= argc) {
      return false;
    }
    const std::string_view value(argv[++index]);
    if (argument == "--ip") {
      options->remoteIp = value;
      continue;
    }
    if (argument == "--port") {
      std::uint32_t port = 0;
      if (!ParseUnsigned(value, 65535, &port) || port == 0) {
        return false;
      }
      options->remotePort = static_cast<std::uint16_t>(port);
      continue;
    }
    if (argument == "--timeout-ms") {
      if (!ParseUnsigned(value, 600000, &options->timeoutMs) ||
          options->timeoutMs == 0) {
        return false;
      }
      continue;
    }
    if (argument == "--type") {
      if (!IsSupportedValueType(value) || !options->rcb) {
        return false;
      }
      options->valueType = Upper(value);
      continue;
    }
    return false;
  }
  return !options->remoteIp.empty();
}

void PrintUsage() {
  std::cerr << "用法: iec61850_mms_worker_smoke [--ip IPv4] [--port 102] "
               "[--timeout-ms 10000] [--rcb] [--type 类型]\n";
}

bool ValidateReport(const IEC61850::MmsReportEvent& report,
                    std::string_view valueType, std::string* failure) {
  if (failure == nullptr) {
    return false;
  }
  if (!report.generalInterrogation) {
    *failure = "收到的InformationReport未声明GI原因";
    return false;
  }
  if (report.reportRef != "IED1LD0/LLN0$UR$urcb1" ||
      report.dataSetRef != "IED1LD0/LLN0$ds1" || report.confRev != 7 ||
      report.sequenceNumber != 1 || report.values.size() != 1) {
    *failure = std::format(
        "GI报告元数据不匹配: RCB={}, DataSet={}, ConfRev={}, SqNum={}, 点数={}",
        report.reportRef, report.dataSetRef, report.confRev,
        report.sequenceNumber, report.values.size());
    return false;
  }
  const auto& value = report.values.front();
  if (value.dataRef != "IED1LD0/LLN0.Beh.stVal" ||
      value.fc != IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST ||
      !value.timestampValid || value.timestampMs != kExpectedReportTimestampMs) {
    *failure = std::format(
        "GI报告点值时标不匹配: dataRef={}, timestampValid={}, timestampMs={}",
        value.dataRef, value.timestampValid, value.timestampMs);
    return false;
  }

  const auto type = Upper(valueType);
  if (type == "BOOLEAN") {
    const auto* decoded = std::get_if<bool>(&value.value);
    if (decoded == nullptr || !*decoded) {
      *failure = "GI报告BOOLEAN值不是true";
      return false;
    }
  } else if (type == "INT32") {
    const auto* decoded = std::get_if<std::int64_t>(&value.value);
    if (decoded == nullptr || *decoded != -42) {
      *failure = "GI报告INT32值不是-42";
      return false;
    }
  } else if (type == "INT32U") {
    const auto* decoded = std::get_if<std::int64_t>(&value.value);
    if (decoded == nullptr || *decoded != 42) {
      *failure = "GI报告INT32U值不是42";
      return false;
    }
  } else if (type == "FLOAT32" || type == "FLOAT64") {
    const auto* decoded = std::get_if<double>(&value.value);
    if (decoded == nullptr || !std::isfinite(*decoded) ||
        std::fabs(*decoded - 12.5) > 1e-6) {
      *failure = "GI报告浮点值不是12.5";
      return false;
    }
  } else if (type == "QUALITY") {
    const auto* decoded = std::get_if<std::vector<std::uint8_t>>(&value.value);
    if (decoded == nullptr || decoded->size() != 2 || (*decoded)[0] != 0 ||
        (*decoded)[1] != 0 || value.quality.validity != IEC61850::MmsValidity::GOOD ||
        value.quality.failure || value.quality.oldData) {
      *failure = "GI报告QUALITY值或品质状态不符合预期";
      return false;
    }
  } else if (type == "TIMESTAMP") {
    const auto* decoded = std::get_if<std::int64_t>(&value.value);
    if (decoded == nullptr || *decoded != kExpectedReportTimestampMs) {
      *failure = "GI报告TIMESTAMP值不符合预期";
      return false;
    }
  } else if (type.starts_with("VISSTRING")) {
    const auto* decoded = std::get_if<std::string>(&value.value);
    if (decoded == nullptr || *decoded != "sim-value") {
      *failure = "GI报告VisibleString值不是sim-value";
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!ParseArguments(argc, argv, &options)) {
    PrintUsage();
    return 2;
  }

  IEC61850::ProtocolIedPlan plan;
  plan.config.set_conn_name("iec61850_mms_smoke");
  plan.config.set_ied_name("IED1");
  plan.config.set_access_point("AP1");
  plan.config.set_enable_mms(true);
  plan.ied.set_name("IED1");
  if (options.rcb) {
    auto* node = plan.ied.add_logical_nodes();
    node->set_node_ref("IED1LD0/LLN0");
    node->set_access_point("AP1");

    auto* attribute = plan.ied.add_data_attributes();
    attribute->set_data_ref("IED1LD0/LLN0.Beh.stVal");
    attribute->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST);
    attribute->set_basic_type(options.valueType);
    attribute->set_count(1);
    attribute->set_access_point("AP1");

    auto* dataSet = plan.ied.add_data_sets();
    dataSet->set_data_set_ref("IED1LD0/LLN0$ds1");
    dataSet->set_access_point("AP1");
    auto* member = dataSet->add_members();
    member->set_data_ref(attribute->data_ref());
    member->set_fc(attribute->fc());

    auto* report = plan.ied.add_report_controls();
    report->set_rcb_ref("IED1LD0/LLN0$UR$urcb1");
    report->set_data_set_ref(dataSet->data_set_ref());
    report->set_report_id("RPT1");
    report->set_buffered(false);
    report->set_config_revision(7);
    report->set_max_instances(1);
    report->set_buffer_time_ms(20);
    report->set_integrity_period_ms(5000);
    report->mutable_trigger_options()->set_data_change(true);
    report->mutable_trigger_options()->set_general_interrogation(true);
    report->mutable_optional_fields()->set_sequence_number(true);
    report->mutable_optional_fields()->set_report_timestamp(true);
    report->mutable_optional_fields()->set_reason_code(true);
    report->mutable_optional_fields()->set_config_revision(true);
  }

  IEC61850::ProtocolNetworkBinding binding;
  binding.channel.set_channel(IEC61850Proto::NETWORK_CHANNEL_A);
  binding.channel.set_enabled(true);
  binding.channel.set_remote_ip(options.remoteIp);
  binding.channel.set_remote_port(options.remotePort);

  std::mutex mutex;
  std::condition_variable condition;
  bool ready = false;
  const bool reportRequired = options.rcb;
  bool reportReceived = !reportRequired;
  std::string failure;

  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(mutex);
    std::cout << "[IEC61850联调客户端] [状态] state="
              << static_cast<int>(event.state)
              << ", active_channel="
              << static_cast<int>(event.activeChannel)
              << ", error=" << event.error << '\n';
    if (event.state == IEC61850::ProtocolSessionState::READY) {
      ready = true;
    } else if (event.state == IEC61850::ProtocolSessionState::ERROR) {
      failure = event.error.empty() ? "MMS工作器报告ERROR" : event.error;
    }
    condition.notify_all();
  };
  callbacks.onMmsReport = [&](IEC61850::MmsReportEvent report) {
    std::lock_guard lock(mutex);
    std::string reportFailure;
    if (!ValidateReport(report, options.valueType, &reportFailure)) {
      failure = std::move(reportFailure);
    } else {
      reportReceived = true;
      std::cout << "[IEC61850联调客户端] [报告] 已收到并校验GI InformationReport: "
                   "值、品质/时标均符合预期\n";
    }
    condition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(plan, {std::move(binding)},
                                    std::move(callbacks));
  const auto startStatus = worker.Start();
  if (!startStatus.ok()) {
    std::cerr << "[IEC61850联调客户端] [错误] 启动MMS工作器失败: "
              << startStatus.error_message() << '\n';
    return 1;
  }

  {
    std::unique_lock lock(mutex);
    condition.wait_for(lock, std::chrono::milliseconds(options.timeoutMs),
                       [&] {
                         return (ready && (!reportRequired || reportReceived)) ||
                                !failure.empty();
                       });
  }
  worker.Stop();

  if (!ready || (reportRequired && !reportReceived)) {
    std::cerr << "[IEC61850联调客户端] [错误] 未在超时前完成READY和GI报告校验: "
              << (failure.empty() ? "等待超时" : failure) << '\n';
    return 1;
  }
  std::cout << "[IEC61850联调客户端] [信息] 外部MMS进程联调通过，已进入READY\n";
  return 0;
}
