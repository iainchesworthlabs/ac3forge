# Install and first run

Mostly this is about the silent device — the thing that stops you hearing every application
twice. It works differently on each platform, and on one of them there is nothing to do at all.

## Windows

### Get it

`ac3forge-crucible-<version>-win64.zip`, from the releases page. It carries the window, the
console runner, its own Qt runtime, the driver's install and remove scripts, the third-party
notices (`NOTICES.txt`) and the licence (`LICENSE.txt`). Unpack it somewhere and run
`ac3crucible.exe`. About > Licences… shows the same notices from inside the window.

It is a separate download from the main `ac3forge` package, and stays one while its driver is
test-signed.

### The silent device

Crucible needs a virtual output device that discards what it is given. Today that driver is
**test-signed**, which means it loads only on a machine that has test signing on and memory
integrity off. On a normal machine it will not load, and the Settings page says so with the two
commands that change it.

```
bcdedit /set testsigning on
```
then restart. Memory integrity is under Windows Security → Device security → Core isolation.

Both are machine-wide security settings. Turning them off to run a demo driver is a real
trade, and the honest position is that it is what a development machine does — not something to
ask of a machine you care about. This goes away when the driver is attestation-signed: it will
then install with the application and need neither setting.

With test signing on, the Settings page's **Install driver** button runs the package's script
elevated. "Speakers (Desktop Atmos)" then appears in your sound settings.

### Without the driver

Crucible still runs. Taps, placements and every output mode work; you will simply hear the direct
mix as well, because applications are still playing to a device you can hear. Two ways around it
short of installing the driver:

- make some endpoint you cannot hear the default — a monitor with no speakers, an idle virtual
  cable, a muted device — and point Crucible's silent-device filter at it under Settings →
  Advanced;
- or accept the doubling while you try it out.

The window says which of these you are in rather than leaving you to work it out.

## Linux

### Requirements

- **PipeWire.** Crucible cannot use the ALSA backend — it exists to tap each application
  separately, and ALSA has no per-application concept at all. A build configured against ALSA is
  refused at configure time with a message saying so.
- `libpipewire-0.3-dev` to build, `pipewire` and a session manager (WirePlumber) to run.
- `libxcb1-dev` for the full-screen rule on X11 (optional; without it the rule is off and the
  Room page says why).
- `qt6-svg-dev` to build and `libqt6svg6` to run, for application icons that exist only as
  SVG in the icon theme (`org.gnome.*` applications, most Flatpaks). Qt SVG is optional: without
  it the configure log says so and those applications show the monogram.

### Build it

Releases do not carry a Linux Crucible package yet: the release legs build the library against
ALSA, and Crucible is only packaged from a PipeWire build. A checkout produces one — the
`crucible` CPack component gives `ac3forge-crucible-<version>-Linux-<arch>.tar.gz` and, where
`dpkg-deb` exists, an `ac3forge-crucible` `.deb` (`cpack -D CPACK_COMPONENTS_ALL=crucible`
in the build directory, [docs/releasing.md](../releasing.md)) — but the plain route is to build
it. From a checkout:

```bash
cmake --preset config-linux-gcc -B build/crucible -DAC3FORGE_BUILD_CRUCIBLE=ON -DAC3FORGE_WITH_ALSA=OFF -DAC3FORGE_WITH_PIPEWIRE=ON
```

```bash
cmake --build build/crucible --target ac3crucible ac3crucible-run
```

`-DAC3FORGE_WITH_ALSA=OFF` is not optional. With ALSA headers present the library selects ALSA,
and Crucible refuses to build against it.

The tarball and the `.deb` carry the notices under `share/doc/ac3forge-crucible/`:
`NOTICES.txt` (what the build links and embeds, with each licence), `LICENSE.txt`, and the
notices once more as `copyright`, the name Debian tools look for. About > Licences… shows the
same text.

### The silent device

Nothing to install. Crucible creates a PipeWire node called **"Crucible (silent)"** while it
runs, and it disappears when Crucible exits. No driver, no signing, no password, nothing left on
your machine. The Settings page's silent-device block says as much on Linux — its button reads
**Create device** rather than Install driver, and there is no driver folder under Advanced,
because there is nothing to point at.

Send applications — from the first-run dialog, the Room rail or the Signal path page — creates
the node if it is not there yet, in the same press; Settings' Create device makes it ahead of
time. Crucible sets it as your default sink through PipeWire's `default.audio.sink` metadata,
the same key `wpctl set-default` writes, and restores your previous default on exit.

