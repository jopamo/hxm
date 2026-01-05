#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

#include "event.h"

extern void xcb_stubs_reset(void);

// Flags to control failure injection
static bool fail_connect = false;
static bool fail_fcntl = false;
static bool fail_keysyms = false;
static bool fail_sigprocmask = false;
static bool fail_signalfd = false;
static bool fail_timerfd = false;
static bool fail_epoll_create = false;
static bool fail_epoll_ctl = false;

// Jump buffer
static jmp_buf exit_buf;
static int exit_status_capture = -1;

void __wrap_exit(int status) {
    exit_status_capture = status;
    longjmp(exit_buf, 1);
}

// xcb_connect_cached wrapper
// We link against xcb_stubs.c which provides xcb_connect (but not cached)
extern xcb_connection_t* xcb_connect(const char* displayname, int* screenp);

xcb_connection_t* __wrap_xcb_connect_cached(void) {
    if (fail_connect) return NULL;
    return xcb_connect(NULL, NULL);
}

// fcntl wrapper
int __wrap_fcntl(int fd, int cmd, ...) {
    (void)fd;
    if (cmd == F_GETFL) return 0;
    if (cmd == F_SETFL && fail_fcntl) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

// keysyms wrapper
// This calls the stub implementation if not failing
extern xcb_key_symbols_t* __real_xcb_key_symbols_alloc(xcb_connection_t* c);
xcb_key_symbols_t* __wrap_xcb_key_symbols_alloc(xcb_connection_t* c) {
    if (fail_keysyms) return NULL;
    return __real_xcb_key_symbols_alloc(c);
}

// sigprocmask wrapper
int __wrap_sigprocmask(int how, const sigset_t* set, sigset_t* oldset) {
    (void)how;
    (void)set;
    (void)oldset;
    if (fail_sigprocmask) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

// signalfd wrapper
int __wrap_signalfd(int fd, const sigset_t* mask, int flags) {
    (void)fd;
    (void)mask;
    (void)flags;
    if (fail_signalfd) {
        errno = EMFILE;
        return -1;
    }
    return 100;  // Fake FD
}

// timerfd_create wrapper
int __wrap_timerfd_create(int clockid, int flags) {
    (void)clockid;
    (void)flags;
    if (fail_timerfd) {
        errno = EMFILE;
        return -1;
    }
    return 101;  // Fake FD
}

// epoll_create1 wrapper
int __wrap_epoll_create1(int flags) {
    (void)flags;
    if (fail_epoll_create) {
        errno = EMFILE;
        return -1;
    }
    return 102;  // Fake FD
}

// epoll_ctl wrapper
int __wrap_epoll_ctl(int epfd, int op, int fd, struct epoll_event* event) {
    (void)epfd;
    (void)op;
    (void)fd;
    (void)event;
    if (fail_epoll_ctl) {
        errno = EBADF;
        return -1;
    }
    return 0;
}

// We need to provide dummy implementations for close() because server_cleanup calls it
// and we are returning fake FDs (100, 101, 102). Closing them might be harmless or error.
// But we are not wrapping close(), so it calls real close.
// Closing 100/101/102 if they are not open is EBADF, which is fine.

void reset_flags(void) {
    fail_connect = false;
    fail_fcntl = false;
    fail_keysyms = false;
    fail_sigprocmask = false;
    fail_signalfd = false;
    fail_timerfd = false;
    fail_epoll_create = false;
    fail_epoll_ctl = false;
    xcb_stubs_reset();
}

typedef void (*setup_fn)(void);

void set_fail_connect(void) { fail_connect = true; }
void set_fail_fcntl(void) { fail_fcntl = true; }
void set_fail_keysyms(void) { fail_keysyms = true; }
void set_fail_sigprocmask(void) { fail_sigprocmask = true; }
void set_fail_signalfd(void) { fail_signalfd = true; }
void set_fail_timerfd(void) { fail_timerfd = true; }
void set_fail_epoll_create(void) { fail_epoll_create = true; }
void set_fail_epoll_ctl(void) { fail_epoll_ctl = true; }

void run_test(const char* name, setup_fn setup) {
    printf("Running %s... ", name);
    reset_flags();
    setup();

    server_t s;
    // Zero out s to avoid garbage
    memset(&s, 0, sizeof(s));
    s.is_test = true;

    exit_status_capture = -1;

    if (setjmp(exit_buf) == 0) {
        server_init(&s);
        fprintf(stderr, "FAILED: server_init did not exit\n");
        server_cleanup(&s);
        exit(1);
    }

    // Check exit status
    if (exit_status_capture != 1) {
        fprintf(stderr, "FAILED: exit status %d, expected 1\n", exit_status_capture);
        server_cleanup(&s);
        exit(1);
    }

    server_cleanup(&s);
    printf("PASSED\n");
}

int main(void) {
    run_test("connect_fail", set_fail_connect);
    run_test("fcntl_fail", set_fail_fcntl);
    run_test("keysyms_fail", set_fail_keysyms);
    run_test("sigprocmask_fail", set_fail_sigprocmask);
    run_test("signalfd_fail", set_fail_signalfd);
    run_test("timerfd_fail", set_fail_timerfd);
    run_test("epoll_create_fail", set_fail_epoll_create);
    run_test("epoll_ctl_fail", set_fail_epoll_ctl);

    return 0;
}
