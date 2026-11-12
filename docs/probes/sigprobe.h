/* sigprobe.h -- the plain-C seam between sigprobe.c (Wine side, windows.h) and
 * sigprobe_be.cpp (Be API side). Wine's headers and the Be headers never meet in one file,
 * the same split winerenku.drv uses (renku_be.h). Probe P3 of docs/NATIVE-DRIVER-PLAN.md. */
#ifndef SIGPROBE_H
#define SIGPROBE_H

#include <signal.h>

/* RenkuOS's signal.h has SIGPOLL (23) and no SIGIO; ntdll's server.c makes the same
 * substitution under __HAIKU__ for server_block_set. */
#if defined(__HAIKU__) && !defined(SIGIO)
#define SIGIO SIGPOLL
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum { EV_READY = 'r', EV_DRAW = 'd', EV_MASK = 'k', EV_PING = 'p' };

/* The signals Wine expects only on its own threads: exactly server_block_set
 * (dlls/ntdll/unix/server.c, server_init_process). ntdll does not export it. */
void be_async_signals(sigset_t *set);

/* Start the BApplication thread and show one window. With block != 0, Wine's asynchronous
 * signals are blocked around each call that spawns a Be thread (rule 3). Returns 0 on success. */
int be_start(int block);

/* Read end of the pipe the Be threads write events to. */
int be_event_fd(void);

/* Wait for one event, skipping others. Returns 1 if it arrived within timeout_ms. */
int be_wait_for(char ev, int timeout_ms, long long *latency_us);

/* The signal masks of the app and window looper threads, read on those threads. 0 = ok. */
int be_looper_masks(sigset_t *app, sigset_t *win);

/* Kernel thread id of the window's looper. */
int be_window_thread(void);

/* Round trip to the window looper: is it still alive and handling messages? 1 = yes. */
int be_ping(int timeout_ms);

/* send_signal() to the window looper thread alone (headers/os/kernel/OS.h). 0 = sent. */
int be_signal_window(int sig);

/* Have the window looper write through a null pointer. */
void be_fault_on_window(void);

/* Quit the window, free the bitmap, quit the app (rule 4 order), join. 0 = joined. */
int be_stop(void);

#ifdef __cplusplus
}
#endif

#endif
