#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "registry_server.h"
#include "vps_bootstrap_interface.h"

namespace {

const std::string kRegistrySocket = "/tmp/vpsdemo_registry.sock";
const std::string kServiceSocket = "/tmp/vpsdemo_service.sock";

volatile std::sig_atomic_t g_stop = 0;
pid_t g_engine_pid = -1;
pid_t g_client_pid = -1;

void SignalHandler(int) {
    g_stop = 1;
}

std::string FindExecutable(const std::string& name) {
    // Look relative to argv[0] or in the same directory.
    // For simplicity, assume executables are in the same directory.
    return "./" + name;
}

pid_t SpawnEngine(const std::string& enginePath) {
    pid_t pid = fork();
    if (pid == 0) {
        execl(enginePath.c_str(), enginePath.c_str(),
              kRegistrySocket.c_str(), kServiceSocket.c_str(),
              nullptr);
        std::cerr << "[supervisor] exec engine failed" << std::endl;
        _exit(1);
    }
    return pid;
}

pid_t SpawnClient(const std::string& clientPath) {
    pid_t pid = fork();
    if (pid == 0) {
        // Set env so client knows where the registry is.
        setenv("OHOS_SA_MOCK_REGISTRY_SOCKET", kRegistrySocket.c_str(), 1);
        execl(clientPath.c_str(), clientPath.c_str(), nullptr);
        std::cerr << "[supervisor] exec client failed" << std::endl;
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

    // Determine executable directory from argv[0].
    std::string dir = ".";
    std::string argv0(argv[0]);
    auto pos = argv0.rfind('/');
    if (pos != std::string::npos) {
        dir = argv0.substr(0, pos);
    }
    std::string enginePath = dir + "/vpsdemo_engine_sa";
    std::string clientPath = dir + "/vpsdemo_client";

    // Start registry server.
    vpsdemo::RegistryServer registry(kRegistrySocket);

    registry.SetLoadCallback([&](int32_t sa_id) -> bool {
        if (sa_id != vpsdemo::kVpsBootstrapSaId) {
            return false;
        }
        if (g_engine_pid > 0) {
            // Already running — wait briefly for registration.
            return true;
        }
        std::cout << "[supervisor] spawning engine SA for load request" << std::endl;
        g_engine_pid = SpawnEngine(enginePath);
        if (g_engine_pid < 0) {
            return false;
        }
        // Give engine time to start and register.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return true;
    });

    registry.SetUnloadCallback([&](int32_t sa_id) {
        if (sa_id != vpsdemo::kVpsBootstrapSaId) {
            return;
        }
        std::cout << "[supervisor] unloading engine SA (pid=" << g_engine_pid << ")"
                  << std::endl;
        KillAndWait(g_engine_pid);
        g_engine_pid = -1;
    });

    if (!registry.Start()) {
        std::cerr << "[supervisor] failed to start registry" << std::endl;
        return 1;
    }
    std::cout << "[supervisor] registry started at " << kRegistrySocket << std::endl;

    // Spawn engine SA.
    g_engine_pid = SpawnEngine(enginePath);
    if (g_engine_pid < 0) {
        std::cerr << "[supervisor] failed to spawn engine" << std::endl;
        registry.Stop();
        return 1;
    }
    std::cout << "[supervisor] engine spawned pid=" << g_engine_pid << std::endl;

    // Wait for engine to register.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Spawn client.
    g_client_pid = SpawnClient(clientPath);
    if (g_client_pid < 0) {
        std::cerr << "[supervisor] failed to spawn client" << std::endl;
        KillAndWait(g_engine_pid);
        registry.Stop();
        return 1;
    }
    std::cout << "[supervisor] client spawned pid=" << g_client_pid << std::endl;

    // Wait for client to finish.
    int clientStatus = 0;
    waitpid(g_client_pid, &clientStatus, 0);
    g_client_pid = -1;
    std::cout << "[supervisor] client exited status="
              << WEXITSTATUS(clientStatus) << std::endl;

    // Cleanup.
    KillAndWait(g_engine_pid);
    g_engine_pid = -1;
    registry.Stop();

    std::cout << "[supervisor] done" << std::endl;
    return WEXITSTATUS(clientStatus);
}
