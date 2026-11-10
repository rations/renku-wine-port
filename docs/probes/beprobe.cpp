/* beprobe.cpp -- can a Wine graphics driver run the Be API the way winerenku.drv plans to?
 *
 * Probes P1 and P2 of docs/NATIVE-DRIVER-PLAN.md, outside Wine. In the driver:
 *   - the BApplication lives on a thread the driver starts itself, never on main(): Wine owns
 *     the process's main thread;
 *   - Win32 threads (here: main()) create, lock and draw into BWindows from outside them;
 *   - BWindow looper threads report events by writing to a pipe that a Win32 thread is
 *     poll()ing -- what Wine's message queue does with the fd handed to set_queue_fd.
 *
 * Each check prints PASS or FAIL; the exit status is the number of failures. It opens one
 * small window for about a second, but nothing here needs a person to look at it.
 *
 *   g++ -O1 -Wall beprobe.cpp -o beprobe -lbe && ./beprobe
 *
 * Facts it relies on, read from the RenkuOS source:
 *   - BLooper::Loop() asserts the looper is locked (src/kits/app/Looper.cpp:501), and a looper
 *     is locked by the thread that constructs it, so BApplication must be constructed AND
 *     Run() on the same thread;
 *   - B_RGB32 is BGR- in memory (headers/os/interface/GraphicsDefs.h:176), the byte order of a
 *     32bpp top-down Windows DIB.
 */
#include <Application.h>
#include <Bitmap.h>
#include <View.h>
#include <Window.h>
#include <OS.h>

#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;
static int event_pipe[2];          /* [0] read by main(), [1] written by looper threads */

enum { EV_READY = 'r', EV_DRAW = 'd', EV_MSG = 'm' };
enum { MSG_PING = 'ping' };

