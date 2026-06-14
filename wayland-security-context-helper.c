#define _GNU_SOURCE
/*
 * wayland-security-context-helper.c
 *
 * Creates a new Wayland listening socket and registers it with Sway via
 * wp_security_context_manager_v1, marking it as a sandboxed connection.
 * The process stays in the foreground and blocks — the caller must background
 * it with `&`, or run it under a systemd unit / supervisor. While it lives,
 * the security context is valid. When it exits (or when the parent dies due to
 * --die-with-parent), Sway drops the context and new connections on the socket
 * are rejected.
 *
 * Build:
 *   wayland-scanner client-header \
 *     /usr/share/wayland-protocols/staging/security-context/security-context-v1.xml \
 *     security-context-v1-client-protocol.h
 *   wayland-scanner private-code \
 *     /usr/share/wayland-protocols/staging/security-context/security-context-v1.xml \
 *     security-context-v1-protocol.c
 *   cc -O2 -Wall -o wayland-security-context-helper \
 *      wayland-security-context-helper.c security-context-v1-protocol.c \
 *      -lwayland-client
 *
 * Usage (foreground daemon):
 *   wayland-security-context-helper \
 *     --app-id org.sandbox.firefox \
 *     --instance-id org.sandbox.firefox.12345 \
 *     --sandbox-engine org.sandbox.bwrap \
 *     --listen-socket /run/user/1000/sandbox-xxx/wayland-1 &
 *   HELPER_PID=$!
 *   # ... run the sandboxed client ...
 *   kill $HELPER_PID
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>

#include "security-context-v1-client-protocol.h"

struct ctx {
    struct wp_security_context_manager_v1 *mgr;
};

static void registry_global(void *data, struct wl_registry *reg,
                            uint32_t name, const char *iface, uint32_t ver) {
    struct ctx *c = data;
    if (strcmp(iface, wp_security_context_manager_v1_interface.name) == 0) {
        c->mgr = wl_registry_bind(reg, name,
                                  &wp_security_context_manager_v1_interface, 1);
    }
}

static void registry_global_remove(void *d, struct wl_registry *r, uint32_t n) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static int make_listen_socket(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "path too long: %s\n", path);
        close(fd); return -1;
    }
    strcpy(addr.sun_path, path);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, 16) < 0) {
        perror("listen"); close(fd); return -1;
    }
    chmod(path, 0600);
    return fd;
}

static volatile sig_atomic_t should_exit = 0;
static void on_signal(int sig) { (void)sig; should_exit = 1; }

int main(int argc, char **argv) {
    const char *app_id = NULL, *instance_id = NULL;
    const char *engine = "org.sandbox.bwrap";
    const char *sock_path = NULL;

    static struct option opts[] = {
        {"app-id",         required_argument, 0, 'a'},
        {"instance-id",    required_argument, 0, 'i'},
        {"sandbox-engine", required_argument, 0, 'e'},
        {"listen-socket",  required_argument, 0, 's'},
        {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "a:i:e:s:", opts, NULL)) != -1) {
        switch (c) {
            case 'a': app_id = optarg; break;
            case 'i': instance_id = optarg; break;
            case 'e': engine = optarg; break;
            case 's': sock_path = optarg; break;
            default: return 2;
        }
    }
    if (!app_id || !instance_id || !sock_path) {
        fprintf(stderr,
            "usage: %s --app-id X --instance-id Y --listen-socket PATH "
            "[--sandbox-engine ENGINE]\n", argv[0]);
        return 2;
    }

    /* Set up signal handlers for clean exit. */
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    /* Ignore SIGPIPE in case the parent dies and we write to a pipe. */
    signal(SIGPIPE, SIG_IGN);

    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) {
        fprintf(stderr, "cannot connect to compositor (WAYLAND_DISPLAY=%s)\n",
                getenv("WAYLAND_DISPLAY") ?: "(unset)");
        return 1;
    }

    struct ctx ctx = {0};
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &registry_listener, &ctx);
    wl_display_roundtrip(dpy);

    if (!ctx.mgr) {
        fprintf(stderr,
            "wp_security_context_manager_v1 not available — "
            "requires Sway >= 1.9, and no other active context may exist "
            "on the same connection.\n");
        return 1;
    }

    int listen_fd = make_listen_socket(sock_path);
    if (listen_fd < 0) return 1;

    /*
     * close_fd lifetime: the security context stays active as long as the
     * write end of this pipe is open. When the last writable reference closes,
     * Sway sees EOF on the read end, drops the context, and new connections on
     * the socket are rejected (existing connections remain unaffected).
     *
     * We pass the read end to the compositor via fd-passing over the Wayland
     * connection. We keep the write end — when we exit (e.g. on SIGTERM), the
     * kernel closes it automatically and the context is released.
     */
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0) { perror("pipe"); return 1; }

    struct wp_security_context_v1 *sc =
        wp_security_context_manager_v1_create_listener(
            ctx.mgr, listen_fd, pipefd[0]);

    wp_security_context_v1_set_sandbox_engine(sc, engine);
    wp_security_context_v1_set_app_id(sc, app_id);
    wp_security_context_v1_set_instance_id(sc, instance_id);
    wp_security_context_v1_commit(sc);
    wp_security_context_v1_destroy(sc);

    /* Wait for the commit to be processed by the compositor. */
    wl_display_roundtrip(dpy);

    /* The compositor now owns listen_fd and the pipe read end. */
    close(listen_fd);
    close(pipefd[0]);

    /* Signal readiness to the caller. The script waits for the socket file to
     * appear, so a single info line on stderr is sufficient. */
    fprintf(stderr, "security-context active: app-id=%s socket=%s\n",
            app_id, sock_path);
    fflush(stderr);

    /*
     * Main loop: pump Wayland events until a signal arrives.
     * wl_display_dispatch blocks until an event is ready. If the compositor
     * disconnects (e.g. Sway exits) it returns -1 and we exit.
     * SIGTERM/SIGINT interrupt the dispatch call with EINTR.
     */
    while (!should_exit) {
        if (wl_display_dispatch(dpy) < 0) {
            if (errno == EINTR) continue;
            break;
        }
    }

    /* Clean up. Closing pipefd[1] signals the compositor that the context is
     * gone. wl_display_disconnect closes the Wayland connection. */
    close(pipefd[1]);
    wl_display_disconnect(dpy);
    unlink(sock_path);
    return 0;
}
