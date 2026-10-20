#pragma once
#include <cstddef>
#include <memory>
#include <string>
namespace lunaviewer {
// Metadata-only sink. Callers must exclude credentials, payloads and captured input.
class SessionLogger {
public:
    explicit SessionLogger(std::string directory = {}, std::size_t maximum_bytes = 1048576,
                           unsigned backups = 3, bool debug = false);
    ~SessionLogger();
    SessionLogger(const SessionLogger&) = delete;
    SessionLogger& operator=(const SessionLogger&) = delete;
    void write(const std::string& level, const std::string& category,
               const std::string& message) noexcept;
    std::string path() const;
    bool available() const;
    std::string error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace lunaviewer
