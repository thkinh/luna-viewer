#include "logger.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <vector>
using lunaviewer::SessionLogger;
static void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
static QByteArray read(const QString &path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "open test log");
    return file.readAll();
}
static unsigned mode(const QString &path) {
    struct stat status{};
    check(::stat(QFile::encodeName(path).constData(), &status) == 0, "stat path");
    return status.st_mode & 0777;
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir root;
        check(root.isValid(), "temporary test directory");
        const auto dir = root.path() + "/nested/logs";
        SessionLogger logger(dir.toStdString());
        check(logger.available() && logger.error().empty(), "new logger available");
        check(QString::fromStdString(logger.path()) == dir + "/session.log", "discoverable path");
        check(mode(dir) == 0700 && mode(root.path() + "/nested") == 0700,
              "new directories private");
        check(mode(dir + "/session.log") == 0600, "file private");
        logger.write("DEBUG", "test", "hidden debug");
        check(read(dir + "/session.log").isEmpty(), "debug disabled by default");
        logger.write("INFO", "test\ncategory",
                     std::string("first\nsecond\r\t") + char(0x1b) + char(0x7f) +
                         std::string(1, '\0'));
        const auto text = read(dir + "/session.log");
        const QRegularExpression format(
            "^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}\\.\\d{3}Z \\[INFO\\] \\[test\\?category\\] "
            "first\\?second\\?\\?\\?\\?\\?\\n$");
        check(format.match(QString::fromUtf8(text)).hasMatch(),
              "UTC millisecond timestamp and sanitized single line");
        const auto existing = root.path() + "/existing";
        check(QDir().mkdir(existing), "create existing directory");
        check(::chmod(QFile::encodeName(existing).constData(), 0755) == 0,
              "set existing directory mode");
        {
            SessionLogger existing_logger(existing.toStdString());
            check(existing_logger.available(), "existing directory supported");
        }
        check(mode(existing) == 0755, "existing directory permissions preserved");
        const auto debugdir = root.path() + "/debug";
        SessionLogger debug(debugdir.toStdString(), 1024, 3, true);
        debug.write("DEBUG", "test", "enabled");
        check(read(debugdir + "/session.log").contains("[DEBUG] [test] enabled"),
              "explicit debug enabled");
        const auto rotationdir = root.path() + "/rotate";
        SessionLogger rotate(rotationdir.toStdString(), 180, 3);
        for (int n = 0; n < 20; ++n)
            rotate.write("INFO", "rotation", std::to_string(n) + std::string(100, 'x'));
        check(rotate.available(), "rotation remains available");
        for (int n = 0; n <= 3; ++n) {
            const auto file =
                rotationdir + "/session.log" + (n ? "." + QString::number(n) : QString());
            const auto body = read(file);
            check(body.size() <= 180 && body.endsWith('\n'), "all rotated files bounded");
            check(mode(file) == 0600, "backups private");
        }
        check(!QFile::exists(rotationdir + "/session.log.4"), "backup count bounded");
        check(read(rotationdir + "/session.log").contains("19xxx") &&
                  read(rotationdir + "/session.log.1").contains("18xxx"),
              "rotation keeps newest records in order");
        const auto bounddir = root.path() + "/bounded";
        SessionLogger bounded(bounddir.toStdString(), 128, 0);
        bounded.write(std::string(300, 'L'), std::string(300, 'C'), std::string(100000, 'M'));
        const auto boundedtext = read(bounddir + "/session.log");
        check(boundedtext.size() <= 128 && boundedtext.endsWith(" [truncated]\n"),
              "oversized record bounded");
        bounded.write("INFO", "test", "next");
        check(!QFile::exists(bounddir + "/session.log.1"), "zero backups supported");
        const auto threadsdir = root.path() + "/threads";
        SessionLogger threaded(threadsdir.toStdString(), 1048576, 3);
        std::vector<std::thread> threads;
        for (int t = 0; t < 6; ++t)
            threads.emplace_back([&, t] {
                for (int n = 0; n < 100; ++n)
                    threaded.write("INFO", "thread", std::to_string(t) + ":" + std::to_string(n));
            });
        for (auto &thread : threads)
            thread.join();
        const auto lines = read(threadsdir + "/session.log").split('\n');
        check(lines.size() == 601, "concurrent complete lines");
        std::set<QByteArray> messages;
        for (const auto &line : lines)
            if (!line.isEmpty()) {
                const auto marker = line.indexOf("[thread] ");
                check(marker >= 0, "concurrent prefix intact");
                messages.insert(line.mid(marker + 9));
            }
        check(messages.size() == 600 && threaded.available(),
              "concurrent records unique and complete");
        QFile blocker(root.path() + "/blocker");
        check(blocker.open(QIODevice::WriteOnly), "create path blocker");
        blocker.close();
        SessionLogger unavailable((root.path() + "/blocker/logs").toStdString());
        check(!unavailable.available() && !unavailable.error().empty(),
              "invalid path graceful unavailable");
        unavailable.write("INFO", "test", "ignored");
        SessionLogger invalid((root.path() + "/invalid").toStdString(), 0, 3);
        check(!invalid.available(), "invalid size unavailable");
        const auto symlinkdir = root.path() + "/symlink";
        check(QDir().mkdir(symlinkdir), "create symlink directory");
        check(QFile::link(root.path() + "/blocker", symlinkdir + "/session.log"),
              "create test symlink");
        SessionLogger symlink(symlinkdir.toStdString());
        check(!symlink.available(), "refuse symlink log file");
        const auto faildir = root.path() + "/rotatefailure";
        SessionLogger failed(faildir.toStdString(), 128, 1);
        check(QDir().mkdir(faildir + "/session.log.1"), "create blocked rotation destination");
        failed.write("INFO", "test", std::string(90, 'x'));
        failed.write("INFO", "test", std::string(90, 'y'));
        check(!failed.available() && !failed.error().empty(),
              "runtime rotation error degrades without throw");
        const auto previous = qgetenv("XDG_STATE_HOME");
        const bool had = qEnvironmentVariableIsSet("XDG_STATE_HOME");
        qputenv("XDG_STATE_HOME", (root.path() + "/state").toUtf8());
        {
            SessionLogger defaults;
            check(defaults.available() && QString::fromStdString(defaults.path()) ==
                                              root.path() + "/state/lunaviewer/logs/session.log",
                  "default XDG state path");
        }
        if (had)
            qputenv("XDG_STATE_HOME", previous);
        else
            qunsetenv("XDG_STATE_HOME");
        std::cout << "Logger rotation, bounds, UTC, sanitization, permissions, thread and failure "
                     "checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
