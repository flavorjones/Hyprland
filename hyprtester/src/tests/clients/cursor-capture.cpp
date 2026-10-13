#include "../../Log.hpp"
#include "../../hyprctlCompat.hpp"
#include "../../shared.hpp"
#include "../shared.hpp"
#include "build.hpp"
#include "tests.hpp"

#include <array>
#include <csignal>
#include <format>
#include <string>
#include <sys/poll.h>
#include <unistd.h>

#include <hyprutils/os/FileDescriptor.hpp>
#include <hyprutils/os/Process.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using namespace Hyprutils::OS;
using namespace Hyprutils::Utils;

static bool waitForLine(const CFileDescriptor& readFd, const std::string& line) {
    pollfd pfd = {.fd = readFd.get(), .events = POLLIN, .revents = 0};
    if (poll(&pfd, 1, 5000) != 1 || !(pfd.revents & POLLIN))
        return false;

    std::array<char, 64> buf = {};
    if (read(readFd.get(), buf.data(), buf.size() - 1) <= 0)
        return false;

    return std::string{buf.data()}.contains(line);
}

TEST_CASE(cursorCaptureSurvivesOutputRemoval) {
    constexpr auto OUTPUT = "HEADLESS-3";

    ASSERT(getFromSocket(std::format("/output create headless {}", OUTPUT)), "ok");

    CScopeGuard outputGuard = {[&]() {
        getFromSocket(std::format("/output remove {}", OUTPUT));
        getFromSocket("/setcursor 24");
    }};

    CProcess    client(std::format("{}/cursor-capture", binaryDir), {OUTPUT});
    client.addEnv("WAYLAND_DISPLAY", WLDISPLAY);

    int pipeFds[2] = {};
    if (pipe(pipeFds) != 0)
        FAIL_TEST("Unable to open pipe to client");

    CFileDescriptor readFd(pipeFds[0]);
    client.setStdoutFD(pipeFds[1]);
    const bool LAUNCHED = client.runAsync();
    close(pipeFds[1]);
    if (!LAUNCHED)
        FAIL_TEST("Unable to launch cursor-capture client");

    CScopeGuard clientGuard = {[&]() {
        if (client.pid() > 0)
            kill(client.pid(), SIGKILL);
    }};

    if (!waitForLine(readFd, "pending"))
        FAIL_TEST("cursor-capture client did not leave a cursor frame pending");

    ASSERT(getFromSocket(std::format("/output remove {}", OUTPUT)), "ok");
    ASSERT(getFromSocket("/setcursor 48"), "ok");

    if (!waitForLine(readFd, "failed"))
        FAIL_TEST("cursor-capture client did not receive failed for its pending frame");

    ASSERT_CONTAINS(getFromSocket("/monitors"), "HEADLESS-2");
}
