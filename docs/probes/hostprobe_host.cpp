/*
 * hostprobe_host: the native host for hostprobe (see hostprobe.c). It shows the Wine window
 * that hostprobe opted in to winerenku.drv's hosted mode in a BView of its own window, the way
 * a plug-in host shows a bridged editor, speaking the protocol in renku_host.h. It sends input
 * through the driver, reads what hostprobe.exe reports on its stdout, and prints PASS or FAIL
 * per check; lines it does not use (SCREEN ...) are passed on for screencheck. Exit status =
 * failures.
 *
 * Build (renku_host.h is in the dev tree's dlls/winerenku.drv):
 *   g++ -O1 -Wall -I$W/dlls/winerenku.drv hostprobe_host.cpp -o hostprobe_host -lbe
 * Run:   $W/loader/wine hostprobe.exe.so | ./hostprobe_host | ./screencheck
 */

#include <Application.h>
#include <Bitmap.h>
#include <Message.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <OS.h>
#include <View.h>
#include <Window.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "renku_host.h"

namespace {

enum
{
    MSG_LINE = 'hpLn',  /* "line": one line of hostprobe.exe's output */
    MSG_TICK = 'hpTk',
};

const int kW = 200, kH = 150;
const bigtime_t kStepTimeout = 4000000;

int failures;

void check(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    fflush(stdout);
    if (!ok) failures++;
}

class HostView : public BView
{
public:
    HostView(BRect frame, team_id team, uint64 cookie)
        : BView(frame, "hostprobe host view", B_FOLLOW_NONE, B_WILL_DRAW),
          fDriver(RENKU_HOST_SIGNATURE, team), fCookie(cookie)
    {
        SetViewColor(B_TRANSPARENT_COLOR);
    }

    ~HostView() override
    {
        delete fBitmap;
        if (fClone >= B_OK) delete_area(fClone);
    }

    void Draw(BRect update) override
    {
        if (fBitmap) DrawBitmap(fBitmap, BPoint(fX, fY));
        else
        {
            SetHighColor(0, 0, 0);
            FillRect(update);
        }
    }

    void MessageReceived(BMessage *msg) override
    {
        switch (msg->what)
        {
        case RENKU_HOST_SURFACE: OnSurface(msg); return;
        case RENKU_HOST_DIRTY: OnDirty(msg); return;
        case RENKU_HOST_GONE: fGone = true; return;
        case RENKU_HOST_REFUSED: fRefused = true; return;
        }
        BView::MessageReceived(msg);
    }

    void Send(BMessage *msg)
    {
        msg->AddInt64(RENKU_HOST_FIELD_COOKIE, (int64)fCookie);
        fDriver.SendMessage(msg, this, 1000000);
    }

    void Hello()
    {
        BMessage msg(RENKU_HOST_HELLO);
        msg.AddInt32(RENKU_HOST_FIELD_VERSION, RENKU_HOST_VERSION);
        msg.AddMessenger(RENKU_HOST_FIELD_HOST, BMessenger(this));
        Send(&msg);
    }

    BPoint Origin() { return ConvertToScreen(BPoint(0, 0)); }

    void Moved()
    {
        BPoint origin = Origin();
        BMessage msg(RENKU_HOST_MOVED);
        msg.AddInt32(RENKU_HOST_FIELD_X, (int32)origin.x);
        msg.AddInt32(RENKU_HOST_FIELD_Y, (int32)origin.y);
        Send(&msg);
    }

    void Focus(bool focus)
    {
        BMessage msg(RENKU_HOST_FOCUS);
        msg.AddBool(RENKU_HOST_FIELD_FOCUS, focus);
        Send(&msg);
    }

    void Click(int x, int y, int button)
    {
        BPoint at = ConvertToScreen(BPoint(x, y));
        BMessage move(RENKU_HOST_MOUSE_MOVE);
        move.AddInt32(RENKU_HOST_FIELD_X, (int32)at.x);
        move.AddInt32(RENKU_HOST_FIELD_Y, (int32)at.y);
        Send(&move);
        for (int down = 1; down >= 0; down--)
        {
            BMessage msg(RENKU_HOST_MOUSE_BUTTON);
            msg.AddInt32(RENKU_HOST_FIELD_X, (int32)at.x);
            msg.AddInt32(RENKU_HOST_FIELD_Y, (int32)at.y);
            msg.AddInt32(RENKU_HOST_FIELD_BUTTON, button);
            msg.AddBool(RENKU_HOST_FIELD_DOWN, down != 0);
            Send(&msg);
        }
    }

