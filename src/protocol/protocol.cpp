#include "lunaviewer/protocol.hpp"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <climits>
#include <memory>
#include <stdexcept>
namespace lunaviewer {
namespace protocol {
namespace {
void require(bool b, const char *s) {
    if (!b)
        throw std::runtime_error(s);
}
void u32(Bytes &b, std::uint32_t n) {
    for (int i = 0; i < 4; ++i)
        b.push_back((n >> (8 * i)) & 255);
}
std::uint32_t read32(const Bytes &b, std::size_t p) {
    require(p <= b.size() && b.size() - p >= 4, "Truncated length");
    return std::uint32_t(b[p]) | (std::uint32_t(b[p + 1]) << 8) | (std::uint32_t(b[p + 2]) << 16) |
           (std::uint32_t(b[p + 3]) << 24);
}
void ascii(const std::string &s, bool field = true) {
    for (unsigned char c : s)
        require(c >= 32 && c <= 126 && (!field || c != ':'), "Unsupported ASCII field");
}
std::string join(const std::vector<std::string> &f) {
    std::string s;
    for (std::size_t i = 0; i < f.size(); ++i) {
        ascii(f[i]);
        if (i)
            s += ':';
        require(s.size() + f[i].size() <= 65536, "Command exceeds limit");
        s += f[i];
    }
    return s;
}
std::vector<std::string> split(const Bytes &b) {
    require(b.size() <= 65536, "Reply exceeds limit");
    auto s = text(b);
    ascii(s, false);
    std::vector<std::string> f;
    std::size_t p = 0;
    for (;;) {
        auto e = s.find(':', p);
        f.push_back(s.substr(p, e == std::string::npos ? e : e - p));
        require(f.size() <= 128, "Too many fields");
        if (e == std::string::npos)
            break;
        p = e + 1;
    }
    return f;
}
long long number(const std::string &s, long long lo, long long hi) {
    require(!s.empty(), "Missing number");
    std::size_t n = 0;
    long long v = 0;
    try {
        v = std::stoll(s, &n);
    } catch (...) {
        throw std::runtime_error("Invalid number");
    }
    require(n == s.size() && s[0] != '+' && v >= lo && v <= hi, "Number out of range");
    for (std::size_t i = s[0] == '-' ? 1 : 0; i < s.size(); ++i)
        require(s[i] >= '0' && s[i] <= '9', "Invalid decimal");
    return v;
}
Bytes digest(const Bytes &b, const EVP_MD *md) {
    Bytes out(EVP_MAX_MD_SIZE);
    unsigned n = 0;
    require(EVP_Digest(b.data(), b.size(), out.data(), &n, md, nullptr) == 1, "Digest failed");
    out.resize(n);
    return out;
}
Bytes cipher(const Bytes &in, bool encrypt) {
    require(in.size() <= max_frame, "Cipher input too large");
    Bytes pw{5, 0, 10, 0, 15, 0, 20, 0, 25, 0};
    auto key = digest(pw, EVP_sha1());
    key.resize(24, 0);
    auto iv = digest({}, EVP_sha1());
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),
                                                                        EVP_CIPHER_CTX_free);
    require(bool(ctx), "Cipher context allocation failed");
    require(EVP_CipherInit_ex(ctx.get(), EVP_des_ede3_cbc(), nullptr, key.data(), iv.data(),
                              encrypt ? 1 : 0) == 1,
            "3DES initialization failed");
    Bytes out(in.size() + EVP_MAX_BLOCK_LENGTH);
    int n = 0, last = 0;
    require(EVP_CipherUpdate(ctx.get(), out.data(), &n, in.data(), int(in.size())) == 1,
            "3DES update failed");
    require(EVP_CipherFinal_ex(ctx.get(), out.data() + n, &last) == 1, "Invalid 3DES padding");
    out.resize(n + last);
    return out;
}
std::array<std::uint8_t, 4> ip(const std::string &s) {
    std::array<std::uint8_t, 4> a{};
    require(inet_pton(AF_INET, s.c_str(), a.data()) == 1, "Invalid IPv4 address");
    return a;
}
} // namespace
Bytes bytes(const std::string &s) {
    return Bytes(s.begin(), s.end());
}
std::string text(const Bytes &b) {
    return std::string(b.begin(), b.end());
}
Bytes frame(const Bytes &b) {
    require(b.size() <= max_frame, "Frame exceeds limit");
    Bytes out;
    out.reserve(b.size() + 4);
    u32(out, b.size());
    out.insert(out.end(), b.begin(), b.end());
    return out;
}
std::vector<Bytes> FrameDecoder::feed(const Bytes &b) {
    std::vector<Bytes> out;
    std::size_t cursor = 0;
    while (cursor < b.size()) {
        std::size_t needed = 4;
        if (buffer_.size() >= 4) {
            auto n = read32(buffer_, 0);
            require(n <= INT32_MAX && n <= limit_, "Frame length exceeds limit");
            needed = 4 + std::size_t(n);
        }
        auto take = std::min(needed - buffer_.size(), b.size() - cursor);
        buffer_.insert(buffer_.end(), b.begin() + cursor, b.begin() + cursor + take);
        cursor += take;
        if (buffer_.size() >= 4) {
            auto n = read32(buffer_, 0);
            require(n <= INT32_MAX && n <= limit_, "Frame length exceeds limit");
            if (buffer_.size() == 4 + std::size_t(n)) {
                out.emplace_back(buffer_.begin() + 4, buffer_.end());
                buffer_.clear();
            }
        }
    }
    return out;
}
void FrameDecoder::finish() const {
    require(buffer_.empty(), "Truncated frame at EOF");
}
void FrameDecoder::reset() {
    buffer_.clear();
}
Bytes multipart(const Parts &ps) {
    Bytes out;
    require(ps.size() <= 4096, "Too many parts");
    for (auto &p : ps) {
        require(p.size() <= max_frame && out.size() + 4 + p.size() <= max_frame, "Multipart limit");
        u32(out, p.size());
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}
Parts parse_multipart(const Bytes &b, std::size_t count) {
    require(b.size() <= max_frame, "Multipart limit");
    Parts out;
    std::size_t p = 0;
    while (p < b.size()) {
        require(out.size() < count, "Too many parts");
        auto n = read32(b, p);
        p += 4;
        require(n <= INT32_MAX && n <= b.size() - p, "Truncated multipart body");
        out.emplace_back(b.begin() + p, b.begin() + p + n);
        p += n;
    }
    return out;
}
Bytes forward(const Route &r, const Bytes &b, bool fmt) {
    auto a = ip(r.address);
    require(r.port != 0, "Invalid route port");
    require(b.size() <= max_frame - 8, "Forward limit");
    Bytes out{0,
              std::uint8_t(fmt),
              a[3],
              a[2],
              a[1],
              a[0],
              std::uint8_t(r.port),
              std::uint8_t(r.port >> 8)};
    out.insert(out.end(), b.begin(), b.end());
    return out;
}
Bytes system(const Bytes &b) {
    require(b.size() < max_frame, "System limit");
    Bytes out{1};
    out.insert(out.end(), b.begin(), b.end());
    return out;
}
Envelope parse_envelope(const Bytes &b) {
    require(!b.empty() && b.size() <= max_frame, "Invalid envelope");
    Envelope e;
    if (b[0] == 1) {
        e.system = true;
        e.payload = Bytes(b.begin() + 1, b.end());
        return e;
    }
    require(b[0] == 0 && b.size() >= 8, "Invalid forward envelope");
    e.format = b[1] != 0;
    e.route.address = std::to_string(b[5]) + "." + std::to_string(b[4]) + "." +
                      std::to_string(b[3]) + "." + std::to_string(b[2]);
    e.route.port = b[6] | (unsigned(b[7]) << 8);
    require(e.route.port != 0, "Invalid route port");
    e.payload = Bytes(b.begin() + 8, b.end());
    return e;
}
std::string http_preamble(const std::string &host) {
    ascii(host);
    require(!host.empty() && host.size() <= 253, "Invalid HTTP Host");
    for (char c : host)
        require((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '-',
                "Invalid HTTP hostname");
    return "POST /UVService/ HTTP/1.1\r\nUser-Agent: Mozilla/4.0 (compatible; MSIE5.01; Windows "
           "NT)\r\nContent-Type: application/octet-stream\r\nConnection: "
           "Keep-Alive\r\nContent-length: 8589934590\r\nHost: " +
           host + "\r\n\r\n";
}
std::string md5_hex(const std::string &s) {
    ascii(s, false);
    auto d = digest(bytes(s), EVP_md5());
    const char *h = "0123456789abcdef";
    std::string out;
    for (auto c : d) {
        out += h[c >> 4];
        out += h[c & 15];
    }
    return out;
}
Bytes command_encrypt(const Bytes &b) {
    return cipher(b, true);
}
Bytes command_decrypt(const Bytes &b) {
    return cipher(b, false);
}
Bytes command_envelope(const std::string &s) {
    ascii(s, false);
    auto c = command_encrypt(bytes(s));
    Bytes out{1, 0};
    out.insert(out.end(), c.begin(), c.end());
    return out;
}
std::string base64(const std::string &s) {
    require(s.size() <= 65536, "Base64 input limit");
    std::string out(4 * ((s.size() + 2) / 3), '\0');
    if (!s.empty())
        EVP_EncodeBlock(reinterpret_cast<unsigned char *>(&out[0]),
                        reinterpret_cast<const unsigned char *>(s.data()), int(s.size()));
    return out;
}
std::string registration_text(std::vector<std::string> f) {
    require(f.size() == 28 && f[0] == "3", "Registration needs 28 fields");
    std::replace(f[9].begin(), f[9].end(), ':', '-');
    ip(f[4]);
    return join(f);
}
RegistrationReply parse_registration(const Bytes &b) {
    RegistrationReply r;
    r.fields = split(b);
    auto &f = r.fields;
    require(f.size() >= 11 && f[0] == "3", "Invalid registration reply");
    r.public_port = number(f[1], 1, 65535);
    require(!f[2].empty() && f[2].size() <= 20, "Invalid assigned ID");
    for (char c : f[2])
        require(c >= '0' && c <= '9', "Invalid assigned ID");
    require(f[2].find_first_not_of('0') != std::string::npos, "Zero assigned ID");
    try {
        std::size_t used = 0;
        auto id = std::stoull(f[2], &used);
        require(id != 0 && used == f[2].size(), "Invalid assigned ID");
    } catch (...) {
        throw std::runtime_error("Assigned ID exceeds UInt64 range");
    }
    r.assigned_id = f[2];
    ip(f[3]);
    r.public_ip = f[3];
    r.status = number(f[7], INT32_MIN, INT32_MAX);
    return r;
}
std::string lookup_text(const LookupRequest &r) {
    return join({"8", r.target_id, md5_hex(std::string(32, ' ') + r.password), r.version,
                 r.license1, r.license2, r.local_id, md5_hex(r.local_id + r.local_password),
                 std::to_string(r.target_session), std::to_string(r.correlation),
                 r.reconnect ? "-1" : "0", r.referral, r.payment, std::to_string(r.utc_offset),
                 std::to_string(r.dictionary_count), r.ping_list, ""});
}
LookupReply parse_lookup(const Bytes &b) {
    LookupReply r;
    r.fields = split(b);
    auto &f = r.fields;
    require(f.size() >= 3 && f[0] == "8", "Invalid lookup reply");
    r.status = number(f[1], INT32_MIN, INT32_MAX);
    r.correlation = number(f.back(), INT32_MIN, INT32_MAX);
    if (r.status == 1) {
        require(f.size() >= 15, "Incomplete successful lookup");
        ip(f[4]);
        ip(f[11]);
        r.route = {f[4], std::uint16_t(number(f[9], 1, 65535))};
        r.relay_ip = f[11];
        r.relay_hostname = f[14];
        if (f.size() > 20)
            r.secret_token = f[20];
    }
    return r;
}
std::string login_text(const LoginRequest &r) {
    require(r.nonce.size() == 8, "Login nonce must be eight digits");
    for (char c : r.nonce)
        require(c >= '0' && c <= '9', "Invalid nonce");
    ascii(r.display_name, false);
    std::string name;
    const std::string removed = "!#$&?:/\\*,.@<>;'\"=";
    for (char c : r.display_name)
        if (removed.find(c) == std::string::npos && name.size() < 20)
            name += c;
    return join({"login",
                 r.nonce,
                 md5_hex(r.nonce + r.password),
                 std::to_string(r.width),
                 std::to_string(r.height),
                 "0",
                 "15",
                 "15",
                 "0",
                 "0",
                 r.local_id,
                 name,
                 std::to_string(r.clear_control_mode ? 0 : r.control_mode),
                 md5_hex(std::string(32, ' ') + r.password),
                 "",
                 r.connection_hash,
                 "2",
                 "45",
                 "0",
                 "0",
                 r.secret_token,
                 r.version,
                 "0"});
}
Bytes keyboard(std::uint8_t kind, std::uint8_t vk, std::uint16_t scan, std::uint16_t ch,
               bool caps) {
    require(kind >= 1 && kind <= 4, "Invalid keyboard kind");
    return {30,
            124,
            124,
            124,
            kind,
            vk,
            std::uint8_t(scan),
            std::uint8_t(scan >> 8),
            std::uint8_t(ch),
            std::uint8_t(ch >> 8),
            std::uint8_t(caps)};
}
Bytes input_payload(const InputEvent &e, std::uint16_t flags) {
    if (e.kind == InputEvent::Kind::Key) {
        require(e.virtual_key <= 255 && e.scan_code <= 65535, "Invalid key code");
        auto scan = std::uint16_t(e.scan_code);
        if (e.extended && (scan & 0xff00) == 0)
            scan |= 0xe000;
        return keyboard(e.system_key ? (e.pressed ? 3 : 4) : (e.pressed ? 1 : 2), e.virtual_key,
                        scan, e.character, e.caps_lock);
    }
    require(e.kind != InputEvent::Kind::ReleaseAll, "ReleaseAll must expand tracked input state");
    std::string s = "8|||";
    if (e.kind == InputEvent::Kind::Move)
        return bytes(s + "512|||" + std::to_string(flags) + "|||" + std::to_string(e.x) + "|||" +
                     std::to_string(e.y));
    require(e.x >= INT16_MIN && e.x <= INT16_MAX && e.y >= INT16_MIN && e.y <= INT16_MAX,
            "Packed mouse coordinates out of range");
    auto pos =
        std::int32_t(std::uint32_t(std::uint16_t(e.x)) | (std::uint32_t(std::uint16_t(e.y)) << 16));
    int msg = 0;
    std::int32_t wp = flags;
    if (e.kind == InputEvent::Kind::Wheel) {
        require(e.wheel >= INT16_MIN && e.wheel <= INT16_MAX, "Wheel out of range");
        msg = 522;
        wp = std::int32_t(std::uint32_t(flags) | (std::uint32_t(std::uint16_t(e.wheel)) << 16));
    } else {
        require(e.kind == InputEvent::Kind::Button && e.button >= 1 && e.button <= 3,
                "Invalid mouse button");
        msg = (e.button == 1 ? 513 : e.button == 2 ? 516 : 519) + (e.pressed ? 0 : 1);
    }
    return bytes(s + std::to_string(msg) + "|||" + std::to_string(wp) + "|||" +
                 std::to_string(pos));
}
std::string random_token(std::size_t n) {
    require(n <= 1024, "Random token limit");
    std::string out;
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    while (out.size() < n) {
        unsigned char v;
        require(RAND_bytes(&v, 1) == 1, "Secure random failure");
        if (v < 248)
            out += alphabet[v % 62];
    }
    return out;
}
std::string random_digits(std::size_t n) {
    require(n <= 1024, "Random digits limit");
    std::string out;
    while (out.size() < n) {
        unsigned char v;
        require(RAND_bytes(&v, 1) == 1, "Secure random failure");
        if (v < 250)
            out += char('0' + v % 10);
    }
    return out;
}
} // namespace protocol
} // namespace lunaviewer
