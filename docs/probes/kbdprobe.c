/*
 * kbdprobe: what Wine's keyboard layout tables say, under the graphics driver of the prefix.
 *
 * For a set of keys, by scancode: the virtual key, the characters typed plain, with Shift
 * and with AltGr (Ctrl+Alt), and the key name. Then VkKeyScan for a few characters. It
 * checks winerenku.drv's tables (keyboard.c) against the keymap loaded in RenkuOS, without
 * typing anything: run it once per keymap and compare with the keymap's source file.
 *
 * Build against a built Wine tree $W (Winelib, like sigprobe):
 *   $W/tools/winegcc/winegcc --wine-objdir $W -mconsole -o kbdprobe.exe kbdprobe.c \
 *     -L$W/libs/winecrt0 -L$W/dlls/kernel32 -L$W/dlls/ntdll -L$W/dlls/user32 \
 *     -lwinecrt0 -lkernel32 -lntdll -luser32
 * Run: $W/loader/wine kbdprobe.exe.so
 */

#include <stdio.h>
#include <windows.h>

static const struct { UINT scan; const char *us; } keys[] =
{
    { 0x02, "1" }, { 0x03, "2" }, { 0x06, "5" }, { 0x0c, "-" }, { 0x0d, "=" },
    { 0x10, "Q" }, { 0x11, "W" }, { 0x12, "E" }, { 0x15, "Y" }, { 0x1a, "[" }, { 0x1b, "]" },
    { 0x1e, "A" }, { 0x27, ";" }, { 0x28, "'" }, { 0x29, "`" }, { 0x2b, "\\" },
    { 0x2c, "Z" }, { 0x32, "M" }, { 0x33, "," }, { 0x34, "." }, { 0x35, "/" }, { 0x56, "ISO <>" },
    { 0x39, "Space" }, { 0x1c, "Enter" }, { 0x0e, "Backspace" }, { 0x53, "Num ." },
};

static void print_char(WCHAR c, int len)
{
    if (len <= 0) printf(" %-6s", len < 0 ? "dead" : "-");
    else if (c < 0x20) printf(" 0x%02x  ", c);
    else if (c < 0x80) printf(" '%c'   ", c);
    else printf(" U+%04X", c);
}

static void type(UINT vkey, UINT scan, BOOL shift, BOOL altgr)
{
    BYTE state[256] = {0};
    WCHAR buf[8];

    if (shift) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
    if (altgr) state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_MENU] = state[VK_RMENU] = 0x80;
    int len = ToUnicode(vkey, scan, state, buf, 8, 0);
    print_char(buf[0], len);
}

int main(void)
{
    static const WCHAR chars[] = { 'z', 'y', 'q', 'a', 'm', '@', 0x20ac, 0xdf, '1', '&' };
    HKL hkl = GetKeyboardLayout(0);
    unsigned int i;

    printf("layout %p\n", hkl);
    printf("scan  us        vkey  plain  shift  altgr  s+altgr name\n");
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
    {
        UINT vkey = MapVirtualKeyExW(keys[i].scan, MAPVK_VSC_TO_VK, hkl);
        WCHAR name[64];
        char name_a[64];

        GetKeyNameTextW(keys[i].scan << 16, name, 64);
        WideCharToMultiByte(CP_UTF8, 0, name, -1, name_a, sizeof(name_a), NULL, NULL);
        printf("0x%02x  %-9s 0x%02x ", keys[i].scan, keys[i].us, vkey);
        type(vkey, keys[i].scan, FALSE, FALSE);
        type(vkey, keys[i].scan, TRUE, FALSE);
        type(vkey, keys[i].scan, FALSE, TRUE);
        type(vkey, keys[i].scan, TRUE, TRUE);
        printf(" %s\n", name_a);
    }

    printf("VkKeyScan:");
    for (i = 0; i < sizeof(chars) / sizeof(chars[0]); i++)
        printf(" U+%04X=%04x", chars[i], (WORD)VkKeyScanExW(chars[i], hkl));
    printf("\n");
    return 0;
}
