#pragma once
#include "lunaviewer/types.hpp"
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>

namespace lunaviewer {
struct UiUpdate {
    FrameSnapshot frame;
    std::optional<std::string> status;
    std::optional<bool> accepted, permission;
    bool connected = false;
};
// One GUI consumer and one session producer. Generation checks and writes share
// the same lock as reset, so an old callback cannot repopulate a new session.
class UiMailbox {
  public:
    std::uint64_t begin_generation() {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto token = ++generation_;
        update_ = UiUpdate{};
        input_.clear();
        canceled_ = false;
        return token;
    }
    std::uint64_t generation() const {
        return generation_.load();
    }
    bool canceled() const {
        return canceled_.load();
    }
    void cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        canceled_ = true;
        update_ = UiUpdate{};
        update_.accepted = false;
        update_.permission = false;
        update_.status = "Disconnected";
        input_.clear();
        InputEvent release;
        release.kind = InputEvent::Kind::ReleaseAll;
        input_.push_back({generation_.load(), release});
    }
    void status(std::uint64_t token, const std::string &status) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (token == generation_ && !canceled_)
            update_.status = status;
    }
    void accepted(std::uint64_t token, bool accepted) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (token != generation_ || (canceled_ && accepted))
            return;
        update_.accepted = accepted;
        update_.connected = accepted;
        if (!accepted)
            update_.frame.reset();
    }
    void permission(std::uint64_t token, bool permission) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (token == generation_ && (!canceled_ || !permission))
            update_.permission = permission;
    }
    void frame(std::uint64_t token, FrameSnapshot frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (token == generation_ && !canceled_ && update_.connected)
            update_.frame = std::move(frame);
    }
    UiUpdate take() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = std::move(update_);
        update_ = UiUpdate{};
        update_.connected = result.connected;
        return result;
    }
    void input(InputEvent event) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto token = generation_.load();
        if (canceled_ && event.kind != InputEvent::Kind::ReleaseAll)
            return;
        if (event.kind == InputEvent::Kind::Move && !input_.empty() &&
            input_.back().second.kind == InputEvent::Kind::Move)
            input_.back() = {token, event};
        else if (input_.size() < 256)
            input_.push_back({token, event});
        else {
            input_.clear();
            InputEvent release;
            release.kind = InputEvent::Kind::ReleaseAll;
            input_.push_back({token, release});
        }
    }
    std::deque<std::pair<std::uint64_t, InputEvent>> take_input() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::deque<std::pair<std::uint64_t, InputEvent>> result;
        result.swap(input_);
        return result;
    }

  private:
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<bool> canceled_{false};
    mutable std::mutex mutex_;
    UiUpdate update_;
    std::deque<std::pair<std::uint64_t, InputEvent>> input_;
};
} // namespace lunaviewer
