#include <csignal>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "registry_server.h"
#include "vps_bootstrap_interface.h"
#include "virus_protection_service_log.h"

namespace {

const std::string REGISTRY_SOCKET = "/tmp/vpsdemo_registry.sock";
const std::string SERVICE_SOCKET = "/tmp/vpsdemo_service.sock";

volatile std::sig_atomic_t g_stop = 0;
pid_t g_engine_pid = -1;
pid_t g_client_pid = -1;

void SignalHandler(int) {
    g_stop = 1;
}

pid_t SpawnEngine(const std::string& enginePath) {
    pid_t pid = fork();
    if (pid == 0) {
        execl(enginePath.c_str(), enginePath.c_str(),
              REGISTRY_SOCKET.c_str(), SERVICE_SOCKET.c_str(),
              nullptr);
        HILOGE("exec engine failed");
        _exit(1);
    }
    return pid;
}

pid_t SpawnClient(const std::string& clientPath) {
    pid_t pid = fork();
    if (pid == 0) {
        setenv("OHOS_SA_MOCK_REGISTRY_SOCKET", REGISTRY_SOCKET.c_str(), 1);
        execl(clientPath.c_str(), clientPath.c_str(), nullptr);
        HILOGE("exec client failed");
        _exit(1);
    }
    return pid;
}

void KillAndWait(pid_t pid) {
    if (pid > 0) {
        kill(pid, SIGTERM);
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGTERM, SignalHandler);
    std::signal(SIGINT, SignalHandler);

    std::string dir = ".";
    std::string argv0(argv[0]);
    auto pos = argv0.rfind('/');
    if (pos != std::string::npos) {
        dir = argv0.substr(0, pos);
    }
    std::string enginePath = dir + "/vpsdemo_engine_sa";
    std::string clientPath = dir + "/vpsdemo_client";

    vpsdemo::RegistryServer registry(REGISTRY_SOCKET);

    registry.SetLoadCallback([&](int32_t sa_id) -> bool {
        if (sa_id != vpsdemo::VPS_BOOTSTRAP_SA_ID) {
            return false;
        }
        if (g_engine_pid > 0) {
            return true;
        }
        HILOGI("spawning engine SA for load request");
        g_engine_pid = SpawnEngine(enginePath);
        if (g_engine_pid < 0) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return true;
    });

    registry.SetUnloadCallback([&](int32_t sa_id) {
        if (sa_id != vpsdemo::VPS_BOOTSTRAP_SA_ID) {
            return;
        }
        HILOGI("unloading engine SA (pid=%{public}d)", g_engine_pid);
        KillAndWait(g_engine_pid);
        g_engine_pid = -1;
    });

    if (!registry.Start()) {
        HILOGE("failed to start registry");
        return 1;
    }
    HILOGI("registry started at %{public}s", REGISTRY_SOCKET.c_str());

    g_engine_pid = SpawnEngine(enginePath);
    if (g_engine_pid < 0) {
        HILOGE("failed to spawn engine");
        registry.Stop();
        return 1;
    }
    HILOGI("engine spawned pid=%{public}d", g_engine_pid);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    g_client_pid = SpawnClient(clientPath);
    if (g_client_pid < 0) {
        HILOGE("failed to spawn client");
        KillAndWait(g_engine_pid);
        registry.Stop();
        return 1;
    }
    HILOGI("client spawned pid=%{public}d", g_client_pid);

    int clientStatus = 0;
    waitpid(g_client_pid, &clientStatus, 0);
    g_client_pid = -1;
    HILOGI("client exited status=%{public}d", WEXITSTATUS(clientStatus));

    KillAndWait(g_engine_pid);
    g_engine_pid = -1;
    registry.Stop();

    HILOGI("supervisor done");
    return WEXITSTATUS(clientStatus);
}
