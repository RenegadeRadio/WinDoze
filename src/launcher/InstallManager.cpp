#include "gamecrate/InstallManager.hpp"

#include "gamecrate/AclManager.hpp"
#include "gamecrate/AppContainerLauncher.hpp"
#include "gamecrate/DataPaths.hpp"
#include "gamecrate/RegistryScanner.hpp"
#include "gamecrate/VirtualStorage.hpp"
#include "gamecrate/Win32Error.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace gamecrate {

namespace {

bool EnsureDirectory(const std::wstring& path, std::wstring* errorOut = nullptr) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (std::filesystem::exists(path, ec)) {
        return true;
    }
    if (errorOut) {
        const DWORD error = ec.value() != 0 ? static_cast<DWORD>(ec.value()) : ERROR_PATH_NOT_FOUND;
        *errorOut = FormatWin32Error(error);
    }
    return false;
}

std::wstring DirectoryFailureMessage(const std::wstring& label, const std::wstring& path, const std::wstring& error) {
    std::wstring message = L"Cannot create " + label + L": " + path;
    if (!error.empty()) {
        message += L" (" + error + L")";
    }
    if (error.find(L"Access is denied") != std::wstring::npos ||
        error.find(L"access is denied") != std::wstring::npos) {
        message +=
            L". Choose a folder you own (e.g. D:\\Games\\MyGame), not Program Files or another protected path.";
    }
    return message;
}

std::wstring StripTrailingSeparators(std::wstring path) {
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    return path;
}

std::wstring ValidateInstallPath(const std::wstring& path) {
    if (path.find(L'"') != std::wstring::npos) {
        return L"Install directory cannot contain a quotation mark.";
    }

    const std::wstring normalized = PathUtils::Normalize(path);
    if (normalized.empty()) {
        return L"Install directory path is invalid.";
    }

    if (normalized.find(L"\\program files") != std::wstring::npos ||
        normalized.find(L"\\windows\\") != std::wstring::npos ||
        (normalized.size() >= 8 && normalized.rfind(L"\\windows") == normalized.size() - 8)) {
        return L"Install directory cannot be under Program Files or Windows.";
    }

    if (normalized.size() >= 3 && normalized[1] == L':' && normalized[2] == L'\\') {
        const size_t firstSep = normalized.find(L'\\', 3);
        if (firstSep == std::wstring::npos) {
            return L"Do not install directly to a drive root. Use a subfolder like D:\\Games\\MyGame.";
        }
        if (normalized.find(L'\\', firstSep + 1) == std::wstring::npos) {
            return L"Install folder \"" + path +
                   L"\" is directly on the drive root (e.g. C:\\Install). Windows often blocks "
                   L"sandbox ACL changes there. Use a folder you own, e.g. "
                   L"C:\\Users\\<you>\\Games\\city-studio or D:\\Games\\city-studio.";
        }
    }

    return L"";
}

bool IsInstallerLikeName(const std::wstring& fileName) {
    const std::wstring lower = PathUtils::ToLower(fileName);
    if (lower == L"setup.exe" || lower == L"install.exe" || lower == L"launcher.exe") {
        return true;
    }
    return lower.find(L"unins") != std::wstring::npos || lower.find(L"installer") != std::wstring::npos;
}

