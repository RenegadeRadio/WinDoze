#include "gamecrate/AclManager.hpp"
#include "gamecrate/AppContainerLauncher.hpp"
#include "gamecrate/ConsoleIo.hpp"
#include "gamecrate/DataPaths.hpp"
#include "gamecrate/InstallManager.hpp"
#include "gamecrate/ProfileStore.hpp"

#include <fstream>
#include <sstream>

namespace {

void PrintUsage() {
    std::wstringstream usage;
    usage
        << L"GameCrate — sandboxed game launcher for Windows\n\n"
        << L"Usage:\n"
        << L"  gamecrate install --id <id> --name <name> --install-dir <path> --installer <path>\n"
        << L"  gamecrate create-profile --id <id> --name <name> --install-dir <path> --executable <path>\n"
        << L"  gamecrate launch --profile <id> [--no-wait]\n"
        << L"  gamecrate grant --profile <id>\n"
        << L"  gamecrate show-install-report --profile <id>\n"
        << L"  gamecrate set-executable --profile <id> --executable <path>\n"
        << L"  gamecrate destroy-profile --profile <id> [--wipe-data]\n"
        << L"  gamecrate list-profiles [--json]\n"
        << L"  gamecrate show-profile --profile <id>\n\n"
        << L"Options for install:\n"
        << L"  --installer-args <args>  Arguments passed to the installer\n"
        << L"  --executable <path>      Skip auto-detection after install\n"
        << L"  --allow-outside-writes   Do not fail on outside writes (default)\n"
        << L"  --strict-outside-writes  Fail if installer writes outside install/save dirs\n"
        << L"  --keep-install-writable  Do not tighten install dir ACLs after install\n"
        << L"  --network                Allow network during install (not recommended)\n"
        << L"  --no-registry / --lpac-com / --no-gpu / --no-virtual-app-data\n\n"
        << L"Options for create-profile:\n"
        << L"  --save-dir <path>       Isolated save directory (default: %LOCALAPPDATA%\\GameCrate\\<id>\\saves)\n"
        << L"  --network               Grant internet and LAN capabilities\n"
        << L"  --no-registry           Do not grant registryRead (stricter, breaks many games)\n"
        << L"  --lpac-com              Grant COM capability for launcher-heavy titles\n"
        << L"  --no-gpu                Disable GPU capabilities (lpacPnpNotifications, lpacMedia)\n"
        << L"  --no-virtual-app-data   Do not redirect APPDATA/LOCALAPPDATA into the sandbox\n";
    gamecrate::WriteStderr(usage.str());
}

std::wstring RequireArg(int argc, wchar_t** argv, int& index) {
    if (index + 1 >= argc) {
        throw std::runtime_error("missing argument value");
    }
    return argv[++index];
}

std::wstring DefaultSaveDir(const std::wstring& id) {
    return gamecrate::DataPaths::DefaultSaveDir(id);
}

int CreateProfile(int argc, wchar_t** argv) {
    gamecrate::SandboxProfile profile;
    profile.registryRead = true;
    profile.gpu = true;
    profile.virtualizeAppData = true;

    for (int i = 2; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--id") {
            profile.id = RequireArg(argc, argv, i);
        } else if (arg == L"--name") {
            profile.name = RequireArg(argc, argv, i);
        } else if (arg == L"--install-dir") {
            profile.installDir = RequireArg(argc, argv, i);
        } else if (arg == L"--executable") {
            profile.executable = RequireArg(argc, argv, i);
        } else if (arg == L"--save-dir") {
            profile.saveDir = RequireArg(argc, argv, i);
        } else if (arg == L"--network") {
            profile.network = true;
        } else if (arg == L"--no-registry") {
            profile.registryRead = false;
        } else if (arg == L"--lpac-com") {
            profile.lpacCom = true;
        } else if (arg == L"--no-gpu") {
            profile.gpu = false;
        } else if (arg == L"--no-virtual-app-data") {
            profile.virtualizeAppData = false;
        }
    }

