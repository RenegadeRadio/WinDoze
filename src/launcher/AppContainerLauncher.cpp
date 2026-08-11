#include "gamecrate/AppContainerLauncher.hpp"

#include "gamecrate/Win32Error.hpp"

#include <algorithm>
#include <sstream>

namespace gamecrate {

namespace {

std::wstring HresultMessage(const wchar_t* context, HRESULT hr) {
    std::wstringstream ss;
    ss << context << L" (HRESULT=0x" << std::hex << static_cast<unsigned long>(hr) << L")";
    return ss.str();
}

void FreeSidArray(PSID* sids, ULONG count) {
    if (!sids) {
        return;
    }
    for (ULONG i = 0; i < count; ++i) {
        if (sids[i]) {
            LocalFree(sids[i]);
        }
    }
    LocalFree(sids);
}

}  // namespace

bool AppContainerLauncher::HasBalancedQuotes(const std::wstring& arguments) {
    return std::count(arguments.begin(), arguments.end(), L'"') % 2 == 0;
}

bool AppContainerLauncher::ResolveCapability(const std::wstring& name, CapabilitySid& out) {
    out.reset();

    PSID sid = nullptr;
    if (ConvertStringSidToSid(name.c_str(), &sid)) {
        out.sid = sid;
        return true;
    }

    PSID* capGroupSids = nullptr;
    DWORD capGroupSidsLen = 0;
    PSID* capSids = nullptr;
    DWORD capSidsLen = 0;

    if (!DeriveCapabilitySidsFromName(
            name.c_str(), &capGroupSids, &capGroupSidsLen, &capSids, &capSidsLen)) {
        return false;
    }

    FreeSidArray(capGroupSids, capGroupSidsLen);

    if (capSidsLen == 0 || capSids == nullptr) {
        LocalFree(capSids);
        return false;
    }

    out.sid = capSids[0];
    LocalFree(capSids);
    return true;
}

bool AppContainerLauncher::CreateOrResolveProfile(
    const std::wstring& moniker,
    const std::wstring& displayName,
    const std::vector<SID_AND_ATTRIBUTES>& capabilities,
    PSID* outSid,
    HRESULT* outHr) {
    if (outHr) {
        *outHr = S_OK;
    }

    PSID packageSid = nullptr;
    const PCWSTR display = displayName.empty() ? moniker.c_str() : displayName.c_str();

    HRESULT hr = CreateAppContainerProfile(
        moniker.c_str(),
        display,
        display,
        capabilities.empty() ? nullptr : const_cast<PSID_AND_ATTRIBUTES>(capabilities.data()),
        static_cast<DWORD>(capabilities.size()),
        &packageSid);

    if (FAILED(hr)) {
        if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
            hr = DeriveAppContainerSidFromAppContainerName(moniker.c_str(), &packageSid);
            if (FAILED(hr)) {
                if (outHr) {
                    *outHr = hr;
                }
                return false;
            }
        } else {
            if (outHr) {
                *outHr = hr;
            }
            return false;
        }
    }

    *outSid = packageSid;
    return true;
}

DWORD AppContainerLauncher::LaunchProcess(
    PSID packageSid,
    const LaunchOptions& options,
    const std::vector<SID_AND_ATTRIBUTES>& capabilities,
    DWORD& outProcessId,
    DWORD& outExitCode) {
    DWORD result = ERROR_SUCCESS;
    DWORD attributeCount = 1;
    if (options.lessPrivileged) {
        ++attributeCount;
    }

    SIZE_T attributeListSize = 0;
    if (!InitializeProcThreadAttributeList(nullptr, attributeCount, 0, &attributeListSize)) {
        result = GetLastError();
        if (result != ERROR_INSUFFICIENT_BUFFER) {
            return result;
        }
    }

    auto* attributeList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, attributeListSize));
    if (!attributeList) {
        return ERROR_OUTOFMEMORY;
    }

    if (!InitializeProcThreadAttributeList(attributeList, attributeCount, 0, &attributeListSize)) {
        HeapFree(GetProcessHeap(), 0, attributeList);
        return GetLastError();
    }

    SECURITY_CAPABILITIES securityCapabilities{};
    securityCapabilities.AppContainerSid = packageSid;
    securityCapabilities.Capabilities =
        capabilities.empty() ? nullptr : const_cast<PSID_AND_ATTRIBUTES>(capabilities.data());
    securityCapabilities.CapabilityCount = static_cast<DWORD>(capabilities.size());

    if (!UpdateProcThreadAttribute(
            attributeList,
            0,
            PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
            &securityCapabilities,
            sizeof(securityCapabilities),
            nullptr,
            nullptr)) {
        result = GetLastError();
        DeleteProcThreadAttributeList(attributeList);
        HeapFree(GetProcessHeap(), 0, attributeList);
        return result;
    }

    if (options.lessPrivileged) {
        DWORD allAppPackagesPolicy = PROCESS_CREATION_ALL_APPLICATION_PACKAGES_OPT_OUT;
        if (!UpdateProcThreadAttribute(
                attributeList,
                0,
                PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY,
                &allAppPackagesPolicy,
                sizeof(allAppPackagesPolicy),
                nullptr,
                nullptr)) {
            result = GetLastError();
            DeleteProcThreadAttributeList(attributeList);
            HeapFree(GetProcessHeap(), 0, attributeList);
            return result;
        }
    }

    STARTUPINFOEXW startupInfo{};
    startupInfo.StartupInfo.cb = sizeof(startupInfo);
    startupInfo.lpAttributeList = attributeList;

    PROCESS_INFORMATION procInfo{};
    std::wstring commandLine = options.arguments.empty()
        ? L"\"" + options.executable + L"\""
        : L"\"" + options.executable + L"\" " + options.arguments;

    const wchar_t* workingDirectory =
        options.workingDirectory.empty() ? nullptr : options.workingDirectory.c_str();

    if (!CreateProcessW(
            nullptr,
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
            options.environmentBlock ? options.environmentBlock.get() : nullptr,
            workingDirectory,
            reinterpret_cast<LPSTARTUPINFOW>(&startupInfo),
            &procInfo)) {
        result = GetLastError();
        DeleteProcThreadAttributeList(attributeList);
        HeapFree(GetProcessHeap(), 0, attributeList);
        return result;
    }

    outProcessId = procInfo.dwProcessId;

    if (options.waitForExit) {
        WaitForSingleObject(procInfo.hProcess, INFINITE);
        if (!GetExitCodeProcess(procInfo.hProcess, &outExitCode)) {
            result = GetLastError();
        } else {
            result = outExitCode;
        }
    }

    CloseHandle(procInfo.hProcess);
    CloseHandle(procInfo.hThread);
    DeleteProcThreadAttributeList(attributeList);
    HeapFree(GetProcessHeap(), 0, attributeList);
    return result;
}

