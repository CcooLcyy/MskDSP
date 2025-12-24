#pragma once

#include <filesystem>
#include <stop_token>
#include <string>

namespace ModuleInterface {
struct Version {
  std::string major;
  std::string minor;
  std::string patch;
  std::string version;
};

struct MetaData {
  std::string name;
  Version version;
  std::string libName;
  std::filesystem::path grpcServer;
};

class ModuleInterface {
public:
  explicit ModuleInterface(std::stop_token stopToken);
  virtual ~ModuleInterface() = default;
  virtual MetaData metaData() = 0;
  virtual void start() = 0;
  virtual void runServer() = 0;

protected:
  MetaData metaData_;
  std::stop_token stopToken_;
};
}  // namespace ModuleInterface