    void Key(int32 key)
    {
        for (int down = 1; down >= 0; down--)
        {
            BMessage msg(RENKU_HOST_KEY);
            msg.AddInt32(RENKU_HOST_FIELD_KEY, key);
            msg.AddBool(RENKU_HOST_FIELD_DOWN, down != 0);
            Send(&msg);
        }
    }

    void Wheel(float y)
    {
        BMessage msg(RENKU_HOST_MOUSE_WHEEL);
        msg.AddFloat(RENKU_HOST_FIELD_WHEEL_X, 0);
        msg.AddFloat(RENKU_HOST_FIELD_WHEEL_Y, y);
        Send(&msg);
    }

    /* The colour the host has at (x, y) of the window, as rrggbb; -1 if it has none there. */
    int32 Pixel(int x, int y)
    {
        if (!fBitmap || x < 0 || y < 0 || x >= fW || y >= fH) return -1;
        const uint8 *p = (const uint8 *)fBitmap->Bits() + y * fBitmap->BytesPerRow() + x * 4;
        return (p[2] << 16) | (p[1] << 8) | p[0];  /* B_RGB32 is B, G, R, X */
    }

    int surfaces = 0, dirties = 0;
    bool fGone = false, fRefused = false;

private:
    void Ack(int32 serial)
    {
        BMessage msg(RENKU_HOST_ACK);
        msg.AddInt32(RENKU_HOST_FIELD_SERIAL, serial);
        Send(&msg);
    }

    void CopyRows(int32 left, int32 top, int32 right, int32 bottom)
    {
        uint8 *dst = (uint8 *)fBitmap->Bits();
        int32 dstStride = fBitmap->BytesPerRow();
        for (int32 y = top; y < bottom; y++)
            memcpy(dst + y * dstStride + left * 4, fPixels + y * fStride + left * 4,
                   (right - left) * 4);
    }

    void OnSurface(BMessage *msg)
    {
        int32 serial = msg->GetInt32(RENKU_HOST_FIELD_SERIAL, -1);
        int32 area = msg->GetInt32(RENKU_HOST_FIELD_AREA, -1);
        int32 w = msg->GetInt32(RENKU_HOST_FIELD_WIDTH, 0), h = msg->GetInt32(RENKU_HOST_FIELD_HEIGHT, 0);
        int32 stride = msg->GetInt32(RENKU_HOST_FIELD_STRIDE, 0);
        area_info info;
        void *address;

        /* From another program: checked before anything is mapped or copied. */
        if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || stride < w * 4)
        {
            check(false, "surface message is sane");
            return;
        }
        if (fClone >= B_OK) delete_area(fClone);
        fClone = clone_area("hostprobe clone", &address, B_ANY_ADDRESS, B_READ_AREA, area);
        if (fClone < B_OK || get_area_info(fClone, &info) != B_OK ||
            info.size < (size_t)stride * h)
        {
            check(false, "the driver's area can be cloned and is big enough");
            return;
        }
        delete fBitmap;
        fBitmap = new BBitmap(BRect(0, 0, w - 1, h - 1), B_RGB32);
        fPixels = (const uint8 *)address;
        fW = w;
        fH = h;
        fStride = stride;
        fX = msg->GetInt32(RENKU_HOST_FIELD_X, 0);
        fY = msg->GetInt32(RENKU_HOST_FIELD_Y, 0);
        fSerial = serial;
        CopyRows(0, 0, w, h);
        surfaces++;
        Invalidate();
        Ack(serial);
    }

    void OnDirty(BMessage *msg)
    {
        int32 serial = msg->GetInt32(RENKU_HOST_FIELD_SERIAL, -1);
        int32 left = msg->GetInt32(RENKU_HOST_FIELD_X, 0), top = msg->GetInt32(RENKU_HOST_FIELD_Y, 0);
        int32 right = msg->GetInt32(RENKU_HOST_FIELD_RIGHT, 0);
        int32 bottom = msg->GetInt32(RENKU_HOST_FIELD_BOTTOM, 0);

        if (serial == fSerial && fBitmap)
        {
            if (left < 0) left = 0;
            if (top < 0) top = 0;
            if (right > fW) right = fW;
            if (bottom > fH) bottom = fH;
            if (left < right && top < bottom)
            {
                CopyRows(left, top, right, bottom);
                dirties++;
                Invalidate(BRect(left + fX, top + fY, right + fX - 1, bottom + fY - 1));
            }
        }
        Ack(serial);
    }

