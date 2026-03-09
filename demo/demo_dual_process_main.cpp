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

namespace {

class DemoHandler : public memrpc::IScanHandler {
 public:
  memrpc::ScanResult HandleScan(const memrpc::ScanRequest& request) override {
    if (request.file_path.find("slow") != std::string::npos) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    memrpc::ScanResult result;
    result.status = memrpc::StatusCode::Ok;
    result.verdict = request.file_path.find("virus") != std::string::npos
                         ? memrpc::ScanVerdict::Infected
                         : memrpc::ScanVerdict::Clean;
    result.message = request.file_path;
    return result;
  }
};

void RunServer(memrpc::BootstrapHandles handles) {
  auto handler = std::make_shared<DemoHandler>();
  memrpc::EngineServer server(handles, handler);
  if (server.Start() != memrpc::StatusCode::Ok) {
    std::cerr << "server start failed" << std::endl;
    std::_Exit(1);
  }
  server.Run();
}

void PrintResult(const char* label, const memrpc::ScanResult& result) {
  std::cout << label << ": status=" << static_cast<int>(result.status)
            << " verdict=" << static_cast<int>(result.verdict)
            << " message=" << result.message << std::endl;
}

}  // namespace

int main() {
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();
  if (bootstrap->StartEngine() != memrpc::StatusCode::Ok) {
    std::cerr << "bootstrap start failed" << std::endl;
    return 1;
  }

  const memrpc::BootstrapHandles server_handles = bootstrap->server_handles();
  const pid_t child = fork();
  if (child == 0) {
    RunServer(server_handles);
    return 0;
  }
  if (child < 0) {
    std::cerr << "fork failed" << std::endl;
    return 1;
  }

  memrpc::EngineClient client(bootstrap);
  if (client.Init() != memrpc::StatusCode::Ok) {
    std::cerr << "client init failed" << std::endl;
    kill(child, SIGTERM);
    waitpid(child, nullptr, 0);
    return 1;
  }

  memrpc::ScanRequest normal;
  normal.file_path = "/tmp/clean-file";
  memrpc::ScanResult normal_result;
  client.Scan(normal, &normal_result);
  PrintResult("normal", normal_result);

  memrpc::ScanRequest high;
  high.file_path = "/tmp/high-virus";
  high.options.priority = memrpc::Priority::High;
  memrpc::ScanResult high_result;
  client.Scan(high, &high_result);
  PrintResult("high", high_result);

  memrpc::ScanRequest timeout;
  timeout.file_path = "/tmp/slow-file";
  timeout.options.exec_timeout_ms = 50;
  memrpc::ScanResult timeout_result;
  client.Scan(timeout, &timeout_result);
  PrintResult("timeout", timeout_result);

  client.Shutdown();
  kill(child, SIGTERM);
  waitpid(child, nullptr, 0);
  return 0;
}
