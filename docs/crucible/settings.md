# Settings

`Ctrl+3`, the page switch in the header, or **Settings…** in the tray menu where there is one.
The page is seven numbered blocks, in the order that matters on a new machine: the two things that
make the whole thing work, then how it sounds, then how it looks and behaves, then the file you
attach to a bug report. It lays out as two columns when the window is wide enough and one when it
is not, and every block wraps inside its column.

Nothing here has an Apply button. Each control writes its setting as you change it, and the
settings live in Qt's own per-user store — the registry on Windows, an INI file under
`~/.config/ac3forge/` on Linux — under the organisation `ac3forge` and the application
`Crucible`. A machine that ran the Desktop Atmos demo has that tree copied across the first time
Crucible starts with nothing of its own, and the demo's tree is left where it is rather than
deleted.

**Four of these restart the stream** when you change them: latency, bitrate, the split-by-default
tick, and the silent device's name. The engine stops and starts again, which is a gap in the
sound, and the status strip says where it ended up.

## 01 Silent device — where applications play

This is the block that makes the rest of the window mean anything, and it is the one that differs
most between platforms. What it is and why there have to be two devices is
[The signal path](signal-path.md); what to do about it on your platform is
[Install and first run](install.md).

The card states three things in the order they matter, each with a tick or a warning sign:

1. **whether the silent device exists** — an endpoint whose name matches the filter;
2. **whether applications play to it** — whether it is the system default output, and if not,
   which device is, with a pointer to the Room or Signal path page where the button that moves it
   lives;
3. **what stands in the way**, shown only while there is no silent device, in the platform's own
   words.

The button under them reads **Install driver** on Windows and **Create device** on Linux, because
they are different actions and the page does not pretend otherwise; it is there only while there
is no silent device. **Check again** stays, and re-reads the machine without changing anything.

**On Windows** the silent device is a kernel driver, because Windows has no user-mode way to
create a render endpoint. The button runs the package's install script elevated, so a UAC prompt
belongs to it. While that driver is test-signed the machine also needs test signing on and memory
integrity off, and it is that pair the third line names when the driver cannot load. An installed
copy of Crucible brings the driver with it; a build from source has to be pointed at a built
package.

**On Linux** there is nothing to install. Crucible creates a PipeWire `support.null-audio-sink`
node named "Crucible (silent)" while it runs and takes it away when it exits, so the action needs
no elevation and cannot fail for signing reasons. The page says as much instead of offering a
driver folder that would mean nothing there.