static void check(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

/* Never blocks a looper on main(): one non-blocking write, like the driver's event queue. */
static void post_event(char ev)
{
    if (write(event_pipe[1], &ev, 1) != 1) perror("write event pipe");
}

/* Wait on the pipe the way a Win32 thread waits on its queue fd. Returns the event, or 0. */
static char wait_event(int timeout_ms, bigtime_t *latency_us)
{
    struct pollfd pfd = { event_pipe[0], POLLIN, 0 };
    bigtime_t start = system_time();
    if (poll(&pfd, 1, timeout_ms) != 1 || !(pfd.revents & POLLIN)) return 0;
    char ev = 0;
    if (read(event_pipe[0], &ev, 1) != 1) return 0;
    if (latency_us) *latency_us = system_time() - start;
    return ev;
}

/* Wait for one specific event, skipping others (e.g. extra Draw()s from app_server). */
static bool wait_for(char want, int timeout_ms, bigtime_t *latency_us)
{
    bigtime_t deadline = system_time() + (bigtime_t)timeout_ms * 1000;
    for (;;)
    {
        bigtime_t left = (deadline - system_time()) / 1000;
        if (left <= 0) return false;
        char ev = wait_event((int)left, latency_us);
        if (ev == want) return true;
        if (!ev) return false;
    }
}

class ProbeApp : public BApplication {
public:
    ProbeApp() : BApplication("application/x-vnd.renku-beprobe") {}
    void ReadyToRun() override { post_event(EV_READY); }
};

/* The view a driver window would use: it only ever blits the surface bitmap. */
class SurfaceView : public BView {
public:
    SurfaceView(BRect frame, BBitmap *bitmap)
        : BView(frame, "surface", B_FOLLOW_ALL, B_WILL_DRAW), fBitmap(bitmap)
    {
        SetViewColor(B_TRANSPARENT_COLOR);   /* no erase: the bitmap covers everything */
    }
    void Draw(BRect update) override
    {
        DrawBitmapAsync(fBitmap, update, update);
        draws++;
        post_event(EV_DRAW);
    }
    BBitmap *fBitmap;
    int32 draws = 0;
};

class SurfaceWindow : public BWindow {
public:
    SurfaceWindow(BRect frame)
        : BWindow(frame, "beprobe", B_TITLED_WINDOW, B_NOT_RESIZABLE | B_NOT_ZOOMABLE) {}
    void MessageReceived(BMessage *msg) override
    {
        if (msg->what == MSG_PING) { pinged_thread = find_thread(NULL); post_event(EV_MSG); return; }
        BWindow::MessageReceived(msg);
    }
    thread_id pinged_thread = -1;
};

static ProbeApp *app;
static thread_id app_thread_id = -1;
static status_t app_init = B_ERROR;

static void *app_thread(void *)
{
    app_thread_id = find_thread(NULL);
    app = new ProbeApp();                 /* constructed here ...                         */
    app_init = app->InitCheck();
    if (app_init == B_OK) app->Run();     /* ... and run here: same thread, see header     */
    else post_event(EV_READY);
    delete app;
    return NULL;
}

int main()
{
    setvbuf(stdout, NULL, _IOLBF, 0);   /* a crash must not swallow the results so far */
    if (pipe(event_pipe) != 0) { perror("pipe"); return 99; }
    thread_id main_thread = find_thread(NULL);

    /* P1a: BApplication on a thread that is not main(). */
    pthread_t thr;
    check(pthread_create(&thr, NULL, app_thread, NULL) == 0, "pthread_create for the app thread");
    bigtime_t lat = 0;
    bool ready = wait_for(EV_READY, 5000, &lat);
    check(ready && app_init == B_OK, "BApplication constructed and Run() on a non-main thread");
    check(app_thread_id != main_thread && be_app == app, "be_app is set and lives off main()");
    if (!ready || app_init != B_OK) return failures;

    /* P1b: a window created from main(), whose surface is a B_RGB32 bitmap filled with
     * DIB byte order (B, G, R, X) by main() -- the role of the window_surface flush. */
    const int W = 160, H = 100;
    BBitmap *bitmap = new BBitmap(BRect(0, 0, W - 1, H - 1), B_RGB32);
    check(bitmap->InitCheck() == B_OK && bitmap->BytesPerRow() >= W * 4, "B_RGB32 BBitmap created");
    for (int y = 0; y < H; y++)
    {
        uint8_t *row = (uint8_t *)bitmap->Bits() + y * bitmap->BytesPerRow();
        for (int x = 0; x < W; x++)
        {
            row[x * 4 + 0] = 0x20;                    /* B */
            row[x * 4 + 1] = (uint8_t)(x * 255 / W);  /* G */
            row[x * 4 + 2] = (uint8_t)(y * 255 / H);  /* R */
            row[x * 4 + 3] = 0xff;                    /* X */
        }
    }
    SurfaceWindow *window = new SurfaceWindow(BRect(100, 100, 100 + W - 1, 100 + H - 1));
    SurfaceView *view = new SurfaceView(window->Bounds(), bitmap);
    window->AddChild(view);
    window->Show();

    /* P2a: the looper thread's Draw() wakes main()'s poll() through the pipe. */
    check(wait_for(EV_DRAW, 5000, &lat), "first Draw() on the window thread woke poll() on main()");
    printf("      latency show -> first draw event: %lld us\n", (long long)lat);

    /* P1c: main() takes the window lock from outside, as a flush would, then invalidates. */
    status_t lock = window->LockWithTimeout(500000);
    check(lock == B_OK, "LockWithTimeout() from a non-looper thread");
    if (lock == B_OK)
    {
        memset(bitmap->Bits(), 0x80, bitmap->BytesPerRow() * 10);   /* "dirty" the top rows */
        view->Invalidate(BRect(0, 0, W - 1, 9));
        window->Unlock();
        check(wait_for(EV_DRAW, 2000, &lat), "Invalidate() from main() produced a Draw() event");
        printf("      latency invalidate -> draw event: %lld us\n", (long long)lat);
    }

    /* P1d: an asynchronous message from main() is handled on the window's own thread. */
    check(window->PostMessage(MSG_PING) == B_OK, "PostMessage() to the window from main()");
    check(wait_for(EV_MSG, 2000, &lat), "message handled and reported back through the pipe");
    check(window->pinged_thread == window->Thread() && window->Thread() != main_thread,
          "the message ran on the window's looper thread, not main()");

    snooze(1000000);   /* leave it up for a second; nothing checks this visually */

    /* Teardown: quit the window, free the bitmap, then quit the app, with no locks held
     * across threads. The order matters: a server-backed BBitmap freed after the
     * BApplication is gone drops into the debugger ("You need to have a valid app_server
     * connection first!", BBitmap::_CleanUp) -- found by this probe's first run. */
    window->PostMessage(B_QUIT_REQUESTED);
    snooze(100000);
    delete bitmap;
    be_app->PostMessage(B_QUIT_REQUESTED);
    check(pthread_join(thr, NULL) == 0, "app thread returned from Run() and joined");

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures;
}
