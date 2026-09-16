# An ESP32-S3 sink

`hearth_sink` makes an ESP32-S3 board a network sink. It is a Sendspin player that Music
Assistant can play stereo to, and that decodes AC-3 and E-AC-3, Atmos objects included, for its
own speakers. This page takes a board from a checkout of this repository to playing in a group.
The example's own [README](https://github.com/iainchesworthlabs/ac3forge/blob/main/esp-idf/ac3forge/examples/hearth_sink/README.md)
is the reference for everything here, with the measurements behind it.

!!! note "Status as of 2026-09-16: played in a group on two boards, with no DAC wired"
    Two boards played one E-AC-3 JOC programme for ten minutes as a group, one at 2.0 and one at
    5.1, with no underrun and their play times within 549 µs of each other. The 2.0 board's
    levels matched a test sink's. Nothing was wired to either board's I2S pins, so nothing was
    heard: the peripheral clocked the audio out with nothing listening. `ac3hearth` cannot play
    to a sink yet. Until it can, a developer tool, `ac3hearth-testserver`, plays E-AC-3 to
    boards ([Play AC-3 and E-AC-3](#play-ac-3-and-e-ac-3)).

## What you need

- **An ESP32-S3 board with 8 MB of octal PSRAM.** The ESP32-S3-DevKitC-1-N16R8 is the board
  this was built on and measured with. Wi-Fi and the E-AC-3 decoder do not fit the part's
  internal RAM together, so the network build needs PSRAM ([Memory](#memory)). A board with quad
  PSRAM needs `CONFIG_SPIRAM_MODE_QUAD` in place of `sdkconfig.psram`'s octal mode; no such
  board has run it.
- **A USB cable** to the S3's own USB connector (the one marked USB on a DevKitC-1). It carries
  the flashing, the console and the network setup.
- **ESP-IDF v6.1**, installed as
  [Espressif's guide](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/get-started/index.html)
  describes, and a checkout of this repository.
- **A 2.4 GHz Wi-Fi network** that passes mDNS between the board and the server. The S3 has no
  5 GHz radio.
- **For sound, a DAC** on the I2S pins ([Wiring](#wiring)). A board with nothing wired still
  plays and reports its levels.

## Build and flash

In an ESP-IDF v6.1 terminal, from `esp-idf/ac3forge/examples/hearth_sink`:

```bash
export SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.hw;sdkconfig.psram;sdkconfig.sendspin"
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

In PowerShell, set the first line as `$env:SDKCONFIG_DEFAULTS = "..."`. `PORT` is the board's
serial port, such as `COM5` or `/dev/ttyACM0`. The three files over the defaults do this:

| File | What it sets |
|---|---|
| `sdkconfig.hw` | The console on the S3's own USB connector, so one cable does everything |
| `sdkconfig.psram` | Octal PSRAM for the decoder's large allocations and the player's ring, a deeper I2S queue, and a 32 KB instruction cache |
| `sdkconfig.sendspin` | The Sendspin player on Wi-Fi, the page and the REST routes on port 80, nothing played at boot, and lwIP's task on core 0 beside the network |

`idf.py set-target` writes a fresh `sdkconfig` from those files. After that, `idf.py menuconfig`
changes one build: the pins, the slot width and the layout are under *ac3forge hearth sink*, and
the player's buffers under *Sendspin player*. If you build more than one shape, give each its own
`-B <build directory>` and `-DSDKCONFIG=<build directory>/sdkconfig`, or the second inherits the
first's settings.

After flashing over the S3's own USB connector, the chip can stay in its download mode, with
`boot:0x0 (DOWNLOAD)` on the console. Press RESET. The port disappears and comes back at every
reset, so `idf.py monitor --no-reset` is the way back to the console after one.

`idf.py flash` leaves the NVS partition alone, so a board keeps its network, its name, its keys
and its pairings when you flash a new build. `idf.py -p PORT erase-flash` clears all of them.

## Join a network

A board that has no network to join listens for [Improv Wi-Fi](https://www.improv-wifi.com/) on
its console port. A freshly flashed board first tries the network its build names, `my-network`
unless you set another under *ac3forge hearth sink* in `idf.py menuconfig`, and listens once that
fails:

1. Close the serial monitor (`Ctrl+]`), so the port is free.
2. Open [improv-wifi.com](https://www.improv-wifi.com/) in a browser with Web Serial (Chrome or
   Edge on a computer) and connect to the board's serial port.
3. The board says it is an *AC3Forge Hearth sink* and gives its name. It does not scan for
   networks, so type the network's name, then its passphrase.
4. The board stores both and joins, then advertises itself and starts its player. Improv offers
   a link to the board's page, `http://<address>/`. If the join fails, Improv says the board
   could not connect, and you can send the network again. A network that accepts the board but
   gives it no address within 30 s counts as a failed join.

A board is called `hearth-` followed by the last six hex digits of its MAC address until you
name it. It advertises `_sendspin._tcp` over mDNS under its name, which is how a server finds it,
and answers as `<name>.local`.

To move a board to another network, use the page's *Network* field, or `PUT /network` with the
SSID and passphrase on two lines. The board joins that network at its next restart. A board that
cannot join its stored network when it starts listens for Improv again, so give it the new
network the same way.

## The page

`http://<name>.local/`, or the board's address, shows three sections:

- **Now:** what the board is doing.
- **Sendspin:** the server playing to it, its clock, the stream's timing, underruns, the levels
  of each output over the last 100 ms, and how many servers are paired. While a pairing runs, a
  six-digit code is shown at the top.
- **Settings:** the name, the slot width, whether a second I2S line is wired, the output layout
  and the network.

The same routes answer without the page (`GET /api` lists them): `GET /status` for a script, and
`PUT /name`, `/layout`, `/slot-width`, `/wiring` and `/network` to change what the board is. A
new name is advertised from the board's next restart. The page and the routes have no password
([Security](#security)).

## Pair a server

Every connection to the player is encrypted with Noise, and a server plays to the board only once
the two are paired. There are two ways to pair.

**By the token.** The console prints the board's pairing token at every start, and again when you
type `pair token`:

```
sendspin: pairing token SP:0AO4BQKDC3YEKAULMZ4UMZFDRQGZSLVY9...
```

A server given the token pairs straight away. Whoever has it can pair, so it is printed nowhere
else.

**By a code.** A server that pairs by code makes the board show six digits, on the console as
`sendspin: PAIRING CODE 482-913` and at the top of the page's Sendspin section, for as long as the
pairing runs. Type them into the server. After twenty wrong codes the board refuses to pair until
you type `pair reset` or choose *Allow pairing again* on the page.

The board keeps eight pairings. Lines typed on the console are commands:

| Command | What it does |
|---|---|
| `pair token` | Prints the pairing token again |
| `pair reset` | Allows pairing again after twenty wrong codes |
| `pair cancel` | Ends the pairing that is running |
| `pair forget` | Removes every pairing and gives the board a new identity and token; *Forget every server* on the page does the same |
| `sendspin` | Prints the player's state: the server, the role, the clock, and the stream's counters |

## Play from Music Assistant

Music Assistant finds the board over mDNS and lists it as a Sendspin player. Once paired, it
plays stereo PCM at 48 kHz, 16 or 24 bits, to the board's `player@v1` role. The board renders
that stereo pair onto its layout: left and right go to the layout's front left and right
speakers, and the other speakers stay silent. On a one-speaker layout, the two channels are
mixed. FLAC and Opus are not offered, because their decoders are not on the board.

A board plays for one server at a time. Music Assistant stays connected to every player it has
found, so a board usually holds its connection when another server dials. As the Sendspin
specification sets out, a server that asks to play takes the board from one that is only
connected or is already playing. The exception is a pairing in progress, which is not
interrupted.

## Play AC-3 and E-AC-3

AC-3 and E-AC-3, with any Atmos objects, go to the board undecoded through Hearth's own role,
`_ac3forge_player@v1`. The board decodes the stream, renders it onto its layout, and applies the
server's settings: the layout, routing, trims, delays and decoder settings.

`ac3hearth` will send it. Until then, `ac3hearth-testserver` plays one file to one or more boards
as a group. It is a developer tool, built from this repository with the `hearth` feature:

```bash
cmake --preset config-linux-gcc -DVCPKG_MANIFEST_FEATURES=hearth -DAC3FORGE_BUILD_HEARTH=ON -DAC3FORGE_BUILD_EXAMPLES=OFF
cmake --build --preset build-linux-gcc --target ac3hearth-testserver
```

Then, with the board's address and the token from its console:

```bash
build/config-linux-gcc/bin/ac3hearth-testserver --play programme.ec3 \
  --player ws://192.168.1.40:8928/sendspin --token SP:0... --layout 5.1 \
  --status http://192.168.1.40/status
```

The server pairs with the board, gives it the layout, plays the file, and prints what the board
reported. `--code-log FILE` pairs by code instead: it reads the code from a file the board's
console is being written to. `--help` lists the rest.

## Groups

A group is every player a server plays one programme to. Each board decodes and renders the
programme onto its own layout, and plays each sample when the server's clock says, so boards
with different layouts stay together. Each board follows the server's clock with Sendspin's time
filter. On a home Wi-Fi network, two boards' reported play times stayed within 549 µs of each
other over ten minutes. Those are the times the software scheduled: nobody has measured two DAC
outputs together.

With the test server, give a `--player` and its options for each board:

```bash
ac3hearth-testserver --play programme.ec3 \
  --player ws://192.168.1.40:8928/sendspin --token SP:0... --layout 2.0 \
  --player ws://192.168.1.41:8928/sendspin --token SP:0... --layout 5.1
```

Music Assistant has groups of its own: group the boards there.

## Wiring

| Signal | Default GPIO | Kconfig |
|---|---|---|
| Bit clock (BCLK) | 5 | `CONFIG_AC3FORGE_EXAMPLE_I2S_BCLK_GPIO` |
| Word select (WS, LRCK) | 6 | `CONFIG_AC3FORGE_EXAMPLE_I2S_WS_GPIO` |
| Data out (DOUT) | 7 | `CONFIG_AC3FORGE_EXAMPLE_I2S_DOUT_GPIO` |
| Second line's data out | 8 | `CONFIG_AC3FORGE_EXAMPLE_I2S_DOUT2_GPIO` |

The defaults avoid the S3's strapping pins, its USB pair and the console UART; any free GPIO will
do. The signals are 3.3 V. The board sends no MCLK, so a DAC that needs one needs it added.

- **A stereo I2S DAC**, such as a PCM5102 or MAX98357A, plays a `2.0` or `1.0` layout: one or two
  channels open standard I2S.
- **A TDM DAC** plays three or more channels, which open TDM. A TDM DAC set up for one frame
  shape over I2C, such as an ESS ES9080, needs `CONFIG_AC3FORGE_EXAMPLE_I2S_FIXED_FRAME=1`, which
  opens the full TDM frame for every layout, stereo included. The firmware does not set the DAC
  up over I2C: whatever does has to match the slot width below.
- **A second line** carries a second DAC. Wire its data pin, and tick *A second I2S line is wired
  to a DAC* on the page (or `PUT /wiring` with `1`). It shares the first line's bit clock and
  word select. Whether the two lines stay sample-aligned with DACs on both has not been checked.
- **A DSP that supplies the clocks**, such as an ADAU1452, needs
  `CONFIG_AC3FORGE_EXAMPLE_I2S_SLAVE=1`. This has not been tried.

## Slot widths

An S3 I2S line carries at most 128 bits a frame, so the slot width decides how many channels a
line takes:

| Slot width | Samples | Channels on one line | With a second line |
|---|---|---|---|
| 32 bits (the default) | 24-bit, left-justified | 4 | 8 |
| 16 bits | 16-bit | 8 | 16 |

So a `5.1` or `7.1` layout needs 16-bit slots, or 32-bit slots on two lines. `7.1.4` and
`9.1.6` need 16-bit slots on two lines. Set the width to what the DAC is set up for: *Slot width*
on the page, or `PUT /slot-width` with `16` or `32`. It takes effect at the next play, and is
refused while one runs. A layout that no longer fits after a change fails at the next play, and
the page says why.

## Layouts

The layout is the board's speakers, one per output slot: a name such as `2.0`, `5.1`, `5.1.4` or
`7.1.4`, or a list such as `L,R,C,LFE,Ls,Rs` in the order the DAC's outputs are wired. Set it on
the page or with `PUT /layout`, and it applies from the next chunk the board plays. A server that
sends its own settings replaces it. At `2.0` and `1.0` the decoder folds the programme down. On a
wider layout each channel plays at its own location, objects are placed by their positions when
the layout has height speakers, and nothing is upmixed. The
[README](https://github.com/iainchesworthlabs/ac3forge/blob/main/esp-idf/ac3forge/examples/hearth_sink/README.md#layouts-and-more-than-two-channels)
has the grammar and the rules.

## Memory

Internal RAM is what this build is short of. Wi-Fi, lwIP, two HTTP servers, the Sendspin session
and the decoder share about 280 KB of it, and the decoder's allocations of up to 16 KB go there
first. In the ten-minute group run, the least internal RAM free while a stream played was
139 bytes on the 2.0 board and 23 bytes on the 5.1 board. Nothing failed in those runs, but in
an earlier one-minute run a 108-byte allocation did, with no effect on the stream. The console's
`sendspin.progress` lines report it as `heap_least` every four seconds or so.

## Security

- **The page and its routes have no password.** Anyone who can reach the board on the network can
  play a URL on it, change its settings or the network it joins at its next restart, and forget
  its pairings. Anyone who can load the page during a pairing can also read the code, and so pair
  a server of their own. Keep the boards on a network you trust.
- **The token is the credential.** It pairs a server with no code, and only the console prints
  it.
- **Keys are stored unencrypted.** The board's identity, its pairing token, its pairings and the
  Wi-Fi passphrase are in NVS, which these builds do not encrypt: anyone holding the board can
  read them.

[The threat model](../threat-model.md#sendspin-hearths-server-and-its-sinks) has the rest.

## When something goes wrong

- **The server does not list the board.** The console's `mdns:` line says whether the board
  advertised itself. Check that mDNS passes between the board's network and the server's: an
  access point that isolates its clients, or a second VLAN, stops it. `http://<address>/status`
  shows whether the board is up.
- **Improv says the board could not connect.** Check the network's name and passphrase, and that
  it is a 2.4 GHz network, then send them again. The board keeps listening.
- **Pairing fails.** Type `sendspin` on the console to see the player's state. After twenty
  wrong codes, `pair reset`. A server that paired before `pair forget` has to pair again.
- **Underruns.** The page's Sendspin section counts them. The console's closing line for each
  stream gives them, with the average time a chunk took to decode and render (`burst_us`). A chunk
  holds 32 ms of audio, so a `burst_us` near 32,000 means the board cannot keep up with the
  programme on its layout. Otherwise, look at the Wi-Fi between the board and the server.
- **A layout is refused.** It has more channels than the slot width and wiring allow
  ([Slot widths](#slot-widths)).
- **The console shows `boot:0x0 (DOWNLOAD)` after flashing.** Press RESET.

## Where to go next

- [The example's README](https://github.com/iainchesworthlabs/ac3forge/blob/main/esp-idf/ac3forge/examples/hearth_sink/README.md):
  every setting, the REST routes, and the measurements.
- [ESP32-S3](../platforms/bare-metal/esp32-s3.md): the decoder on this part, and its timing and
  memory.
- [The plan](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/hearth-reference-player.md):
  what Hearth is building, and in what order.