    if (profile.id.empty() || profile.name.empty() || profile.installDir.empty()) {
        gamecrate::WriteStderr(L"create-profile requires --id, --name, and --install-dir.\n");
        return 1;
    }

    if (profile.moniker.empty()) {
        profile.moniker = L"GameCrate." + profile.id;
    }
    if (profile.saveDir.empty()) {
        profile.saveDir = DefaultSaveDir(profile.id);
    }
    if (profile.executable.empty()) {
        profile.executable = profile.installDir;
    }

    profile.writablePaths = {profile.installDir, profile.saveDir};
    gamecrate::InstallManager::ApplyVirtualStorage(profile);

    if (!gamecrate::ProfileStore::Save(profile)) {
        gamecrate::WriteStderr(L"Failed to save profile.\n");
        return 1;
    }

    std::vector<SID_AND_ATTRIBUTES> capabilityAttributes;
    std::vector<gamecrate::CapabilitySid> ownedCapabilities;
    for (const auto& capabilityName : gamecrate::ProfileStore::CapabilitiesFor(profile)) {
        gamecrate::CapabilitySid capability;
        if (gamecrate::AppContainerLauncher::ResolveCapability(capabilityName, capability)) {
            capabilityAttributes.push_back({capability.sid, capability.attributes});
            ownedCapabilities.push_back(std::move(capability));
        }
    }

    PSID packageSid = nullptr;
    if (!gamecrate::AppContainerLauncher::CreateOrResolveProfile(
            profile.moniker, profile.name, capabilityAttributes, &packageSid)) {
        gamecrate::WriteStderr(L"Failed to register AppContainer profile.\n");
        return 1;
    }
    if (packageSid) {
        FreeSid(packageSid);
    }

    if (!gamecrate::InstallManager::ApplyProfileAcls(profile, gamecrate::AclMode::Run)) {
        gamecrate::WriteStderr(L"Profile saved but ACL grants failed. Re-run 'gamecrate grant --profile " +
                               profile.id + L"'.\n");
        return 1;
    }

    gamecrate::WriteStdout(L"Created profile '" + profile.id + L"' with moniker " + profile.moniker + L"\n");
    return 0;
}

