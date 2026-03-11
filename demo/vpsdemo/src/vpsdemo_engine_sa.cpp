#include <csignal>
#include <string>
#include <thread>
#include <unistd.h>

#include "iservice_registry.h"
#include "registry_backend.h"
#include "vps_bootstrap_interface.h"
#include "vps_session_service.h"
#include "virus_executor_service.h"
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

    // Inject backend so Publish() routes through the registry.
    auto backend = std::make_shared<vpsdemo::RegistryBackend>(registrySocket);
    OHOS::SystemAbilityManagerClient::GetInstance().SetBackend(backend);

    // Create engine session service — owns bootstrap, RPC server, and demo service.
    vpsdemo::VpsDemoService service;
    auto sessionService = std::make_shared<vpsdemo::EngineSessionService>(&service);

    // Create SA stub — delegates OpenSession to the session service.
    auto stub = std::make_shared<vpsdemo::VirusExecutorService>(sessionService);

    // Set service path before OnStart so transport knows where to listen.
    stub->AsObject()->SetServicePath(serviceSocket);
    stub->OnStart();

    // Publish to SA registry — backend routes to RegistryServer.
    stub->Publish(stub.get());

    HLOGI("engine ready");

    // Run until signaled.
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    HLOGI("engine shutting down");
    stub->OnStop();
    return 0;
}
