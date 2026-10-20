#include "lunaviewer/ui.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <limits>
#include <map>
#include <set>
#include <utility>
namespace lunaviewer {
namespace {
class ScreenCanvas : public QWidget {
  public:
    explicit ScreenCanvas(std::function<void(InputEvent)> send, QWidget *parent)
        : QWidget(parent), send_(std::move(send)) {
        setObjectName("remoteCanvas");
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setMinimumSize(320, 200);
    }
    void set_enabled(bool enabled) {
        if (!enabled)
            release();
        enabled_ = enabled;
        setCursor(enabled ? Qt::CrossCursor : Qt::ArrowCursor);
    }
    void set_frame(FrameSnapshot frame) {
        if (frame_ && (frame_->geometry.width != frame->geometry.width ||
                       frame_->geometry.height != frame->geometry.height ||
                       frame_->geometry.left != frame->geometry.left ||
                       frame_->geometry.top != frame->geometry.top))
            release();
        frame_ = std::move(frame);
        update();
    }
    void clear() {
        release();
        frame_.reset();
        update();
    }
    void release() {
        const bool held = !keys_.empty() || !buttons_.empty();
        keys_.clear();
        buttons_.clear();
        if (held) {
            InputEvent event;
            event.kind = InputEvent::Kind::ReleaseAll;
            send(event);
        }
    }

  protected:
    bool event(QEvent *e) override {
        if (e->type() == QEvent::WindowDeactivate)
            release();
        if (active() && e->type() == QEvent::ShortcutOverride) {
            e->accept();
            return true;
        }
        if (active() && (e->type() == QEvent::KeyPress || e->type() == QEvent::KeyRelease)) {
            auto *key = static_cast<QKeyEvent *>(e);
            InputEvent input;
            if (!map_key(key->key(), key->modifiers().testFlag(Qt::KeypadModifier), input))
                return QWidget::event(e);
            if (key->isAutoRepeat() && e->type() == QEvent::KeyRelease)
                return true;
            // Qt nativeVirtualKey is an XKB keysym on supported Linux backends.
            // Native scan codes are backend-specific and never sent to Windows.
            switch (key->nativeVirtualKey()) {
            case 0xffe2:
                input.scan_code = 0x36;
                break; // right Shift
            case 0xffe4:
            case 0xffea:
                input.extended = true;
                break; // right Ctrl/Alt
            default:
                break;
            }
            input.pressed = e->type() == QEvent::KeyPress;
            if (input.pressed && !key->isAutoRepeat() && key->key() == Qt::Key_CapsLock)
                caps_lock_ = !caps_lock_;
            input.caps_lock = caps_lock_;
            input.system_key =
                key->key() == Qt::Key_Alt || key->modifiers().testFlag(Qt::AltModifier);
            if (key->text().size() == 1)
                input.character = key->text().at(0).unicode();
            const int id =
                int(input.virtual_key | (input.scan_code << 8) | (input.extended ? 0x1000000 : 0));
            if (input.pressed) {
                keys_[id] = input;
                send(input);
            } else {
                const auto found = keys_.find(id);
                if (found != keys_.end()) {
                    input = found->second;
                    input.pressed = false;
                    keys_.erase(found);
                    send(input);
                }
            }
            e->accept();
            return true;
        }
        return QWidget::event(e);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(28, 30, 34));
        if (!frame_) {
            painter.setPen(Qt::lightGray);
            painter.drawText(rect(), Qt::AlignCenter, "Waiting for remote screen");
            return;
        }
        const auto &g = frame_->geometry;
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
        const QImage image(frame_->bgra.data(), g.width, g.height, g.width * 4,
                           QImage::Format_ARGB32);
#else
        const QImage image =
            QImage(frame_->bgra.data(), g.width, g.height, g.width * 4, QImage::Format_RGBA8888)
                .rgbSwapped();
#endif
        painter.drawImage(frame_rect(g, size()), image);
    }
    void focusOutEvent(QFocusEvent *event) override {
        release();
        QWidget::focusOutEvent(event);
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        move(event->position(), !buttons_.empty());
    }
    void mousePressEvent(QMouseEvent *event) override {
        button(event, true);
    }
    void mouseDoubleClickEvent(QMouseEvent *event) override {
        button(event, true);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        button(event, false);
    }
    void wheelEvent(QWheelEvent *event) override {
        if (!move(event->position(), false) || !event->angleDelta().y())
            return;
        InputEvent input;
        input.kind = InputEvent::Kind::Wheel;
        input.x = last_.x();
        input.y = last_.y();
        input.wheel = event->angleDelta().y();
        send(input);
        event->accept();
    }

