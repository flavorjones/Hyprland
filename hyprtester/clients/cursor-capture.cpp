#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/poll.h>
#include <unistd.h>
#include <vector>

#include <wayland-client.h>
#include <wayland.hpp>
#include <ext-image-capture-source-v1.hpp>
#include <ext-image-copy-capture-v1.hpp>

#include <hyprutils/memory/Casts.hpp>
#include <hyprutils/memory/SharedPtr.hpp>

using namespace Hyprutils::Memory;

// Opens a pointer cursor capture session on the named output, captures one
// frame, then requests a second one. The second frame stays pending until the
// cursor changes. Prints "pending" once it is, then keeps dispatching.

struct SOutput {
    CSharedPointer<CCWlOutput> resource;
    std::string                name;
};

struct SState {
    wl_display*                                            display = nullptr;
    CSharedPointer<CCWlRegistry>                           registry;
    CSharedPointer<CCWlShm>                                shm;
    CSharedPointer<CCWlSeat>                               seat;
    CSharedPointer<CCExtOutputImageCaptureSourceManagerV1> sourceMgr;
    CSharedPointer<CCExtImageCopyCaptureManagerV1>         copyMgr;
    std::vector<CSharedPointer<SOutput>>                   outputs;

    CSharedPointer<CCWlPointer>                            pointer;
    CSharedPointer<CCExtImageCaptureSourceV1>              source;
    CSharedPointer<CCExtImageCopyCaptureCursorSessionV1>   cursorSession;
    CSharedPointer<CCExtImageCopyCaptureSessionV1>         session;
    CSharedPointer<CCExtImageCopyCaptureFrameV1>           frame;
    CSharedPointer<CCWlShmPool>                            pool;
    CSharedPointer<CCWlBuffer>                             buffer;
    int                                                    bufferFD = -1;

    uint32_t                                               bufferFormat = 0;
    bool                                                   hasFormat    = false;
    uint32_t                                               bufferWidth  = 0;
    uint32_t                                               bufferHeight = 0;

    bool                                                   constraintsDone = false;
    bool                                                   frameReady      = false;
    bool                                                   frameFailed     = false;
};

static bool bindGlobals(SState& state) {
    state.registry = makeShared<CCWlRegistry>(rc<wl_proxy*>(wl_display_get_registry(state.display)));

    state.registry->setGlobal([&state](CCWlRegistry* r, uint32_t name, const char* iface, uint32_t ver) {
        const std::string_view IFACE = iface;

        if (IFACE == "wl_shm")
            state.shm = makeShared<CCWlShm>(rc<wl_proxy*>(wl_registry_bind(rc<wl_registry*>(r->resource()), name, &wl_shm_interface, 1)));
        else if (IFACE == "wl_seat")
            state.seat = makeShared<CCWlSeat>(rc<wl_proxy*>(wl_registry_bind(rc<wl_registry*>(r->resource()), name, &wl_seat_interface, 1)));
        else if (IFACE == "wl_output" && ver >= 4) {
            const auto OUTPUT = makeShared<SOutput>();
            OUTPUT->resource  = makeShared<CCWlOutput>(rc<wl_proxy*>(wl_registry_bind(rc<wl_registry*>(r->resource()), name, &wl_output_interface, 4)));
            auto* output      = OUTPUT.get();
            OUTPUT->resource->setName([output](CCWlOutput*, const char* outputName) { output->name = outputName; });
            state.outputs.emplace_back(OUTPUT);
        } else if (IFACE == "ext_output_image_capture_source_manager_v1")
            state.sourceMgr = makeShared<CCExtOutputImageCaptureSourceManagerV1>(
                rc<wl_proxy*>(wl_registry_bind(rc<wl_registry*>(r->resource()), name, &ext_output_image_capture_source_manager_v1_interface, 1)));
        else if (IFACE == "ext_image_copy_capture_manager_v1")
            state.copyMgr =
                makeShared<CCExtImageCopyCaptureManagerV1>(rc<wl_proxy*>(wl_registry_bind(rc<wl_registry*>(r->resource()), name, &ext_image_copy_capture_manager_v1_interface, 1)));
    });
    state.registry->setGlobalRemove([](CCWlRegistry*, uint32_t) {});

    if (wl_display_roundtrip(state.display) < 0 || wl_display_roundtrip(state.display) < 0)
        return false;

    return state.shm && state.seat && state.sourceMgr && state.copyMgr;
}