int InstallProfile(int argc, wchar_t** argv) {
    gamecrate::InstallOptions options;
    options.registryRead = true;
    options.gpu = true;
    options.network = false;
    options.virtualizeAppData = true;

    for (int i = 2; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--id") {
            options.id = RequireArg(argc, argv, i);
        } else if (arg == L"--name") {
            options.name = RequireArg(argc, argv, i);
        } else if (arg == L"--install-dir") {
            options.installDir = RequireArg(argc, argv, i);
        } else if (arg == L"--installer") {
            options.installer = RequireArg(argc, argv, i);
        } else if (arg == L"--installer-args") {
            options.installerArgs = RequireArg(argc, argv, i);
        } else if (arg == L"--executable") {
            options.executable = RequireArg(argc, argv, i);
        } else if (arg == L"--save-dir") {
            options.saveDir = RequireArg(argc, argv, i);
        } else if (arg == L"--network") {
            options.network = true;
        } else if (arg == L"--no-registry") {
            options.registryRead = false;
        } else if (arg == L"--lpac-com") {
            options.lpacCom = true;
        } else if (arg == L"--no-gpu") {
            options.gpu = false;
        } else if (arg == L"--allow-outside-writes") {
            options.failOnOutsideWrites = false;
        } else if (arg == L"--strict-outside-writes") {
            options.failOnOutsideWrites = true;
        } else if (arg == L"--keep-install-writable") {
            options.tightenAclsAfter = false;
        } else if (arg == L"--no-virtual-app-data") {
            options.virtualizeAppData = false;
        }
    }

    const gamecrate::InstallResult result = gamecrate::InstallManager::Run(options);

    if (!result.success && !result.message.empty()) {
        gamecrate::WriteStderr(result.message + L"\n");
    }

    gamecrate::WriteStdout(L"Installer exit code: " + std::to_wstring(result.installerExitCode) + L"\n");
    gamecrate::WriteStdout(L"Installed files: " + std::to_wstring(result.installedFiles.size()) + L"\n");
    gamecrate::WriteStdout(L"Outside writes: " + std::to_wstring(result.outsideWrites.size()) + L"\n");
    gamecrate::WriteStdout(L"Registry changes: " + std::to_wstring(result.registryChanges.size()) + L"\n");

    if (!result.executable.empty()) {
        gamecrate::WriteStdout(L"Executable: " + result.executable + L"\n");
    }
    if (!result.reportPath.empty()) {
        gamecrate::WriteStdout(L"Report: " + result.reportPath + L"\n");
    }

    if (!result.outsideWrites.empty()) {
        gamecrate::WriteStderr(L"\nFiles written outside sandbox:\n");
        for (const auto& path : result.outsideWrites) {
            gamecrate::WriteStderr(L"  " + path + L"\n");
        }
    }

    if (!result.suspiciousOutsideWrites.empty()) {
        gamecrate::WriteStderr(L"\nSuspicious outside writes:\n");
        for (const auto& path : result.suspiciousOutsideWrites) {
            gamecrate::WriteStderr(L"  " + path + L"\n");
        }
    }

    if (!result.suspiciousRegistryChanges.empty()) {
        gamecrate::WriteStderr(L"\nSuspicious registry persistence keys:\n");
        for (const auto& entry : result.suspiciousRegistryChanges) {
            gamecrate::WriteStderr(L"  " + entry + L"\n");
        }
    } else if (!result.registryChanges.empty()) {
        gamecrate::WriteStdout(L"\nRegistry changes (informational, install not blocked):\n");
        for (const auto& entry : result.registryChanges) {
            gamecrate::WriteStdout(L"  " + entry + L"\n");
        }
    }

    if (result.success && !result.message.empty()) {
        gamecrate::WriteStdout(result.message + L"\n");
    }

    return result.success ? 0 : 1;
}

int LaunchProfile(int argc, wchar_t** argv) {
    std::wstring profileId;
    bool waitForExit = true;

    for (int i = 2; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--profile") {
            profileId = RequireArg(argc, argv, i);
        } else if (arg == L"--no-wait") {
            waitForExit = false;
        }
    }

    if (profileId.empty()) {
        gamecrate::WriteStderr(L"launch requires --profile <id>.\n");
        return 1;
    }

    gamecrate::SandboxProfile profile;
    if (!gamecrate::ProfileStore::Load(profileId, profile)) {
        gamecrate::WriteStderr(L"Profile not found: " + profileId + L"\n");
        return 1;
    }

    gamecrate::InstallManager::ApplyVirtualStorage(profile);

    if (!gamecrate::AppContainerLauncher::HasBalancedQuotes(profile.arguments)) {
        gamecrate::WriteStderr(L"Profile arguments have an unterminated quote.\n");
        return 1;
    }

    if (!gamecrate::InstallManager::ApplyProfileAcls(profile, gamecrate::AclMode::Run)) {
        gamecrate::WriteStderr(L"Warning: ACL grants may be incomplete.\n");
    }

    gamecrate::LaunchOptions options;
    options.moniker = profile.moniker;
    options.displayName = profile.name;
    options.executable = profile.executable;
    options.arguments = profile.arguments;
    options.capabilities = gamecrate::ProfileStore::CapabilitiesFor(profile);
    options.lessPrivileged = true;
    options.waitForExit = waitForExit;
    options.retainProfile = true;
    options.workingDirectory = profile.installDir;
    options.environmentBlock = gamecrate::InstallManager::BuildEnvironmentForProfile(profile);

    const gamecrate::LaunchResult result = gamecrate::AppContainerLauncher::Launch(options);
    if (!result.success) {
        gamecrate::WriteStderr(result.message + L"\n");
        return static_cast<int>(result.errorCode);
    }

    std::wstringstream launchMessage;
    launchMessage << L"Launched PID " << result.processId;
    if (waitForExit) {
        launchMessage << L" (exit code " << result.exitCode << L")";
    }
    launchMessage << L"\n";
    gamecrate::WriteStdout(launchMessage.str());
    return static_cast<int>(result.exitCode);
}

