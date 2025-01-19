# Use `make DEBUG=1` to enable debug options.
DEBUG =
ifdef DEBUG
  OPT = -Og -ggdb
else
  OPT = -O3 -DNDEBUG
endif

CFLAGS += -std=c11 -Wall -Wextra -pedantic $(OPT)

ALL = jackcv jack-stdin-to-midi jack-midi-to-stdin

.PHONY: all
all: $(ALL)

jackcv: jackcv.c
jack-stdin-to-midi: jack-stdin-to-midi.c
jack-midi-to-stdin: jack-midi-to-stdin.c

$(ALL):
	gcc $< -o $@ -ljack $(CFLAGS)

.PHONY: clean
clean:
	rm -f $(ALL)