std::wstring EscapeJson(const std::wstring& value) {
    std::wstring escaped;
    escaped.reserve(value.size());
    for (wchar_t ch : value) {
        if (ch == L'\\' || ch == L'"') {
            escaped.push_back(L'\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

std::wstring WriteStringArrayJson(const std::vector<std::wstring>& values) {
    std::wstringstream ss;
    ss << L"[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            ss << L", ";
        }
        ss << L"\"" << EscapeJson(values[i]) << L"\"";
    }
    ss << L"]";
    return ss.str();
}

std::wstring BuildInstallerArguments(const InstallOptions& options) {
    if (!options.installerArgs.empty()) {
        return options.installerArgs;
    }

    // Inno Setup and many Windows installers accept /DIR= to steer the target path.
    return L"/DIR=\"" + StripTrailingSeparators(options.installDir) + L"\"";
}

bool RegisterProfile(const SandboxProfile& profile, std::wstring& errorMessage) {
    std::vector<SID_AND_ATTRIBUTES> capabilityAttributes;
    std::vector<CapabilitySid> ownedCapabilities;
    for (const auto& capabilityName : ProfileStore::CapabilitiesFor(profile)) {
        CapabilitySid capability;
        if (AppContainerLauncher::ResolveCapability(capabilityName, capability)) {
            capabilityAttributes.push_back({capability.sid, capability.attributes});
            ownedCapabilities.push_back(std::move(capability));
        }
    }

    PSID packageSid = nullptr;
    HRESULT hr = S_OK;
    if (!AppContainerLauncher::CreateOrResolveProfile(
            profile.moniker, profile.name, capabilityAttributes, &packageSid, &hr)) {
        errorMessage = L"Failed to register AppContainer profile";
        if (FAILED(hr)) {
            std::wstringstream ss;
            ss << errorMessage << L" (HRESULT=0x" << std::hex << static_cast<unsigned long>(hr) << L")";
            errorMessage = ss.str();
        }
        errorMessage += L".";
        return false;
    }
    if (packageSid) {
        FreeSid(packageSid);
    }
    return true;
}

}  // namespace

void InstallManager::ApplyVirtualStorage(SandboxProfile& profile) {
    if (!profile.virtualizeAppData) {
        return;
    }

    VirtualStorage::Ensure(profile.id);
    for (const auto& root : VirtualStorage::WritableRoots(profile.id)) {
        const std::wstring normalizedRoot = PathUtils::Normalize(root);
        bool alreadyGranted = false;
        for (const auto& path : profile.writablePaths) {
            if (PathUtils::Normalize(path) == normalizedRoot) {
                alreadyGranted = true;
                break;
            }
        }
        if (!alreadyGranted) {
            profile.writablePaths.push_back(root);
        }
    }
}

std::unique_ptr<wchar_t[]> InstallManager::BuildEnvironmentForProfile(const SandboxProfile& profile) {
    if (!profile.virtualizeAppData) {
        return nullptr;
    }

    const VirtualStorageLayout layout = VirtualStorage::Ensure(profile.id);
    return VirtualStorage::BuildEnvironmentBlock(VirtualStorage::EnvironmentOverrides(layout));
}

bool InstallManager::ApplyProfileAcls(
    const SandboxProfile& profile,
    AclMode mode,
    std::wstring* errorOut,
    const std::vector<AclGrant>* extraGrants) {
    if (errorOut) {
        errorOut->clear();
    }

    PSID packageSid = nullptr;
    HRESULT hr = DeriveAppContainerSidFromAppContainerName(profile.moniker.c_str(), &packageSid);
    if (FAILED(hr)) {
        std::vector<SID_AND_ATTRIBUTES> capabilities;
        if (!AppContainerLauncher::CreateOrResolveProfile(
                profile.moniker, profile.name, capabilities, &packageSid)) {
            if (errorOut) {
                *errorOut = L"Failed to resolve AppContainer SID for ACL grants.";
            }
            return false;
        }
    }

    const PathAccess installAccess =
        mode == AclMode::Install ? PathAccess::ReadWriteExecute : PathAccess::ReadExecute;

    std::vector<AclGrant> grants;
    auto addGrant = [&](const std::wstring& path, PathAccess access) {
        if (!path.empty()) {
            grants.push_back({path, access});
        }
    };

    addGrant(profile.installDir, installAccess);
    addGrant(profile.saveDir, PathAccess::ReadWrite);

    for (const auto& path : profile.writablePaths) {
        if (PathUtils::Normalize(path) == PathUtils::Normalize(profile.installDir)) {
            continue;
        }
        addGrant(path, PathAccess::ReadWrite);
    }
    for (const auto& path : profile.readablePaths) {
        addGrant(path, PathAccess::ReadExecute);
    }
    if (extraGrants) {
        for (const auto& grant : *extraGrants) {
            addGrant(grant.path, grant.access);
        }
    }

    std::wstring failedPath;
    DWORD failedError = ERROR_SUCCESS;
    const bool ok = AclManager::GrantAppContainerAccess(packageSid, grants, failedPath, failedError);
    if (!ok && errorOut) {
        *errorOut = L"ACL grant failed on " + failedPath + L": " + FormatWin32Error(failedError);
        if (failedError == ERROR_ACCESS_DENIED) {
            *errorOut +=
                L". Use an install folder you own (e.g. D:\\Games\\MyGame), not Program Files. "
                L"Run GameCrate as a standard user — elevated installs are not required.";
        }
    }

    if (packageSid) {
        FreeSid(packageSid);
    }
    return ok;
}

bool InstallManager::RemoveProfileAcls(const SandboxProfile& profile) {
    PSID packageSid = nullptr;
    HRESULT hr = DeriveAppContainerSidFromAppContainerName(profile.moniker.c_str(), &packageSid);
    if (FAILED(hr)) {
        std::vector<SID_AND_ATTRIBUTES> capabilities;
        if (!AppContainerLauncher::CreateOrResolveProfile(
                profile.moniker, profile.name, capabilities, &packageSid)) {
            return false;
        }
    }

    std::vector<std::wstring> paths;
    auto addPath = [&](const std::wstring& path) {
        if (!path.empty()) {
            paths.push_back(path);
        }
    };

    addPath(profile.installDir);
    addPath(profile.saveDir);
    for (const auto& path : profile.writablePaths) {
        addPath(path);
    }
    for (const auto& path : profile.readablePaths) {
        addPath(path);
    }

    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());

    bool ok = true;
    for (const auto& path : paths) {
        ok = AclManager::RemoveAppContainerAccess(packageSid, path) && ok;
    }

    if (packageSid) {
        FreeSid(packageSid);
    }
    return ok;
}

