#ifndef SVMS_CONTROL_SERVER_H
#define SVMS_CONTROL_SERVER_H

// POSIX transport for the Control API (see SVMSControlRegistry.h): a Unix
// stream socket at $XDG_RUNTIME_DIR/svms/svms-<pid>.sock (fallback
// /tmp/svms-<uid>/), 0700 directory + 0600 socket, one poll() thread serving
// every client. Registry getters/setters run on that thread; they must only
// touch atomics / mailboxes (the audio thread applies changes at block start).

#include "driver/SVMSControlRegistry.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace svms::ctl {

// Directory holding every running driver's socket (clients scan svms-*.sock).
std::string SocketDirectory();

class ControlServer {
public:
    ControlServer(Registry& registry, std::string product, std::string version)
        : registry_(registry), product_(std::move(product)),
          version_(std::move(version)) {}
    ~ControlServer() { Stop(); }

    bool Start(std::string& error);
    void Stop();
    const std::string& Path() const { return path_; }

private:
    struct Client {
        int fd = -1;
        std::string inbox;
        std::string outbox;
        int hz = 0;
        double nextMetricsMs = 0.0;
    };
    void Run();
    void Queue(Client& c, const std::string& line) { c.outbox += line; c.outbox += '\n'; }

    Registry& registry_;
    std::string product_, version_, path_;
    int listenFd_ = -1;
    int wakeFd_[2] = {-1, -1};
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::vector<Client> clients_;
};

} // namespace svms::ctl

#endif
