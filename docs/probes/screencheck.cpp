/* screencheck: checks screen pixels that another program says it has drawn.
 *
 * Native RenkuOS program: g++ -O1 -Wall screencheck.cpp -o screencheck -lbe
 * Reads lines from stdin and passes them through; a line
 *     SCREEN <x> <y> <rrggbb> <what...>
 * is answered by reading that pixel from the screen (BScreen::GetBitmap, src/kits/interface/
 * PrivateScreen.cpp) and printing PASS or FAIL with what was found. The exit status is the
 * number of failures. Used with glprobe.exe --screen, whose GL frames reach the screen without
 * passing through anything Win32 can read back. */

#include <Application.h>
#include <Bitmap.h>
#include <Screen.h>

#include <stdio.h>
#include <string.h>

int main(void)
{
    BApplication app("application/x-vnd.renku-screencheck");  /* BScreen talks to app_server through it */
    BScreen screen;
    char line[512];
    int failures = 0;

    while (fgets(line, sizeof(line), stdin))
    {
        unsigned int want;
        int x, y, n = 0;

        if (sscanf(line, "SCREEN %d %d %x %n", &x, &y, &want, &n) != 3 || !n)
        {
            fputs(line, stdout);
            fflush(stdout);
            continue;
        }

        BRect rect(x, y, x, y);
        BBitmap *bitmap = NULL;
        unsigned int got = 0xffffffff;
        const char *what = line + n;
        size_t len = strlen(what);

        if (len && what[len - 1] == '\n') len--;
        if (screen.GetBitmap(&bitmap, false, &rect) == B_OK)
        {
            const unsigned char *p = (const unsigned char *)bitmap->Bits();
            color_space space = bitmap->ColorSpace();
            if (space == B_RGB32 || space == B_RGBA32)
                got = (p[2] << 16) | (p[1] << 8) | p[0];  /* B, G, R, X in memory */
            else
                fprintf(stdout, "      screen colour space %#x not handled\n", (unsigned)space);
            delete bitmap;
        }
        if (got != want) failures++;
        fprintf(stdout, "%s  screen: %.*s: (%d,%d) is %06x, want %06x\n", got == want ? "PASS" : "FAIL",
                (int)len, what, x, y, got, want);
        fflush(stdout);
    }
    fprintf(stdout, "screencheck: %d failure(s)\n", failures);
    return failures;
}