DWORD AppContainerLauncher::LaunchMonitored(
    const LaunchOptions& options,
    DWORD& outProcessId,
    DWORD& outExitCode) {
    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);

    PROCESS_INFORMATION procInfo{};
    std::wstring commandLine = options.arguments.empty()
        ? L"\"" + options.executable + L"\""
        : L"\"" + options.executable + L"\" " + options.arguments;

    const wchar_t* workingDirectory =
        options.workingDirectory.empty() ? nullptr : options.workingDirectory.c_str();

    if (!CreateProcessW(
            nullptr,
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_UNICODE_ENVIRONMENT,
            options.environmentBlock ? options.environmentBlock.get() : nullptr,
            workingDirectory,
            &startupInfo,
            &procInfo)) {
        return GetLastError();
    }

    outProcessId = procInfo.dwProcessId;

    DWORD result = ERROR_SUCCESS;
    if (options.waitForExit) {
        WaitForSingleObject(procInfo.hProcess, INFINITE);
        if (!GetExitCodeProcess(procInfo.hProcess, &outExitCode)) {
            result = GetLastError();
        } else {
            result = outExitCode;
        }
    }

    CloseHandle(procInfo.hProcess);
    CloseHandle(procInfo.hThread);
    return result;
}

LaunchResult AppContainerLauncher::Launch(const LaunchOptions& options) {
    LaunchResult launchResult;

    if (options.executable.empty()) {
        launchResult.message = L"Executable is required.";
        launchResult.errorCode = ERROR_INVALID_PARAMETER;
        return launchResult;
    }

    if (!options.useAppContainer) {
        DWORD exitCode = 0;
        DWORD processId = 0;
        const DWORD launchError = LaunchMonitored(options, processId, exitCode);

        launchResult.processId = processId;
        launchResult.exitCode = exitCode;
        launchResult.errorCode = launchError;
        launchResult.success = launchError == ERROR_SUCCESS || launchError == STILL_ACTIVE;

        if (!launchResult.success) {
            launchResult.message =
                L"Installer process failed: " + FormatWin32Error(launchError) + L" (" +
                std::to_wstring(launchError) + L")";
        }
        return launchResult;
    }

    if (options.moniker.empty()) {
        launchResult.message = L"Moniker is required for AppContainer launch.";
        launchResult.errorCode = ERROR_INVALID_PARAMETER;
        return launchResult;
    }

    std::vector<CapabilitySid> ownedCapabilities;
    std::vector<SID_AND_ATTRIBUTES> capabilityAttributes;

    for (const auto& capabilityName : options.capabilities) {
        CapabilitySid capability;
        if (!ResolveCapability(capabilityName, capability)) {
            launchResult.message = L"Failed to resolve capability: " + capabilityName;
            launchResult.errorCode = ERROR_INVALID_PARAMETER;
            return launchResult;
        }
        capabilityAttributes.push_back({capability.sid, capability.attributes});
        ownedCapabilities.push_back(std::move(capability));
    }

    PSID packageSid = nullptr;
    HRESULT profileHr = S_OK;
    if (!CreateOrResolveProfile(
            options.moniker,
            options.displayName.empty() ? options.moniker : options.displayName,
            capabilityAttributes,
            &packageSid,
            &profileHr)) {
        launchResult.message = HresultMessage(L"Failed to create or resolve AppContainer profile", profileHr);
        launchResult.errorCode = FAILED(profileHr) ? static_cast<DWORD>(profileHr) : ERROR_ACCESS_DENIED;
        return launchResult;
    }

    DWORD exitCode = 0;
    DWORD processId = 0;
    const DWORD launchError =
        LaunchProcess(packageSid, options, capabilityAttributes, processId, exitCode);

    if (!options.retainProfile) {
        DeleteAppContainerProfile(options.moniker.c_str());
    }

    if (packageSid) {
        FreeSid(packageSid);
    }

    launchResult.processId = processId;
    launchResult.exitCode = exitCode;
    launchResult.errorCode = launchError;
    launchResult.success = launchError == ERROR_SUCCESS || launchError == STILL_ACTIVE;

    if (!launchResult.success) {
        launchResult.message =
            L"CreateProcess failed: " + FormatWin32Error(launchError) + L" (" + std::to_wstring(launchError) + L")";
        if (launchError == ERROR_ACCESS_DENIED) {
            launchResult.message +=
                L". The sandbox could not run the installer — GameCrate grants read access to the "
                L"installer file during setup, but the installer may require administrator rights or "
                L"cannot run inside LPAC. Try a user-writable install folder (not C:\\Install).";
        }
    }

    return launchResult;
}

}  // namespace gamecrate
