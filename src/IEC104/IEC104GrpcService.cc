#include "IEC104GrpcService.h"

#include <grpcpp/support/status.h>

#include "IEC104.h"

namespace IEC104 {
void IEC104GrpcServiceImpl::getIEC104(IEC104 *iec104) {
  iec104_ = iec104;
}
grpc::Status IEC104GrpcServiceImpl::Test(grpc::ServerContext *context, const IEC104Proto::Empty *, IEC104Proto::Empty *) {
  return grpc::Status::OK;
}
}  // namespace IEC104