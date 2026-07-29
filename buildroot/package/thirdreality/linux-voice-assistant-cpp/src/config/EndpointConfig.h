#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

namespace lva::config {

inline constexpr const char* kDefaultConfigPath =
    "/data/cortana/config.json";
inline constexpr const char* kDefaultCredentialPath =
    "/data/cortana/credential";

class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct EndpointConfig {
    int schema_version = 1;
    std::string endpoint;
    std::string satellite_id;
    std::string expected_area_id;
    std::string credential;

    static EndpointConfig Load(
        const std::filesystem::path& config_path = kDefaultConfigPath,
        const std::filesystem::path& credential_path =
            kDefaultCredentialPath);

    // Contains endpoint identity and only a boolean for the credential.
    // The credential itself must never be serialized or logged.
    std::string RedactedStatusJson() const;
};

}  // namespace lva::config
