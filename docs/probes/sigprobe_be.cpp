/* sigprobe_be.cpp -- Be API half of probe P3 (see sigprobe.c). Built with plain g++, no Wine
 * headers: it only sees sigprobe.h.
 *
 * Facts it relies on, read from the RenkuOS source:
 *   - BApplication must be constructed and Run() on the same thread (BLooper::Loop() asserts
 *     the lock, src/kits/app/Looper.cpp:501);
 *   - BWindow::Show() calls Run() the first time, and BLooper::Run() spawns the looper thread
 *     from the calling thread (src/kits/interface/Window.cpp, BWindow::Show;
 *     src/kits/app/Looper.cpp:480);
 *   - a new user thread starts with its creator's signal mask
 *     (src/system/kernel/thread.cpp:694).
 */
#include "sigprobe.h"

#include <Application.h>
#include <Bitmap.h>
#include <View.h>
#include <Window.h>
#include <OS.h>

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { MSG_MASK = 'mask', MSG_PING = 'ping', MSG_FAULT = 'flt!' };

static int event_pipe[2];

static void post_event(char ev)
{
    if (write(event_pipe[1], &ev, 1) != 1) perror("write event pipe");
}

extern "C" void be_async_signals(sigset_t *set)
{
    sigemptyset(set);
    sigaddset(set, SIGALRM);
    sigaddset(set, SIGIO);
    sigaddset(set, SIGINT);
    sigaddset(set, SIGHUP);
    sigaddset(set, SIGQUIT);
    sigaddset(set, SIGUSR1);
    sigaddset(set, SIGUSR2);
    sigaddset(set, SIGCHLD);
}

static sigset_t app_mask, win_mask;

static void read_mask(sigset_t *out)
{
    pthread_sigmask(SIG_BLOCK, NULL, out);
    post_event(EV_MASK);
}

class ProbeApp : public BApplication {
public:
    ProbeApp() : BApplication("application/x-vnd.renku-sigprobe") {}
    void ReadyToRun() override { post_event(EV_READY); }
    void MessageReceived(BMessage *msg) override
    {
        if (msg->what == MSG_MASK) { read_mask(&app_mask); return; }
        BApplication::MessageReceived(msg);
    }
};

class SurfaceView : public BView {
public:
    SurfaceView(BRect frame, BBitmap *bitmap)
        : BView(frame, "surface", B_FOLLOW_ALL, B_WILL_DRAW), fBitmap(bitmap)
    {
        SetViewColor(B_TRANSPARENT_COLOR);
    }
    void Draw(BRect update) override
    {
        DrawBitmapAsync(fBitmap, update, update);
        post_event(EV_DRAW);
    }
    BBitmap *fBitmap;
};

class SurfaceWindow : public BWindow {
public:
    SurfaceWindow(BRect frame)
        : BWindow(frame, "sigprobe", B_TITLED_WINDOW, B_NOT_RESIZABLE | B_NOT_ZOOMABLE) {}
    void MessageReceived(BMessage *msg) override
    {
        switch (msg->what)
        {
        case MSG_MASK: read_mask(&win_mask); return;
        case MSG_PING: post_event(EV_PING); return;
        case MSG_FAULT: *(volatile int *)0 = 1; return;
        }
        BWindow::MessageReceived(msg);
    }
};

static ProbeApp *app;
static status_t app_init = B_ERROR;
static pthread_t app_pthread;
static BBitmap *bitmap;
static SurfaceWindow *window;

static void *app_thread(void *)
{
    app = new ProbeApp();                 /* constructed and run on the same thread */
    app_init = app->InitCheck();
    if (app_init == B_OK) app->Run();
    else post_event(EV_READY);
    delete app;
    return NULL;
}

extern "C" int be_event_fd(void) { return event_pipe[0]; }

extern "C" int be_wait_for(char want, int timeout_ms, long long *latency_us)
{
    bigtime_t start = system_time(), deadline = start + (bigtime_t)timeout_ms * 1000;
    for (;;)
    {
        bigtime_t left = (deadline - system_time()) / 1000;
        if (left <= 0) return 0;
        struct pollfd pfd = { event_pipe[0], POLLIN, 0 };
        int r = poll(&pfd, 1, (int)left);
        if (r < 0 && errno == EINTR) continue;   /* a Wine thread may take a signal here */
        if (r != 1) return 0;
        char ev = 0;
        if (read(event_pipe[0], &ev, 1) != 1) return 0;
        if (ev == want)
        {
            if (latency_us) *latency_us = system_time() - start;
            return 1;
        }
    }
}

/* Rule 3: block Wine's asynchronous signals around fn, so the threads it spawns start with
 * them blocked, then restore the caller's mask. */
template <typename F> static void spawning(int block, F fn)
{
    sigset_t set, old;
    if (block)
    {
        be_async_signals(&set);
        pthread_sigmask(SIG_BLOCK, &set, &old);
    }
    fn();
    if (block) pthread_sigmask(SIG_SETMASK, &old, NULL);
}

extern "C" int be_start(int block)
{
    if (pipe(event_pipe) != 0) { perror("pipe"); return -1; }

    int created = -1;
    spawning(block, [&] { created = pthread_create(&app_pthread, NULL, app_thread, NULL); });
    if (created != 0) return -1;
    if (!be_wait_for(EV_READY, 5000, NULL) || app_init != B_OK) return -1;

    const int W = 160, H = 100;
    bitmap = new BBitmap(BRect(0, 0, W - 1, H - 1), B_RGB32);
    if (bitmap->InitCheck() != B_OK) return -1;
    for (int y = 0; y < H; y++)
    {
        uint8_t *row = (uint8_t *)bitmap->Bits() + y * bitmap->BytesPerRow();
        for (int x = 0; x < W; x++)
        {
            row[x * 4 + 0] = 0x20;
            row[x * 4 + 1] = (uint8_t)(x * 255 / W);
            row[x * 4 + 2] = (uint8_t)(y * 255 / H);
            row[x * 4 + 3] = 0xff;
        }
    }
    window = new SurfaceWindow(BRect(100, 100, 100 + W - 1, 100 + H - 1));
    window->AddChild(new SurfaceView(window->Bounds(), bitmap));
    spawning(block, [&] { window->Show(); });   /* the first Show() spawns the looper */
    return be_wait_for(EV_DRAW, 5000, NULL) ? 0 : -1;
}

extern "C" int be_looper_masks(sigset_t *app_out, sigset_t *win_out)
{
    if (be_app->PostMessage(MSG_MASK) != B_OK || !be_wait_for(EV_MASK, 2000, NULL)) return -1;
    if (window->PostMessage(MSG_MASK) != B_OK || !be_wait_for(EV_MASK, 2000, NULL)) return -1;
    *app_out = app_mask;
    *win_out = win_mask;
    return 0;
}

extern "C" int be_window_thread(void) { return window->Thread(); }

extern "C" int be_ping(int timeout_ms)
{
    return window->PostMessage(MSG_PING) == B_OK && be_wait_for(EV_PING, timeout_ms, NULL);
}

extern "C" int be_signal_window(int sig)
{
    return send_signal(window->Thread(), sig) == B_OK ? 0 : -1;
}

extern "C" void be_fault_on_window(void) { window->PostMessage(MSG_FAULT); }

extern "C" int be_stop(void)
{
    window->PostMessage(B_QUIT_REQUESTED);
    snooze(100000);
    delete bitmap;                        /* before the app goes: rule 4 */
    be_app->PostMessage(B_QUIT_REQUESTED);
    return pthread_join(app_pthread, NULL);
}
