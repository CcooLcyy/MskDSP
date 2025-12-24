#pragma once

#include <stop_token>
#include <string>

struct LibVersion {
  std::string major;
  std::string minor;
  std::string patch;
};

struct MetaData {
  std::string name;
  LibVersion version;
  std::string libName;
  std::string grpcServer;
};

class ModuleInterface {
public:
  explicit ModuleInterface(std::stop_token stopToken);
  virtual ~ModuleInterface() = default;
  virtual MetaData metaData() = 0;
  virtual void start() = 0;

protected:
  MetaData metaData_;
  std::stop_token stopToken_;
};