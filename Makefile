# Use `make DEBUG=1` to enable debug options.
DEBUG =
ifdef DEBUG
  OPT = -Og -ggdb
else
  OPT = -O3 -DNDEBUG
endif

CFLAGS += -std=c11 -Wall -Wextra -pedantic $(OPT)

ALL = jacl-cv jacl-stdio2midi jacl-midi2stdio

.PHONY: all
all: $(ALL)

jacl-cv: cv.c
jacl-stdio2midi: stdio2midi.c
jacl-midi2stdio: midi2stdio.c

$(ALL):
	$(CC) $< -o $@ -ljack $(CFLAGS)

.PHONY: clean
clean:
	rm -f $(ALL)
