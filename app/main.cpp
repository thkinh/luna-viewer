#include "session.hpp"
#include "ui_mailbox.hpp"
#include "logger.hpp"
#include "lunaviewer/ui.hpp"
#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QTimer>
#include <QStatusBar>
#include <atomic>
#include <csignal>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
using namespace lunaviewer;
namespace {
class Worker : public QObject {
  public:
    Worker(SessionConfig config, std::shared_ptr<UiMailbox> ui,
           std::shared_ptr<SessionLogger> logger)
        : config_(std::move(config)), ui_(std::move(ui)), logger_(std::move(logger)) {
    }
    void initialize() {
        clock_.start();
        command_ = std::make_unique<TlsTransport>();
        peer_ = std::make_unique<TlsTransport>();
        timer_ = new QTimer(this);
        QObject::connect(timer_, &QTimer::timeout, this, [this] {
            auto input = ui_->take_input();
            for (const auto &event : input)
                if (valid(event.first) && session_ &&
                    (!ui_->canceled() || event.second.kind == InputEvent::Kind::ReleaseAll))
                    session_->input(event.second);
            if (session_)
                session_->tick(clock_.elapsed());
        });
        timer_->start(10);
    }
    void connect_peer(Credentials credentials, std::uint64_t generation) {
        if (session_)
            session_->disconnect();
        generation_ = generation;
        last_status_.clear();
        logger_->write("INFO", "session", "Starting new connection; local control disabled");
        auto token = generation_;
        SessionCallbacks c;
        c.open = [this, token](Channel ch, const TlsConfig &config) {
            if (!valid(token))
                return;
            const std::string channel = ch == Channel::Command ? "command" : "peer-relay";
            logger_->write("INFO", "transport",
                           channel + " TLS opening connect=" + config.connect_address + ":" +
                               std::to_string(config.port) + "; sni=" + config.sni + "; verify=" +
                               config.verify_name + "; http_host=" + config.http_host +
                               "; trust=" + (config.ca_file.empty() ? "system" : "system+custom"));
            auto &wire = transport(ch);
            wire.ready = [this, token, ch] {
                if (valid(token) && session_) {
                    logger_->write("INFO", "transport",
                                   ch == Channel::Command ? "command TLS ready"
                                                          : "peer-relay TLS ready");
                    session_->ready(ch);
                }
            };
            wire.frame = [this, token, ch](Bytes body) {
                if (valid(token) && session_)
                    session_->receive(ch, body);
            };
            wire.error = [this, token, ch](std::string error) {
                error = std::string(ch == Channel::Command ? "Command service: " : "Peer relay: ") +
                        error;
                std::cerr << error << std::endl;
                logger_->write("ERROR", "tls", error);
                // Defer failure until the transport callback returns. Never
                // destroy or restart a transport from its own callback stack.
                QTimer::singleShot(0, this, [this, token, error] {
                    if (valid(token) && session_)
                        session_->transport_error(error);
                });
            };
            wire.start(config);
        };
        c.send = [this, token](Channel ch, const Bytes &bytes) {
            if (valid(token))
                transport(ch).send(bytes);
        };
        c.close = [this] {
            command_->finish();
            peer_->finish();
        };
        c.diagnostic = [this](const std::string &message) {
            logger_->write("INFO", "protocol", message);
        };
        c.status = [this, token](const std::string &s) {
            ui_->status(token, s);
            // The first keyboard gate and its aggregate count are diagnostic
            // events; do not log a status line for every rejected key.
            if (s.rfind("Keyboard unavailable:", 0) != 0 && s != last_status_) {
                logger_->write("INFO", "session", s);
                last_status_ = s;
            }
        };
        c.accepted = [this, token](bool accepted) { ui_->accepted(token, accepted); };
        c.permission = [this, token](bool allowed) { ui_->permission(token, allowed); };
        c.frame = [this, token](FrameSnapshot frame) { ui_->frame(token, std::move(frame)); };
        session_ = std::make_unique<Session>(config_, std::move(c));
        session_->start(std::move(credentials), clock_.elapsed());
    }
    void disconnect_peer() {
        if (session_) {
            logger_->write("INFO", "session", "Disconnect requested");
            session_->disconnect();
        }
    }
    void control(bool enabled, std::uint64_t token) {
        if (valid(token) && session_)
            session_->control_enabled(enabled);
    }
    void shutdown() {
        if (timer_)
            timer_->stop();
        if (session_)
            session_->disconnect();
        session_.reset();
        command_.reset();
        peer_.reset();
    }