    BMessenger fDriver;
    uint64 fCookie;
    BBitmap *fBitmap = NULL;
    area_id fClone = -1;
    const uint8 *fPixels = NULL;
    int32 fW = 0, fH = 0, fStride = 0, fX = 0, fY = 0, fSerial = -1;
};

class ProbeWindow : public BWindow
{
public:
    ProbeWindow(team_id team, uint64 cookie)
        : BWindow(BRect(100, 100, 100 + kW + 19, 100 + kH + 79), "hostprobe host",
                  B_TITLED_WINDOW, B_NOT_RESIZABLE | B_QUIT_ON_WINDOW_CLOSE)
    {
        fView = new HostView(BRect(10, 10, 10 + kW - 1, 10 + kH - 1), team, cookie);
        AddChild(fView);
    }

    void Start()
    {
        BMessage tick(MSG_TICK);
        fTicker = new BMessageRunner(BMessenger(this), &tick, 50000);
        fView->Hello();
        fSince = system_time();
    }

    void MessageReceived(BMessage *msg) override
    {
        if (msg->what == MSG_LINE)
        {
            fLines.push_back(msg->GetString("line", ""));
            return;
        }
        if (msg->what == MSG_TICK)
        {
            Step();
            return;
        }
        BWindow::MessageReceived(msg);
    }

private:
    /* Whether hostprobe.exe printed this line (consumed if so). */
    bool Seen(const std::string &line)
    {
        for (size_t i = 0; i < fLines.size(); i++)
            if (fLines[i] == line)
            {
                fLines.erase(fLines.begin() + i);
                return true;
            }
        return false;
    }

    std::string Pos(BPoint p)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "POS %d %d", (int)p.x, (int)p.y);
        return buf;
    }

    void Screen(int x, int y, const char *color, const char *what)
    {
        BPoint p = fView->ConvertToScreen(BPoint(x, y));
        printf("SCREEN %d %d %s %s\n", (int)p.x, (int)p.y, color, what);
        fflush(stdout);
    }

    void Next()
    {
        fStep++;
        fSince = system_time();
    }

    /* Waits for cond, or times out with a FAIL; returns whether the step is over. */
    bool Wait(bool cond, const char *what)
    {
        if (cond) check(true, what);
        else if (system_time() - fSince > kStepTimeout) check(false, what);
        else return false;
        Next();
        return true;
    }

    bool Pause(bigtime_t delay)
    {
        if (system_time() - fSince < delay) return false;
        Next();
        return true;
    }

    void Step()
    {
        switch (fStep)
        {
        case 0:
            if (fView->fRefused) { check(false, "the driver accepts the hello"); fStep = 100; break; }
            if (!Wait(fView->surfaces > 0, "the driver sends the window's surface")) break;
            check(fView->Pixel(100, 10) == 0x00ff00 && fView->Pixel(100, 140) == 0xff0000,
                  "the host has the window's pixels (green over red)");
            fView->Moved();
            fView->Focus(true);
            break;
        case 1:
            Wait(Seen(Pos(fView->Origin())), "Wine places the window where the host shows it");
            break;
        case 2:
            if (!Pause(300000)) break;
            Screen(100, 10, "00ff00", "hosted window on screen, top");
            Screen(100, 140, "ff0000", "hosted window on screen, bottom");
            break;
        case 3:
            if (Wait(Seen("ACTIVATE 1"), "host focus activates the window"))
                fView->Click(20, 30, 0);
            break;
        case 4:
            if (Wait(Seen("LBUTTONDOWN 20 30"), "a click through the host lands at 20,30"))
                fView->Key(0x3c);  /* A, by position */
            break;
        case 5:
            Wait(Seen("KEYDOWN 0x41"), "a key through the host arrives as VK_A");
            break;
        case 6:
            /* Keymap-dependent: 'a' on US and German, 'q' on French. */
            Wait(Seen("CHAR 0x61"), "and types 'a' (US-type keymap)");
            break;
        case 7:
            if (!Wait(fView->dirties > 0 && fView->Pixel(100, 75) == 0x0000ff,
                      "a repaint reaches the host (blue)"))
                break;
            break;
        case 8:
            if (!Pause(300000)) break;
            Screen(100, 75, "0000ff", "repainted hosted window on screen");
            break;
        case 9:
            /* The GL child at the top right (hostprobe.c): drawn with GDI into the surface. */
            if (!Wait(fView->Pixel(kW - 25, 25) == 0xff00ff,
                      "a GL child of the hosted window reaches the host (magenta)"))
                break;
            fView->Wheel(1);
            break;
        case 10:
            if (Wait(Seen("WHEEL -120"), "a wheel notch down arrives as -120"))
            {
                MoveBy(60, 40);
                fView->Moved();
            }
            break;
        case 11:
            if (Wait(Seen(Pos(fView->Origin())), "Wine follows the host window's move"))
                fView->Click(50, 50, 1);
            break;
        case 12:
        {
            BPoint o = fView->Origin();
            char want[64];
            snprintf(want, sizeof(want), "POPUP %d %d", (int)o.x, (int)o.y + kH + 10);
            Wait(Seen(want), "the program's popup opens below the window, where the host shows it");
            break;
        }
        case 13:
            if (!Pause(500000)) break;
            /* The popup is a BWindow of its own, in front of the host's. */
            Screen(50, kH + 30, "ffff00", "the popup is on screen over the host's window");
            Screen(kW - 25, 25, "ff00ff", "GL child of the hosted window on screen");
            fView->Key(0x01);  /* Escape: the program destroys the window */
            break;
        case 14:
            Wait(fView->fGone, "the driver tells the host the window is gone");
            break;
        case 15:
            if (!Pause(2000000)) break;
            fStep = 100;
            break;
        case 100:
            delete fTicker;
            fTicker = NULL;
            be_app->PostMessage(B_QUIT_REQUESTED);
            fStep = 101;
            break;
        }
    }

    HostView *fView;
    BMessageRunner *fTicker = NULL;
    std::vector<std::string> fLines;
    int fStep = 0;
    bigtime_t fSince = 0;
};

