#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

#include "memrpc/client.h"
#include "memrpc/demo_bootstrap.h"
#include "memrpc/server.h"

namespace MemRpc = OHOS::Security::VirusProtectionService::MemRpc;

namespace {

class DemoHandler : public MemRpc::IScanHandler {
 public:
  MemRpc::ScanResult HandleScan(const MemRpc::ScanRequest& request) override {
    if (request.file_path.find("slow") != std::string::npos) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    MemRpc::ScanResult result;
    result.status = MemRpc::StatusCode::Ok;
    result.verdict = request.file_path.find("virus") != std::string::npos
                         ? MemRpc::ScanVerdict::Infected
                         : MemRpc::ScanVerdict::Clean;
    result.message = request.file_path;
    return result;
  }
};

void RunServer(MemRpc::BootstrapHandles handles) {
  auto handler = std::make_shared<DemoHandler>();
  MemRpc::EngineServer server(handles, handler);
  if (server.Start() != MemRpc::StatusCode::Ok) {
    std::cerr << "server start failed" << std::endl;
    std::_Exit(1);
  }
  server.Run();
}

void PrintResult(const char* label, const MemRpc::ScanResult& result) {
  std::cout << label << ": status=" << static_cast<int>(result.status)
            << " verdict=" << static_cast<int>(result.verdict)
            << " message=" << result.message << std::endl;
}

}  // namespace

int main() {
  auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
  if (bootstrap->StartEngine() != MemRpc::StatusCode::Ok) {
    std::cerr << "bootstrap start failed" << std::endl;
    return 1;
  }

  const MemRpc::BootstrapHandles server_handles = bootstrap->server_handles();
  const pid_t child = fork();
  if (child == 0) {
    RunServer(server_handles);
    return 0;
  }
  if (child < 0) {
    std::cerr << "fork failed" << std::endl;
    return 1;
  }

  MemRpc::EngineClient client(bootstrap);
  if (client.Init() != MemRpc::StatusCode::Ok) {
    std::cerr << "client init failed" << std::endl;
    kill(child, SIGTERM);
    waitpid(child, nullptr, 0);
    return 1;
  }

  MemRpc::ScanRequest normal;
  normal.file_path = "/tmp/clean-file";
  MemRpc::ScanResult normal_result;
  client.Scan(normal, &normal_result);
  PrintResult("normal", normal_result);

  MemRpc::ScanRequest high;
  high.file_path = "/tmp/high-virus";
  high.options.priority = MemRpc::Priority::High;
  MemRpc::ScanResult high_result;
  client.Scan(high, &high_result);
  PrintResult("high", high_result);

  MemRpc::ScanRequest timeout;
  timeout.file_path = "/tmp/slow-file";
  timeout.options.exec_timeout_ms = 50;
  MemRpc::ScanResult timeout_result;
  client.Scan(timeout, &timeout_result);
  PrintResult("timeout", timeout_result);

  client.Shutdown();
  kill(child, SIGTERM);
  waitpid(child, nullptr, 0);
  return 0;
}
