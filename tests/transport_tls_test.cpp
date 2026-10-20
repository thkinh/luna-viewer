#include "lunaviewer/transport.hpp"
#include "lunaviewer/protocol.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <thread>
#include <iostream>
#include <stdexcept>
#include <memory>
using namespace lunaviewer;
void check(bool v, const char *why) {
    if (!v)
        throw std::runtime_error(why);
}
struct Fixture {
    EVP_PKEY *key = nullptr;
    X509 *cert = nullptr;
    SSL_CTX *ctx = nullptr;
    std::string ca;
    Fixture(const std::string &path) : ca(path) {
        auto *gen = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        check(gen, "key context");
        check(EVP_PKEY_keygen_init(gen) == 1 && EVP_PKEY_CTX_set_rsa_keygen_bits(gen, 2048) == 1 &&
                  EVP_PKEY_keygen(gen, &key) == 1,
              "key generation");
        EVP_PKEY_CTX_free(gen);
        cert = X509_new();
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), -60);
        X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
        X509_set_pubkey(cert, key);
        auto *name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char *>("verify.example"), -1,
                                   -1, 0);
        X509_set_issuer_name(cert, name);
        auto *san = X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name,
                                        const_cast<char *>("DNS:verify.example"));
        X509_add_ext(cert, san, -1);
        X509_EXTENSION_free(san);
        check(X509_sign(cert, key, EVP_sha256()) > 0, "cert signing");
        FILE *f = fopen(ca.c_str(), "w");
        check(f, "CA output");
        PEM_write_X509(f, cert);
        fclose(f);
        ctx = SSL_CTX_new(TLS_server_method());
        check(ctx && SSL_CTX_use_certificate(ctx, cert) == 1 &&
                  SSL_CTX_use_PrivateKey(ctx, key) == 1,
              "server TLS config");
    }
    ~Fixture() {
        SSL_CTX_free(ctx);
        X509_free(cert);
        EVP_PKEY_free(key);
    }
};
void scenario(Fixture &fixture, bool wrong_name, bool stall, bool drain = false) {
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check(listener >= 0, "listen socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0 &&
              listen(listener, 1) == 0,
          "bind/listen");
    socklen_t size = sizeof(address);
    getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size);
    int port = ntohs(address.sin_port);
    std::string server_error;
    bool saw_sni = false, saw_host = false, saw_request = false, saw_drain = false;
    QEventLoop loop;
    std::thread server([&] {
        int fd = -1;
        SSL *ssl = nullptr;
        try {
            pollfd p{listener, POLLIN, 0};
            check(poll(&p, 1, 5000) > 0, "server accept timeout");
            fd = accept(listener, nullptr, nullptr);
            check(fd >= 0, "accept");
            timeval timeout{3, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            if (stall) {
                poll(nullptr, 0, 150);
            } else {
                ssl = SSL_new(fixture.ctx);
                SSL_set_fd(ssl, fd);
                int r = SSL_accept(ssl);
                if (!wrong_name) {
                    check(r == 1, "server handshake");
                    const char *sni = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
                    saw_sni = sni && std::string(sni) == "ultraviewer";
                    Bytes request;
                    std::size_t header_end = std::string::npos;
                    for (;;) {
                        unsigned char b[16384];
                        int n = SSL_read(ssl, b, sizeof(b));
                        check(n > 0, "read first request");
                        request.insert(request.end(), b, b + n);
                        auto text = protocol::text(request);
                        auto at = text.find("\r\n\r\n");
                        if (at != std::string::npos) {
                            header_end = at + 4;
                            std::uint32_t len = 0;
                            if (request.size() >= header_end + 4) {
                                for (int j = 0; j < 4; ++j)
                                    len |= std::uint32_t(request[header_end + j]) << (8 * j);
                                if (request.size() >= header_end + 4 + len)
                                    break;
                            }
                        }
                        check(request.size() < 2 * 1024 * 1024, "request limit");
                    }
                    saw_host = protocol::text(request).find("Host: relay.example\r\n") !=
                               std::string::npos;
                    protocol::FrameDecoder decoder;
                    auto frames = decoder.feed(Bytes(request.begin() + header_end, request.end()));
                    saw_request = frames.size() == 1 && frames[0] == Bytes(1024 * 1024, 0x55);
                    auto header = protocol::bytes(
                        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n");
                    auto reply = protocol::frame({1, 'p', 'o', 'n', 'g'});
                    header.push_back(reply[0]);
                    check(SSL_write(ssl, header.data(), int(header.size())) == int(header.size()),
                          "write split header");
                    for (std::size_t i = 1; i < reply.size(); ++i)
                        check(SSL_write(ssl, &reply[i], 1) == 1, "write fragmented reply");
                    if (drain) {
                        poll(nullptr, 0, 80);
                        protocol::FrameDecoder release_decoder;
                        while (!saw_drain) {
                            unsigned char buffer[16384];
                            int count = SSL_read(ssl, buffer, sizeof(buffer));
                            check(count > 0, "read drained disconnect");
                            for (auto &pending :
                                 release_decoder.feed(Bytes(buffer, buffer + count)))
                                saw_drain = pending == Bytes(4 * 1024 * 1024, 0x19);
                        }
                        QMetaObject::invokeMethod(&loop, &QEventLoop::quit, Qt::QueuedConnection);
                    }
                } else
                    check(r != 1, "wrong name unexpectedly accepted");
            }
        } catch (const std::exception &e) {
            server_error = e.what();
        }
        if (ssl)
            SSL_free(ssl);
        if (fd >= 0)
            ::close(fd);
        ::close(listener);
    });
    TlsTransport client;
    bool got_frame = false, got_error = false, timed_out = false;
    std::string failure;
    client.ready = [&] { client.send(Bytes(1024 * 1024, 0x55)); };
    client.frame = [&](Bytes b) {
        got_frame = b == Bytes({1, 'p', 'o', 'n', 'g'});
        if (drain) {
            client.send(Bytes(4 * 1024 * 1024, 0x19));
            client.finish(2500);
        } else {
            client.close();
            loop.quit();
        }
    };
    client.error = [&](std::string e) {
        got_error = true;
        failure = e;
        loop.quit();
    };
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        timed_out = true;
        client.close();
        loop.quit();
    });
    timer.start(6000);
    TlsConfig config;
    config.connect_address = "127.0.0.1";
    config.port = port;
    config.sni = "ultraviewer";
    config.verify_name = wrong_name ? "wrong.example" : "verify.example";
    config.http_host = "relay.example";
    config.ca_file = fixture.ca;
    config.deadline_ms = stall ? 40 : 3000;
    client.start(config);
    loop.exec();
    client.close();
    server.join();
    check(!timed_out, "client test timeout");
    if (!server_error.empty())
        throw std::runtime_error(server_error);
    if (wrong_name || stall) {
        check(got_error && !got_frame, "expected TLS rejection/deadline");
        if (wrong_name) {
            check(failure.find("ssl_error=") != std::string::npos &&
                      failure.find("hostname mismatch") != std::string::npos &&
                      failure.find("sni=ultraviewer") != std::string::npos &&
                      failure.find("verify=wrong.example") != std::string::npos,
                  "TLS diagnostics omit identity/reason");
        }
    } else {
        if (got_error)
            throw std::runtime_error(failure);
        check(got_frame && saw_sni && saw_host && saw_request && (!drain || saw_drain),
              "TLS route/SNI/Host/fragmentation mismatch");
    }
}
int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir temp;
        check(temp.isValid(), "temporary directory");
        Fixture fixture((temp.path() + "/ca.pem").toStdString());
        scenario(fixture, false, false);
        scenario(fixture, false, false, true);
        scenario(fixture, true, false);
        scenario(fixture, false, true);
        std::cout << "TLS loopback tests passed (identity/SNI/HTTP/partial I/O/deadline)\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