std::wstring InstallManager::DetectExecutable(const std::wstring& installDir) {
    struct Candidate {
        std::wstring path;
        uintmax_t size = 0;
        int depth = 0;
    };

    std::vector<Candidate> candidates;
    std::error_code ec;
    if (!std::filesystem::exists(installDir, ec)) {
        return L"";
    }

    const std::filesystem::directory_options options =
        std::filesystem::directory_options::skip_permission_denied;

    std::filesystem::recursive_directory_iterator it(installDir, options, ec);
    const std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        const auto& entry = *it;
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        if (it.depth() > 3) {
            it.disable_recursion_pending();
            continue;
        }
        if (entry.path().extension() != L".exe") {
            continue;
        }

        const std::wstring fileName = entry.path().filename().wstring();
        if (IsInstallerLikeName(fileName)) {
            continue;
        }

        candidates.push_back(
            {entry.path().wstring(), entry.file_size(ec), static_cast<int>(it.depth())});
    }

    if (candidates.empty()) {
        return L"";
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.depth != b.depth) {
            return a.depth < b.depth;
        }
        return a.size > b.size;
    });

    return candidates.front().path;
}

bool InstallManager::WriteReport(const InstallResult& result, const InstallOptions& options) {
    const std::wstring reportDir = DataPaths::ProfileDataRoot(options.id);
    if (!EnsureDirectory(reportDir)) {
        return false;
    }

    const std::wstring reportPath = reportDir + L"\\install-report.json";
    std::wstringstream json;
    json << L"{\n";
    json << L"  \"profileId\": \"" << EscapeJson(options.id) << L"\",\n";
    json << L"  \"installer\": \"" << EscapeJson(options.installer) << L"\",\n";
    json << L"  \"installerExitCode\": " << result.installerExitCode << L",\n";
    json << L"  \"executable\": \"" << EscapeJson(result.executable) << L"\",\n";
    json << L"  \"installedFileCount\": " << result.installedFiles.size() << L",\n";
    json << L"  \"outsideWriteCount\": " << result.outsideWrites.size() << L",\n";
    json << L"  \"suspiciousOutsideWriteCount\": " << result.suspiciousOutsideWrites.size() << L",\n";
    json << L"  \"registryChangeCount\": " << result.registryChanges.size() << L",\n";
    json << L"  \"outsideRegistryChangeCount\": " << result.outsideRegistryChanges.size() << L",\n";
    json << L"  \"suspiciousRegistryChangeCount\": " << result.suspiciousRegistryChanges.size()
         << L",\n";
    json << L"  \"installedFiles\": " << WriteStringArrayJson(result.installedFiles) << L",\n";
    json << L"  \"outsideWrites\": " << WriteStringArrayJson(result.outsideWrites) << L",\n";
    json << L"  \"suspiciousOutsideWrites\": " << WriteStringArrayJson(result.suspiciousOutsideWrites)
         << L",\n";
    json << L"  \"registryChanges\": " << WriteStringArrayJson(result.registryChanges) << L",\n";
    json << L"  \"outsideRegistryChanges\": " << WriteStringArrayJson(result.outsideRegistryChanges)
         << L",\n";
    json << L"  \"suspiciousRegistryChanges\": "
         << WriteStringArrayJson(result.suspiciousRegistryChanges) << L"\n";
    json << L"}\n";

    std::wofstream out(reportPath);
    if (!out.is_open()) {
        return false;
    }
    out << json.str();
    return true;
}

