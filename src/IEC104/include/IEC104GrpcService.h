#pragma once

#include "IEC104.grpc.pb.h"
#include "IEC104.h"
#include "IEC104.pb.h"

namespace IEC104 {
class IEC104GrpcServiceImpl : public IEC104Proto::IEC104Service::Service {
public:
  void getIEC104(IEC104 *iec104);
  grpc::Status Test(grpc::ServerContext *context, const IEC104Proto::Empty *, IEC104Proto::Empty *) override;

private:
  IEC104 *iec104_;
};
}  // namespace IEC104