### What you get, and what you do not

The window builds and runs on Linux (`ac3crucible`), and so does `ac3crucible-run`, a console
runner over the same engine that lists the applications it can see, takes positions, and reports
which output it chose and why. Building the window needs Qt 6.5 or later with Quick, Quick
Controls 2, Widgets and Linguist Tools; Quick 3D is optional and adds the 3D room, Qt SVG is
optional and adds the SVG-only icons.

Application icons come from the icon theme. An application gets one when it names an icon
through PipeWire (`application.icon-name`), when a `.desktop` entry matches it — by its Flatpak
application id, or by `Exec`, `TryExec`, `StartupWMClass` or `Name` — or when the theme has an
icon named after its binary. A script, an interpreter or a command-line player (`python3`, `sh`,
`aplay`) has none of those, and shows the monogram.

```
ac3crucible-run [--null-sink SUBSTR] [--key PATH] [--pin MODE]
  list                      the applications and their slots
  pos <app> <x> <y> <z>     position one (x,y in [0,1], z in [-1,1])
  bed <app>                 send it back to the bed
  status                    one line of engine state
```

Two gaps worth knowing before you start:

- **The full-screen rule is off under Wayland.** The rule makes the full-screen application the
  bed. Under X11 it is on: Crucible reads the active window's `_NET_WM_STATE` and `_NET_WM_PID`
  through libxcb. No Wayland client can ask which window is full-screen — that is Wayland's
  security model — so there the rule is off, and the Room page says which reason applies
  (Wayland, no display, or a build without libxcb) rather than silently dropping the rule.
- **A bitstream has reached a receiver's HDMI sink from Linux, but its lock is not yet
  confirmed.** WirePlumber enables a sink's compressed codecs from the display's own EDID (the
  `iec958.codecs` property), so on a receiver that advertises them nothing needs configuring by
  hand — and Crucible only offers a bitstream mode on a sink that has them. What is not yet
  written down is a receiver's display reading "Dolby Digital Plus" during a Linux stream.

## macOS

Not built. The design is settled and needs no driver — macOS process taps can mute an application
where they capture it — but it is blocked on a Mac to run it and a Developer ID certificate to
sign it, since the tap's consent prompt does not fire for an unsigned binary.

[The plan](promotion.md) has the detail.

## First run

The first time Crucible opens, a dialog says what it is about to do to your sound settings
before it does it. It names the silent device this platform uses ("Desktop Atmos" on Windows,
"Crucible (silent)" on Linux), says that your default output will move to it so that every
application plays into it and Crucible taps each one there, and that the previous default is put
back when Crucible quits. If there is no silent device yet, it says how this platform gets one,
and on Windows repeats what stands in the way of loading the driver.

Three ways out:

- **Send applications to …** moves the default output now. On Linux this also creates the node
  if it is not there yet.
- **Not now** changes nothing; the same button on the Room rail and the Signal path page does
  the move later.
- **Open Settings** goes to the Settings page, where the silent device's state and the install
  tools live.

A tick in the dialog, "Do this every time Crucible starts", is the Behaviour setting *Move the
default output to the silent device on launch*, and can be changed in Settings at any time. The
dialog is shown once per user; closing it by any route, Escape included, counts as seen. A
machine whose settings were carried over from the Desktop Atmos demo sees it once too, and the
dialog says so, because the demo never explained this.

The header then shows the path as a single line — `apps → stereo · Your Receiver`, or
`⚠ apps heard direct → …` while the default output is still a device you can hear — and the
Room rail carries the one button that fixes it.

Quitting from the tray, or closing the window with "Keep running in the tray" off, restores the
previous default output when Crucible moved it; a default you moved by hand is left where you
put it. Closing the window while that setting is on only hides it, so applications stay on the
silent device until you quit or press Restore.

On a platform that never moves the default (macOS, when its half exists), the dialog says that
nothing in the sound settings changes and offers no Send.

## A signing key, on any platform

Without one, Crucible streams 5.1 and your placements pan within the bed; height does nothing.
With one, you get Atmos objects.

Point Settings at a key file, or set the same environment variable `ac3cli` reads:

```bash
export AC3FORGE_SIGNING_KEY_FILE=/path/to/key
```

Crucible stores the *path*, never the key material. Plaintext key material never reaches a
settings file, a log or a crash dump, and the diagnostics file Settings can save carries neither
the key nor the path to it (see
[Saving a diagnostics file](troubleshooting.md#saving-a-diagnostics-file)).
