#pragma once
#include "lunaviewer/protocol.hpp"
#include "lunaviewer/screen.hpp"
#include "lunaviewer/transport.hpp"
#include <chrono>
#include <functional>
#include <map>
namespace lunaviewer {
enum class Channel { Command, Peer };
struct SessionConfig {
    TlsConfig command;
    std::vector<std::string> registration;
    std::string dictionary_path;
    std::string display_name = "LunaViewer";
    int display_quality = 45;
    bool render_wallpaper = true;
    bool require_dictionary = true; // Disable only for synthetic offline fixture sessions.
    int local_width = 1280, local_height = 720, utc_offset_minutes = 0;
};
struct SessionCallbacks {
    std::function<void(Channel, const TlsConfig &)> open;
    std::function<void(Channel, const Bytes &)> send;
    std::function<void()> close;
    std::function<void(const std::string &)> status, diagnostic;
    std::function<void(bool)> permission, accepted;
    std::function<void(FrameSnapshot)> frame;
    std::function<void(const PerformanceStats &)> performance;
};
// All entrypoints execute on the same serialized executor. No Qt dependency.
class Session {
  public:
    Session(SessionConfig config, SessionCallbacks callbacks);
    void start(Credentials credentials, std::int64_t now_ms);
    void ready(Channel channel);
    void receive(Channel channel, const Bytes &body);
    void tick(std::int64_t now_ms);
    void control_enabled(bool enabled);
    void input(const InputEvent &event);
    void set_visual_preferences(int display_quality, bool render_wallpaper);
    void disconnect();
    void transport_error(const std::string &message);
    bool active() const;

  private:
    enum class Phase { Idle, Registering, Lookup, ConnectingPeer, Login, Active, Closed };
    SessionConfig config_;
    SessionCallbacks callbacks_;
    Phase phase_ = Phase::Idle;
    Credentials credentials_;
    protocol::RegistrationReply registration_;
    protocol::LookupReply lookup_;
    std::unique_ptr<ScreenRenderer> renderer_;
    std::string connection_hash_, version_;
    std::uint64_t blocked_keyboard_ = 0, gated_input_ = 0, forwarded_input_ = 0;
    int correlation_ = 0, heartbeat_count_ = 0, pointer_x_ = 0, pointer_y_ = 0;
    bool allowed_ = false, enabled_ = false, keyboard_supported_ = false,
         permission_reported_ = false;
    std::int64_t now_ = 0, deadline_ = 0, heartbeat_at_ = 0;
    std::map<std::uint32_t, InputEvent> held_keys_;
    std::map<int, InputEvent> held_buttons_;
    std::uint16_t mouse_flags() const;
    void send_peer(const Bytes &payload, bool format = false);
    void login(int mode = 0, bool clear = false);
    void release_inputs();
    void fail(const std::string &message);
    void status(const std::string &message);
    void permission_diagnostic(const std::string &source);
    void command_frame(const Bytes &body);
    void peer_frame(const Bytes &body);
    bool visual_preferences_dirty_ = false;
    std::chrono::steady_clock::time_point performance_window_start_{};
    std::uint64_t performance_frames_ = 0, performance_payload_bytes_ = 0;
    double performance_decode_ms_ = 0.0;
};
} // namespace lunaviewer
