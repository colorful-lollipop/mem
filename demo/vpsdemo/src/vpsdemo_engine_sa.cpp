#include <csignal>
#include <string>
#include <thread>
#include <unistd.h>

#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/server/rpc_server.h"
#include "registry_client.h"
#include "vps_bootstrap_interface.h"
#include "vps_bootstrap_stub.h"
#include "vpsdemo_service.h"
#include "virus_protection_service_log.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void SignalHandler(int) {
    g_stop = 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        HLOGE("usage: vpsdemo_engine_sa <registry_socket> <service_socket>");
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
        HLOGE("bootstrap OpenSession failed");
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

    // Start memrpc RPC server with demo handlers.
    memrpc::RpcServer rpcServer(serverHandles);
    vpsdemo::VpsDemoService service;
    service.RegisterHandlers(&rpcServer);
    if (rpcServer.Start() != memrpc::StatusCode::Ok) {
        HLOGE("RpcServer start failed");
        return 1;
    }
    HLOGI("RpcServer started");

    // Create bootstrap stub SA and start its service socket.
    auto stub = std::make_shared<vpsdemo::VpsBootstrapStub>();
    stub->SetBootstrapHandles(serverHandles);
    stub->OnStart();

    if (!stub->StartServiceSocket(serviceSocket)) {
        HLOGE("failed to start service socket at %{public}s", serviceSocket.c_str());
        rpcServer.Stop();
        return 1;
    }
    HLOGI("service socket at %{public}s", serviceSocket.c_str());

    // Register with the SA registry.
    vpsdemo::RegistryClient registry(registrySocket);
    if (!registry.RegisterService(vpsdemo::kVpsBootstrapSaId, serviceSocket)) {
        HLOGE("registry registration failed");
    }

    // Publish to in-process SA registry (for completeness).
    stub->Publish(stub.get());

    HLOGI("engine ready");

    // Run until signaled.
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    HLOGI("engine shutting down");
    stub->OnStop();
    rpcServer.Stop();
    return 0;
}
