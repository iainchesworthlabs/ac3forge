# Keyboard and screen readers

Crucible can be operated without a mouse, and the parts of it a person acts on carry a role, a
name and a description for a screen reader. This page is the key map, the focus order, what is
announced, the text size setting, and what is still mouse-only.

## The key map

| Key | What it does |
| --- | --- |
| `Tab` / `Shift+Tab` | Move to the next or previous control. Disabled controls are skipped. |
| `Space` or `Enter` | Press the focused button, tick the focused checkbox, open the focused disclosure. |
| `Left` `Right` `Home` `End` | Choose within a segmented control (Room / Signal path / Settings, Theme, Palette, Text size, Latency, 3D layout). Left from the first wraps to the last. |
| `Ctrl+1` `Ctrl+2` `Ctrl+3` | The Room, the Signal path and the Settings page. |
| `F1` | About. |
| `Esc` | Close the open dialog. |

In the **applications list**:

| Key | What it does |
| --- | --- |
| `Up` / `Down` | Choose an application. The room, the bed tray and the selected card follow. |
| `Enter` or `Space` | Place the chosen application in the centre of the room and move the keyboard to the room. |

In the **room** — one tab stop that moves whichever application is chosen, whichever picture is
on screen:

| Key | What it does |
| --- | --- |
| `Left` / `Right` | Move it across the room, 0.05 of the width per press. |
| `Up` / `Down` | Move it towards the front or the back. |
| `Shift` + an arrow | The fine step, 0.01. |
| `Ctrl` + an arrow | The coarse step, 0.25 — a quarter of the room. |
| `Page Up` / `Page Down` | Raise and lower it. Height needs objects, so it does nothing while the stream is the 5.1 bed only, and the announcement says so. |
| `Home` | Put it back in the centre. |
| `Enter` | Place an application that is still in the bed; for one already in the room, say where it is. |
| `Delete` or `Backspace` | Send it back to the bed. |
| `Plus` / `Minus` | Grow and shrink the object's size. |

An application that is still in the bed starts from the centre of the room: one arrow press both
places it and moves it. A full-screen application does not move at all — it is the bed, by the
rule the Room page states — and the key says so rather than doing nothing.

On a **bed chip**, `Enter` places that application in the centre of the room and the keyboard
follows it there. On the **size slider**, `Left` and `Right` step by 5% (`Shift` for 1%),
and `Home` and `End` take it to a point and to the whole room.

## The focus order

Tab walks the window in reading order: the header's signal-path pill, the page switch, About,
then the page. On the Room page that is the applications list, the room-view switch where the
build has the 3D picture, the room itself, the selected application's buttons, the size slider,
the bed chips, the right rail, and the Start/Stop button in the status strip.

Whatever has the keyboard draws a two-pixel ring just outside its own border. Clicking a button
does not move the keyboard to it, so a mouse user sees no rings; clicking an application row, a
marker or a bed chip does, because that is where the arrow keys continue from.

The three pages are a stack and only one of them is on screen, so Tab never reaches a control on
a page you are not looking at.

## What is announced

A screen reader is told about a change once, when it happens:

- the engine starting, stopping, or refusing to start, with the reason;
- what you hear it on, when the mode or the endpoint changes;
- where applications play, when the default output moves;
- what the silent device did, when it is installed, created or removed;
- the signing key's state, when it changes;
- every keyboard move in the room: the application, where it now is in words, and the figures.

Crucible reads its own state a few times a second, and every one of those readings republishes
all of it. Each sentence is compared with the last one said for that fact, so a reading that
changed nothing says nothing. The same sentence goes into the diagnostics file, so a bug report
carries what the window said as well as what the engine did.

Every room object reports its name, where it is in words and in figures, whether it is the
chosen one, and an action that chooses it. Each station of the signal path is a named group
carrying its own warning, so a warning is heard as belonging to the stage it is about. Each
endpoint row is named for its endpoint, and so are the two buttons on it, which would otherwise
read as a dozen identical "Hear it here".

## Text size

**Settings → Appearance → Text size** is System, 100%, 125%, 150% or 175%. Every size in the
window is a multiple of it, and the controls derive their heights from their labels, so larger
text makes a taller button rather than a clipped one.

System is the default and reads the size the desktop's own text setting reports, which is how a
larger-text setting made outside Crucible reaches it.

## Colour and contrast

The palettes are checked by the test suite rather than by eye. In all three palettes and both
modes, body text reaches at least 7:1 against the background, muted text and the accent used as
ink at least 4.5:1, and the focus ring and control borders at least 3:1.

One number does not reach 4.5:1: the label on a primary button's accent fill. The fill is the
palette's accent, which is the design system's colour and is not darkened to suit the label, so
the label takes whichever end of the palette reads better on it — 3.95:1 in the light signal
palette and 4.22:1 in the light console palette, above the 3:1 floor for a control but below the
4.5:1 one for small text. Every primary button is also a plain word that appears elsewhere on
the page, and the dark modes and the ink palette are all above 5.7:1.

## What is still mouse-only

- **The 3D room's camera.** Orbit and zoom are the mouse and the wheel. The picture follows what
  the keys do to an object; there is no key that turns the camera.
- **One side of a split pair.** A split application's two objects can be dragged apart
  individually; the keyboard moves the pair as one, and Standard stereo puts the two back.

Both are on the list for the next pass.

## What has not been checked

The names, roles, descriptions and announcements on this page are asserted by the Qt Quick
suites (`tst_accessibility.qml`, `tst_keyboard.qml`) on every build. Those suites run under the
offscreen platform, which has no accessibility bridge, so they prove that the window offers the
right things and not that a particular screen reader speaks them as intended.

**No manual screen-reader pass has been run.** Neither NVDA on Windows nor Orca on Linux has
been through the window. That check is still to do, and until it is, treat the screen-reader
behaviour described here as designed rather than as observed.