InstallResult InstallManager::Run(const InstallOptions& options) {
    InstallResult result;

    if (options.id.empty() || options.name.empty() || options.installDir.empty() ||
        options.installer.empty()) {
        result.message = L"install requires --id, --name, --install-dir, and --installer.";
        return result;
    }

    if (GetFileAttributesW(options.installer.c_str()) == INVALID_FILE_ATTRIBUTES) {
        result.message = L"Installer not found: " + options.installer;
        return result;
    }

    InstallOptions normalized = options;
    if (normalized.saveDir.empty()) {
        normalized.saveDir = DataPaths::DefaultSaveDir(normalized.id);
    }

    const std::wstring installPathError = ValidateInstallPath(normalized.installDir);
    if (!installPathError.empty()) {
        result.message = installPathError;
        return result;
    }
    normalized.installDir = StripTrailingSeparators(normalized.installDir);

    std::wstring directoryError;
    if (!EnsureDirectory(normalized.installDir, &directoryError)) {
        result.message = DirectoryFailureMessage(L"install directory", normalized.installDir, directoryError);
        return result;
    }
    if (!EnsureDirectory(normalized.saveDir, &directoryError)) {
        result.message = DirectoryFailureMessage(L"save directory", normalized.saveDir, directoryError);
        return result;
    }

    SandboxProfile profile;
    profile.id = normalized.id;
    profile.name = normalized.name;
    profile.moniker = L"GameCrate." + normalized.id;
    profile.installDir = normalized.installDir;
    profile.saveDir = normalized.saveDir;
    profile.executable = normalized.executable;
    profile.network = normalized.network;
    profile.registryRead = normalized.registryRead;
    profile.lpacCom = normalized.lpacCom;
    profile.gpu = normalized.gpu;
    profile.virtualizeAppData = normalized.virtualizeAppData;
    profile.writablePaths = {profile.installDir, profile.saveDir};
    ApplyVirtualStorage(profile);

    if (!RegisterProfile(profile, result.message)) {
        return result;
    }

    std::wstring aclError;
    if (!ApplyProfileAcls(profile, AclMode::Install, &aclError)) {
        result.message = aclError.empty() ? L"Failed to apply install-phase ACL grants." : aclError;
        return result;
    }

    const auto allowedRoots =
        FootprintScanner::AllowedInstallRoots(profile.id, profile.installDir, profile.saveDir);
    const auto watchPaths = FootprintScanner::SensitiveWatchPaths();

    const FootprintSnapshot beforeAllowed = FootprintScanner::ScanRoots(allowedRoots);
    const FootprintSnapshot beforeWatch = FootprintScanner::ScanRoots(watchPaths);
    const RegistrySnapshot beforeRegistry = RegistryScanner::CaptureWatchKeys();

    LaunchOptions launchOptions;
    launchOptions.executable = normalized.installer;
    launchOptions.arguments = BuildInstallerArguments(normalized);
    launchOptions.useAppContainer = false;
    launchOptions.waitForExit = true;
    launchOptions.workingDirectory = normalized.installDir;
    launchOptions.environmentBlock = BuildEnvironmentForProfile(profile);

    const LaunchResult launchResult = AppContainerLauncher::Launch(launchOptions);
    result.installerExitCode = launchResult.exitCode;

    if (!launchResult.success) {
        result.message = L"Installer failed: " + launchResult.message;
        if (launchResult.message.find(L"Access is denied") != std::wstring::npos ||
            launchResult.message.find(L"access is denied") != std::wstring::npos) {
            result.message +=
                L" Many installers (Inno Setup, repacks) need Administrator — approve the UAC prompt if "
                L"one appears, or right-click GameCrate.Gui.exe → Run as administrator.";
        }
        return result;
    }

    const FootprintSnapshot afterAllowed = FootprintScanner::ScanRoots(allowedRoots);
    const FootprintSnapshot afterWatch = FootprintScanner::ScanRoots(watchPaths);
    const RegistrySnapshot afterRegistry = RegistryScanner::CaptureWatchKeys();

    const FootprintDiff allowedDiff = FootprintScanner::Compare(beforeAllowed, afterAllowed);
    const FootprintDiff watchDiff = FootprintScanner::Compare(beforeWatch, afterWatch);

    for (const auto& file : allowedDiff.added) {
        result.installedFiles.push_back(file.path);
    }
    for (const auto& file : allowedDiff.modified) {
        result.installedFiles.push_back(file.path);
    }

    for (const auto& file : watchDiff.added) {
        if (PathUtils::IsUnderAnyRoot(file.path, allowedRoots)) {
            continue;
        }
        if (FootprintScanner::IsBenignOutsidePath(file.path)) {
            continue;
        }
        result.outsideWrites.push_back(file.path);
        if (FootprintScanner::IsSuspiciousOutsidePath(file.path)) {
            result.suspiciousOutsideWrites.push_back(file.path);
        }
    }

    const RegistryDiff registryDiff = RegistryScanner::Compare(beforeRegistry, afterRegistry);
    for (const auto& entry : registryDiff.added) {
        const std::wstring formatted = RegistryScanner::FormatEntry(entry);
        result.registryChanges.push_back(formatted);
        if (RegistryScanner::IsSuspiciousKey(entry.keyPath)) {
            result.suspiciousRegistryChanges.push_back(formatted);
            result.outsideRegistryChanges.push_back(formatted);
        }
    }
    for (const auto& entry : registryDiff.modified) {
        const std::wstring formatted = RegistryScanner::FormatEntry(entry);
        result.registryChanges.push_back(formatted);
        if (RegistryScanner::IsSuspiciousKey(entry.keyPath)) {
            result.suspiciousRegistryChanges.push_back(formatted);
            result.outsideRegistryChanges.push_back(formatted);
        }
    }

    if (profile.executable.empty()) {
        profile.executable = DetectExecutable(profile.installDir);
    }
    result.executable = profile.executable;

    if (profile.executable.empty()) {
        result.message =
            L"Install finished but no game executable was detected. Set --executable manually.";
        result.success = false;
    } else {
        result.success = true;
    }

    if (!result.suspiciousOutsideWrites.empty() || !result.suspiciousRegistryChanges.empty() ||
        (normalized.failOnOutsideWrites && !result.outsideWrites.empty())) {
        result.success = false;
        if (normalized.failOnOutsideWrites && !result.outsideWrites.empty()) {
            result.message = L"Detected file writes outside the sandbox footprint.";
        } else if (!result.suspiciousOutsideWrites.empty()) {
            result.message = L"Detected suspicious file writes outside the sandbox footprint.";
        } else {
            result.message = L"Detected suspicious registry persistence keys (Run/RunOnce).";
        }
    }

    if (!ProfileStore::Save(profile)) {
        result.success = false;
        result.message = L"Failed to save profile after install.";
        return result;
    }

    if (normalized.tightenAclsAfter) {
        ApplyProfileAcls(profile, AclMode::Run);
    }

    result.reportPath = DataPaths::ProfileDataRoot(profile.id) + L"\\install-report.json";
    WriteReport(result, normalized);

    if (result.success) {
        result.message = result.outsideWrites.empty()
            ? L"Install completed successfully."
            : L"Install completed successfully. Review the install report for recorded outside writes.";
    }

    return result;
}

}  // namespace gamecrate
