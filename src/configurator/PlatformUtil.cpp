#include "configurator/PlatformUtil.h"

#ifdef _WIN32
#include <shellapi.h>
#else
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace svms::cfg::util {

#ifdef _WIN32

bool OpenExternal(const std::string& urlOrPath) {
    const std::wstring target = Utf8ToWide(urlOrPath);
    if (target.empty()) return false;
    return reinterpret_cast<INT_PTR>(ShellExecuteW(
        nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

#else

namespace {

bool InPath(const char* program) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string list(path);
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(':', start);
        if (end == std::string::npos) end = list.size();
        const std::string candidate =
            list.substr(start, end - start) + "/" + program;
        if (access(candidate.c_str(), X_OK) == 0) return true;
        start = end + 1;
    }
    return false;
}

// Single-quotes a string for /bin/sh.
std::string Quote(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "'\''";
        else out += c;
    }
    return out + "'";
}

} // namespace

bool OpenExternal(const std::string& urlOrPath) {
    if (urlOrPath.empty() || !InPath("xdg-open")) return false;
    // Double fork so the opener never becomes a zombie of ours.
    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        if (fork() == 0) {
            // Detach from our terminal; xdg-open chatter is not the user's business.
            if (freopen("/dev/null", "w", stdout) && freopen("/dev/null", "w", stderr)) {}
            execlp("xdg-open", "xdg-open", urlOrPath.c_str(), static_cast<char*>(nullptr));
        }
        _exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return true;
}

bool HaveFilePicker() { return InPath("kdialog") || InPath("zenity"); }

bool PickFile(bool save, const std::string& title, const std::string& startDir,
              const std::string& patterns, std::string& path) {
    std::string command;
    if (InPath("kdialog")) {
        command = std::string("kdialog ") +
                  (save ? "--getsavefilename " : "--getopenfilename ") +
                  Quote(startDir.empty() ? std::string(".") : startDir) + " " +
                  Quote(patterns) + " --title " + Quote(title);
    } else if (InPath("zenity")) {
        command = std::string("zenity --file-selection ") +
                  (save ? "--save --confirm-overwrite " : "") +
                  "--title=" + Quote(title) +
                  (startDir.empty() ? "" : " --filename=" + Quote(startDir + "/")) +
                  " --file-filter=" + Quote("Files | " + patterns);
    } else {
        return false;
    }
    command += " 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return false;
    std::string out;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe)) out += buffer;
    const int rc = pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (rc != 0 || out.empty()) return false;
    path = out;
    return true;
}

#endif

} // namespace svms::cfg::util
