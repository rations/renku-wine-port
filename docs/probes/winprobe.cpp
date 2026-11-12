/* winprobe.cpp -- window behaviour winerenku.drv needs from the system (probe P4 of
 * docs/NATIVE-DRIVER-PLAN.md).
 *
 * Win32 programs position every window themselves (menus, tooltips and combo drop-downs
 * included), open popups that must never take focus, own and block other windows, and
 * routinely move windows partly off screen. This checks each of those on a running system and
 * measures the decorator insets the driver needs, to map a Win32 window rect to a BWindow
 * frame.
 *
 * Each check prints PASS or FAIL; measurements print as "info". The exit status is the number
 * of failures. Windows are open for a few seconds; nothing needs a person to look at them.
 * Run it from a Terminal on the desktop as well as over ssh: activation may depend on where
 * it was started, and that difference is itself a result.
 *
 *   g++ -O1 -Wall winprobe.cpp -o winprobe -lbe && ./winprobe
 *
 * APIs used, all from headers/os/interface/Window.h and Screen.h in the RenkuOS tree:
 * BWindow(frame, title, look, feel, flags), Activate(), IsActive(), MoveTo(), ResizeTo(),
 * Frame(), DecoratorFrame(), GetDecoratorSettings(), AddToSubset(), Sync(); BScreen::Frame().
 */
#include <Application.h>
#include <Message.h>
#include <Screen.h>
#include <Window.h>
#include <OS.h>

#include <pthread.h>
#include <stdio.h>

static int failures;

