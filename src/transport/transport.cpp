#include "lunaviewer/transport.hpp"
#include "lunaviewer/protocol.hpp"
#include "sectigo_intermediate.hpp"
#include <openssl/pem.h>
#include <QHostInfo>
#include <QHostAddress>
#include <QObject>
#include <QSocketNotifier>
#include <QTimer>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <algorithm>
#include <deque>
#include <stdexcept>
#include <cstring>
#include <sstream>
namespace lunaviewer {
struct TlsTransport::Impl : QObject {
    std::string connected_ip;
    int last_ssl_error = 0;
    std::size_t generation = 0;
    TlsTransport &owner;
    TlsConfig config;
    int fd = -1, dns = -1;
    SSL_CTX *ctx = nullptr;
    SSL *ssl = nullptr;
    QSocketNotifier *reader = nullptr, *writer = nullptr;
    QTimer deadline, finish_deadline;
    bool finishing = false;
    enum class Phase { Closed, Resolving, Connecting, Handshake, Open };
    Phase phase = Phase::Closed;
    std::deque<Bytes> outgoing;
    std::size_t offset = 0, queued = 0;
    bool header_done = false, pumping = false, write_needs_read = false, read_needs_write = false;
    Bytes header;
    protocol::FrameDecoder decoder;
    explicit Impl(TlsTransport &o) : owner(o) {
        QObject::connect(&deadline, &QTimer::timeout, this,
                         [this] { fail("Transport setup deadline exceeded"); });
        deadline.setSingleShot(true);
        finish_deadline.setSingleShot(true);
        QObject::connect(&finish_deadline, &QTimer::timeout, this, [this] { close(); });
    }
    ~Impl() {
        close();
    }
    void close() {
        ++generation;
        deadline.stop();
        finish_deadline.stop();
        finishing = false;
        if (dns >= 0) {
            QHostInfo::abortHostLookup(dns);
            dns = -1;
        }
        if (reader) {
            reader->setEnabled(false);
            reader->deleteLater();
            reader = nullptr;
        }
        if (writer) {
            writer->setEnabled(false);
            writer->deleteLater();
            writer = nullptr;
        }
        if (ssl) {
            SSL_free(ssl);
            ssl = nullptr;
        }
        if (ctx) {
            SSL_CTX_free(ctx);
            ctx = nullptr;
        }
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
        phase = Phase::Closed;
        outgoing.clear();
        offset = queued = 0;
        header.clear();
        header_done = false;
        write_needs_read = read_needs_write = false;
        decoder.reset();
    }
    std::string context() const {
        return " [connect=" + config.connect_address + ":" + std::to_string(config.port) +
               ", ip=" + connected_ip + ", sni=" + config.sni + ", verify=" + config.verify_name +
               ", trust=" + (config.ca_file.empty() ? "system" : "custom") + "]";
    }
    std::string ssl_failure(const char *operation, int saved_errno) {
        std::ostringstream out;
        out << operation << " failed: ssl_error=" << last_ssl_error;
        if (ssl) {
            auto verify = SSL_get_verify_result(ssl);
            out << ", verify=" << verify << " (" << X509_verify_cert_error_string(verify) << ")";
        }
        if (saved_errno)
            out << ", errno=" << saved_errno << " (" << std::strerror(saved_errno) << ")";
        for (int i = 0; i < 8; ++i) {
            auto code = ERR_get_error();
            if (!code)
                break;
            const char *reason = ERR_reason_error_string(code);
            out << ", openssl=" << code << " (" << (reason ? reason : "unknown") << ")";
        }
        return out.str();
    }
    void fail(const std::string &s) {
        auto message = s + context();
        close();
        if (owner.error)
            owner.error(message);
    }
    void enqueue(Bytes b) {
        if (b.size() > config.max_queued_bytes || queued > config.max_queued_bytes - b.size())
            throw std::runtime_error("Transport send queue limit exceeded");
        queued += b.size();
        outgoing.push_back(std::move(b));
    }
    void start(const TlsConfig &c) {
        close();
        config = c;
        try {
            protocol::http_preamble(c.http_host);
            if (c.connect_address.empty() || c.sni.empty() || c.verify_name.empty() ||
                c.port == 0 || c.deadline_ms <= 0)
                throw std::runtime_error("Incomplete TLS configuration");
            ctx = SSL_CTX_new(TLS_client_method());
            if (!ctx)
                throw std::runtime_error("TLS context allocation failed");
            SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
            if (SSL_CTX_set_default_verify_paths(ctx) != 1)
                throw std::runtime_error("System trust loading failed");
            if (!c.ca_file.empty() &&
                SSL_CTX_load_verify_locations(ctx, c.ca_file.c_str(), nullptr) != 1)
                throw std::runtime_error("Additional CA file loading failed");
            // Some relays omit their Sectigo R36 issuer. Supply that authenticated
            // certificate for chain construction while still requiring a trusted root.
            BIO *issuer_bio = BIO_new_mem_buf(detail::sectigo_r36_pem, -1);
            if (!issuer_bio)
                throw std::runtime_error("Intermediate allocation failed");
            X509 *issuer = PEM_read_bio_X509(issuer_bio, nullptr, nullptr, nullptr);
            BIO_free(issuer_bio);
            if (!issuer)
                throw std::runtime_error("Intermediate parsing failed");
            const int installed = X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), issuer);
            X509_free(issuer);
            if (installed != 1)
                throw std::runtime_error("Intermediate installation failed");
            X509_VERIFY_PARAM_clear_flags(SSL_CTX_get0_param(ctx), X509_V_FLAG_PARTIAL_CHAIN);
            phase = Phase::Resolving;
            deadline.start(c.deadline_ms);
            auto current_generation = generation;
            dns = QHostInfo::lookupHost(QString::fromStdString(c.connect_address), this,
                                        [this, current_generation](const QHostInfo &info) {
                                            if (current_generation != generation)
                                                return;
                                            dns = -1;
                                            if (phase != Phase::Resolving)
                                                return;
                                            if (info.error() != QHostInfo::NoError) {
                                                fail("DNS lookup failed");
                                                return;
                                            }
                                            for (const auto &a : info.addresses())
                                                if (a.protocol() == QAbstractSocket::IPv4Protocol) {
                                                    connect_ip(a.toString().toStdString());
                                                    return;
                                                }
                                            fail("No IPv4 transport address");
                                        });
        } catch (const std::exception &e) {
            fail(e.what());
        }
    }
    void connect_ip(const std::string &address) {
        connected_ip = address;
        fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            fail("Socket creation failed");
            return;
        }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(config.port);
        inet_pton(AF_INET, address.c_str(), &a.sin_addr);
        int r = ::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a));
        if (r < 0 && errno != EINPROGRESS) {
            fail("TCP connect failed");
            return;
        }
        phase = Phase::Connecting;
        reader = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        writer = new QSocketNotifier(fd, QSocketNotifier::Write, this);
        auto connection_generation = generation;
        QObject::connect(reader, &QSocketNotifier::activated, this,
                         [this, connection_generation](QSocketDescriptor, QSocketNotifier::Type) {
                             if (connection_generation == generation)
                                 pump();
                         });
        QObject::connect(writer, &QSocketNotifier::activated, this,
                         [this, connection_generation](QSocketDescriptor, QSocketNotifier::Type) {
                             if (connection_generation == generation)
                                 pump();
                         });
        reader->setEnabled(false);
        writer->setEnabled(true);
        if (r == 0)
            pump();
    }
    void interests(bool rd, bool wr) {
        if (reader)
            reader->setEnabled(rd);
        if (writer)
            writer->setEnabled(wr);
    }
    bool want(int r, bool &needs_read, bool &needs_write) {
        int err = SSL_get_error(ssl, r);
        last_ssl_error = err;
        needs_read = err == SSL_ERROR_WANT_READ;
        needs_write = err == SSL_ERROR_WANT_WRITE;
        if (needs_read || needs_write)
            return true;
        return false;
    }
    void pump() {
        if (pumping || phase == Phase::Closed || phase == Phase::Resolving)
            return;
        pumping = true;
        try {
            pump_inner();
        } catch (const std::exception &e) {
            fail(e.what());
        }
        pumping = false;
    }
    void pump_inner() {
        if (phase == Phase::Connecting) {
            int e = 0;
            socklen_t len = sizeof(e);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &len) != 0 || e != 0)
                throw std::runtime_error("TCP connection failed");
            ssl = SSL_new(ctx);
            if (!ssl)
                throw std::runtime_error("TLS allocation failed");
            if (SSL_set_fd(ssl, fd) != 1 ||
                SSL_set_tlsext_host_name(ssl, config.sni.c_str()) != 1 ||
                SSL_set1_host(ssl, config.verify_name.c_str()) != 1)
                throw std::runtime_error("TLS identity configuration failed");
            SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE);
            phase = Phase::Handshake;
        }
        if (phase == Phase::Handshake) {
            ERR_clear_error();
            errno = 0;
            int r = SSL_connect(ssl);
            int saved_errno = errno;
            if (r != 1) {
                bool rd = false, wr = false;
                if (want(r, rd, wr)) {
                    interests(rd, wr);
                    return;
                }
                throw std::runtime_error(ssl_failure("TLS handshake", saved_errno));
            }
            if (SSL_get_verify_result(ssl) != X509_V_OK)
                throw std::runtime_error("TLS peer verification failed");
            phase = Phase::Open;
            enqueue(protocol::bytes(protocol::http_preamble(config.http_host)));
            deadline.stop();
            if (owner.ready)
                owner.ready();
            if (phase != Phase::Open)
                return;
        }
        int budget = 128;
        while (!outgoing.empty() && budget-- > 0) {
            auto &b = outgoing.front();
            if (b.empty()) {
                outgoing.pop_front();
                offset = 0;
                continue;
            }
            ERR_clear_error();
            errno = 0;
            int n = SSL_write(ssl, b.data() + offset, int(b.size() - offset));
            int saved_errno = errno;
            if (n > 0) {
                offset += n;
                queued -= n;
                write_needs_read = false;
                if (offset == b.size()) {
                    outgoing.pop_front();
                    offset = 0;
                }
            } else {
                bool rd = false, wr = false;
                if (!want(n, rd, wr))
                    throw std::runtime_error(ssl_failure("TLS write", saved_errno));
                write_needs_read = rd;
                break;
            }
        }
        if (finishing && outgoing.empty()) {
            SSL_shutdown(ssl);
            close();
            return;
        }
        budget = 128;
        while (phase == Phase::Open && budget-- > 0) {
            unsigned char data[16384];
            ERR_clear_error();
            errno = 0;
            int n = SSL_read(ssl, data, sizeof(data));
            int saved_errno = errno;
            if (n > 0) {
                read_needs_write = false;
                receive(Bytes(data, data + n));
            } else {
                bool rd = false, wr = false;
                if (want(n, rd, wr)) {
                    read_needs_write = wr;
                    break;
                }
                int err = last_ssl_error;
                if (err == SSL_ERROR_ZERO_RETURN) {
                    decoder.finish();
                    if (!header_done)
                        throw std::runtime_error("EOF before HTTP response");
                    throw std::runtime_error("Peer closed transport");
                }
                throw std::runtime_error(ssl_failure("TLS read", saved_errno));
            }
        }
        if (phase == Phase::Open) {
            interests(true, read_needs_write || (!outgoing.empty() && !write_needs_read));
            if (SSL_pending(ssl) > 0) {
                auto connection_generation = generation;
                QTimer::singleShot(0, this, [this, connection_generation] {
                    if (connection_generation == generation)
                        pump();
                });
            }
        }
    }
    void receive(Bytes b) {
        if (!header_done) {
            header.insert(header.end(), b.begin(), b.end());
            const Bytes marker{13, 10, 13, 10};
            auto it = std::search(header.begin(), header.end(), marker.begin(), marker.end());
            if (it == header.end()) {
                if (header.size() > 65536)
                    throw std::runtime_error("HTTP response header exceeds limit");
                return;
            }
            auto end = std::size_t(it - header.begin()) + 4;
            if (end > 65536)
                throw std::runtime_error("HTTP response header exceeds limit");
            std::string first(header.begin(),
                              std::find(header.begin(), header.end(), std::uint8_t(13)));
            if (first.compare(0, 9, "HTTP/1.1 ") != 0 && first.compare(0, 9, "HTTP/1.0 ") != 0)
                throw std::runtime_error("Malformed HTTP response");
            if (first.size() < 12 || first.substr(9, 3) != "200")
                throw std::runtime_error("HTTP relay rejected request");
            b = Bytes(header.begin() + end, header.end());
            header.clear();
            header_done = true;
        }
        auto frames = decoder.feed(b);
        for (auto &f : frames) {
            if (owner.frame)
                owner.frame(std::move(f));
            if (phase != Phase::Open)
                return;
        }
    }
};
TlsTransport::TlsTransport() : impl_(new Impl(*this)) {
}
TlsTransport::~TlsTransport() = default;
void TlsTransport::start(const TlsConfig &c) {
    impl_->start(c);
}
void TlsTransport::close() {
    impl_->close();
}
void TlsTransport::finish(int ms) {
    if (impl_->phase != Impl::Phase::Open || ms <= 0) {
        impl_->close();
        return;
    }
    impl_->finishing = true;
    impl_->finish_deadline.start(ms);
    if (impl_->outgoing.empty()) {
        SSL_shutdown(impl_->ssl);
        impl_->close();
    } else
        impl_->pump();
}
void TlsTransport::send(const Bytes &b) {
    try {
        if (impl_->phase != Impl::Phase::Open || impl_->finishing)
            throw std::runtime_error("Send before TLS ready");
        impl_->enqueue(protocol::frame(b));
        impl_->pump();
    } catch (const std::exception &e) {
        impl_->fail(e.what());
    }
}
} // namespace lunaviewer
