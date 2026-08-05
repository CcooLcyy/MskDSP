#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

std::uint16_t FindFreeLoopbackPort() {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return 0;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(0);
  const bool bound = bind(fd, reinterpret_cast<const sockaddr*>(&address),
                          sizeof(address)) == 0;
  socklen_t length = sizeof(address);
  const bool named = bound &&
                     getsockname(fd, reinterpret_cast<sockaddr*>(&address),
                                 &length) == 0;
  close(fd);
  return named ? ntohs(address.sin_port) : 0;
}

std::filesystem::path MakeReadyFilePath(std::uint16_t port) {
  return std::filesystem::temp_directory_path() /
         std::format("mskdsp-iec61850-mms-ready-{}-{}", getpid(), port);
}

bool WaitForReadyFile(const std::filesystem::path& path,
                      std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error) && !error) {
      return true;
    }
    std::this_thread::sleep_for(20ms);
  }
  return false;
}

class ChildProcess final {
 public:
  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  ~ChildProcess() { Stop(); }

  bool Start(const std::string& executable,
             const std::vector<std::string>& arguments) {
    if (pid_ > 0 || executable.empty()) {
      return false;
    }
    pid_ = fork();
    if (pid_ < 0) {
      pid_ = -1;
      return false;
    }
    if (pid_ == 0) {
      std::vector<std::string> command;
      command.reserve(arguments.size() + 1);
      command.push_back(executable);
      command.insert(command.end(), arguments.begin(), arguments.end());
      std::vector<char*> argv;
      argv.reserve(command.size() + 1);
      for (auto& item : command) {
        argv.push_back(item.data());
      }
      argv.push_back(nullptr);
      execv(executable.c_str(), argv.data());
      _exit(127);
    }
    return true;
  }

  bool Wait(std::chrono::milliseconds timeout, int* exitCode) {
    if (pid_ <= 0) {
      return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      int status = 0;
      const pid_t result = waitpid(pid_, &status, WNOHANG);
      if (result == pid_) {
        pid_ = -1;
        if (exitCode != nullptr) {
          *exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        return WIFEXITED(status);
      }
      if (result < 0 || std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(20ms);
    }
  }

  void Stop() noexcept {
    if (pid_ <= 0) {
      return;
    }
    kill(pid_, SIGTERM);
    int status = 0;
    for (int attempt = 0; attempt < 100; ++attempt) {
      const pid_t result = waitpid(pid_, &status, WNOHANG);
      if (result == pid_) {
        pid_ = -1;
        return;
      }
      if (result < 0) {
        pid_ = -1;
        return;
      }
      std::this_thread::sleep_for(20ms);
    }
    kill(pid_, SIGKILL);
    waitpid(pid_, &status, 0);
    pid_ = -1;
  }

 private:
  pid_t pid_ = -1;
};

bool RunScenario(const std::string& simulator, const std::string& worker,
                 bool rcb, std::string_view valueType = "BOOLEAN") {
  const auto port = FindFreeLoopbackPort();
  if (port == 0) {
    std::cerr << "无法为MMS进程联调分配回环端口\n";
    return false;
  }

  const auto readyFile = MakeReadyFilePath(port);
  std::error_code removeError;
  std::filesystem::remove(readyFile, removeError);

  std::vector<std::string> simulatorArguments{
      "--listen-ip", "127.0.0.1", "--port", std::to_string(port),
      "--ied", "IED1", "--access-point", "AP1", "--domain", "IED1LD0",
      "--ready-file", readyFile.string(), "--once"};
  if (rcb) {
    simulatorArguments.emplace_back("--rcb");
    simulatorArguments.emplace_back("--type");
    simulatorArguments.emplace_back(valueType);
  }

  ChildProcess simulatorProcess;
  if (!simulatorProcess.Start(simulator, simulatorArguments) ||
      !WaitForReadyFile(readyFile, 5s)) {
    std::cerr << "MMS模拟IED未能在规定时间内监听端口\n";
    std::filesystem::remove(readyFile, removeError);
    return false;
  }

  std::vector<std::string> workerArguments{
      "--ip", "127.0.0.1", "--port", std::to_string(port), "--timeout-ms",
      "15000"};
  if (rcb) {
    workerArguments.emplace_back("--rcb");
    workerArguments.emplace_back("--type");
    workerArguments.emplace_back(valueType);
  }

  ChildProcess workerProcess;
  if (!workerProcess.Start(worker, workerArguments)) {
    std::cerr << "MMS Worker进程启动失败\n";
    std::filesystem::remove(readyFile, removeError);
    return false;
  }

  int workerExitCode = -1;
  if (!workerProcess.Wait(20s, &workerExitCode) || workerExitCode != 0) {
    std::cerr << "MMS Worker进程未成功进入READY，退出码=" << workerExitCode
              << "\n";
    std::filesystem::remove(readyFile, removeError);
    return false;
  }

  int simulatorExitCode = -1;
  const bool simulatorExited =
      simulatorProcess.Wait(5s, &simulatorExitCode) && simulatorExitCode == 0;
  std::filesystem::remove(readyFile, removeError);
  if (!simulatorExited) {
    std::cerr << "MMS模拟IED在--once会话结束后未正常退出，退出码="
              << simulatorExitCode << "\n";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "用法: iec61850_mms_process_test <模拟IED> <MMS Worker>\n";
    return 2;
  }

  if (!RunScenario(argv[1], argv[2], false)) {
    return 1;
  }
  if (!RunScenario(argv[1], argv[2], true)) {
    return 1;
  }
  if (!RunScenario(argv[1], argv[2], true, "INT32")) {
    return 1;
  }
  if (!RunScenario(argv[1], argv[2], true, "FLOAT32")) {
    return 1;
  }
  if (!RunScenario(argv[1], argv[2], true, "QUALITY")) {
    return 1;
  }
  std::cout << "IEC61850 MMS独立进程空模型和RCB/GI联调测试通过\n";
  return 0;
}
