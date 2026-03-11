#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <memory>

#include "apps/minirpc/child/minirpc_service.h"
#include "apps/minirpc/parent/minirpc_client.h"
#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/server/rpc_server.h"

namespace OHOS::Security::VirusProtectionService {
namespace {

namespace Mini = ::OHOS::Security::VirusProtectionService::MiniRpc;
namespace Mem = ::OHOS::Security::VirusProtectionService::MemRpc;

void RunChild(Mem::BootstrapHandles handles) {
  Mem::RpcServer server;
  server.SetBootstrapHandles(handles);

  Mini::MiniRpcService service;
  service.RegisterHandlers(&server);
  if (server.Start() != Mem::StatusCode::Ok) {
    std::cerr << "minirpc server start failed" << std::endl;
    std::_Exit(1);
  }
  server.Run();
}

}  // namespace
}  // namespace OHOS::Security::VirusProtectionService

int main() {
  namespace Mini = ::OHOS::Security::VirusProtectionService::MiniRpc;
  namespace Mem = ::OHOS::Security::VirusProtectionService::MemRpc;

  auto bootstrap = std::make_shared<Mem::PosixDemoBootstrapChannel>();
  Mem::BootstrapHandles unused_handles;
  if (bootstrap->OpenSession(&unused_handles) != Mem::StatusCode::Ok) {
    std::cerr << "bootstrap open session failed" << std::endl;
    return 1;
  }
  close(unused_handles.shm_fd);
  close(unused_handles.high_req_event_fd);
  close(unused_handles.normal_req_event_fd);
  close(unused_handles.resp_event_fd);
  close(unused_handles.req_credit_event_fd);
  close(unused_handles.resp_credit_event_fd);

  const Mem::BootstrapHandles server_handles = bootstrap->server_handles();
  const pid_t child = fork();
  if (child == 0) {
    OHOS::Security::VirusProtectionService::RunChild(server_handles);
    return 0;
  }
  if (child < 0) {
    std::cerr << "fork failed" << std::endl;
    
    return 1;
  }

  Mini::MiniRpcClient client(bootstrap);
  if (client.Init() != Mem::StatusCode::Ok) {
    std::cerr << "client init failed" << std::endl;
    kill(child, SIGTERM);
    waitpid(child, nullptr, 0);
    return 1;
  }

  Mini::EchoReply echo_reply;
  if (client.Echo("hello", &echo_reply) == Mem::StatusCode::Ok) {
    std::cout << "echo: " << echo_reply.text << std::endl;
  }

  Mini::AddReply add_reply;
  if (client.Add(7, 8, &add_reply) == Mem::StatusCode::Ok) {
    std::cout << "add: " << add_reply.sum << std::endl;
  }

  client.Shutdown();
  kill(child, SIGTERM);
  waitpid(child, nullptr, 0);
  return 0;
}
