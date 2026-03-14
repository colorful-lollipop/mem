#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "iservice_registry.h"
#include "transport/registry_backend.h"
#include "transport/registry_server.h"
#include "transport/ves_control_interface.h"
#include "client/ves_client.h"
#include "ves/ves_types.h"

namespace {

const std::string REGISTRY_SOCKET = "/tmp/virus_executor_service_it_registry.sock";
const std::string SERVICE_SOCKET = "/tmp/virus_executor_service_it_service.sock";

std::mutex g_engine_mutex;
pid_t g_engine_pid = -1;

pid_t SpawnEngine(const std::string& enginePath) {
    pid_t pid = fork();
    if (pid == 0) {
        execl(enginePath.c_str(), enginePath.c_str(),
              REGISTRY_SOCKET.c_str(), SERVICE_SOCKET.c_str(),
              nullptr);
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

bool WaitForEngineExit(pid_t pid, std::chrono::milliseconds timeout) {
    if (pid <= 0) {
        return true;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            std::lock_guard<std::mutex> lock(g_engine_mutex);
            if (g_engine_pid == pid) {
                g_engine_pid = -1;
            }
            return true;
        }
        if (result < 0) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

std::string EnginePathFromSelf() {
    char buf[1024] = {};
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return "./VirusExecutorService";
    std::string self(buf, static_cast<size_t>(len));
    auto pos = self.rfind('/');
    if (pos == std::string::npos) return "./VirusExecutorService";
    return self.substr(0, pos) + "/VirusExecutorService";
}

}  // namespace

TEST(VesCrashRecoveryTest, CrashThenRecover) {
    const std::string enginePath = EnginePathFromSelf();
    std::atomic<int> loadCount{0};

    VirusExecutorService::RegistryServer registry(REGISTRY_SOCKET);
    registry.SetLoadCallback([&](int32_t sa_id) -> bool {
        if (sa_id != VirusExecutorService::VES_CONTROL_SA_ID) return false;
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        if (g_engine_pid > 0) {
            int status = 0;
            const pid_t result = waitpid(g_engine_pid, &status, WNOHANG);
            if (result == 0) {
                return true;
            }
            g_engine_pid = -1;
        }
        g_engine_pid = SpawnEngine(enginePath);
        if (g_engine_pid < 0) return false;
        loadCount.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return true;
    });
    registry.SetUnloadCallback([&](int32_t sa_id) {
        if (sa_id != VirusExecutorService::VES_CONTROL_SA_ID) return;
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(g_engine_pid);
        g_engine_pid = -1;
    });

    ASSERT_TRUE(registry.Start());

    auto backend = std::make_shared<VirusExecutorService::RegistryBackend>(REGISTRY_SOCKET);
    OHOS::SystemAbilityManagerClient::GetInstance().SetBackend(backend);
    VirusExecutorService::VesClient::RegisterProxyFactory();

    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        g_engine_pid = SpawnEngine(enginePath);
    }
    ASSERT_GT(g_engine_pid, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    auto remote = sam->LoadSystemAbility(VirusExecutorService::VES_CONTROL_SA_ID, 5000);
    ASSERT_NE(remote, nullptr);

    VirusExecutorService::VesClientOptions options;
    options.execTimeoutRestartDelayMs = 0;
    options.engineDeathRestartDelayMs = 0;
    auto client = std::make_unique<VirusExecutorService::VesClient>(remote, options);

    ASSERT_EQ(client->Init(), MemRpc::StatusCode::Ok);

    VirusExecutorService::ScanFileReply reply;
    ASSERT_EQ(client->ScanFile("/data/clean.apk", &reply), MemRpc::StatusCode::Ok);

    const pid_t crashedPid = g_engine_pid;

    // Crash request
    (void)client->ScanFile("/data/crash.apk", &reply);
    ASSERT_TRUE(WaitForEngineExit(crashedPid, std::chrono::seconds(5)));

    const int previousLoadCount = loadCount.load();
    ASSERT_EQ(client->ScanFile("/data/clean_after.apk", &reply), MemRpc::StatusCode::Ok);
    ASSERT_GT(loadCount.load(), previousLoadCount);

    client->Shutdown();
    registry.Stop();
    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(g_engine_pid);
        g_engine_pid = -1;
    }
}