  private:
    bool valid(std::uint64_t token) const {
        return token == generation_ && token == ui_->generation();
    }
    TlsTransport &transport(Channel ch) {
        return ch == Channel::Command ? *command_ : *peer_;
    }
    SessionConfig config_;
    std::shared_ptr<UiMailbox> ui_;
    std::shared_ptr<SessionLogger> logger_;
    std::string last_status_;
    std::uint64_t generation_ = 0;
    QElapsedTimer clock_;
    QTimer *timer_ = nullptr;
    std::unique_ptr<Session> session_;
    std::unique_ptr<TlsTransport> command_, peer_;
};
SessionConfig load_config(const QCommandLineParser &args) {
    SessionConfig c;
    c.command.connect_address = "functions3.ultraviewer.net";
    c.command.verify_name = "functions3.ultraviewer.net";
    c.command.http_host = c.command.verify_name;
    c.utc_offset_minutes = QDateTime::currentDateTime().offsetFromUtc() / 60;
    if (args.isSet("profile")) {
        QFile file(args.value("profile"));
        if (!file.open(QIODevice::ReadOnly) || file.size() > 65536)
            throw std::runtime_error("Cannot read bounded identity profile");
        QJsonParseError error;
        auto json = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !json.isObject())
            throw std::runtime_error("Identity profile must be a JSON object");
        auto object = json.object();
        auto fields = object.value("registration_fields");
        if (!fields.isArray())
            throw std::runtime_error("Profile requires registration_fields array");
        for (auto f : fields.toArray()) {
            if (!f.isString())
                throw std::runtime_error("Registration fields must be strings");
            c.registration.push_back(f.toString().toStdString());
        }
        protocol::registration_text(c.registration);
        auto value = [&object](const char *key, const std::string &fallback) {
            return object.contains(key) ? object.value(key).toString().toStdString() : fallback;
        };
        c.command.connect_address = value("command_address", c.command.connect_address);
        c.command.verify_name = value("command_hostname", c.command.verify_name);
        c.command.http_host = c.command.verify_name;
        c.command.sni = value("sni", c.command.sni);
        c.command.ca_file = value("ca_file", "");
        auto port = object.value("command_port").toInt(443);
        if (port < 1 || port > 65535)
            throw std::runtime_error("Invalid command port");
        c.command.port = static_cast<std::uint16_t>(port);
        c.dictionary_path = value("dictionary_path", "");
        c.display_name = value("display_name", "LunaViewer");
    }
    if (args.isSet("dictionary"))
        c.dictionary_path = args.value("dictionary").toStdString();
    return c;
}
} // namespace