**On macOS** there is nothing here to install. The design needs no silent device at all, because
process taps mute each application where they capture it, so there would be nothing to install and
no default output to move: the macOS seam answers that a silent device is not needed
(`apps/crucible/engine/platform/macos/virtual_device.cpp`), and the whole card is hidden wherever
a platform answers that way. That is read off the source rather than seen: Crucible's macOS half
has never run on a Mac, CI compiles it rather than running it, and whether it compiles past
configure is not yet known ([Install](install.md#macos)).

### Advanced

A disclosure under the card, closed by default, holding what a source build and an unusual machine
need.

- **Driver folder** — where `install.ps1`, `remove.ps1` and the built package live: beside the
  application by default, or `apps/windows/driver` in a source tree. Windows only; on Linux there
  is no folder to point at, and the row is not shown.
- **Remove driver** / **Remove device** — undoes what the button above installed or created. On
  Linux this removes the application's own node, which also goes when the application does. It
  shares the Create device button's enabling condition there, so it greys once the node exists and
  quitting is what takes the node away in practice; that is read off the source rather than tried
  on a machine.
- **Silent device** — the name filter. Any endpoint whose name contains this text is treated as
  the silent device and is never chosen as an output. It defaults to the platform's own name for
  its silent device — "Desktop Atmos" on Windows, "Crucible (silent)" on Linux — and it is the
  setting that lets a machine with no driver point Crucible at some other endpoint it cannot hear
  ([Install](install.md#without-the-driver)). Changing it restarts the stream.

## 02 Signing key

Objects need a signing key. Without one the stream is the 5.1 bed only and your placements pan
within it; [the room page](room.md#what-height-and-depth-do-and-what-they-do-not) says the same
thing where the placing happens, and says what height and size stop doing.

**Browse…** points Crucible at a key file and **Clear** forgets it. A coloured dot and one
sentence from the engine below them say what the key did. Loading or clearing a key does not need
a restart: the encoder is rebuilt and the output re-probed in place, so the mode line follows on
the next probe.

Only the **path** is stored, under the settings prefix `signing/`; the key material stays in its
file. With no file chosen here the environment is honoured instead — `AC3FORGE_SIGNING_KEY_FILE`
names a key file and `AC3FORGE_SIGNING_KEY` carries the key itself, the same two variables
`ac3cli` reads. Neither the key nor the path reaches a log or the diagnostics file; see
[block 07](#07-diagnostics) and
[Saving a diagnostics file](troubleshooting.md#saving-a-diagnostics-file).

## 03 Latency

**Normal · 32 ms frames** or **Low · 5.3 ms frames**. Low latency shortens the E-AC-3 frame to one
block and raises the bitrate to about 1.5 Mb/s, so that fifteen objects' metadata still fits in
the shorter frame. It shortens Crucible's own cadence and leaves the receiver's decode delay where
it was. What the whole chain comes to has not been measured end to end;
[Troubleshooting](troubleshooting.md#sound-is-behind-the-picture) carries the estimate. Changing
it restarts the stream.

## 04 Codec

**Bitrate** is automatic or one of seven fixed rates from 256 kb/s to 2048 kb/s. Automatic is
448 kb/s, or 1536 kb/s in low latency. Changing it restarts the stream.

**Split stereo applications into two objects** is the default for applications the engine meets
from now on; the Split button on the Room page overrides it either way for one application. That
button sits on the selected application's card under the room, while the note beside this tick
still places it on the application's row in the rail. A split costs a second slot, and an
application that cannot get two free ones waits in the bed until it can. Changing this restarts
the stream, which is what applies it to the applications the engine already has.

## 05 Appearance

**Theme** is System, Light or Dark. System follows the platform's colour scheme and changes with
it while the window is open.

**Palette** is System, Signal, Ink or Console. Signal is the design system's red, Ink a cool blue,
Console a studio amber. System derives its accent ramp from the desktop's own accent colour and
wears Ink's neutrals. The three named palettes are checked for contrast by the test suite in both
modes ([Colour and contrast](accessibility.md#colour-and-contrast)). The System palette is not
among them: its accent comes from the desktop while the window runs.

**Language** is System, English, or one of the six the window is translated into: French, German,
Spanish, Arabic, Hebrew and Yiddish. Choosing one of the six swaps in its translators, the
application font where the language needs glyph coverage the window's own face has not, and the
window's layout direction, and retranslates the running window without a restart; English takes
the translators away and leaves the source strings. Every string the window has is translated in
all six: the catalogues carry 385 messages each and none is left unfinished, so nothing falls back
to English. What is not settled is whether each rendering is the right one. The translations are
machine-made and no speaker of any of the six has read them, which is what the window's own note
under this control says. That note also words System as following the language the desktop is set
to, which is what it does on Windows and on Linux alike. [Languages](localisation.md) is the
glossary they are held to and the review that has yet to run.

**Text size** is 100%, 125%, 150%, 175% or System, and every size in the window is a multiple of
it. What System means, and why it starts a Linux window larger than a Windows one, is
[Text size](accessibility.md#text-size).

**3D layout** is the reference speaker layout the [3D room](room.md#the-3d-view) draws — Auto,
5.1, 7.1 or 7.1.4. Auto shows 5.1 while the stream is the bed only and 7.1.4 once objects are on.
It changes the picture and nothing about the stream.

## 06 Behaviour

**Move the default output to the silent device on launch.** Off by default. With it on, Crucible
moves your default output when it starts rather than waiting for you to press Send applications,
and restores the previous device on quit. It is the same setting as the tick in the first-run
dialog ([First run](install.md#first-run)).

**Keep running in the tray when the window is closed.** On where there is a tray, and closing the
window then hides it: the engine keeps running, applications stay on the silent device, and the
tray icon brings the window back. Quitting from the tray is what puts your default output back.

The setting is **unavailable on a session with no tray** — no Windows notification area, no Linux
panel that shows tray icons. There is then nowhere to keep the window, so the tick is disabled,
the note beside it says which, and closing the window quits.
[Troubleshooting](troubleshooting.md#there-is-no-tray-icon) says how to check what your session
has.

Linux published no tray at all before 2026-09-06, because publishing one crashed the window on
nine or ten launches out of ten. That was a bug in Qt rather than a preference — a `Menu` nested
inside a tray icon's menu is handed Qt's QWidget fallback and then read as a D-Bus menu — and
the tray's menu is flat now, which is why the signal path appears there as a heading and seven
choices rather than as a submenu. `apps/crucible/ui/platform/linux/tray_support.cpp` carries the
finding.

**Show applications with no audio.** On by default. Running applications with a window but no
audio session, greyed until they play. Off hides them unless they are placed. This is a Windows
distinction in practice: on Linux an application without a stream is not in the list to hide
([An application is not in the list](troubleshooting.md#an-application-is-not-in-the-list)).

**Show background processes in the room.** Off by default. Processes with sound but no window of
their own — a virtual machine's backend, the text-input host. They stay in the bed whether or not
they are shown. This one is Windows-only in effect as well: there is no portable way on Linux to
ask whether a process owns a visible window, and no way at all under Wayland, so the Linux build
reports everything with an audio stream as an application and has nothing to hide by this rule.

## 07 Diagnostics

**Save diagnostics…** writes a plain-text file for a bug report, suggesting a dated name in your
Documents folder; the page says where it went once it is written. The note above the button says
both what the file holds and what it withholds, so that it is read before it is attached: it
carries the version and platform, the engine's counters, the endpoints the probe found, the two
devices of the signal path, this application's settings and its recent messages, and it names your
audio devices and running applications. It does not carry the signing key, the path to the key
file, or the value of any environment variable.

The full contents, the two limits and how the withholding is held are in
[Saving a diagnostics file](troubleshooting.md#saving-a-diagnostics-file).
