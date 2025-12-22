#pragma once

#include <stop_token>
#include <string>

struct MetaData {
  std::string name;
  std::string version;
  std::string libName;
  std::string description;
  std::string grpcServer;
};

class ModuleInterface {
public:
  ModuleInterface(std::stop_token stopToken);
  virtual MetaData metaData() = 0;
  virtual void start() = 0;
  virtual ~ModuleInterface() = default;

protected:
  MetaData metaData_;
  std::stop_token stopToken_;
};