#include "IEC61850ProtocolStack.h"

#include <memory>
#include <string>

namespace IEC61850 {
namespace {

class UnavailableProtocolStack final : public ProtocolStackAdapter {
public:
  grpc::Status StartIed(ProtocolIedPlan,
                        ProtocolEventCallbacks) override {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        "尚未配置可用于x64/ARM64的IEC61850协议栈");
  }

  grpc::Status StopIed(std::string_view) override {
    return grpc::Status::OK;
  }
};

}  // namespace

std::shared_ptr<ProtocolStackAdapter> MakeUnavailableProtocolStack() {
  return std::make_shared<UnavailableProtocolStack>();
}

}  // namespace IEC61850
