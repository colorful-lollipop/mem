#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <cstdlib>
#include <string>
#include <thread>

#include "transport/registry_server.h"
#include "transport/ves_control_interface.h"
#include "virus_protection_service_log.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
pid_t g_engine_pid = -1;
pid_t g_client_pid = -1;

void SignalHandler(int)
{
    g_stop = 1;
}

std::string MakeSocketPath(const char* prefix)
{
    return std::string(prefix) + "_" + std::to_string(getpid()) + ".sock";
}

pid_t SpawnEngine(const std::string& enginePath, const std::string& registrySocket, const std::string& serviceSocket)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl(enginePath.c_str(), enginePath.c_str(), registrySocket.c_str(), serviceSocket.c_str(), nullptr);
        HILOGE("exec engine failed");
        _exit(1);
    }
    return pid;
}

pid_t SpawnClient(const std::string& clientPath, const std::string& registrySocket)
{
    pid_t pid = fork();
    if (pid == 0) {
        setenv("OHOS_SA_MOCK_REGISTRY_SOCKET", registrySocket.c_str(), 1);
        execl(clientPath.c_str(), clientPath.c_str(), nullptr);
        HILOGE("exec client failed");
        _exit(1);
    }
    return pid;
}

void KillAndWait(pid_t pid)
{
    if (pid > 0) {
        kill(pid, SIGTERM);
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

std::string ResolveBinaryDir(const char* argv0)
{
    std::string dir = ".";
    std::string programPath(argv0);
    const auto pos = programPath.rfind('/');
    if (pos != std::string::npos) {
        dir = programPath.substr(0, pos);
    }
    return dir;
}

bool ConfigureRegistry(VirusExecutorService::RegistryServer& registry,
                       const std::string& enginePath,
                       const std::string& registrySocket,
                       const std::string& serviceSocket)
{
    registry.SetLoadCallback([&](int32_t sa_id) -> bool {
        if (sa_id != VirusExecutorService::VIRUS_PROTECTION_EXECUTOR_SA_ID) {
            return false;
        }
        if (g_engine_pid > 0) {
            return true;
        }
        HILOGI("spawning engine SA for load request");
        g_engine_pid = SpawnEngine(enginePath, registrySocket, serviceSocket);
        if (g_engine_pid < 0) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return true;
    });

    registry.SetUnloadCallback([&](int32_t sa_id) {
        if (sa_id != VirusExecutorService::VIRUS_PROTECTION_EXECUTOR_SA_ID) {
            return;
        }
        HILOGI("unloading engine SA (pid=%{public}d)", g_engine_pid);
        KillAndWait(g_engine_pid);
        g_engine_pid = -1;
    });
    return true;
}

bool StartRegistryAndEngine(VirusExecutorService::RegistryServer& registry,
                            const std::string& enginePath,
                            const std::string& registrySocket,
                            const std::string& serviceSocket)
{
    ConfigureRegistry(registry, enginePath, registrySocket, serviceSocket);
    if (!registry.Start()) {
        HILOGE("failed to start registry");
        return false;
    }
    HILOGI("registry started at %{public}s", registrySocket.c_str());

    g_engine_pid = SpawnEngine(enginePath, registrySocket, serviceSocket);
    if (g_engine_pid < 0) {
        HILOGE("failed to spawn engine");
        registry.Stop();
        return false;
    }
    HILOGI("engine spawned pid=%{public}d", g_engine_pid);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return true;
}

int RunClientSession(const std::string& clientPath, const std::string& registrySocket)
{
    g_client_pid = SpawnClient(clientPath, registrySocket);
    if (g_client_pid < 0) {
        HILOGE("failed to spawn client");
        return 1;
    }
    HILOGI("client spawned pid=%{public}d", g_client_pid);

    int clientStatus = 0;
    waitpid(g_client_pid, &clientStatus, 0);
    g_client_pid = -1;
    HILOGI("client exited status=%{public}d", WEXITSTATUS(clientStatus));
    return WEXITSTATUS(clientStatus);
}

void ShutdownSupervisor(VirusExecutorService::RegistryServer& registry)
{
    KillAndWait(g_engine_pid);
    g_engine_pid = -1;
    registry.Stop();
}

}  // namespace

int main([[maybe_unused]] int argc, char* argv[])
{
    std::signal(SIGTERM, SignalHandler);
    std::signal(SIGINT, SignalHandler);

    const std::string dir = ResolveBinaryDir(argv[0]);
    std::string enginePath = dir + "/VirusExecutorService";
    std::string clientPath = dir + "/virus_executor_service_client";
    const std::string registrySocket = MakeSocketPath("/tmp/virus_executor_service_registry");
    const std::string serviceSocket = MakeSocketPath("/tmp/virus_executor_service_service");

    VirusExecutorService::RegistryServer registry(registrySocket);
    if (!StartRegistryAndEngine(registry, enginePath, registrySocket, serviceSocket)) {
        return 1;
    }

    const int clientStatus = RunClientSession(clientPath, registrySocket);
    ShutdownSupervisor(registry);
    HILOGI("supervisor done");
    return clientStatus;
}