int GrantProfile(int argc, wchar_t** argv) {
    std::wstring profileId;
    for (int i = 2; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--profile") {
            profileId = RequireArg(argc, argv, i);
        }
    }

    if (profileId.empty()) {
        gamecrate::WriteStderr(L"grant requires --profile <id>.\n");
        return 1;
    }

    gamecrate::SandboxProfile profile;
    if (!gamecrate::ProfileStore::Load(profileId, profile)) {
        gamecrate::WriteStderr(L"Profile not found: " + profileId + L"\n");
        return 1;
    }

    gamecrate::InstallManager::ApplyVirtualStorage(profile);

    if (!gamecrate::InstallManager::ApplyProfileAcls(profile, gamecrate::AclMode::Run)) {
        gamecrate::WriteStderr(L"Failed to apply ACL grants.\n");
        return 1;
    }

    gamecrate::WriteStdout(L"ACL grants applied for profile '" + profileId + L"'.\n");
    return 0;
}

int ShowInstallReport(int argc, wchar_t** argv) {
    std::wstring profileId;
    for (int i = 2; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--profile") {
            profileId = RequireArg(argc, argv, i);
        }
    }

    if (profileId.empty()) {
        gamecrate::WriteStderr(L"show-install-report requires --profile <id>.\n");
        return 1;
    }

    const std::wstring reportPath =
        gamecrate::DataPaths::ProfileDataRoot(profileId) + L"\\install-report.json";

    std::wifstream in(reportPath);
    if (!in.is_open()) {
        gamecrate::WriteStderr(L"Install report not found: " + reportPath + L"\n");
        return 1;
    }

    std::wstringstream buffer;
    buffer << in.rdbuf();
    gamecrate::WriteStdout(buffer.str());
    return 0;
}

int ListProfiles(int argc, wchar_t** argv) {
    bool asJson = false;
    for (int i = 2; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--json") {
            asJson = true;
        }
    }

    if (asJson) {
        gamecrate::WriteStdout(gamecrate::ProfileStore::ListProfilesJson());
        return 0;
    }

    const auto profiles = gamecrate::ProfileStore::ListProfiles();
    if (profiles.empty()) {
        gamecrate::WriteStdout(L"No profiles found.\n");
        return 0;
    }

    for (const auto& id : profiles) {
        gamecrate::SandboxProfile profile;
        if (gamecrate::ProfileStore::Load(id, profile)) {
            gamecrate::WriteStdout(id + L" — " + profile.name + L" (" + profile.installDir + L")\n");
        } else {
            gamecrate::WriteStdout(id + L"\n");
        }
    }
    return 0;
}

int ShowProfile(int argc, wchar_t** argv) {
    std::wstring profileId;
    for (int i = 2; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--profile") {
            profileId = RequireArg(argc, argv, i);
        }
    }

    if (profileId.empty()) {
        gamecrate::WriteStderr(L"show-profile requires --profile <id>.\n");
        return 1;
    }

    gamecrate::SandboxProfile profile;
    if (!gamecrate::ProfileStore::Load(profileId, profile)) {
        gamecrate::WriteStderr(L"Profile not found: " + profileId + L"\n");
        return 1;
    }

    std::wstringstream profileText;
    profileText << L"id: " << profile.id << L"\n"
                << L"name: " << profile.name << L"\n"
                << L"moniker: " << profile.moniker << L"\n"
                << L"installDir: " << profile.installDir << L"\n"
                << L"executable: " << profile.executable << L"\n"
                << L"saveDir: " << profile.saveDir << L"\n"
                << L"network: " << (profile.network ? L"true" : L"false") << L"\n"
                << L"registryRead: " << (profile.registryRead ? L"true" : L"false") << L"\n"
                << L"gpu: " << (profile.gpu ? L"true" : L"false") << L"\n"
                << L"virtualizeAppData: " << (profile.virtualizeAppData ? L"true" : L"false") << L"\n"
                << L"lpacCom: " << (profile.lpacCom ? L"true" : L"false") << L"\n";
    gamecrate::WriteStdout(profileText.str());
    return 0;
}

