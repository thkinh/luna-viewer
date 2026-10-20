#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lunaviewer {
    using Bytes = std::vector<std::uint8_t>;
    using Parts = std::vector<Bytes>;
    struct Geometry { int width=0, height=0, left=0, top=0; };
    struct Frame { Geometry geometry; std::int32_t sequence=0; Bytes bgra; };

    using FrameSnapshot = std::shared_ptr<const Frame>;
    struct InputEvent {
        enum class Kind { Move, Button, Wheel, Key, ReleaseAll };
        Kind kind=Kind::Move;
        int x=0, y=0; // Coordinates in remote virtual desktop pixels.
        int button=0; // 1=left, 2=right, 3=middle.
        int wheel=0; // Windows wheel units, normally 120 per notch.
        std::uint32_t virtual_key=0, scan_code=0;
        std::uint16_t character=0;
        bool pressed=false, extended=false, caps_lock=false, system_key=false;
    };
    struct Credentials { std::string target_id, password; };
} // namespace lunaviewer
