#include "logger.hpp"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
namespace lunaviewer {
namespace {
std::string system_error(const char *action) {
    return std::string(action) + ": " + std::strerror(errno);
}
// Only directories actually created here get private permissions.
void make_directory(const QString &directory) {
    const QFileInfo info(directory);
    if (info.exists()) {
        if (!info.isDir())
            throw std::runtime_error("Log path is not a directory");
        return;
    }
    const auto parent = info.dir().absolutePath();
    if (parent == directory)
        throw std::runtime_error("Cannot create log directory");
    make_directory(parent);
    const auto native = QFile::encodeName(directory);
    if (::mkdir(native.constData(), 0700) == 0) {
        if (::chmod(native.constData(), 0700) != 0)
            throw std::runtime_error(system_error("Protect log directory"));
    } else if (errno != EEXIST || !QFileInfo(directory).isDir())
        throw std::runtime_error(system_error("Create log directory"));
}
std::string clean(const std::string &input, std::size_t limit) {
    std::string result;
    result.reserve(std::min(input.size(), limit));
    for (std::size_t i = 0; i < input.size() && result.size() < limit; ++i) {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        result.push_back(c < 32 || c == 127 ? '?' : char(c));
    }
    return result;
}
} // namespace
struct SessionLogger::Impl {
    mutable std::mutex mutex;
    std::string filename, failure;
    int fd = -1;
    std::size_t maximum = 0, bytes = 0;
    unsigned backups = 0;
    bool debug = false;
    ~Impl() {
        if (fd >= 0)
            ::close(fd);
    }
    void disable(const std::string &message) {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
        failure = message;
    }
    void open_file() {
        fd = ::open(filename.c_str(),
                    O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
        if (fd < 0)
            throw std::runtime_error(system_error("Open session log"));
        struct stat info{};
        if (::fstat(fd, &info) != 0)
            throw std::runtime_error(system_error("Inspect session log"));
        if (!S_ISREG(info.st_mode))
            throw std::runtime_error("Session log is not a regular file");
        if (::fchmod(fd, 0600) != 0)
            throw std::runtime_error(system_error("Protect session log"));
        if (info.st_size < 0 || static_cast<unsigned long long>(info.st_size) > maximum) {
            // Changing a configured size limit must not retain an oversized active file.
            if (::ftruncate(fd, 0) != 0)
                throw std::runtime_error(system_error("Bound session log"));
            bytes = 0;
        } else
            bytes = static_cast<std::size_t>(info.st_size);
    }
    void rotate() {
        ::close(fd);
        fd = -1;
        if (!backups) {
            if (::unlink(filename.c_str()) != 0 && errno != ENOENT)
                throw std::runtime_error(system_error("Remove old session log"));
        } else {
            const auto last = filename + "." + std::to_string(backups);
            if (::unlink(last.c_str()) != 0 && errno != ENOENT)
                throw std::runtime_error(system_error("Remove oldest log"));
            for (unsigned n = backups; n > 1; --n) {
                const auto from = filename + "." + std::to_string(n - 1),
                           to = filename + "." + std::to_string(n);
                if (::rename(from.c_str(), to.c_str()) != 0 && errno != ENOENT)
                    throw std::runtime_error(system_error("Rotate log backup"));
            }
            if (::rename(filename.c_str(), (filename + ".1").c_str()) != 0 && errno != ENOENT)
                throw std::runtime_error(system_error("Rotate session log"));
        }
        open_file();
    }
};
SessionLogger::SessionLogger(std::string directory, std::size_t maximum_bytes, unsigned backups,
                             bool debug)
    : impl_(new Impl) {
    auto &p = *impl_;
    p.maximum = maximum_bytes;
    p.backups = backups;
    p.debug = debug;
    try {
        if (directory.empty()) {
            QString root = qEnvironmentVariable("XDG_STATE_HOME");
            if (root.isEmpty() || !QDir::isAbsolutePath(root))
                root = QDir::homePath() + "/.local/state";
            directory = (root + "/lunaviewer/logs").toStdString();
        }
        const auto dir = QDir(QString::fromStdString(directory)).absolutePath();
        p.filename = (dir + "/session.log").toStdString();
        if (maximum_bytes < 64 ||
            maximum_bytes > static_cast<std::size_t>(std::numeric_limits<off_t>::max()) ||
            backups > 100)
            throw std::runtime_error("Invalid log size or backup count");
        make_directory(dir);
        p.open_file();
    } catch (const std::exception &e) {
        p.disable(e.what());
    }
}
SessionLogger::~SessionLogger() = default;
void SessionLogger::write(const std::string &level, const std::string &category,
                          const std::string &message) noexcept {
    try {
        auto &p = *impl_;
        std::lock_guard<std::mutex> lock(p.mutex);
        if (p.fd < 0 || (level == "DEBUG" && !p.debug))
            return;
        const auto stamp =
            QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd'T'HH:mm:ss.zzz'Z'").toStdString();
        std::string line = stamp + " [" + clean(level, 16) + "] [" + clean(category, 64) + "] " +
                           clean(message, 8192);
        const auto limit = std::min<std::size_t>(p.maximum, 8192) - 1;
        if (line.size() > limit) {
            static const std::string marker = " [truncated]";
            line.resize(limit - marker.size());
            line += marker;
        }
        line += '\n';
        if (p.bytes > p.maximum - line.size())
            p.rotate();
        std::size_t offset = 0;
        while (offset < line.size()) {
            const auto count = ::write(p.fd, line.data() + offset, line.size() - offset);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                throw std::runtime_error(system_error("Write session log"));
            offset += static_cast<std::size_t>(count);
            p.bytes += static_cast<std::size_t>(count);
        }
    } catch (const std::exception &e) {
        try {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->disable(e.what());
        } catch (...) {
        }
    } catch (...) {
        try {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->disable("Unexpected logging failure");
        } catch (...) {
        }
    }
}
std::string SessionLogger::path() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->filename;
}
bool SessionLogger::available() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->fd >= 0;
}
std::string SessionLogger::error() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->failure;
}
} // namespace lunaviewer