int SetExecutable(int argc, wchar_t** argv) {
    std::wstring profileId;
    std::wstring executable;
    for (int i = 2; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--profile") {
            profileId = RequireArg(argc, argv, i);
        } else if (arg == L"--executable") {
            executable = RequireArg(argc, argv, i);
        }
    }

    if (profileId.empty() || executable.empty()) {
        gamecrate::WriteStderr(L"set-executable requires --profile <id> and --executable <path>.\n");
        return 1;
    }

    if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) {
        gamecrate::WriteStderr(L"Executable not found: " + executable + L"\n");
        return 1;
    }

    gamecrate::SandboxProfile profile;
    if (!gamecrate::ProfileStore::Load(profileId, profile)) {
        gamecrate::WriteStderr(L"Profile not found: " + profileId + L"\n");
        return 1;
    }

    profile.executable = executable;
    if (!gamecrate::ProfileStore::Save(profile)) {
        gamecrate::WriteStderr(L"Failed to save profile.\n");
        return 1;
    }

    if (!gamecrate::InstallManager::ApplyProfileAcls(profile, gamecrate::AclMode::Run)) {
        gamecrate::WriteStderr(L"Profile saved but ACL grants failed. Re-run 'gamecrate grant --profile " +
                               profileId + L"'.\n");
        return 1;
    }

    gamecrate::WriteStdout(L"Set executable for profile '" + profileId + L"' to " + executable + L"\n");
    return 0;
}

int DestroyProfile(int argc, wchar_t** argv) {
    std::wstring profileId;
    bool wipeData = false;

    for (int i = 2; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--profile") {
            profileId = RequireArg(argc, argv, i);
        } else if (arg == L"--wipe-data") {
            wipeData = true;
        }
    }

    if (profileId.empty()) {
        gamecrate::WriteStderr(L"destroy-profile requires --profile <id>.\n");
        return 1;
    }

    if (!gamecrate::ProfileStore::Destroy(profileId, wipeData)) {
        gamecrate::WriteStderr(L"Failed to destroy profile '" + profileId + L"'.\n");
        return 1;
    }

    std::wstring destroyed = L"Destroyed profile '" + profileId + L"'";
    if (wipeData) {
        destroyed += L" and removed sandbox data";
    }
    destroyed += L".\n";
    gamecrate::WriteStdout(destroyed);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    const std::wstring command = argv[1];
    try {
        if (command == L"install") {
            return InstallProfile(argc, argv);
        }
        if (command == L"create-profile") {
            return CreateProfile(argc, argv);
        }
        if (command == L"launch") {
            return LaunchProfile(argc, argv);
        }
        if (command == L"grant") {
            return GrantProfile(argc, argv);
        }
        if (command == L"show-install-report") {
            return ShowInstallReport(argc, argv);
        }
        if (command == L"set-executable") {
            return SetExecutable(argc, argv);
        }
        if (command == L"destroy-profile") {
            return DestroyProfile(argc, argv);
        }
        if (command == L"list-profiles") {
            return ListProfiles(argc, argv);
        }
        if (command == L"show-profile") {
            return ShowProfile(argc, argv);
        }
    } catch (const std::exception& ex) {
        const std::string message = ex.what();
        gamecrate::WriteStderr(std::wstring(message.begin(), message.end()) + L"\n");
        return 1;
    }

    PrintUsage();
    return 1;
}
