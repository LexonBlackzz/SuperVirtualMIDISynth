#include "SVMSControlServer.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace svms::ctl {

namespace {

constexpr size_t kMaxLine = 64u * 1024u;
constexpr size_t kMaxOutbox = 4u * 1024u * 1024u;  // a stuck client is dropped, never blocks

void SetNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

double NowMs() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

std::string SocketDirectory() {
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"))
        if (*runtime) return std::string(runtime) + "/svms";
    return "/tmp/svms-" + std::to_string(unsigned(getuid()));
}

bool ControlServer::Start(std::string& error) {
    if (running_.load()) return true;
    const std::string dir = SocketDirectory();
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        error = "cannot create " + dir + ": " + std::strerror(errno);
        return false;
    }
    // Refuse a directory someone else owns (the /tmp fallback is shared).
    struct stat st{};
    if (stat(dir.c_str(), &st) != 0 || st.st_uid != getuid()) {
        error = dir + " is not owned by this user";
        return false;
    }
    path_ = dir + "/svms-" + std::to_string(long(getpid())) + ".sock";
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path_.size() >= sizeof(addr.sun_path)) {
        error = "socket path too long: " + path_;
        return false;
    }
    std::memcpy(addr.sun_path, path_.c_str(), path_.size() + 1u);
    unlink(path_.c_str());  // our own pid's stale leftover
    listenFd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd_ < 0 ||
        bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        chmod(path_.c_str(), 0600) != 0 || listen(listenFd_, 8) != 0 ||
        pipe(wakeFd_) != 0) {
        error = std::string("control socket: ") + std::strerror(errno);
        if (listenFd_ >= 0) close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    SetNonBlocking(listenFd_);
    SetNonBlocking(wakeFd_[0]);
    running_.store(true);
    thread_ = std::thread(&ControlServer::Run, this);
    return true;
}

void ControlServer::Stop() {
    if (!running_.exchange(false)) return;
    const char byte = 0;
    if (write(wakeFd_[1], &byte, 1) < 0) {}
    if (thread_.joinable()) thread_.join();
    for (Client& c : clients_) close(c.fd);
    clients_.clear();
    close(listenFd_);
    close(wakeFd_[0]);
    close(wakeFd_[1]);
    listenFd_ = wakeFd_[0] = wakeFd_[1] = -1;
    unlink(path_.c_str());
}

void ControlServer::Run() {
    while (running_.load()) {
        std::vector<pollfd> fds;
        fds.push_back({wakeFd_[0], POLLIN, 0});
        fds.push_back({listenFd_, POLLIN, 0});
        for (const Client& c : clients_)
            fds.push_back({c.fd, short(POLLIN | (c.outbox.empty() ? 0 : POLLOUT)), 0});
        if (poll(fds.data(), nfds_t(fds.size()), 20) < 0 && errno != EINTR) break;
        if (!running_.load()) break;

        if (fds[1].revents & POLLIN) {
            const int fd = accept(listenFd_, nullptr, nullptr);
            if (fd >= 0) {
                SetNonBlocking(fd);
                Client c;
                c.fd = fd;
                Queue(c, registry_.Hello(product_, version_, long(getpid()), "linux"));
                clients_.push_back(std::move(c));
                fds.push_back({fd, 0, 0});  // keep indices aligned for this pass
            }
        }

        std::vector<std::string> broadcast;
        std::vector<bool> drop(clients_.size(), false);
        for (size_t i = 0; i < clients_.size() && i + 2u < fds.size(); ++i) {
            Client& c = clients_[i];
            const short ev = fds[i + 2u].revents;
            if (ev & (POLLERR | POLLNVAL)) { drop[i] = true; continue; }
            if (ev & (POLLIN | POLLHUP)) {
                char buf[4096];
                const ssize_t n = read(c.fd, buf, sizeof(buf));
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                    drop[i] = true;
                    continue;
                }
                if (n > 0) c.inbox.append(buf, size_t(n));
                if (c.inbox.size() > kMaxLine && c.inbox.find('\n') == std::string::npos) {
                    drop[i] = true;
                    continue;
                }
                size_t nl;
                while ((nl = c.inbox.find('\n')) != std::string::npos) {
                    const std::string line = c.inbox.substr(0, nl);
                    c.inbox.erase(0, nl + 1u);
                    Reply reply;
                    registry_.Handle(line, reply);
                    for (const std::string& s : reply.toSender) Queue(c, s);
                    for (std::string& s : reply.broadcast) broadcast.push_back(std::move(s));
                    if (reply.subscribeHz >= 0) {
                        c.hz = reply.subscribeHz;
                        c.nextMetricsMs = 0.0;
                    }
                }
            }
        }
        for (const std::string& line : broadcast)
            for (Client& c : clients_) Queue(c, line);

        const double now = NowMs();
        std::string metrics;
        for (Client& c : clients_) {
            if (c.hz <= 0 || now < c.nextMetricsMs) continue;
            if (metrics.empty()) metrics = registry_.MetricsLine();
            Queue(c, metrics);
            c.nextMetricsMs = now + 1000.0 / c.hz;
        }

        for (size_t i = 0; i < clients_.size(); ++i) {
            Client& c = clients_[i];
            while (!c.outbox.empty()) {
                const ssize_t n = send(c.fd, c.outbox.data(), c.outbox.size(), MSG_NOSIGNAL);
                if (n > 0) { c.outbox.erase(0, size_t(n)); continue; }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                if (n < 0 && errno == EINTR) continue;
                if (i < drop.size()) drop[i] = true;
                break;
            }
            if (c.outbox.size() > kMaxOutbox && i < drop.size()) drop[i] = true;
        }
        for (size_t i = clients_.size(); i-- > 0;) {
            if (i < drop.size() && drop[i]) {
                close(clients_[i].fd);
                clients_.erase(clients_.begin() + long(i));
            }
        }
    }
}

} // namespace svms::ctl