static CSharedPointer<CCWlOutput> findOutput(SState& state, const std::string& name) {
    for (const auto& output : state.outputs) {
        if (output->name == name)
            return output->resource;
    }
    return nullptr;
}

static bool createCaptureBuffer(SState& state) {
    const uint32_t STRIDE = state.bufferWidth * 4;
    const size_t   SIZE   = sc<size_t>(STRIDE) * state.bufferHeight;
    if (SIZE == 0)
        return false;

    const std::string NAME = std::format("/wl-shm-cursor-capture-{}", getpid());
    const int         FD   = shm_open(NAME.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (FD < 0)
        return false;
    if (shm_unlink(NAME.c_str()) < 0 || ftruncate(FD, sc<off_t>(SIZE)) < 0) {
        close(FD);
        return false;
    }

    state.bufferFD = FD;
    state.pool     = makeShared<CCWlShmPool>(state.shm->sendCreatePool(FD, sc<int32_t>(SIZE)));
    if (!state.pool || !state.pool->resource())
        return false;

    state.buffer =
        makeShared<CCWlBuffer>(state.pool->sendCreateBuffer(0, sc<int32_t>(state.bufferWidth), sc<int32_t>(state.bufferHeight), sc<int32_t>(STRIDE), state.bufferFormat));
    return state.buffer && state.buffer->resource();
}

static bool startCursorSession(SState& state, const CSharedPointer<CCWlOutput>& output) {
    state.pointer       = makeShared<CCWlPointer>(state.seat->sendGetPointer());
    state.source        = makeShared<CCExtImageCaptureSourceV1>(state.sourceMgr->sendCreateSource(rc<wl_proxy*>(output->resource())));
    state.cursorSession = makeShared<CCExtImageCopyCaptureCursorSessionV1>(
        state.copyMgr->sendCreatePointerCursorSession(rc<wl_proxy*>(state.source->resource()), rc<wl_proxy*>(state.pointer->resource())));
    state.session = makeShared<CCExtImageCopyCaptureSessionV1>(state.cursorSession->sendGetCaptureSession());

    if (!state.session || !state.session->resource())
        return false;

    state.session->setBufferSize([&state](CCExtImageCopyCaptureSessionV1*, uint32_t w, uint32_t h) {
        state.bufferWidth  = w;
        state.bufferHeight = h;
    });
    state.session->setShmFormat([&state](CCExtImageCopyCaptureSessionV1*, uint32_t format) {
        if (state.hasFormat)
            return;
        state.bufferFormat = format;
        state.hasFormat    = true;
    });
    state.session->setDmabufDevice([](CCExtImageCopyCaptureSessionV1*, wl_array*) {});
    state.session->setDmabufFormat([](CCExtImageCopyCaptureSessionV1*, uint32_t, wl_array*) {});
    state.session->setDone([&state](CCExtImageCopyCaptureSessionV1*) { state.constraintsDone = true; });
    state.session->setStopped([](CCExtImageCopyCaptureSessionV1*) {});

    return true;
}

static void requestFrame(SState& state) {
    state.frameReady  = false;
    state.frameFailed = false;
    state.frame.reset();
    state.frame = makeShared<CCExtImageCopyCaptureFrameV1>(state.session->sendCreateFrame());

    state.frame->setTransform([](CCExtImageCopyCaptureFrameV1*, uint32_t) {});
    state.frame->setDamage([](CCExtImageCopyCaptureFrameV1*, int32_t, int32_t, int32_t, int32_t) {});
    state.frame->setPresentationTime([](CCExtImageCopyCaptureFrameV1*, uint32_t, uint32_t, uint32_t) {});
    state.frame->setReady([&state](CCExtImageCopyCaptureFrameV1*) { state.frameReady = true; });
    state.frame->setFailed([&state](CCExtImageCopyCaptureFrameV1*, uint32_t) { state.frameFailed = true; });

    state.frame->sendAttachBuffer(rc<wl_proxy*>(state.buffer->resource()));
    state.frame->sendDamageBuffer(0, 0, sc<int32_t>(state.bufferWidth), sc<int32_t>(state.bufferHeight));
    state.frame->sendCapture();
}

template <typename F>
static bool dispatchUntil(SState& state, F&& done, std::chrono::steady_clock::time_point deadline) {
    while (!done()) {
        while (wl_display_prepare_read(state.display) != 0) {
            if (wl_display_dispatch_pending(state.display) < 0)
                return false;
            if (done())
                return true;
        }

        if (wl_display_flush(state.display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(state.display);
            return false;
        }

        const auto NOW = std::chrono::steady_clock::now();
        if (NOW >= deadline) {
            wl_display_cancel_read(state.display);
            return false;
        }

        const auto REMAINING_MS = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - NOW).count();
        pollfd     pfd          = {.fd = wl_display_get_fd(state.display), .events = POLLIN, .revents = 0};
        const int  POLL_RET     = poll(&pfd, 1, sc<int>(REMAINING_MS));

        if (POLL_RET <= 0) {
            wl_display_cancel_read(state.display);
            if (POLL_RET < 0 && errno == EINTR)
                continue;
            return false;
        }

        if (wl_display_read_events(state.display) < 0)
            return false;
        if (wl_display_dispatch_pending(state.display) < 0)
            return false;
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::println(stderr, "usage: cursor-capture <output-name>");
        return 1;
    }

    SState state;
    state.display = wl_display_connect(nullptr);
    if (!state.display) {
        std::println(stderr, "failed to connect to Wayland display");
        return 1;
    }

    if (!bindGlobals(state)) {
        std::println(stderr, "wl_shm, wl_seat, ext_output_image_capture_source_manager_v1, or ext_image_copy_capture_manager_v1 unavailable");
        return 1;
    }

    const auto OUTPUT = findOutput(state, argv[1]);
    if (!OUTPUT) {
        std::string seen;
        for (const auto& output : state.outputs)
            seen += std::format(" '{}'", output->name);
        std::println(stderr, "no wl_output named '{}', saw:{}", argv[1], seen);
        return 1;
    }

    if (!startCursorSession(state, OUTPUT)) {
        std::println(stderr, "failed to create the cursor capture session");
        return 1;
    }

    const auto DEADLINE = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    if (!dispatchUntil(state, [&state] { return state.constraintsDone; }, DEADLINE) || !state.hasFormat) {
        std::println(stderr, "did not receive cursor capture constraints");
        return 1;
    }

    if (!createCaptureBuffer(state)) {
        std::println(stderr, "failed to allocate capture buffer");
        return 1;
    }

    requestFrame(state);
    if (!dispatchUntil(state, [&state] { return state.frameReady || state.frameFailed; }, DEADLINE) || !state.frameReady) {
        std::println(stderr, "first cursor frame was not captured");
        return 1;
    }

    requestFrame(state);
    if (wl_display_roundtrip(state.display) < 0 || state.frameReady || state.frameFailed) {
        std::println(stderr, "second cursor frame did not stay pending");
        return 1;
    }

    std::println("pending");
    std::fflush(stdout);

    if (!dispatchUntil(state, [&state] { return state.frameReady || state.frameFailed; }, std::chrono::steady_clock::now() + std::chrono::seconds(10)) || !state.frameFailed) {
        std::println(stderr, "pending cursor frame did not fail");
        return 1;
    }

    std::println("failed");
    std::fflush(stdout);
    return 0;
}
