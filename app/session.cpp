#include "session.hpp"
#include "peer_version.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <sstream>
#include <stdexcept>
namespace lunaviewer {
namespace {
bool same_route(const protocol::Route &a, const protocol::Route &b) {
    return a.address == b.address && a.port == b.port;
}
Bytes utf16le(const std::string &text) {
    Bytes result;
    result.reserve(text.size() * 2);
    for (unsigned char c : text) {
        result.push_back(c);
        result.push_back(0);
    }
    return result;
}
void send_visual_preferences(const SessionConfig &config,
                             const std::function<void(const Bytes &, bool)> &send) {
    send(protocol::bytes("32|||2"), false);
    send(protocol::bytes("31|||" + std::to_string(config.display_quality)), false);
    send(utf16le(std::string("23|||") + (config.render_wallpaper ? "-1" : "0")), true);
}
} // namespace
Session::Session(SessionConfig config, SessionCallbacks callbacks)
    : config_(std::move(config)), callbacks_(std::move(callbacks)) {
}
bool Session::active() const {
    return phase_ == Phase::Active;
}
void Session::status(const std::string &message) {
    if (callbacks_.status)
        callbacks_.status(message);
}
void Session::permission_diagnostic(const std::string &source) {
    if (callbacks_.diagnostic)
        callbacks_.diagnostic("permission source=" + source +
                              "; active=" + (active() ? "yes" : "no") + "; remote=" +
                              (!active() && !permission_reported_ ? "unavailable"
                               : allowed_                         ? "available"
                                                                  : "denied") +
                              "; local_control=" + (enabled_ ? "enabled" : "disabled"));
}
void Session::start(Credentials credentials, std::int64_t now_ms) {
    if (phase_ != Phase::Idle)
        disconnect();
    now_ = now_ms;
    try {
        protocol::registration_text(config_.registration);
        if (credentials.target_id.empty() || credentials.password.empty())
            throw std::runtime_error("Partner ID and password are required");
        if (!std::all_of(credentials.target_id.begin(), credentials.target_id.end(),
                         [](unsigned char c) { return c >= '0' && c <= '9'; }))
            throw std::runtime_error("Partner ID must contain only digits");
        blocked_keyboard_ = 0;
        gated_input_ = 0;
        forwarded_input_ = 0;
        credentials_ = std::move(credentials);
        correlation_ = std::stoi(protocol::random_digits(8));
        connection_hash_ = protocol::random_token(16);
        renderer_ = std::make_unique<ScreenRenderer>();
        if (config_.require_dictionary && config_.dictionary_path.empty())
            throw std::runtime_error(
                "Provide the existing local screen dictionary with --dictionary");
        renderer_->load_dictionary(config_.dictionary_path);
        phase_ = Phase::Registering;
        deadline_ = now_ + 20000;
        heartbeat_at_ = 0;
        heartbeat_count_ = 0;
        allowed_ = false;
        enabled_ = false;
        permission_reported_ = false;
        if (callbacks_.permission)
            callbacks_.permission(false);
        permission_diagnostic("connection-start");
        status("Registering local identity");
        callbacks_.open(Channel::Command, config_.command);
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
void Session::ready(Channel channel) {
    try {
        if (channel == Channel::Command && phase_ == Phase::Registering)
            callbacks_.send(channel, protocol::command_envelope(
                                         protocol::registration_text(config_.registration)));
        else if (channel == Channel::Peer && phase_ == Phase::ConnectingPeer)
            login();
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
void Session::receive(Channel channel, const Bytes &body) {
    if (phase_ == Phase::Closed || phase_ == Phase::Idle)
        return;
    try {
        if (channel == Channel::Command)
            command_frame(body);
        else
            peer_frame(body);
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
void Session::command_frame(const Bytes &body) {
    auto envelope = protocol::parse_envelope(body);
    if (!envelope.system)
        return;
    const auto reply = protocol::text(envelope.payload);
    if (phase_ == Phase::Registering && reply.rfind("3:", 0) == 0) {
        registration_ = protocol::parse_registration(envelope.payload);
        protocol::LookupRequest request;
        request.target_id = credentials_.target_id;
        request.password = credentials_.password;
        request.version = config_.registration[7];
        request.license1 = config_.registration[25];
        request.license2 = config_.registration[26];
        request.local_id = registration_.assigned_id;
        request.local_password = config_.registration[2];
        request.correlation = correlation_;
        request.referral = config_.registration[20];
        request.utc_offset = config_.utc_offset_minutes;
        phase_ = Phase::Lookup;
        deadline_ = now_ + 20000;
        status("Looking up partner");
        callbacks_.send(Channel::Command,
                        protocol::command_envelope(protocol::lookup_text(request)));
    } else if (phase_ == Phase::Lookup && reply.rfind("8:", 0) == 0) {
        auto result = protocol::parse_lookup(envelope.payload);
        if (result.correlation != correlation_)
            return;
        if (result.status == 2)
            throw std::runtime_error("Partner password was rejected");
        if (result.status != 1)
            throw std::runtime_error("Partner lookup failed: status " +
                                     std::to_string(result.status));
        lookup_ = std::move(result);
        TlsConfig peer = config_.command;
        peer.connect_address = lookup_.relay_ip;
        peer.verify_name =
            lookup_.relay_hostname.empty() ? config_.command.verify_name : lookup_.relay_hostname;
        peer.http_host = peer.verify_name;
        phase_ = Phase::ConnectingPeer;
        deadline_ = now_ + 20000;
        status("Connecting to partner relay");
        callbacks_.open(Channel::Peer, peer);
    }
}
void Session::send_peer(const Bytes &payload, bool format) {
    callbacks_.send(Channel::Peer, protocol::forward(lookup_.route, payload, format));
}
void Session::login(int mode, bool clear) {
    protocol::LoginRequest request;
    request.password = credentials_.password;
    request.nonce = protocol::random_digits(8);
    request.local_id = registration_.assigned_id;
    request.display_name = config_.display_name;
    request.connection_hash = connection_hash_;
    request.secret_token = lookup_.secret_token;
    request.version = config_.registration[7];
    request.width = config_.local_width;
    request.height = config_.local_height;
    request.control_mode = mode;
    request.clear_control_mode = clear;
    request.display_quality = config_.display_quality;
    request.remote_wallpaper_disabled = config_.render_wallpaper ? 0 : -1;
    phase_ = Phase::Login;
    deadline_ = now_ + 20000;
    status("Waiting for partner initialization");
    send_peer(protocol::bytes(protocol::login_text(request)));
}
void Session::peer_frame(const Bytes &body) {
    auto envelope = protocol::parse_envelope(body);
    if (envelope.system)
        return;
    // Only the bounded hash-bound command26 can migrate an established route.
    Parts parts;
    if (!same_route(envelope.route, lookup_.route)) {
        try {
            parts = protocol::parse_multipart(envelope.payload);
        } catch (const std::exception &) {
            return;
        }
        if (parts.size() < 2 || parts[0].empty() || parts[0][0] != 26 ||
            parts[1] != protocol::bytes(connection_hash_))
            return;
    } else
        parts = protocol::parse_multipart(envelope.payload);
    if (parts.empty() || parts[0].empty())
        throw std::runtime_error("Empty peer command");
    const auto command = parts[0][0];
    if (command == 0 && (phase_ == Phase::Login || phase_ == Phase::Active)) {
        if (parts.size() < 11)
            throw std::runtime_error("Incomplete modern initialization");
        const auto reported = protocol::text(parts[6]);
        const auto peer_version = PeerVersion::parse(reported);
        const auto preview = PeerVersion::describe(reported);
        keyboard_supported_ = peer_version.at_least({6, 2, 13});
        if (callbacks_.diagnostic)
            callbacks_.diagnostic("peer_init version=\"" + preview +
                                  "\"; version_bytes=" + std::to_string(parts[6].size()) +
                                  "; parser=" + (peer_version.valid ? "valid" : "invalid") +
                                  "; keyboard_mode=" +
                                  (keyboard_supported_  ? "modern-binary"
                                   : peer_version.valid ? "blocked-legacy-unimplemented"
                                                        : "blocked-invalid-version"));
        if (!peer_version.at_least({6, 0, 0}))
            throw std::runtime_error(
                "Unsupported or malformed peer screen version; see session log");
        version_ = reported;
        renderer_->configure(parts);

        // Reference initializes remoteViewOnly=false. Accepted init permits the
        // normal control capability when optional command49 was omitted.
        // A command49 seen before init is authoritative and is never reset here.
        if (!permission_reported_)
            allowed_ = true;
        phase_ = Phase::Active;
        deadline_ = 0;
        permission_diagnostic(permission_reported_ ? "host-command49-preserved"
                                                   : "accepted-init-default");
        if (!heartbeat_at_)
            heartbeat_at_ = now_ + 10000;
        performance_window_start_ = std::chrono::steady_clock::now();
        performance_frames_ = performance_payload_bytes_ = 0;
        performance_decode_ms_ = 0.0;
        if (callbacks_.accepted)
            callbacks_.accepted(true);
        if (callbacks_.permission)
            callbacks_.permission(allowed_);
        status("Connected; waiting for screen");
        send_peer(protocol::bytes("17"));
        if (visual_preferences_dirty_) {
            send_visual_preferences(config_, [this](const Bytes &payload, bool format) {
                send_peer(payload, format);
            });
            visual_preferences_dirty_ = false;
        }
    } else if (command == 1 && active()) {
        const auto decode_started = std::chrono::steady_clock::now();
        auto frame = renderer_->apply(parts);
        const auto decode_ms = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - decode_started)
                                   .count();
        if (callbacks_.frame)
            callbacks_.frame(std::move(frame));
        const auto now = std::chrono::steady_clock::now();
        if (performance_window_start_.time_since_epoch().count() == 0)
            performance_window_start_ = now;
        ++performance_frames_;
        performance_decode_ms_ += decode_ms;
        if (parts.size() > 2)
            performance_payload_bytes_ += parts[2].size();
        if (parts.size() > 7)
            performance_payload_bytes_ += parts[7].size();
        send_peer(protocol::bytes("18"));
        status("Connected");
    } else if (command == 49) {
        if (parts.size() < 2 || parts[1].empty())
            throw std::runtime_error("Invalid input permission");
        const bool allowed = parts[1][0] == 0;
        if (!allowed)
            release_inputs();
        allowed_ = allowed;
        permission_reported_ = true;
        permission_diagnostic("host-command49-byte-" + std::to_string(parts[1][0]));
        if (callbacks_.permission)
            callbacks_.permission(active() && allowed_);
    } else if (command == 48 && active())
        renderer_->set_tile_size(parts);
    else if (command == 23 && active()) {
        release_inputs();
        login(-1, false);
    } else if (command == 26 && parts.size() > 1 && parts[1] == protocol::bytes(connection_hash_)) {
        release_inputs();
        lookup_.route = envelope.route;
        renderer_ = std::make_unique<ScreenRenderer>();
        renderer_->load_dictionary(config_.dictionary_path);
        if (active())
            login(-1, true);
    } else if (command == 27 || command == 41 || command == 54 || command == 39) {
        fail(command == 39 ? "Partner blocked the connection" : "Partner ended the session");
    }
}
void Session::set_visual_preferences(int display_quality, bool render_wallpaper) {
    if (display_quality != 30 && display_quality != 45 && display_quality != 70) {
        status("Unsupported display quality preset");
        return;
    }
    try {
        const bool quality_changed = config_.display_quality != display_quality;
        const bool wallpaper_changed = config_.render_wallpaper != render_wallpaper;
        config_.display_quality = display_quality;
        config_.render_wallpaper = render_wallpaper;
        if (!quality_changed && !wallpaper_changed)
            return;
        if (active()) {
            if (quality_changed) {
                send_peer(protocol::bytes("32|||2"));
                send_peer(protocol::bytes("31|||" + std::to_string(display_quality)));
            }
            if (wallpaper_changed)
                send_peer(utf16le(std::string("23|||") + (render_wallpaper ? "-1" : "0")),
                          true);
            visual_preferences_dirty_ = false;
        } else
            visual_preferences_dirty_ = true;
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
std::uint16_t Session::mouse_flags() const {
    std::uint16_t flags = 0;
    if (held_buttons_.count(1))
        flags |= 1;
    if (held_buttons_.count(2))
        flags |= 2;
    if (held_buttons_.count(3))
        flags |= 16;
    for (const auto &entry : held_keys_) {
        auto key = entry.second.virtual_key;
        if (key == 0x10 || key == 0xa0 || key == 0xa1)
            flags |= 4;
        if (key == 0x11 || key == 0xa2 || key == 0xa3)
            flags |= 8;
    }
    return flags;
}
void Session::input(const InputEvent &event) {
    try {
        if (event.kind == InputEvent::Kind::ReleaseAll) {
            release_inputs();
            return;
        }
        if (!active() || !allowed_ || !enabled_) {
            ++gated_input_;
            return;
        }
        if (event.kind == InputEvent::Kind::Key) {
            if (!keyboard_supported_) {
                ++blocked_keyboard_;
                if (blocked_keyboard_ == 1 && callbacks_.diagnostic)
                    callbacks_.diagnostic(
                        "keyboard_gate peer_version=" + PeerVersion::describe(version_) +
                        "; reason=binary-requires-6.2.13; legacy-typing=unimplemented");
                status("Keyboard unavailable: peer reports " + version_ +
                       "; binary keyboard needs 6.2.13 or newer");
                return;
            }
            const auto id =
                event.virtual_key | (event.scan_code << 8) | (event.extended ? 0x1000000u : 0u);
            if (event.pressed)
                held_keys_[id] = event;
            else
                held_keys_.erase(id);
        } else if (event.kind == InputEvent::Kind::Button) {
            if (event.pressed)
                held_buttons_[event.button] = event;
            else
                held_buttons_.erase(event.button);
        }
        if (event.kind != InputEvent::Kind::Key) {
            pointer_x_ = event.x;
            pointer_y_ = event.y;
        }
        send_peer(protocol::input_payload(event, mouse_flags()));
        ++forwarded_input_;
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
void Session::release_inputs() {
    // Releases bypass permission gates only for inputs sent by this session.
    auto keys = std::move(held_keys_);
    held_keys_.clear();
    auto buttons = std::move(held_buttons_);
    held_buttons_.clear();
    if (!active())
        return;
    for (auto &entry : keys) {
        entry.second.pressed = false;
        send_peer(protocol::input_payload(entry.second, mouse_flags()));
    }
    for (auto &entry : buttons) {
        entry.second.x = pointer_x_;
        entry.second.y = pointer_y_;
        entry.second.pressed = false;
        send_peer(protocol::input_payload(entry.second, mouse_flags()));
    }
}
void Session::control_enabled(bool enabled) {
    try {
        if (!enabled)
            release_inputs();
        enabled_ = enabled;
        permission_diagnostic("local-toggle");
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
void Session::disconnect() {
    if ((blocked_keyboard_ || gated_input_ || forwarded_input_) && callbacks_.diagnostic)
        callbacks_.diagnostic("input_summary forwarded_events=" + std::to_string(forwarded_input_) +
                              "; gated_events=" + std::to_string(gated_input_) +
                              "; blocked_keyboard_events=" + std::to_string(blocked_keyboard_));
    blocked_keyboard_ = 0;
    gated_input_ = 0;
    forwarded_input_ = 0;
    try {
        release_inputs();
        if (active())
            send_peer(protocol::bytes("19"));
    } catch (...) {
    }
    phase_ = Phase::Closed;
    deadline_ = 0;
    heartbeat_at_ = 0;
    enabled_ = false;
    allowed_ = false;
    credentials_.password.clear();
    if (callbacks_.close)
        callbacks_.close();
    if (callbacks_.permission)
        callbacks_.permission(false);
    if (callbacks_.accepted)
        callbacks_.accepted(false);
    renderer_.reset();
}
void Session::fail(const std::string &message) {
    disconnect();
    status(message);
}
void Session::transport_error(const std::string &message) {
    if (phase_ != Phase::Closed)
        fail(message);
}
void Session::tick(std::int64_t now_ms) {
    now_ = now_ms;
    try {
        if (active() && callbacks_.performance &&
            performance_window_start_.time_since_epoch().count() != 0) {
            const auto now = std::chrono::steady_clock::now();
            const auto seconds = std::chrono::duration<double>(now - performance_window_start_).count();
            if (seconds >= 1.0) {
                PerformanceStats stats;
                const auto geometry = renderer_->geometry();
                stats.width = geometry.width;
                stats.height = geometry.height;
                stats.decoded_fps = performance_frames_ / seconds;
                stats.payload_kib_per_second = performance_payload_bytes_ / 1024.0 / seconds;
                stats.average_decode_ms = performance_frames_ ? performance_decode_ms_ / performance_frames_ : 0.0;
                stats.frames_in_window = performance_frames_;
                callbacks_.performance(stats);
                performance_window_start_ = now;
                performance_frames_ = performance_payload_bytes_ = 0;
                performance_decode_ms_ = 0.0;
            }
        }
        if (deadline_ && now_ >= deadline_) {
            fail("Connection stage timed out");
            return;
        }
        if (active() && heartbeat_at_ && now_ >= heartbeat_at_) {
            heartbeat_at_ = now_ + 10000;
            callbacks_.send(Channel::Peer, protocol::system(protocol::bytes("ping")));
            send_peer(protocol::bytes("20"));
            if (++heartbeat_count_ % 6 == 0)
                callbacks_.send(Channel::Command, protocol::command_envelope("2"));
        }
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
} // namespace lunaviewer
