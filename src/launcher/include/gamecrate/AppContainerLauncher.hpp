#pragma once

#include <Windows.h>
#include <UserEnv.h>
#include <sddl.h>

#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "Userenv.lib")
#pragma comment(lib, "onecoreuap.lib")

namespace gamecrate {

struct CapabilitySid {
    PSID sid = nullptr;
    DWORD attributes = SE_GROUP_ENABLED;

    CapabilitySid() = default;
    CapabilitySid(const CapabilitySid&) = delete;
    CapabilitySid& operator=(const CapabilitySid&) = delete;

    CapabilitySid(CapabilitySid&& other) noexcept {
        sid = other.sid;
        attributes = other.attributes;
        other.sid = nullptr;
    }

    CapabilitySid& operator=(CapabilitySid&& other) noexcept {
        if (this != &other) {
            reset();
            sid = other.sid;
            attributes = other.attributes;
            other.sid = nullptr;
        }
        return *this;
    }

    ~CapabilitySid() { reset(); }

    void reset() {
        if (sid) {
            LocalFree(sid);
            sid = nullptr;
        }
    }
};

struct LaunchOptions {
    std::wstring executable;
    std::wstring arguments;
    std::wstring moniker;
    std::wstring displayName;
    std::vector<std::wstring> capabilities;
    bool lessPrivileged = true;
    bool useAppContainer = true;
    bool waitForExit = true;
    bool retainProfile = true;
    std::wstring workingDirectory;
    std::unique_ptr<wchar_t[]> environmentBlock;
};

struct LaunchResult {
    bool success = false;
    DWORD errorCode = 0;
    DWORD processId = 0;
    DWORD exitCode = 0;
    std::wstring message;
};

class AppContainerLauncher {
public:
    static LaunchResult Launch(const LaunchOptions& options);
    static bool HasBalancedQuotes(const std::wstring& arguments);
    static bool ResolveCapability(const std::wstring& name, CapabilitySid& out);
    static bool CreateOrResolveProfile(
        const std::wstring& moniker,
        const std::wstring& displayName,
        const std::vector<SID_AND_ATTRIBUTES>& capabilities,
        PSID* outSid,
        HRESULT* outHr = nullptr);

private:
    static DWORD LaunchMonitored(
        const LaunchOptions& options,
        DWORD& outProcessId,
        DWORD& outExitCode);

    static DWORD LaunchProcess(
        PSID packageSid,
        const LaunchOptions& options,
        const std::vector<SID_AND_ATTRIBUTES>& capabilities,
        DWORD& outProcessId,
        DWORD& outExitCode);
};

}  // namespace gamecrate
