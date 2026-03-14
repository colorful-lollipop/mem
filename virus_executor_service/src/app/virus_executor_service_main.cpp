#include <csignal>
#include <string>
#include <thread>

#include "iservice_registry.h"
#include "transport/registry_backend.h"
#include "service/virus_executor_service.h"
#include "virus_protection_service_log.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void SignalHandler(int) {
    g_stop = 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        HILOGE("usage: VirusExecutorService <registry_socket> <service_socket>");
        return 1;
    }
    const std::string registrySocket = argv[1];
    const std::string serviceSocket = argv[2];

    std::signal(SIGTERM, SignalHandler);
    std::signal(SIGINT, SignalHandler);

    // Inject backend so Publish() routes through the registry.
    auto backend = std::make_shared<VirusExecutorService::RegistryBackend>(registrySocket);
    OHOS::SystemAbilityManagerClient::GetInstance().SetBackend(backend);

    // Create SA — self-creates VirusExecutorService + EngineSessionService internally.
    auto stub = std::make_shared<VirusExecutorService::VirusExecutorService>();
    stub->AsObject()->SetServicePath(serviceSocket);
    stub->OnStart();

    // Publish to SA registry — framework auto-starts MockServiceSocket.
    stub->Publish(stub.get());

    HILOGI("engine ready");

    // Run until signaled.
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    HILOGI("engine shutting down");
    stub->OnStop();
    return 0;
}
