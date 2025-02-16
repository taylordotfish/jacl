#define _POSIX_C_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <jack/jack.h>
#include <jack/metadata.h>
#include <jack/midiport.h>
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
        bin = "jack-stdin-to-midi";
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

typedef struct Node {
    struct Node * _Atomic next;
    size_t length;
    unsigned char message[];
} Node;

typedef struct State {
    jack_client_t *client;
    jack_port_t *port;
    Node *malloc_head;
    Node * _Atomic head;
    Node * _Atomic tail;
} State;

static Node *node_new(const size_t length, unsigned char * const message) {
    Node * const node = malloc(sizeof(Node) + length);
    if (node == NULL) {
        abort();
    }
    atomic_init(&node->next, NULL);
    node->length = length;
    if (message != NULL) {
        memcpy(node->message, message, length);
    }
    return node;
}

static Node *node_blank(void) {
    return node_new(0, NULL);
}

static void push_back(State * const state, Node * const node) {
    assert(atomic_load_explicit(&node->next, memory_order_acquire) == NULL);
    Node * const old =
        atomic_exchange_explicit(&state->tail, node, memory_order_acq_rel);
    assert(old);
    atomic_store_explicit(&old->next, node, memory_order_release);
}

static void free_excess(State * const state) {
    Node * const head =
        atomic_load_explicit(&state->head, memory_order_acquire);
    Node *node = state->malloc_head;
    state->malloc_head = head;
    while (node != head) {
        assert(node);
        Node * const next =
            atomic_load_explicit(&node->next, memory_order_relaxed);
        free(node);
        node = next;
    }
}

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

    void * const buffer = jack_port_get_buffer(port, nframes);
    if (buffer == NULL) {
        return -1;
    }

    jack_midi_clear_buffer(buffer);
    Node *node = atomic_load_explicit(&state->head, memory_order_relaxed);
    while (true) {
        Node * const next =
            atomic_load_explicit(&node->next, memory_order_relaxed);
        if (next == NULL) {
            break;
        }
        node = next;
        jack_midi_event_write(buffer, 0, node->message, node->length);
    }
    atomic_store_explicit(&state->head, node, memory_order_release);
    return 0;
}

static int hex_to_int(const char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void handle_line(
    State * const state,
    const char * const line,
    const size_t len
) {
    free_excess(state);
    if (len & 1) {
        fputs("bad message length\n", stderr);
        return;
    }
    Node * const node = node_new(len / 2, NULL);
    for (size_t i = 0; i < len; ++i) {
        const char c = line[i];
        int value = hex_to_int(c);
        if (value == -1) {
            fprintf(stderr, "invalid hex digit: %c (0x%x)\n", c, c);
            return;
        }
        if (i % 2 == 0) {
            node->message[i / 2] = value << 4;
        } else {
            node->message[i / 2] |= value;
        }
    }
    push_back(state, node);
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

    const char * const name = argc > argi ? argv[argi] : "js2m";
    jack_status_t status = 0;
    jack_client_t * const client =
        jack_client_open(name, JackNoStartServer, &status);
    if (client == NULL) {
        fprintf(stderr, "jack_client_open() failed: 0x%x\n", (int)status);
        return EXIT_FAILURE;
    }

    Node * const blank = node_blank();
    State state = {
        .client = client,
        .port = NULL,
        .malloc_head = blank,
        .head = blank,
        .tail = blank,
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
        "out",
        JACK_DEFAULT_MIDI_TYPE,
        JackPortIsOutput,
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

    char line[1024];
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
                    if (buf[i] == 'X') {
                        // Discard partial line.
                        linelen = 0;
                        continue;
                    }
                    if (buf[i] == '\n') {
                        line[linelen] = '\0';
                        handle_line(&state, line, linelen);
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