  private:
    bool active() const {
        return enabled_ && bool(frame_);
    }
    void send(const InputEvent &e) {
        if (send_)
            send_(e);
    }
    bool move(const QPointF &pos, bool clamp) {
        QPoint remote;
        if (!active() || !map_position(frame_->geometry, size(), pos, remote, clamp))
            return false;
        last_ = remote;
        InputEvent e;
        e.kind = InputEvent::Kind::Move;
        e.x = remote.x();
        e.y = remote.y();
        send(e);
        return true;
    }
    void button(QMouseEvent *event, bool pressed) {
        const int b = event->button() == Qt::LeftButton     ? 1
                      : event->button() == Qt::RightButton  ? 2
                      : event->button() == Qt::MiddleButton ? 3
                                                            : 0;
        if (!b || !active())
            return;
        if (!pressed && !buttons_.count(b))
            return;
        if (!move(event->position(), !pressed))
            return;
        if (pressed) {
            setFocus(Qt::MouseFocusReason);
            buttons_.insert(b);
        } else
            buttons_.erase(b);
        InputEvent e;
        e.kind = InputEvent::Kind::Button;
        e.button = b;
        e.pressed = pressed;
        e.x = last_.x();
        e.y = last_.y();
        send(e);
        event->accept();
    }
    std::function<void(InputEvent)> send_;
    FrameSnapshot frame_;
    bool enabled_ = false, caps_lock_ = false;
    QPoint last_;
    std::map<int, InputEvent> keys_;
    std::set<int> buttons_;
};
} // namespace
struct ControllerWindow::Impl {
    WindowCallbacks callbacks;
    ScreenCanvas *canvas = nullptr;
    QLineEdit *target = nullptr;
    QLineEdit *password = nullptr;
    QPushButton *connect_button = nullptr;
    QPushButton *disconnect_button = nullptr;
    QCheckBox *control = nullptr;
    QLabel *status = nullptr;
    QLabel *permission = nullptr;
    bool connected = false, remote_allowed = false;
    void update_control() {
        canvas->set_enabled(connected && remote_allowed && control->isChecked());
    }
};
ControllerWindow::ControllerWindow(WindowCallbacks callbacks, QWidget *parent)
    : QMainWindow(parent), impl_(new Impl) {
    auto &p = *impl_;
    p.callbacks = std::move(callbacks);
    setWindowTitle("LunaViewer");
    resize(1000, 700);
    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    auto *connection = new QHBoxLayout;
    p.target = new QLineEdit(central);
    p.target->setObjectName("targetId");
    p.target->setPlaceholderText("Partner ID");
    p.target->setAccessibleName("Partner ID");
    p.password = new QLineEdit(central);
    p.password->setObjectName("password");
    p.password->setEchoMode(QLineEdit::Password);
    p.password->setPlaceholderText("Password");
    p.password->setAccessibleName("Password");
    p.connect_button = new QPushButton("Connect", central);
    p.connect_button->setObjectName("connectButton");
    p.disconnect_button = new QPushButton("Disconnect", central);
    p.disconnect_button->setObjectName("disconnectButton");
    p.disconnect_button->setEnabled(false);
    connection->addWidget(p.target);
    connection->addWidget(p.password);
    connection->addWidget(p.connect_button);
    connection->addWidget(p.disconnect_button);
    layout->addLayout(connection);
    p.status = new QLabel("Disconnected", central);
    p.status->setObjectName("sessionStatus");
    p.status->setTextFormat(Qt::PlainText);
    p.status->setWordWrap(true);
    layout->addWidget(p.status);
    auto *controls = new QHBoxLayout;
    p.control = new QCheckBox("Enable remote control", central);
    p.control->setObjectName("controlToggle");
    p.control->setEnabled(false);
    controls->addWidget(p.control);
    p.permission = new QLabel("Remote control permission: unavailable", central);
    p.permission->setObjectName("remotePermission");
    controls->addWidget(p.permission);
    controls->addStretch();
    layout->addLayout(controls);
    p.canvas = new ScreenCanvas(p.callbacks.input, central);
    layout->addWidget(p.canvas, 1);
    setCentralWidget(central);
    QObject::connect(p.connect_button, &QPushButton::clicked, this, [this] {
        auto &s = *impl_;
        const auto id = s.target->text().trimmed();
        if (id.isEmpty() || s.password->text().isEmpty()) {
            set_status("Enter a partner ID and password.");
            return;
        }
        Credentials credentials{id.toStdString(), s.password->text().toStdString()};
        s.password->clear();
        s.connect_button->setEnabled(false);
        s.disconnect_button->setEnabled(true);
        s.target->setEnabled(false);
        s.password->setEnabled(false);
        set_status("Connecting…");
        if (s.callbacks.connect)
            s.callbacks.connect(std::move(credentials));
    });
    QObject::connect(p.disconnect_button, &QPushButton::clicked, this, [this] {
        set_connected(false);
        if (impl_->callbacks.disconnect)
            impl_->callbacks.disconnect();
    });
    QObject::connect(p.control, &QCheckBox::toggled, this, [this](bool enabled) {
        impl_->update_control();
        if (impl_->callbacks.control_enabled)
            impl_->callbacks.control_enabled(enabled);
    });
    QObject::connect(qApp, &QApplication::applicationStateChanged, this,
                     [this](Qt::ApplicationState state) {
                         if (state != Qt::ApplicationActive)
                             impl_->canvas->release();
                     });
}
ControllerWindow::~ControllerWindow() {
    impl_->canvas->release();
}
void ControllerWindow::closeEvent(QCloseEvent *event) {
    set_connected(false);
    if (impl_->callbacks.disconnect)
        impl_->callbacks.disconnect();
    QMainWindow::closeEvent(event);
}
void ControllerWindow::display_frame(FrameSnapshot frame) {
    if (!frame || frame->geometry.width <= 0 || frame->geometry.height <= 0 ||
        frame->geometry.width > std::numeric_limits<int>::max() / 4) {
        set_status("Invalid remote frame.");
        return;
    }
    const auto width = std::size_t(frame->geometry.width),
               height = std::size_t(frame->geometry.height);
    if (height > std::numeric_limits<std::size_t>::max() / width / 4 ||
        frame->bgra.size() != width * height * 4) {
        set_status("Invalid remote frame size.");
        return;
    }
    impl_->canvas->set_frame(std::move(frame));
}
void ControllerWindow::set_status(const std::string &status) {
    impl_->status->setText(QString::fromStdString(status));
}
void ControllerWindow::set_remote_permission(bool allowed) {
    auto &p = *impl_;
    p.remote_allowed = allowed;
    if (!allowed) {
        p.canvas->release();
        p.control->setChecked(false);
    }
    p.permission->setText(!p.connected ? "Remote control permission: unavailable"
                          : allowed    ? "Remote control permission: allowed"
                                       : "Remote control permission: denied");
    p.control->setEnabled(p.connected && allowed);
    p.update_control();
}
void ControllerWindow::set_connected(bool connected) {
    auto &p = *impl_;
    if (!connected) {
        p.canvas->release();
        p.control->setChecked(false);
        p.remote_allowed = false;
        p.canvas->clear();
        p.permission->setText("Remote control permission: unavailable");
        set_status("Disconnected");
    }
    p.connected = connected;
    p.target->setEnabled(!connected);
    p.password->setEnabled(!connected);
    p.connect_button->setEnabled(!connected);
    p.disconnect_button->setEnabled(connected);
    p.control->setEnabled(connected && p.remote_allowed);
    p.update_control();
}
} // namespace lunaviewer