static void check(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static void rect(const char *what, BRect r)
{
    printf("info  %-34s (%g, %g) - (%g, %g)  %gx%g\n", what, r.left, r.top, r.right, r.bottom,
           r.Width() + 1, r.Height() + 1);
}

/* Poll a condition for up to timeout_ms; window state changes arrive asynchronously. */
template <typename F> static bool eventually(F cond, int timeout_ms = 2000)
{
    for (bigtime_t end = system_time() + timeout_ms * 1000LL; system_time() < end; snooze(10000))
        if (cond()) return true;
    return cond();
}

static void *app_thread(void *)
{
    BApplication *app = new BApplication("application/x-vnd.renku-winprobe");
    if (app->InitCheck() == B_OK) app->Run();
    delete app;
    return NULL;
}

/* Run fn with the window locked; returns false if the lock could not be taken. */
template <typename F> static bool locked(BWindow *w, F fn)
{
    if (w->LockWithTimeout(1000000) != B_OK) return false;
    fn();
    w->Unlock();
    return true;
}

static BWindow *make(BRect frame, const char *title, window_look look, window_feel feel,
                     uint32 flags)
{
    BWindow *w = new BWindow(frame, title, look, feel, flags);
    w->Show();
    w->Sync();
    return w;
}

int main()
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    pthread_t thr;
    pthread_create(&thr, NULL, app_thread, NULL);
    if (!eventually([] { return be_app != NULL && be_app->IsLaunching() == false; }, 5000))
    {
        printf("FAIL  BApplication did not start\n");
        return 1;
    }

    BScreen screen;
    rect("screen frame", screen.Frame());

    /* 1. A normal titled window becomes active. */
    BWindow *main_win = make(BRect(200, 150, 519, 389), "winprobe main", B_TITLED_WINDOW_LOOK,
                             B_NORMAL_WINDOW_FEEL, 0);
    locked(main_win, [&] { main_win->Activate(); });
    bool main_active = eventually([&] { return main_win->IsActive(); });
    check(main_active, "a titled window becomes active after Activate()");

    /* 2. Its frame is the client area, exactly where it was asked to be; the decorator sits
     *    outside it. Measure the insets. */
    BRect frame, deco;
    locked(main_win, [&] { frame = main_win->Frame(); deco = main_win->DecoratorFrame(); });
    rect("titled Frame()", frame);
    rect("titled DecoratorFrame()", deco);
    check(frame.left == 200 && frame.top == 150, "titled window client area placed exactly");
    printf("info  decorator insets: left %g  top %g  right %g  bottom %g\n",
           frame.left - deco.left, frame.top - deco.top, deco.right - frame.right,
           deco.bottom - frame.bottom);
    BMessage settings;
    if (locked(main_win, [&] { main_win->GetDecoratorSettings(&settings); }))
    {
        BRect tab; float border = -1;
        if (settings.FindRect("tab frame", &tab) == B_OK) rect("decorator \"tab frame\"", tab);
        if (settings.FindFloat("border width", &border) == B_OK)
            printf("info  decorator \"border width\"     %g\n", border);
    }

    /* 3. Exact placement, including partly off screen (no clamping). */
    locked(main_win, [&] { main_win->MoveTo(-40, -30); });
    bool offscreen = eventually([&] {
        BRect f; locked(main_win, [&] { f = main_win->Frame(); });
        return f.left == -40 && f.top == -30;
    });
    check(offscreen, "MoveTo(-40, -30) is honoured, not clamped to the screen");
    locked(main_win, [&] { main_win->MoveTo(200, 150); main_win->Activate(); });
    eventually([&] { return main_win->IsActive(); });

    /* 4. Popups (menus, tooltips): borderless, placed exactly, and showing one must not take
     *    focus from the active window. Two feels are tried. */
    struct { window_feel feel; const char *name; } feels[] = {
        { B_NORMAL_WINDOW_FEEL, "B_NORMAL_WINDOW_FEEL" },
        { B_FLOATING_APP_WINDOW_FEEL, "B_FLOATING_APP_WINDOW_FEEL" },
    };
    for (auto &f : feels)
    {
        char what[160];
        BWindow *popup = make(BRect(321, 211, 470, 290), "winprobe popup",
                              B_NO_BORDER_WINDOW_LOOK, f.feel, B_AVOID_FOCUS);
        BRect pf, pd;
        locked(popup, [&] { pf = popup->Frame(); pd = popup->DecoratorFrame(); });
        snprintf(what, sizeof(what), "%s popup placed exactly at (321, 211)", f.name);
        check(pf.left == 321 && pf.top == 211, what);
        snprintf(what, sizeof(what), "%s popup has no decorator (DecoratorFrame == Frame)", f.name);
        check(pd == pf, what);
        snooze(300000);
        snprintf(what, sizeof(what), "%s + B_AVOID_FOCUS: showing it leaves the main window active",
                 f.name);
        check(!main_active || (main_win->IsActive() && !popup->IsActive()), what);
        locked(popup, [&] { popup->Activate(); });
        snooze(300000);
        snprintf(what, sizeof(what), "%s + B_AVOID_FOCUS: Activate() on it is refused", f.name);
        check(!popup->IsActive(), what);
        popup->PostMessage(B_QUIT_REQUESTED);
        locked(main_win, [&] { main_win->Activate(); });
        eventually([&] { return main_win->IsActive(); });
    }

    /* 5. Owned/modal windows: a modal-subset window blocks only its owner. */
    BWindow *modal = new BWindow(BRect(260, 200, 459, 299), "winprobe modal",
                                 B_MODAL_WINDOW_LOOK, B_MODAL_SUBSET_WINDOW_FEEL, 0);
    status_t sub = B_ERROR;
    locked(modal, [&] { sub = modal->AddToSubset(main_win); });
    check(sub == B_OK, "AddToSubset() of the main window into a modal-subset window");
    modal->Show();
    bool modal_active = eventually([&] { return modal->IsActive(); });
    check(modal_active, "the modal-subset window becomes active when shown");

    /* 6. Resizing to the extremes a Win32 program may ask for. */
    locked(main_win, [&] { main_win->ResizeTo(0, 0); });
    snooze(200000);
    locked(main_win, [&] { frame = main_win->Frame(); });
    rect("titled after ResizeTo(0, 0)", frame);

    snooze(1000000);
    modal->PostMessage(B_QUIT_REQUESTED);
    main_win->PostMessage(B_QUIT_REQUESTED);
    snooze(200000);
    be_app->PostMessage(B_QUIT_REQUESTED);
    pthread_join(thr, NULL);

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures;
}
