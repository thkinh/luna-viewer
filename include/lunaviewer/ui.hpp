#pragma once
#include "lunaviewer/types.hpp"
#include <QMainWindow>
#include <QPointF>
#include <QRectF>
#include <functional>
#include <memory>

namespace lunaviewer {
struct WindowCallbacks {
    std::function<void(Credentials)> connect;
    std::function<void()> disconnect;
    std::function<void(bool)> control_enabled;
    std::function<void(InputEvent)> input;
    std::function<void(int, bool)> visual_preferences_changed;
};
// Pure mapping helpers shared by the canvas and offline input tests.
QRectF frame_rect(const Geometry &geometry, const QSizeF &viewport);
bool map_position(const Geometry &geometry, const QSizeF &viewport, const QPointF &position,
                  QPoint &remote, bool clamp = false);
bool map_key(int qt_key, bool keypad, InputEvent &result);
class ControllerWindow : public QMainWindow {
  public:
    explicit ControllerWindow(WindowCallbacks callbacks, int display_quality = 45,
                              bool render_wallpaper = true, QWidget *parent = nullptr);
    ~ControllerWindow() override;
    void display_frame(FrameSnapshot frame);
    void set_status(const std::string &status);
    void set_remote_permission(bool allowed);
    // Session bridge calls on successful connection/disconnection.
    void set_connected(bool connected);
    void set_performance(const PerformanceStats &stats);

  protected:
    void closeEvent(QCloseEvent *event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace lunaviewer
