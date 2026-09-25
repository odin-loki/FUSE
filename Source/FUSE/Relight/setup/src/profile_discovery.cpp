// FUSE Relight RL-6.3: profile discovery and loading (see profile_discovery.hpp).
#include <fuse/relight/setup/profile_discovery.hpp>

#include <fuse/relight/hash/xxh.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>

namespace fuse::relight::setup {

namespace {

namespace fs = std::filesystem;

char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lowerAscii(a[i]) != lowerAscii(b[i])) {
            return false;
        }
    }
    return true;
}

std::string fileNameOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string dirOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string join(const std::string& dir, const char* leaf) {
    if (dir.empty()) {
        return leaf;
    }
    const char last = dir.back();
    return (last == '/' || last == '\\') ? dir + leaf : dir + "/" + leaf;
}

std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : text) {
        if (c == ';' || c == ',') {
            if (!current.empty()) {
                out.push_back(current);
            }
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        out.push_back(current);
    }
    return out;
}

std::vector<std::string> jsonFilesIn(const std::string& dir) {
    std::vector<std::string> files;
    std::error_code ec;
    const fs::path root(dir);
    if (!fs::is_directory(root, ec)) {
        return files;
    }
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc)) {
            continue;
        }
        const std::string ext = it->path().extension().string();
        if (iequals(ext, ".json")) {
            files.push_back(it->path().string());
        }
    }
    std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) {
        return fileNameOf(a) < fileNameOf(b);
    });
    return files;
}

std::mutex& runtimeMutex() {
    static std::mutex m;
    return m;
}

std::string& runtimeName() {
    static std::string name;
    return name;
}

} // namespace

std::optional<std::uint64_t> hashExecutableFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return hash::xxh3_64(bytes.data(), bytes.size());
}

int matchScore(const ProfileMatch& match, ExecutableIdentity& exe) {
    if (!match.exeHashes.empty()) {
        if (!exe.hash && !exe.path.empty()) {
            exe.hash = hashExecutableFile(exe.path);
        }
        if (exe.hash && std::find(match.exeHashes.begin(), match.exeHashes.end(), *exe.hash) != match.exeHashes.end()) {
            return 2;
        }
    }
    if (match.requireHash) {
        return 0;
    }
    const std::string name = exe.name.empty() ? fileNameOf(exe.path) : exe.name;
    for (const std::string& candidate : match.exeNames) {
        if (!name.empty() && iequals(fileNameOf(candidate), name)) {
            return 1;
        }
    }
    return 0;
}

DiscoveryResult discoverProfile(DiscoveryRequest request) {
    DiscoveryResult result;
    if (request.exe.name.empty()) {
        request.exe.name = fileNameOf(request.exe.path);
    }
    if (!request.explicitProfile.empty()) {
        std::string error;
        result.profile = loadProfileFile(request.explicitProfile, &error);
        if (result.profile) {
            result.score = 3;
            result.log.push_back("explicit profile " + request.explicitProfile);
        } else {
            result.log.push_back("explicit profile not loaded: " + error);
        }
        return result;
    }
    for (const std::string& dir : request.searchDirs) {
        for (const std::string& file : jsonFilesIn(dir)) {
            std::string error;
            std::optional<GameProfile> candidate = loadProfileFile(file, &error);
            if (!candidate) {
                result.log.push_back("skipped " + error);
                continue;
            }
            const int score = matchScore(candidate->match, request.exe);
            result.log.push_back(file + ": " + (score == 2 ? "matches by hash" : score == 1 ? "matches by name" : "no match"));
            if (score > result.score) {
                result.score = score;
                result.profile = std::move(candidate);
            }
        }
    }
    return result;
}

std::vector<std::string> defaultProfileSearchDirs(const std::string& exePath, const std::string& baseDirectory) {
    const std::string env = options::getEnvironmentVariable(kProfilePathEnvVar);
    if (!env.empty()) {
        return splitList(env);
    }
    std::vector<std::string> dirs;
    const std::string exeDir = dirOf(exePath);
    dirs.push_back(join(exeDir.empty() ? std::string(".") : exeDir, "relight/profiles"));
    const std::string base = join(baseDirectory.empty() ? std::string(".") : baseDirectory, "relight/profiles");
    std::error_code ec;
    if (std::find(dirs.begin(), dirs.end(), base) == dirs.end() &&
        !(fs::exists(base, ec) && fs::exists(dirs.front(), ec) && fs::equivalent(base, dirs.front(), ec))) {
        dirs.push_back(base);
    }
    return dirs;
}

options::OptionSystemDesc runtimeOptionSystemDesc(const std::string& baseDirectory) {
    options::OptionSystemDesc desc;
    desc.baseDirectory = baseDirectory;
    const std::string exePath = options::currentExecutablePath();
    desc.exeName = options::currentExecutableName();

    const std::string choice = options::getEnvironmentVariable(kProfileEnvVar);
    if (choice == "0" || iequals(choice, "off") || iequals(choice, "none")) {
        std::fprintf(stderr, "fuse-relight: profile: disabled (%s=%s)\n", kProfileEnvVar, choice.c_str());
        return desc;
    }
    DiscoveryRequest request;
    request.exe.path = exePath;
    request.exe.name = desc.exeName;
    request.explicitProfile = choice;
    if (choice.empty()) {
        request.searchDirs = defaultProfileSearchDirs(exePath, baseDirectory);
    }
    const DiscoveryResult found = discoverProfile(std::move(request));
    if (!found.profile) {
        for (const std::string& line : found.log) {
            if (line.rfind("skipped", 0) == 0 || line.rfind("explicit", 0) == 0) {
                std::fprintf(stderr, "fuse-relight: profile: %s\n", line.c_str());
            }
        }
        return desc;
    }
    desc.appConfig = profileToConfig(*found.profile);
    {
        std::lock_guard<std::mutex> lock(runtimeMutex());
        runtimeName() = found.profile->name.empty() ? found.profile->sourcePath : found.profile->name;
    }
    std::fprintf(stderr, "fuse-relight: profile '%s' (%s) %s: %zu option(s) in the app-config layer\n",
                 found.profile->name.c_str(), found.profile->sourcePath.c_str(),
                 found.score == 3 ? "chosen by FUSE_RELIGHT_PROFILE" : found.score == 2 ? "matched by exe hash"
                                                                                       : "matched by exe name",
                 desc.appConfig.size());
    return desc;
}

const std::string& runtimeProfileName() {
    std::lock_guard<std::mutex> lock(runtimeMutex());
    return runtimeName();
}

options::OptionLayerHandle applyProfileLayer(const GameProfile& profile, std::uint32_t priority) {
    const options::OptionConfig config = profileToConfig(profile);
    const std::string name = "profile:" + (profile.name.empty() ? profile.sourcePath : profile.name);
    return options::OptionManager::acquireLayer("", options::OptionLayerKey(priority, name),
                                                options::kDefaultLayerBlendStrength,
                                                options::kDefaultLayerBlendThreshold, false, &config);
}

} // namespace fuse::relight::setup
