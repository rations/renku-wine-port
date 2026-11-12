/* sigprobe.c -- probe P3 of docs/NATIVE-DRIVER-PLAN.md: inside a running Wine process, can a
 * BApplication thread and a BWindow looper live alongside Wine's signal handling?
 *
 * Wine installs process-wide handlers for SIGINT, SIGQUIT, SIGUSR1, SIGSEGV and others
 * (dlls/ntdll/unix/signal_x86_64.c, signal_init_process). On x86-64 every handler finds the
 * thread's data as rsp & ~signal_stack_mask (0xffff, unix_private.h:182), with no check: that
 * is only valid on the signal stack Wine gives its own threads. Be threads are not Wine
 * threads. Two ways a Wine handler can run on one:
 *
 *   - a process-directed signal (Ctrl-C's SIGINT from a Terminal, SIGQUIT, SIGHUP): RenkuOS
 *     queues it on the team and any thread that does not block it takes it
 *     (src/system/kernel/signal.cpp, send_signal_to_team_locked);
 *   - a synchronous fault in Be code (SIGSEGV), which cannot be blocked.
 *
 * Thread-directed signals from wineserver (SIGUSR1 to suspend) target Wine threads by id
 * (server/ptrace.c, tkill -> send_signal), so they never reach a Be thread.
 *
 * Rule 3 of the plan: block Wine's asynchronous signals around every call that spawns a Be
 * thread, so the Be threads start with them blocked. This checks that rule and its control:
 *
 *   wine sigprobe.exe.so              rule 3 applied: masks, thread- and team-directed SIGINT
 *   wine sigprobe.exe.so --unblocked  control: no blocking, then SIGINT to the window looper
 *   wine sigprobe.exe.so --fault      rule 3 applied, then SIGSEGV on the window looper
 *
 * The default mode prints PASS/FAIL and exits with the number of failures. The other two
 * report what happened; they may crash, and that is their result.
 *
 * Built as a Winelib program so that this file can use Win32 (SetConsoleCtrlHandler) and the
 * Be half can use libbe, on Wine's own threads; see probes/README.md for the commands.
 */
#include <windows.h>

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "sigprobe.h"

static int failures;

static void check(int ok, const char *what)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static const struct { int sig; const char *name; } async_sigs[] = {
    { SIGALRM, "ALRM" }, { SIGIO, "IO" }, { SIGINT, "INT" }, { SIGHUP, "HUP" },
    { SIGQUIT, "QUIT" }, { SIGUSR1, "USR1" }, { SIGUSR2, "USR2" }, { SIGCHLD, "CHLD" },
};

/* Prints the blocked members of Wine's asynchronous set; returns how many are blocked. */
static int show_mask(const char *who, const sigset_t *mask)
{
    char buf[128] = "";
    int n = 0;
    for (size_t i = 0; i < sizeof(async_sigs) / sizeof(async_sigs[0]); i++)
        if (sigismember(mask, async_sigs[i].sig))
        {
            strcat(buf, " ");
            strcat(buf, async_sigs[i].name);
            n++;
        }
    printf("info  %-22s blocks%s\n", who, n ? buf : " none of Wine's async signals");
    return n;
}

static const int n_async = sizeof(async_sigs) / sizeof(async_sigs[0]);

static volatile LONG ctrl_c_events;

static BOOL WINAPI on_ctrl(DWORD type)
{
    if (type == CTRL_C_EVENT) InterlockedIncrement(&ctrl_c_events);
    return TRUE;   /* handled: the default would be ExitProcess */
}

/* Wait until ctrl_c_events reaches want, or timeout_ms passes. */
static int wait_ctrl_c(LONG want, int timeout_ms)
{
    for (int t = 0; t < timeout_ms; t += 10)
    {
        if (ctrl_c_events >= want) return 1;
        Sleep(10);
    }
    return ctrl_c_events >= want;
}

static int wine_handler_installed(int sig)
{
    struct sigaction act;
    if (sigaction(sig, NULL, &act) != 0) return 0;
    return (act.sa_flags & SA_SIGINFO) && act.sa_sigaction != NULL &&
           (void *)act.sa_sigaction != (void *)SIG_DFL && (void *)act.sa_sigaction != (void *)SIG_IGN;
}

