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
#include "transport/ves_bootstrap_interface.h"
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

std::string EnginePathFromSelf() {
    char buf[1024] = {};
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return "./virus_executor_service";
    std::string self(buf, static_cast<size_t>(len));
    auto pos = self.rfind('/');
    if (pos == std::string::npos) return "./virus_executor_service";
    return self.substr(0, pos) + "/virus_executor_service";
}

}  // namespace

TEST(VesCrashRecoveryTest, CrashThenRecover) {
    const std::string enginePath = EnginePathFromSelf();

    virus_executor_service::RegistryServer registry(REGISTRY_SOCKET);
    registry.SetLoadCallback([&](int32_t sa_id) -> bool {
        if (sa_id != virus_executor_service::VES_BOOTSTRAP_SA_ID) return false;
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        if (g_engine_pid > 0) return true;
        g_engine_pid = SpawnEngine(enginePath);
        if (g_engine_pid < 0) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return true;
    });
    registry.SetUnloadCallback([&](int32_t sa_id) {
        if (sa_id != virus_executor_service::VES_BOOTSTRAP_SA_ID) return;
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(g_engine_pid);
        g_engine_pid = -1;
    });

    ASSERT_TRUE(registry.Start());

    auto backend = std::make_shared<virus_executor_service::RegistryBackend>(REGISTRY_SOCKET);
    OHOS::SystemAbilityManagerClient::GetInstance().SetBackend(backend);
    virus_executor_service::VesClient::RegisterProxyFactory();

    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        g_engine_pid = SpawnEngine(enginePath);
    }
    ASSERT_GT(g_engine_pid, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    auto remote = sam->LoadSystemAbility(virus_executor_service::VES_BOOTSTRAP_SA_ID, 5000);
    ASSERT_NE(remote, nullptr);

    std::atomic<int> engineRestarts{0};

    auto client = std::make_unique<virus_executor_service::VesClient>(remote);
    client->SetEngineRestartCallback([&]() {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        if (g_engine_pid > 0) {
            int status = 0;
            waitpid(g_engine_pid, &status, WNOHANG);
            g_engine_pid = -1;
        }
        g_engine_pid = SpawnEngine(enginePath);
        if (g_engine_pid > 0) {
            engineRestarts++;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });

    ASSERT_EQ(client->Init(), MemRpc::StatusCode::Ok);

    virus_executor_service::ScanFileReply reply;
    ASSERT_EQ(client->ScanFile("/data/clean.apk", &reply), MemRpc::StatusCode::Ok);

    // Crash request
    (void)client->ScanFile("/data/crash.apk", &reply);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (engineRestarts.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_GT(engineRestarts.load(), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    ASSERT_EQ(client->ScanFile("/data/clean_after.apk", &reply), MemRpc::StatusCode::Ok);

    client->Shutdown();
    registry.Stop();
    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(g_engine_pid);
        g_engine_pid = -1;
    }
}
