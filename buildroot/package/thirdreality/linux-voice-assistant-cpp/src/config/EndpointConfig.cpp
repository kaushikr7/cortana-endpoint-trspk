#include "config/EndpointConfig.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <set>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lva::config {

namespace {

constexpr std::size_t kMaxConfigBytes = 64 * 1024;
constexpr std::size_t kMaxCredentialBytes = 4096;
constexpr std::size_t kMinCredentialBytes = 32;

class UniqueFd {
public:
    explicit UniqueFd(int fd) : fd_(fd) {}
    ~UniqueFd() {
        if (fd_ >= 0) ::close(fd_);
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    int get() const { return fd_; }

private:
    int fd_;
};

std::string DisplayPath(const std::filesystem::path& path) {
    // These are operator-selected paths, not secret values.
    return path.string();
}

void ValidateProtectedDirectory(const std::filesystem::path& path) {
    const int raw_fd = ::open(path.c_str(),
                              O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (raw_fd < 0) {
        throw ConfigError("cannot open protected config directory " +
                          DisplayPath(path) + ": " + std::strerror(errno));
    }
    const UniqueFd fd(raw_fd);

    struct stat st {};
    if (::fstat(fd.get(), &st) != 0) {
        throw ConfigError("cannot inspect protected config directory " +
                          DisplayPath(path));
    }
    if (!S_ISDIR(st.st_mode)) {
        throw ConfigError("protected config path is not a directory: " +
                          DisplayPath(path));
    }
    if (st.st_uid != ::geteuid()) {
        throw ConfigError("protected config directory is not owned by the "
                          "endpoint user: " + DisplayPath(path));
    }
    if ((st.st_mode & 0077) != 0 || (st.st_mode & 0700) != 0700) {
        throw ConfigError("protected config directory must have mode 0700: " +
                          DisplayPath(path));
    }
}

std::string ReadProtectedFile(const std::filesystem::path& path,
                              std::size_t max_bytes,
                              std::string_view description) {
    ValidateProtectedDirectory(path.parent_path());

    const int raw_fd =
        ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (raw_fd < 0) {
        throw ConfigError("cannot open " + std::string(description) + " " +
                          DisplayPath(path) + ": " + std::strerror(errno));
    }
    const UniqueFd fd(raw_fd);

    struct stat st {};
    if (::fstat(fd.get(), &st) != 0) {
        throw ConfigError("cannot inspect " + std::string(description) + " " +
                          DisplayPath(path));
    }
    if (!S_ISREG(st.st_mode)) {
        throw ConfigError(std::string(description) +
                          " must be a regular file");
    }
    if (st.st_uid != ::geteuid()) {
        throw ConfigError(std::string(description) +
                          " is not owned by the endpoint user");
    }
    if ((st.st_mode & 0777) != 0600) {
        throw ConfigError(std::string(description) +
                          " must have mode 0600");
    }
    if (st.st_nlink != 1) {
        throw ConfigError(std::string(description) +
                          " must not have additional hard links");
    }
    if (st.st_size < 0 || static_cast<std::uintmax_t>(st.st_size) > max_bytes) {
        throw ConfigError(std::string(description) + " is too large");
    }

    std::string result;
    result.reserve(static_cast<std::size_t>(st.st_size));
    char buffer[4096];
    while (true) {
        const ssize_t count = ::read(fd.get(), buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            throw ConfigError("cannot read " + std::string(description));
        }
        result.append(buffer, static_cast<std::size_t>(count));
        if (result.size() > max_bytes) {
            throw ConfigError(std::string(description) + " is too large");
        }
    }
    return result;
}

bool IsSatelliteId(std::string_view value) {
    if (value.empty() || value.size() > 64) return false;
    if (value.front() < 'a' || value.front() > 'z') return false;
    return std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '-';
    });
}

bool IsAreaId(std::string_view value) {
    if (value.empty() || value.size() > 64) return false;
    if (value.front() < 'a' || value.front() > 'z') return false;
    return std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '_';
    });
}