int main(int argc, char *argv[])
{
    int unblocked = argc > 1 && !strcmp(argv[1], "--unblocked");
    int fault = argc > 1 && !strcmp(argv[1], "--fault");
    sigset_t before, after, app_mask, win_mask;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("info  mode: %s; Win32 thread %04lx, pid %d\n",
           unblocked ? "--unblocked (control)" : fault ? "--fault" : "rule 3 applied",
           GetCurrentThreadId(), (int)getpid());

    /* 0. This really is a Wine process, and its main thread takes the async signals. */
    check(wine_handler_installed(SIGINT) && wine_handler_installed(SIGQUIT) &&
          wine_handler_installed(SIGUSR1) && wine_handler_installed(SIGSEGV),
          "Wine's handlers are installed for SIGINT, SIGQUIT, SIGUSR1, SIGSEGV");
    pthread_sigmask(SIG_BLOCK, NULL, &before);
    check(!sigismember(&before, SIGINT) && !sigismember(&before, SIGUSR1),
          "the Win32 main thread has SIGINT and SIGUSR1 unblocked");
    check(SetConsoleCtrlHandler(on_ctrl, TRUE), "SetConsoleCtrlHandler()");

    /* 1. BApplication thread and one window, started from this Win32 thread. */
    check(be_start(!unblocked) == 0, "BApplication started and the window drew (event via pipe)");
    pthread_sigmask(SIG_BLOCK, NULL, &after);
    check(!memcmp(&before, &after, sizeof(before)), "the Win32 thread's own mask is restored");

    /* 2. The masks the Be threads actually run with. */
    if (be_looper_masks(&app_mask, &win_mask) != 0)
    {
        check(0, "read the looper threads' signal masks");
        return failures;
    }
    int app_n = show_mask("app looper thread", &app_mask);
    int win_n = show_mask("window looper thread", &win_mask);
    if (!unblocked)
    {
        check(app_n == n_async, "the app looper starts with all of Wine's async signals blocked");
        check(win_n == n_async, "the window looper starts with all of Wine's async signals blocked");
    }
    else
    {
        check(app_n == 0 && win_n == 0,
              "control: without rule 3 the loopers inherit Wine's unblocked mask");
    }

    if (fault)
    {
        /* 3f. A synchronous fault on a Be thread runs Wine's segv_handler there. */
        printf("info  window looper thread %d writes through a null pointer now\n",
               be_window_thread());
        be_fault_on_window();
        for (int s = 1; s <= 10; s++)
        {
            Sleep(1000);
            printf("info  %d s later the process is still running; window looper %s\n", s,
                   be_ping(500) ? "answers" : "does not answer");
        }
        printf("RESULT  --fault: the process survived 10 s after the fault\n");
        return 0;
    }

    /* 3. A signal sent to the window looper thread alone. */
    check(be_signal_window(SIGINT) == 0, "send_signal(window looper, SIGINT)");
    int handled = wait_ctrl_c(1, 1000);
    int alive = be_ping(1000);
    if (!unblocked)
    {
        check(!handled, "the looper did not run Wine's SIGINT handler (signal stays pending)");
        check(alive, "the window looper still answers messages");
    }
    else
    {
        printf("RESULT  --unblocked: Wine's SIGINT handler %s on the looper; looper %s\n",
               handled ? "ran (Ctrl-C event delivered)" : "did not deliver a Ctrl-C event",
               alive ? "still answers" : "no longer answers");
        printf("info  tearing down\n");
        be_stop();
        printf("RESULT  --unblocked: process survived teardown\n");
        return 0;
    }

    /* 4. Process-directed SIGINT, as Ctrl-C in a Terminal sends: with the Be threads blocking
     *    it, a Wine thread must take every one. */
    const int N = 10;
    LONG start = ctrl_c_events;
    int got = 0;
    for (int i = 0; i < N; i++)
    {
        kill(getpid(), SIGINT);
        if (wait_ctrl_c(start + i + 1, 2000)) got++;
    }
    printf("info  %d of %d process-directed SIGINTs reached the Ctrl-C handler\n", got, N);
    check(got == N, "every process-directed SIGINT was handled by Wine on a Wine thread");
    check(be_ping(1000), "the window looper still answers messages afterwards");

    check(be_stop() == 0, "app thread returned from Run() and joined");
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures;
}