class ProbeApp : public BApplication
{
public:
    ProbeApp() : BApplication("application/x-vnd.renku-hostprobe") {}

    void MessageReceived(BMessage *msg) override
    {
        if (msg->what != MSG_LINE)
        {
            BApplication::MessageReceived(msg);
            return;
        }
        const char *line = msg->GetString("line", "");
        int team;
        unsigned long cookie;

        if (sscanf(line, "HOSTPROBE %d %lu", &team, &cookie) == 2)
        {
            check(team > 0, "the driver gives the window a team property");
            if (team <= 0 || fWindow) return;
            fWindow = new ProbeWindow(team, cookie);
            fWindow->Show();
            if (fWindow->Lock())
            {
                fWindow->Start();
                fWindow->Unlock();
            }
            return;
        }
        if (!strncmp(line, "EOF", 3) && !fWindow)
        {
            check(false, "hostprobe.exe printed its HOSTPROBE line");
            PostMessage(B_QUIT_REQUESTED);
            return;
        }
        /* SCREEN lines for screencheck, and hostprobe.exe's own FAIL lines. */
        if (!strncmp(line, "SCREEN ", 7) || !strncmp(line, "FAIL", 4))
        {
            printf("%s\n", line);
            fflush(stdout);
            if (!strncmp(line, "FAIL", 4)) failures++;
            return;
        }
        if (fWindow) fWindow->PostMessage(msg);
    }

private:
    ProbeWindow *fWindow = NULL;
};

void *reader(void *)
{
    char buf[512];

    while (fgets(buf, sizeof(buf), stdin))
    {
        buf[strcspn(buf, "\r\n")] = 0;
        BMessage msg(MSG_LINE);
        msg.AddString("line", buf);
        be_app->PostMessage(&msg);
    }
    BMessage msg(MSG_LINE);
    msg.AddString("line", "EOF");
    be_app->PostMessage(&msg);
    return NULL;
}

}  /* namespace */

int main(void)
{
    ProbeApp app;
    pthread_t thread;

    pthread_create(&thread, NULL, reader, NULL);
    pthread_detach(thread);
    app.Run();
    printf("hostprobe_host: %d failure(s)\n", failures);
    return failures;
}
