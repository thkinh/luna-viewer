#pragma once
#include "types.hpp"
#include <string>
#include <vector>

namespace lunaviewer {
namespace protocol {
constexpr std::size_t max_frame = 16 * 1024 * 1024;
Bytes bytes(const std::string &);
std::string text(const Bytes &);
Bytes frame(const Bytes &);

class FrameDecoder {
  public:
    explicit FrameDecoder(std::size_t limit = max_frame) : limit_(limit) {};
    std::vector<Bytes> feed(const Bytes &);
    void finish() const;
    void reset();

  private:
    Bytes buffer_;
    std::size_t limit_;
};

Bytes multipart(const Parts &);
Parts parse_multipart(const Bytes &, std::size_t max_parts = 4096);
struct Route {
    std::string address;
    std::uint16_t port = 0;
};
struct Envelope {
    bool system = false, format = false;
    Route route;
    Bytes payload;
};

Bytes forward(const Route &, const Bytes &, bool format = false);
Bytes system(const Bytes &);
Envelope parse_envelope(const Bytes &);

std::string http_preamble(const std::string &host);
std::string md5_hex(const std::string &);
Bytes command_encrypt(const Bytes &);
Bytes command_decrypt(const Bytes &);
Bytes command_envelope(const std::string &);
std::string base64(const std::string &);

// Explicit complete wire profile: command included, exactly 28 fields. Disk field9 colons become
// '-'.
std::string registration_text(std::vector<std::string> fields);
struct RegistrationReply {
    std::vector<std::string> fields;
    std::string assigned_id, public_ip;
    std::uint16_t public_port = 0;
    int status = 0;
};

RegistrationReply parse_registration(const Bytes &);

struct LookupRequest {
    std::string target_id, password, version = "6.6.133", license1, license2, local_id,
                                     local_password;
    int target_session = -1, correlation = 1;
    bool reconnect = false;
    std::string referral = "0", payment = "0", ping_list;
    int utc_offset = 0, dictionary_count = 0;
};
std::string lookup_text(const LookupRequest &);

struct LookupReply {
    std::vector<std::string> fields;
    int status = 0, correlation = 0;
    Route route;
    std::string relay_ip, relay_hostname, secret_token;
};

LookupReply parse_lookup(const Bytes &);

struct LoginRequest {
    std::string password, nonce, local_id, display_name = "LunaViewer", connection_hash,
                                           secret_token, version = "6.6.133";
    int width = 1280, height = 720, control_mode = 0;
    bool clear_control_mode = false;
};

std::string login_text(const LoginRequest &);
Bytes keyboard(std::uint8_t kind, std::uint8_t vk, std::uint16_t scan, std::uint16_t character = 0,
               bool caps = false);
Bytes input_payload(const InputEvent &, std::uint16_t key_flags = 0);
std::string random_digits(std::size_t count);
std::string random_token(std::size_t count);
} // namespace protocol
} // namespace lunaviewer
