#include "lunaviewer/protocol.hpp"
#include <iostream>
#include <stdexcept>
#include <functional>
using namespace lunaviewer;
using namespace lunaviewer::protocol;
void check(bool b) {
    if (!b)
        throw std::runtime_error("transport assertion failed");
}
void rejects(const std::function<void()> &f) {
    bool caught = false;
    try {
        f();
    } catch (const std::exception &) {
        caught = true;
    }
    check(caught);
}
Bytes hex(const std::string &s) {
    Bytes out;
    for (std::size_t i = 0; i < s.size(); i += 2)
        out.push_back(std::stoul(s.substr(i, 2), nullptr, 16));
    return out;
}
int main() {
    try {
        const auto route = Route{"192.0.2.10", 12345};
        const auto encoded = forward(route, {0xaa, 0xbb});
        check(encoded == hex("00000a0200c03930aabb"));
        check(frame(encoded) == hex("0a00000000000a0200c03930aabb"));
        auto e = parse_envelope(encoded);
        check(e.route.address == route.address && e.route.port == route.port &&
              e.payload == Bytes({0xaa, 0xbb}));
        check(parse_envelope({1, 1, 3}).payload == Bytes({1, 3}));
        auto stream = frame(encoded);
        auto empty = frame({});
        stream.insert(stream.end(), empty.begin(), empty.end());
        for (std::size_t split = 0; split <= stream.size(); ++split) {
            FrameDecoder d;
            auto a = d.feed(Bytes(stream.begin(), stream.begin() + split));
            auto b = d.feed(Bytes(stream.begin() + split, stream.end()));
            a.insert(a.end(), b.begin(), b.end());
            check(a.size() == 2 && a[0] == encoded && a[1].empty());
            d.finish();
        }
        FrameDecoder bounded(10);
        auto two = frame(Bytes(10, 1));
        auto next = frame(Bytes(10, 2));
        two.insert(two.end(), next.begin(), next.end());
        check(bounded.feed(two).size() == 2);
        FrameDecoder bytewise;
        std::vector<Bytes> got;
        for (auto c : stream) {
            auto a = bytewise.feed({c});
            got.insert(got.end(), a.begin(), a.end());
        }
        check(got.size() == 2);
        bytewise.finish();
        rejects([] {
            FrameDecoder d;
            d.feed({0xff, 0xff, 0xff, 0xff});
        });
        rejects([] {
            FrameDecoder d;
            d.feed({4, 0, 0, 0, 1});
            d.finish();
        });
        Parts ps{{0, 1}, {}, {255}};
        check(parse_multipart(multipart(ps)) == ps);
        rejects([] { parse_multipart({1, 0, 0}); });
        rejects([] { parse_multipart({5, 0, 0, 0, 1}); });
        rejects([] { parse_multipart({0, 0, 0, 0}, 0); });
        check(command_encrypt(bytes("8:fixture")) == hex("c842551785856d0b8f866afed8dcd8b1"));
        check(command_decrypt(hex("c842551785856d0b8f866afed8dcd8b1")) == bytes("8:fixture"));
        check(command_envelope("2") == hex("01002d932971dd9dcb2d"));
        rejects([] { command_decrypt({1, 2, 3}); });
        check(md5_hex("password") == "5f4dcc3b5aa765d61d8327deb882cf99");
        std::vector<std::string> fields(28);
        fields[0] = "3";
        fields[1] = "truthful-linux-device";
        fields[2] = "123";
        fields[4] = "127.0.0.1";
        fields[9] = "disk:identity";
        auto registration = registration_text(fields);
        check(registration.find("disk-identity") != std::string::npos);
        rejects([] { registration_text({"3"}); });
        fields[1] = "bad:field";
        rejects([&] { registration_text(fields); });
        auto r = parse_registration(
            bytes("3:12345:123456:192.0.2.3::0:ping:1:20/10/2026:hostname:opaque:"));
        check(r.assigned_id == "123456" && r.public_port == 12345 && r.fields.back().empty());
        rejects([] { parse_registration(bytes("3:0:0:bad")); });
        LookupRequest l;
        l.target_id = "123";
        l.password = "password";
        l.local_id = "789";
        l.local_password = "local";
        l.correlation = 42;
        auto lookup = lookup_text(l);
        check(lookup.find(md5_hex(std::string(32, ' ') + "password")) != std::string::npos);
        auto lr = parse_lookup(bytes("8:1:10.0.0.1::192.0.2.10:22:23:24:name:12345::127.0.0.1:0:-1:"
                                     "relay.example::::::secret:42"));
        check(lr.route.port == 12345 && lr.correlation == 42 && lr.secret_token == "secret");
        rejects([] { parse_lookup(bytes("8:1:42")); });
        LoginRequest login;
        login.nonce = "12345678";
        login.password = "password";
        login.display_name = "Li:nux!";
        check(login_text(login).find(":Linux:") != std::string::npos);
        login.nonce = "123";
        rejects([&] { login_text(login); });
        check(keyboard(1, 65, 0x1e, 65, true) == hex("1e7c7c7c01411e00410001"));
        InputEvent input;
        input.kind = InputEvent::Kind::Move;
        input.x = -20;
        input.y = 40;
        check(text(input_payload(input)) == "8|||512|||0|||-20|||40");
        input.kind = InputEvent::Kind::Wheel;
        input.wheel = -120;
        check(text(input_payload(input)) == "8|||522|||-7864320|||2686956");
        check(http_preamble("relay.example")
                  .find("Content-length: 8589934590\r\nHost: relay.example\r\n\r\n") !=
              std::string::npos);
        rejects([] { http_preamble("evil\r\nHost: injected"); });
        check(base64("Linux") == "TGludXg=");
        check(random_digits(8).size() == 8 && random_token(16).size() == 16);
        InputEvent extended;
        extended.kind = InputEvent::Kind::Key;
        extended.pressed = true;
        extended.extended = true;
        extended.virtual_key = 0x25;
        extended.scan_code = 0x4b;
        check(input_payload(extended) == hex("1e7c7c7c01254be0000000"));
        extended.virtual_key = 0xa3;
        extended.scan_code = 0x1d;
        check(input_payload(extended) == hex("1e7c7c7c01a31de0000000"));
        extended.virtual_key = 0x0d;
        extended.scan_code = 0x1c;
        extended.character = 13;
        check(input_payload(extended) == hex("1e7c7c7c010d1ce00d0000"));
        extended.scan_code = 0xe01c;
        check(input_payload(extended) == hex("1e7c7c7c010d1ce00d0000"));
        extended.extended = false;
        check(input_payload(extended) == hex("1e7c7c7c010d1ce00d0000"));
        std::cout << "transport codec tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
