#define _POSIX_C_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <jack/jack.h>
#include <jack/metadata.h>
#include <jack/midiport.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static int sigfd_write;

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
        bin = "jack-midi-to-stdin";
    }
    fprintf(stream, "Usage: %s [client-name]\n", bin);
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
    bool line_completed;
} State;

static int close_and_fail(jack_client_t * const client) {
    jack_client_close(client);
    return EXIT_FAILURE;
}

static char int_to_hex(int n) {
    if (n >= 0 && n <= 9) {
        return '0' + n;
    }
    if (n >= 10 && n <= 15) {
        return 'a' + (n - 10);
    }
    return '?';
}

static bool flush_buf(
    const char * const buf,
    const size_t len,
    State * const state
) {
    const ssize_t written = write(STDOUT_FILENO, buf, len);
    if (written < 1) {
        return false;
    }
    if (written > (ssize_t)len) {
        state->line_completed = false;
        return false;
    }
    switch (buf[written - 1]) {
        case '\n':
        case 'X':
            state->line_completed = true;
            break;
        default:
            state->line_completed = false;
            break;
    }
    return written == (ssize_t)len;
}

static int process(const jack_nframes_t nframes, void * const arg) {
    State * const state = arg;
    jack_port_t * const port = state->port;
    if (port == NULL) {
        return 0;
    }

    void * const buffer = jack_port_get_buffer(port, nframes);
    if (buffer == NULL) {
        return -1;
    }

    char buf[128];
    size_t buflen = 0;
    if (!state->line_completed) {
        buf[buflen++] = 'X';
    }

    const jack_nframes_t count = jack_midi_get_event_count(buffer);
    for (jack_nframes_t i = 0; i < count; ++i) {
        jack_midi_event_t event;
        if (jack_midi_event_get(&event, buffer, i) != 0) {
            break;
        }
        for (size_t i = 0; i < event.size; ++i) {
            const unsigned char value = event.buffer[i];
            buf[buflen++] = int_to_hex(value >> 4);
            buf[buflen++] = int_to_hex(value & 0xf);
            if (buflen < sizeof(buf)) {
                continue;
            }
            if (!flush_buf(buf, buflen, state)) {
                return 0;
            }
            buflen = 0;
        }
        buf[buflen++] = '\n';
    }
    flush_buf(buf, buflen, state);
    return 0;
}

int main(const int argc, char ** const argv) {
    int argi = 1;
    if (argc > argi) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            usage(stdout, argv[0]);
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

    set_nonblock(STDOUT_FILENO);
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

    const char * const name = argc > argi ? argv[argi] : "jm2s";
    jack_status_t status = 0;
    jack_client_t * const client =
        jack_client_open(name, JackNoStartServer, &status);
    if (client == NULL) {
        fprintf(stderr, "jack_client_open() failed: 0x%x\n", (int)status);
        return EXIT_FAILURE;
    }

    State state = {
        .client = client,
        .port = NULL,
        .line_completed = true,
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
        "in",
        JACK_DEFAULT_MIDI_TYPE,
        JackPortIsInput,
        0
    );
    if (port == NULL) {
        fputs("jack_port_register() failed\n", stderr);
        return close_and_fail(client);
    }
    state.port = port;

    const int astatus = jack_activate(client);
    if (astatus != 0) {
        fprintf(stderr, "jack_activate() failed: %d\n", astatus);
        return close_and_fail(client);
    }

    const int sigfd_read = sigfds[0];
    for (char c; read(sigfd_read, &c, 1) == -1 && errno == EINTR;) {}
    jack_client_close(client);

    const int tty = open("/dev/tty", O_WRONLY);
    if (tty != -1) {
        while (write(tty, "\n", 1) == -1 && errno == EINTR) {}
        close(tty);
    }
    return EXIT_SUCCESS;
}
