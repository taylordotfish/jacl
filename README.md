jacl
====

Command-line clients for [JACK](https://jackaudio.org).

* jacl-cv: a CV output port whose value is set from standard input.
* jacl-midi2stdio: writes incoming JACK MIDI to standard output.
* jacl-stdio2midi: converts standard input into JACK MIDI output. Together with
  jacl-midi2stdio this can be used to tunnel MIDI data over a network.

Building
--------

Ensure JACK’s development files are installed (e.g., `libjack-dev` or
`libjack-jackd2-dev`), then run `make`.

Once compiled, pass `--help` to any of the programs for a detailed usage
description.

License
-------

jacl is licensed under the GNU General Public License, version 3 or (at your
option) any later version. See [LICENSE].
