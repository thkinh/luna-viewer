#include "lunaviewer/ui.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QTabWidget>
#include <QWheelEvent>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace lunaviewer;
static void check(bool good, const char *message) {
    if (!good)
        throw std::runtime_error(message);
}
static void key(QWidget *target, QEvent::Type type, int code,
                Qt::KeyboardModifiers mods = Qt::NoModifier, QString text = {}) {
    QKeyEvent event(type, code, mods, text);
    QApplication::sendEvent(target, &event);
}
int main(int argc, char **argv) {
    QApplication application(argc, argv);
    try {
        const Geometry g{1920, 1080, -1920, -100};
        const QSizeF viewport(1000, 1000);
        QPoint remote;
        const auto rect = frame_rect(g, viewport);
        check(rect == QRectF(0, 218.75, 1000, 562.5), "aspect-fit rectangle");
        check(map_position(g, viewport, {0, 218.75}, remote) && remote == QPoint(-1920, -100),
              "virtual origin mapping");
        check(map_position(g, viewport, {999.9, 781.2}, remote) && remote == QPoint(-1, 979),
              "last pixel mapping");
        check(!map_position(g, viewport, {0, 0}, remote), "letterbox rejected");
        check(!map_position(g, viewport, {1000, 781.25}, remote), "exclusive right/bottom edges");
        check(map_position(g, viewport, {1100, 1100}, remote, true) && remote == QPoint(-1, 979),
              "drag clamp");
        check(!map_position({}, viewport, {0, 0}, remote), "empty geometry");
        InputEvent mapped;
        check(map_key(Qt::Key_A, false, mapped) && mapped.virtual_key == 0x41 &&
                  mapped.scan_code == 0x1e,
              "letter key mapping");
        check(map_key(Qt::Key_F12, false, mapped) && mapped.virtual_key == 0x7b &&
                  mapped.scan_code == 0x58,
              "F12 mapping");
        check(map_key(Qt::Key_Enter, true, mapped) && mapped.extended && mapped.scan_code == 0x1c,
              "keypad enter mapping");
        check(map_key(Qt::Key_Question, false, mapped) && mapped.virtual_key == 0xbf,
              "shifted punctuation mapping");
        check(map_key(Qt::Key_1, true, mapped) && mapped.virtual_key == 0x61 &&
                  mapped.scan_code == 0x4f,
              "keypad digit mapping");
        check(!map_key(Qt::Key_unknown, false, mapped), "unknown key not invented");
        std::vector<InputEvent> events;
        Credentials credentials;
        bool connect_called = false;
        int disconnects = 0;
        int selected_quality = 45;
        bool selected_wallpaper = true;
        WindowCallbacks callbacks{[&](Credentials c) {
                                      credentials = std::move(c);
                                      connect_called = true;
                                  },
                                  [&] { ++disconnects; }, [](bool) {},
                                  [&](InputEvent e) { events.push_back(e); }};
        callbacks.visual_preferences_changed = [&](int quality, bool wallpaper) {
            selected_quality = quality;
            selected_wallpaper = wallpaper;
        };
        ControllerWindow window(std::move(callbacks));
        window.show();
        application.processEvents();
        auto *canvas = window.findChild<QWidget *>("remoteCanvas");
        auto *toggle = window.findChild<QCheckBox *>("controlToggle");
        auto *target = window.findChild<QLineEdit *>("targetId");
        auto *password = window.findChild<QLineEdit *>("password");
        auto *connect = window.findChild<QPushButton *>("connectButton");
        auto *disconnect = window.findChild<QPushButton *>("disconnectButton");
        auto *quality = window.findChild<QComboBox *>("remoteQuality");
        auto *wallpaper = window.findChild<QCheckBox *>("renderWallpaper");
        auto *fps = window.findChild<QLabel *>("liveFps");
        auto *data_rate = window.findChild<QLabel *>("liveDataRate");
        auto *status = window.findChild<QLabel *>("sessionStatus");
        auto *tabs = window.findChild<QTabWidget *>("applicationTabs");
        auto *status_row = window.findChild<QHBoxLayout *>("connectionStatusRow");
        check(fps && data_rate && status && tabs && status_row &&
                  status_row->indexOf(status) == 0 && status_row->indexOf(fps) == 1 &&
                  status_row->indexOf(data_rate) == 2 && fps->parentWidget() == tabs->widget(0) &&
                  data_rate->parentWidget() == tabs->widget(0),
              "Control status, FPS and data rate must share one horizontal row");
        check(fps->isVisible() && fps->geometry().center().y() == status->geometry().center().y() &&
                  fps->geometry().top() > password->geometry().bottom() &&
                  fps->text() == "0.0 FPS" && data_rate->isVisible() &&
                  data_rate->geometry().left() > fps->geometry().right() &&
                  data_rate->text() == "0.0 KiB/s",
              "FPS row must be visible directly below connection inputs");
        check(canvas && toggle && target && password && connect && disconnect, "named UI controls");
        check(quality && wallpaper && quality->currentData().toInt() == 45 &&
                  wallpaper->isChecked(),
              "visual setting defaults");
        quality->setCurrentIndex(2);
        wallpaper->setChecked(false);
        check(selected_quality == 70 && !selected_wallpaper, "visual setting callbacks");
        window.set_performance({12.0, 64.0, 3.5, 1920, 1080});
        check(fps->text() == "0.0 FPS" && data_rate->text() == "0.0 KiB/s",
              "disconnected performance sample ignored");
        check(!toggle->isEnabled() && !toggle->isChecked(), "control starts gated");
        window.set_remote_permission(false);
        check(window.findChild<QLabel *>("remotePermission")->text().endsWith("unavailable"),
              "unknown permission mislabeled denied before connection");
        connect->click();
        check(!connect_called, "empty credentials rejected");
        target->setText(" 123456 ");
        password->setText("secret");
        connect->click();
        check(connect_called && credentials.target_id == "123456" &&
                  credentials.password == "secret",
              "credential callback");
        check(password->text().isEmpty() && !connect->isEnabled(),
              "password cleared and duplicate connect blocked");
        auto frame = std::make_shared<Frame>();
        frame->geometry = {4, 2, -4, 10};
        frame->bgra.resize(32);
        for (std::size_t i = 0; i < 32; i += 4) {
            frame->bgra[i + 2] = 255;
            frame->bgra[i + 3] = 255;
        }
        std::weak_ptr<const Frame> lifetime = frame;
        window.display_frame(frame);
        frame.reset();
        check(!lifetime.expired(), "canvas owns immutable snapshot lifetime");
        const auto image = canvas->grab().toImage();
        const auto pixel = image.pixelColor(image.width() / 2, image.height() / 2);
        check(pixel.red() == 255 && pixel.green() == 0 && pixel.blue() == 0, "BGRA renders red");
        key(canvas, QEvent::KeyPress, Qt::Key_A);
        check(events.empty(), "no input before connection/permission/local toggle");
        window.set_connected(true);
        check(status->text() == "Connected" && fps->text() == "0.0 FPS",
              "connection initializes status and FPS");
        window.set_performance({12.0, 64.0, 3.5, 1920, 1080});
        window.set_status("Connected; receiving screen");
        check(fps->text() == "12.0 FPS" && data_rate->text() == "64.0 KiB/s" &&
                  window.findChild<QLabel *>("screenPerformance")->text().contains("12.0 FPS"),
              "periodic sample updates both labels and status preserves FPS");
        tabs->setCurrentIndex(1);
        application.processEvents();
        const auto original_lifetime = lifetime;
        auto hidden_frame = std::make_shared<Frame>(*lifetime.lock());
        window.display_frame(hidden_frame);
        lifetime = hidden_frame;
        hidden_frame.reset();
        window.set_performance({0.0, 0.0, 0.0, 1920, 1080, 0});
        check(original_lifetime.expired() && !lifetime.expired() && fps->text() == "0.0 FPS",
              "hidden Control tab receives frames and idle performance updates");
        window.set_performance({7.0, 32.0, 2.0, 1920, 1080, 7});
        tabs->setCurrentIndex(0);
        application.processEvents();
        check(fps->isVisible() && fps->text() == "7.0 FPS" && data_rate->text() == "32.0 KiB/s",
              "returning to Control preserves the latest performance sample");
        window.set_remote_permission(true);
        key(canvas, QEvent::KeyPress, Qt::Key_A);
        check(events.empty(), "permission alone insufficient");
        toggle->setChecked(true);
        canvas->setFocus();
        application.processEvents();
        key(canvas, QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, "a");
        check(events.size() == 1 && events.back().pressed && events.back().character == 'a',
              "enabled key down with UTF16");
        QFocusEvent focus_out(QEvent::FocusOut);
        QApplication::sendEvent(canvas, &focus_out);
        check(events.back().kind == InputEvent::Kind::ReleaseAll, "focus loss releases held keys");
        auto count = events.size();
        key(canvas, QEvent::KeyRelease, Qt::Key_A);
        check(events.size() == count, "orphan release ignored");
        key(canvas, QEvent::KeyPress, Qt::Key_Question, Qt::ShiftModifier, "?");
        key(canvas, QEvent::KeyRelease, Qt::Key_Slash);
        check(events.back().virtual_key == 0xbf && !events.back().pressed,
              "shifted key identity released");
        key(canvas, QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
        check(events.back().system_key, "Alt system-key metadata");
        key(canvas, QEvent::KeyRelease, Qt::Key_Alt);
        const QPointF center(canvas->width() / 2.0, canvas->height() / 2.0);
        QMouseEvent down(QEvent::MouseButtonPress, center, center, Qt::LeftButton, Qt::LeftButton,
                         Qt::NoModifier);
        QApplication::sendEvent(canvas, &down);
        check(events.back().kind == InputEvent::Kind::Button && events.back().pressed &&
                  events.back().x == -2 && events.back().y == 11,
              "mouse button and remote origin");
        window.set_remote_permission(false);
        check(events.back().kind == InputEvent::Kind::ReleaseAll && !toggle->isChecked() &&
                  !toggle->isEnabled(),
              "permission revocation releases and disables");
        count = events.size();
        key(canvas, QEvent::KeyPress, Qt::Key_B);
        check(events.size() == count, "revoked input gated");
        window.set_remote_permission(true);
        toggle->setChecked(true);
        QWheelEvent wheel(center, center, QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(canvas, &wheel);
        check(events.back().kind == InputEvent::Kind::Wheel && events.back().wheel == -120,
              "signed wheel units");
        key(canvas, QEvent::KeyPress, Qt::Key_B);
        disconnect->click();
        check(disconnects == 1 && events.back().kind == InputEvent::Kind::ReleaseAll &&
                  lifetime.expired(),
              "disconnect releases input and frame");
        check(!toggle->isChecked() && connect->isEnabled(), "disconnected UI reset");
        window.set_performance({99.0, 64.0, 3.5, 1920, 1080, 99});
        check(status->text() == "Disconnected" && fps->text() == "0.0 FPS" &&
                  data_rate->text() == "0.0 KiB/s" &&
                  window.findChild<QLabel *>("screenPerformance")->text() ==
                      "No screen frames received yet",
              "disconnect clears statistics and rejects late samples");
        window.set_remote_permission(false);
        check(window.findChild<QLabel *>("remotePermission")->text().endsWith("unavailable"),
              "disconnected permission mislabeled denied");
        auto invalid = std::make_shared<Frame>();
        invalid->geometry = {4, 2, 0, 0};
        window.display_frame(invalid);
        check(window.findChild<QLabel *>("sessionStatus")->text() == "Invalid remote frame size.",
              "invalid frame rejected before QImage");
        window.close();
        check(disconnects == 2, "window close disconnects");
        std::cout << "UI mapping, gating, BGRA lifetime, focus-release and offscreen smoke checks "
                     "passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
