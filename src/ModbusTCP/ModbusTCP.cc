#include "ModbusTCP.h"
#include "ModbusTCPGrpcService.h"
#include "ModbusTCPLibInfo.h"
#include "ModuleManager.pb.h"
#include <boost/dll.hpp>
#include <condition_variable>
#include "Logger.h"
namespace {
const std::string& manifestData() { static const std::string data = [] { ModuleManagerProto::ModuleManifest m; m.set_module_name(ModbusTCPLibInfo.LIB_NAME); auto* v=m.mutable_version(); v->set_major(ModbusTCPLibInfo.VERSION_MAJOR); v->set_minor(ModbusTCPLibInfo.VERSION_MINOR); v->set_patch(ModbusTCPLibInfo.VERSION_PATCH); v->set_version(ModbusTCPLibInfo.VERSION); auto* d=m.add_dependencies(); d->set_module_name("DataCenter"); d->set_version_range("=0.0.1"); return m.SerializeAsString(); }(); return data; }
}
namespace ModbusTCP {
ModbusTCP::ModbusTCP() : service_(std::make_shared<GrpcService>()), commandService_(std::make_shared<CommandService>()), manager_("./conf/config.db") { initLibInfo(ModbusTCPLibInfo); }
ModbusTCP::~ModbusTCP() = default;
void ModbusTCP::start(std::stop_token stopToken) { LOG_INFO("ModbusTCP 模块启动"); service_->setModule(this); commandService_->setModule(this); grpcServerBuilder(std::vector<std::shared_ptr<grpc::Service>>{service_, commandService_}); manager_.LoadPersistedConfig(); std::mutex mu; std::condition_variable_any cv; std::stop_callback cb(stopToken,[&cv]{cv.notify_all();}); std::unique_lock lock(mu); cv.wait(lock,[&]{return stopToken.stop_requested();}); LOG_INFO("ModbusTCP 模块停止"); }
LinkManager& ModbusTCP::linkManager() { return manager_; }
}
extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface* create() { return new ModbusTCP::ModbusTCP(); }
extern "C" BOOST_SYMBOL_EXPORT bool GetModuleManifestPb(const uint8_t** data, size_t* size) { if(!data||!size) return false; const auto& s=manifestData(); *data=reinterpret_cast<const uint8_t*>(s.data()); *size=s.size(); return true; }
