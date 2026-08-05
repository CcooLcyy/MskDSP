#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <grpcpp/support/status.h>

#include "IEC61850.pb.h"

namespace IEC61850 {

class SclParser {
public:
  struct Limits {
    std::size_t maxDocumentBytes = 32U * 1024U * 1024U;
    std::size_t maxIeds = 4096;
    std::size_t maxExpandedDataAttributes = 2U * 1024U * 1024U;
  };

  SclParser();
  explicit SclParser(Limits limits);

  grpc::Status Parse(const std::string& modelName,
                     const std::string& sourceName,
                     std::string_view content,
                     IEC61850Proto::NormalizedSclModel* out,
                     std::vector<IEC61850Proto::ValidationIssue>* issues) const;

  static IEC61850Proto::SclModelSummary BuildSummary(
      const IEC61850Proto::NormalizedSclModel& model);

private:
  Limits limits_;
};

}  // namespace IEC61850
