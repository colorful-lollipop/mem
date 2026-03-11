#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

#include "apps/vps/child/virus_engine_service.h"
#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/server/rpc_server.h"
#include "registry_client.h"
#include "vps_bootstrap_interface.h"
#include "vps_bootstrap_stub.h"

using namespace OHOS::Security::VirusProtectionService;

namespace {

volatile std::sig_atomic_t g_stop = 0;

void SignalHandler(int) {
    g_stop = 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    // Expected args: <registry_socket> <service_socket>
    if (argc < 3) {
        std::cerr << "[engine] usage: vpsdemo_engine_sa <registry_socket> <service_socket>"
                  << std::endl;
        return 1;
    }
    const std::string registrySocket = argv[1];
    const std::string serviceSocket = argv[2];

    std::signal(SIGTERM, SignalHandler);
    std::signal(SIGINT, SignalHandler);

    // Create shared memory + eventfd resources.
    auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();
    memrpc::BootstrapHandles clientHandles;
    if (bootstrap->OpenSession(&clientHandles) != memrpc::StatusCode::Ok) {
        std::cerr << "[engine] bootstrap OpenSession failed" << std::endl;
        return 1;
    }
    // Close the client-side duplicate handles; engine only needs server handles.
    close(clientHandles.shm_fd);
    close(clientHandles.high_req_event_fd);
    close(clientHandles.normal_req_event_fd);
    close(clientHandles.resp_event_fd);
    close(clientHandles.req_credit_event_fd);
    close(clientHandles.resp_credit_event_fd);

    const memrpc::BootstrapHandles serverHandles = bootstrap->server_handles();

    // Start memrpc RPC server with VPS handlers.
    memrpc::RpcServer rpcServer(serverHandles);
    VirusEngineService service;
    service.RegisterHandlers(&rpcServer);
    if (rpcServer.Start() != memrpc::StatusCode::Ok) {
        std::cerr << "[engine] RpcServer start failed" << std::endl;
        return 1;
    }
    std::cout << "[engine] RpcServer started" << std::endl;

    // Create bootstrap stub SA and start its service socket.
    auto stub = std::make_shared<vpsdemo::VpsBootstrapStub>();
    stub->SetBootstrapHandles(serverHandles);
    stub->OnStart();

    if (!stub->StartServiceSocket(serviceSocket)) {
        std::cerr << "[engine] failed to start service socket at " << serviceSocket
                  << std::endl;
        rpcServer.Stop();
        return 1;
    }
    std::cout << "[engine] service socket at " << serviceSocket << std::endl;

    // Register with the SA registry.
    vpsdemo::RegistryClient registry(registrySocket);
    if (!registry.RegisterService(vpsdemo::kVpsBootstrapSaId, serviceSocket)) {
        std::cerr << "[engine] registry registration failed" << std::endl;
    }

    // Publish to in-process SA registry (for completeness).
    stub->Publish(stub.get());

    std::cout << "[engine] ready" << std::endl;

    // Run until signaled.
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "[engine] shutting down" << std::endl;
    stub->OnStop();
    rpcServer.Stop();
    return 0;
}
