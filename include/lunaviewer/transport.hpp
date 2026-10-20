#pragma once
#include "types.hpp"
#include <functional>
#include <memory>
#include <string>
namespace lunaviewer {
struct TlsConfig {
    std::string connect_address, sni = "ultraviewer", verify_name, http_host;
    std::uint16_t port = 443;
    std::string ca_file; // Empty uses system trust. Verification is always enabled.
    int deadline_ms = 20000;
    std::size_t max_queued_bytes = 32 * 1024 * 1024;
};
// Construct, call and destroy on one thread with a running Qt event loop.
// ready follows verified TLS + queuing the streaming HTTP preamble; frames are unframed envelope
// bodies.
class TlsTransport {
  public:
    TlsTransport();
    ~TlsTransport();
    TlsTransport(const TlsTransport &) = delete;
    TlsTransport &operator=(const TlsTransport &) = delete;
    std::function<void()> ready;
    std::function<void(Bytes)> frame;
    std::function<void(std::string)> error;
    void start(const TlsConfig &);
    void send(const Bytes &envelope);
    // Drain queued application data until written or deadline, then close. New sends rejected.
    void finish(int deadline_ms = 500);
    void close();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace lunaviewer