bool IsHttpsOrigin(std::string_view value) {
    constexpr std::string_view kScheme = "https://";
    if (!value.starts_with(kScheme) || value.size() > 512) return false;

    value.remove_prefix(kScheme.size());
    if (value.empty()) return false;
    if (value.back() == '/') value.remove_suffix(1);
    if (value.empty() || value.find('/') != std::string_view::npos) return false;
    if (value.find_first_of("?#@") != std::string_view::npos) return false;
    if (std::any_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isspace(c) || std::iscntrl(c);
        })) {
        return false;
    }

    const auto colon = value.rfind(':');
    const std::string_view host = colon == std::string_view::npos
        ? value
        : value.substr(0, colon);
    const std::string_view port = colon == std::string_view::npos
        ? std::string_view{}
        : value.substr(colon + 1);
    if (host.empty()) return false;
    if (!std::all_of(host.begin(), host.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '.' || c == '-';
        })) {
        return false;
    }
    if (!port.empty()) {
        unsigned int port_number = 0;
        for (const char c : port) {
            if (c < '0' || c > '9') return false;
            port_number = port_number * 10u + static_cast<unsigned int>(c - '0');
            if (port_number > 65535u) return false;
        }
        if (port_number == 0) return false;
    }
    return colon == std::string_view::npos || !port.empty();
}

std::string RequiredString(const nlohmann::json& document,
                           const char* field) {
    const auto it = document.find(field);
    if (it == document.end() || !it->is_string()) {
        throw ConfigError(std::string("config field '") + field +
                          "' must be a string");
    }
    return it->get<std::string>();
}

void ValidateCredential(std::string_view credential) {
    if (credential.size() < kMinCredentialBytes) {
        throw ConfigError("credential must contain at least 32 characters");
    }
    if (credential.size() > kMaxCredentialBytes) {
        throw ConfigError("credential is too large");
    }
    if (!std::all_of(credential.begin(), credential.end(),
                     [](unsigned char c) { return c >= 33 && c <= 126; })) {
        throw ConfigError("credential must contain printable ASCII without "
                          "whitespace");
    }
}

}  // namespace

EndpointConfig EndpointConfig::Load(
    const std::filesystem::path& config_path,
    const std::filesystem::path& credential_path) {
    const std::string config_text =
        ReadProtectedFile(config_path, kMaxConfigBytes, "config file");

    nlohmann::json document;
    try {
        document = nlohmann::json::parse(config_text);
    } catch (...) {
        throw ConfigError("config file contains invalid JSON");
    }
    if (!document.is_object()) {
        throw ConfigError("config root must be a JSON object");
    }

    static const std::set<std::string> kAllowedFields = {
        "schemaVersion", "endpoint", "satelliteId", "expectedAreaId",
    };
    for (const auto& [key, value] : document.items()) {
        (void)value;
        if (!kAllowedFields.contains(key)) {
            throw ConfigError("unknown config field: " + key);
        }
    }

    const auto schema = document.find("schemaVersion");
    if (schema == document.end() || !schema->is_number_integer() ||
        schema->get<int>() != 1) {
        throw ConfigError("config field 'schemaVersion' must be integer 1");
    }

    EndpointConfig result;
    result.endpoint = RequiredString(document, "endpoint");
    result.satellite_id = RequiredString(document, "satelliteId");
    if (const auto area = document.find("expectedAreaId");
        area != document.end()) {
        if (!area->is_string()) {
            throw ConfigError("config field 'expectedAreaId' must be a string");
        }
        result.expected_area_id = area->get<std::string>();
        if (!IsAreaId(result.expected_area_id)) {
            throw ConfigError("config field 'expectedAreaId' must match "
                              "[a-z][a-z0-9_]{0,63}");
        }
    }

    if (!IsHttpsOrigin(result.endpoint)) {
        throw ConfigError("config field 'endpoint' must be an HTTPS origin "
                          "without a path, query, or credentials");
    }
    if (result.endpoint.back() == '/') result.endpoint.pop_back();
    if (!IsSatelliteId(result.satellite_id)) {
        throw ConfigError("config field 'satelliteId' must match "
                          "[a-z][a-z0-9-]{0,63}");
    }
    result.credential = ReadProtectedFile(
        credential_path, kMaxCredentialBytes, "credential file");
    ValidateCredential(result.credential);
    return result;
}

std::string EndpointConfig::RedactedStatusJson() const {
    nlohmann::json status = {
        {"configured", true},
        {"schemaVersion", schema_version},
        {"endpoint", endpoint},
        {"satelliteId", satellite_id},
        {"credentialPresent", !credential.empty()},
    };
    if (!expected_area_id.empty()) {
        status["expectedAreaId"] = expected_area_id;
    }
    return status.dump();
}

}  // namespace lva::config
