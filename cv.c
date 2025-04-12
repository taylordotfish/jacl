/*
 * Copyright (C) 2025 taylor.fish <contact@taylor.fish>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#define _POSIX_C_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <jack/jack.h>
#include <jack/metadata.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static int sigfd_write;

static const char *USAGE = "\
Usage: %s [client-name]\n\
\n\
Provides a CV output port whose value is determined by standard input (one \n\
base-10 floating-point number per line).\n\
\n\
[client-name] is the name of the JACK client to create; if not provided, the\n\
default is 'jacl-cv'.\n\
";

static void usage(FILE * const stream, const char * const arg0) {
    const char *bin = arg0 ? arg0 : "";
    size_t start = 0;
    for (size_t i = 0; bin[i] != '\0'; ++i) {
        if (bin[i] == '/') {
            start = i + 1;
        }
    }
    bin += start;
    if (*bin == '\0') {
        bin = "jacl-cv";
    }
    fprintf(stream, USAGE, bin);
}

static void on_exit(const int signum) {
    (void)signum;
    close(sigfd_write);
}

static bool install_exit_handler(const int signum) {
    sigset_t mask;
    sigemptyset(&mask);
    const struct sigaction act = {
        .sa_handler = on_exit,
        .sa_mask = mask,
        .sa_flags = 0,
    };
    if (sigaction(signum, &act, NULL) == 0) {
        return true;
    }
    fprintf(stderr, "sigaction(%d) failed", signum);
    perror("");
    return false;
}

static bool set_nonblock(const int fd) {
    const int flags = fcntl(fd, F_GETFL);
    if (flags == -1) {
        fprintf(stderr, "fcntl(%d, F_GETFL) failed", fd);
        perror("");
        return false;
    }
    if (flags & O_NONBLOCK) {
        return true;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        fprintf(stderr, "fcntl(%d, F_SETFL) failed", fd);
        perror("");
        return false;
    }
    return true;
}

typedef struct State {
    jack_client_t *client;
    jack_port_t *port;
    _Atomic float value;
} State;

static int close_and_fail(jack_client_t * const client) {
    jack_client_close(client);
    return EXIT_FAILURE;
}

static int process(const jack_nframes_t nframes, void * const arg) {
    const State * const state = arg;
    jack_port_t * const port = state->port;
    if (port == NULL) {
        return 0;
    }

    jack_default_audio_sample_t * const buffer =
        jack_port_get_buffer(port, nframes);
    if (buffer == NULL) {
        return -1;
    }

    const float value =
        atomic_load_explicit(&state->value, memory_order_relaxed);
    for (jack_nframes_t i = 0; i < nframes; ++i) {
        buffer[i] = value;
    }
    return 0;
}

static void handle_line(State * const state, const char * const line) {
    errno = 0;
    char *endptr = NULL;
    float value = strtof(line, &endptr);
    if (!endptr || endptr == line || errno != 0) {
        fputs("error: could not parse as a float\n", stderr);
        return;
    }
    // Check for NaN
    if (value != value) {
        fputs("error: value cannot be NaN\n", stderr);
        return;
    }
    static const bool clamp =
        #if JACLI_CV_CLAMP
            true
        #else
            false
        #endif
    ;
    if (!clamp) {
    } else if (value < 0) {
        fputs("value clamped to 0\n", stderr);
        value = 0;
    } else if (value > 1) {
        fputs("value clamped to 1\n", stderr);
        value = 1;
    }
    atomic_store_explicit(&state->value, value, memory_order_relaxed);
}

int main(const int argc, char ** const argv) {
    int argi = 1;
    if (argc > argi) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            usage(stdout, argv[0]);
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[1], "--version") == 0) {
            puts("0.1");
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[1], "--") == 0) {
            ++argi;
        }
    }
    if (argc - argi > 1) {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    int sigfds[2];
    if (pipe(sigfds) != 0) {
        perror("pipe() failed");
        return EXIT_FAILURE;
    }
    sigfd_write = sigfds[1];

    static const int signals[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM};
    for (size_t i = 0; i < sizeof(signals) / sizeof(*signals); ++i) {
        if (!install_exit_handler(signals[i])) {
            return EXIT_FAILURE;
        }
    }

    const char * const name = argc > argi ? argv[argi] : "jacl-cv";
    jack_status_t status = 0;
    jack_client_t * const client =
        jack_client_open(name, JackNoStartServer, &status);
    if (client == NULL) {
        fprintf(stderr, "jack_client_open() failed: 0x%x\n", (int)status);
        return EXIT_FAILURE;
    }

    State state = {
        .client = client,
    };
    const int spc_status = jack_set_process_callback(client, process, &state);
    if (spc_status != 0) {
        fprintf(
            stderr,
            "jack_set_process_callback() failed: %d\n",
            spc_status
        );
        return close_and_fail(client);
    }

    jack_port_t * const port = jack_port_register(
        client,
        "value",
        JACK_DEFAULT_AUDIO_TYPE,
        JackPortIsOutput,
        0
    );
    if (port == NULL) {
        fputs("jack_port_register() failed\n", stderr);
        return close_and_fail(client);
    }
    state.port = port;

    const jack_uuid_t uuid = jack_port_uuid(port);
    const int sp_status = jack_set_property(
        client,
        uuid,
        JACK_METADATA_SIGNAL_TYPE,
        "CV",
        "text/plain"
    );
    if (sp_status != 0) {
        fprintf(stderr, "jack_set_property() failed: %d\n", sp_status);
    }

    const int astatus = jack_activate(client);
    if (astatus != 0) {
        fprintf(stderr, "jack_activate() failed: %d\n", astatus);
        return close_and_fail(client);
    }

    const int sigfd_read = sigfds[0];
    if (!set_nonblock(sigfd_read) || !set_nonblock(STDIN_FILENO)) {
        return close_and_fail(client);
    }
    struct pollfd pollfds[] = {
        {
            .fd = sigfd_read,
            .events = 0,
        },
        {
            .fd = STDIN_FILENO,
            .events = POLLIN,
        },
    };

    char line[128];
    size_t linelen = 0;
    while (true) {
        const int status =
            poll(pollfds, sizeof(pollfds) / sizeof(*pollfds), -1);
        if (status > 0) {
        } else if (status == 0 || errno == EINTR) {
            continue;
        } else {
            perror("poll() failed");
            return close_and_fail(client);
        }
        for (size_t i = 0; i < sizeof(pollfds) / sizeof(*pollfds); ++i) {
            if (pollfds[i].revents & POLLNVAL) {
                fprintf(stderr, "unexpected POLLNVAL on #%zu\n", i);
                return close_and_fail(client);
            }
        }
        if (pollfds[0].revents) {
            break;
        }
        if (pollfds[1].revents & POLLIN) {
            char buf[64];
            while (true) {
                const ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
                if (n < 0) {
                    break;
                }
                if (n == 0) {
                    close(STDIN_FILENO);
                    pollfds[1].fd = -1;
                    break;
                }
                for (size_t i = 0; i < (size_t)n; ++i) {
                    if (buf[i] == '\n') {
                        line[linelen] = '\0';
                        handle_line(&state, line);
                        linelen = 0;
                        continue;
                    }
                    if (linelen < sizeof(line) - 1) {
                        line[linelen++] = buf[i];
                    }
                }
            }
        } else if (pollfds[1].revents) {
            pollfds[1].fd = -1;
        }
    }

    jack_client_close(client);
    const int tty = open("/dev/tty", O_WRONLY);
    if (tty != -1) {
        while (write(tty, "\n", 1) == -1 && errno == EINTR) {}
        close(tty);
    }
    return EXIT_SUCCESS;
}