int main(int argc, char **argv) {
    std::signal(SIGPIPE, SIG_IGN);
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("LunaViewer");
    QCoreApplication::setApplicationVersion("0.1.0");
    QCommandLineParser args;
    args.setApplicationDescription(
        "Experimental relay controller; Windows interoperability is not yet validated.");
    args.addHelpOption();
    args.addVersionOption();
    args.addOption({"profile", "Explicit local identity profile JSON", "path"});
    args.addOption({"dictionary", "Existing local UltraViewer screen dictionary", "path"});
    args.addOption(
        {"log-dir", "Persistent log directory (default: XDG_STATE_HOME/lunaviewer/logs)", "path"});
    args.addOption({"debug", "Enable additional metadata-only diagnostic logging"});
    args.addOption(
        {"smoke-exit", "Show the window then exit after this many milliseconds (offline)", "ms"});
    args.process(app);
    auto logger = std::make_shared<SessionLogger>(args.value("log-dir").toStdString(), 1048576, 3,
                                                  args.isSet("debug"));
    const auto log_banner = logger->available() ? "Log file: " + logger->path()
                                                : "Logging unavailable: " + logger->error() +
                                                      " (requested " + logger->path() + ")";
    std::cerr << log_banner << std::endl;
    logger->write("INFO", "app", "LunaViewer 0.1.0 started; log_file=" + logger->path());
    logger->write("DEBUG", "app", "Qt platform=" + QGuiApplication::platformName().toStdString());
    try {
        auto config = load_config(args);
        int smoke_ms = 0;
        if (args.isSet("smoke-exit")) {
            bool ok = false;
            smoke_ms = args.value("smoke-exit").toInt(&ok);
            if (!ok || smoke_ms < 1 || smoke_ms > 60000)
                throw std::runtime_error("Invalid smoke-exit duration");
        }
        auto ui = std::make_shared<UiMailbox>();
        QThread thread;
        auto *worker = new Worker(config, ui, logger);
        worker->moveToThread(&thread);
        QObject::connect(&thread, &QThread::started, worker, [worker] { worker->initialize(); });
        QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        WindowCallbacks callbacks;
        callbacks.connect = [worker, ui](Credentials credentials) {
            auto generation = ui->begin_generation();
            QMetaObject::invokeMethod(
                worker,
                [worker, credentials = std::move(credentials), generation]() mutable {
                    worker->connect_peer(std::move(credentials), generation);
                },
                Qt::QueuedConnection);
        };
        callbacks.disconnect = [worker, ui] {
            ui->cancel();
            QMetaObject::invokeMethod(
                worker, [worker] { worker->disconnect_peer(); }, Qt::QueuedConnection);
        };
        callbacks.control_enabled = [worker, ui](bool enabled) {
            auto token = ui->generation();
            QMetaObject::invokeMethod(
                worker, [worker, token, enabled] { worker->control(enabled, token); },
                Qt::QueuedConnection);
        };
        callbacks.input = [ui](InputEvent input) { ui->input(input); };
        ControllerWindow window(std::move(callbacks));
        window.statusBar()->showMessage(QString::fromStdString(log_banner));
        QTimer present;
        QObject::connect(&present, &QTimer::timeout, &window, [&window, ui] {
            auto update = ui->take();
            if (update.accepted)
                window.set_connected(*update.accepted);
            if (update.permission)
                window.set_remote_permission(*update.permission);
            if (update.status)
                window.set_status(*update.status);
            if (update.frame && update.connected)
                window.display_frame(std::move(update.frame));
        });
        present.start(16);
        thread.start();
        if (config.registration.empty())
            window.set_status("Supply --profile before connecting; see README.md");
        window.show();
        if (smoke_ms)
            QTimer::singleShot(smoke_ms, &app, &QCoreApplication::quit);
        int result = app.exec();
        // Let queued key/button releases and disconnect19 drain with a bound.
        QMetaObject::invokeMethod(
            worker, [worker] { worker->disconnect_peer(); }, Qt::BlockingQueuedConnection);
        QEventLoop drain;
        QTimer::singleShot(550, &drain, &QEventLoop::quit);
        drain.exec();
        QMetaObject::invokeMethod(
            worker, [worker] { worker->shutdown(); }, Qt::BlockingQueuedConnection);
        thread.quit();
        thread.wait();
        logger->write("INFO", "app", "LunaViewer stopped cleanly");
        return result;
    } catch (const std::exception &e) {
        logger->write("ERROR", "app", e.what());
        std::cerr << "LunaViewer: " << e.what() << '\n';
        return 1;
    }
}
