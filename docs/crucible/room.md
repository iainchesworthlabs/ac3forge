# The room

The Room page is where an application becomes an object. This page is what its rail and its
pictures show, how an application is chosen, placed, sized and sent back to the bed, what the ten
slots are, and what the keyboard does with all of it.

Two things it leaves to their own pages: which stream leaves the machine is
[The signal path](signal-path.md), and the whole key map, the focus order and what a screen
reader is told are [Keyboard and screen readers](accessibility.md).

## The shape of the page

The page is five numbered blocks, in the order the window numbers them.

| Block | Where | What it holds |
|---|---|---|
| **01 APPLICATIONS** | left rail | every application the platform lets Crucible see, and the rule for what is in the list |
| **02 ROOM** | centre | the plan and elevation views, or the 3D picture, and the selected application's card |
| **03 BED** | centre, below | a chip for every application that is not placed |
| **04 SIGNAL PATH** | right rail | the three stations in short, with the button that moves the default output |
| **05 SIGNING** | right rail | whether a key is loaded, and which file it came from |

`Ctrl+1` comes back to this page from anywhere in the window. The header above it carries the
path as one line and the status strip below it carries the counters and the Start/Stop button;
both belong to the window rather than to this page.

## 01 The applications list

**What is listed is not the same on both platforms**, and the line under the list is the
platform's own sentence rather than a general one.

On **Windows**, every running application with a window is listed, and one with nothing to tap is
greyed until it plays. A browser's windows and tabs share one entry. On **Linux**, an application
is listed while it is playing: PipeWire gives it a stream when it starts making sound and takes
the stream away when it stops, so the list follows the sound rather than the windows. On both, a
placed application keeps its place while it runs, silent or not.

The order is sound first, then applications with a session but no sound, then silent ones, and by
name within each group. That means the rail reorders itself while you watch it. The selection is
what decides and the list row follows the selection, never the other way about, so an application
floating to the top does not take the arrow keys with it.

Each row carries the application's icon (or a monogram where no icon was found), its name, a tag
reading **placed** or **full-screen**, a detail line, and a level bar spanning −60 to 0 dBFS. The
detail line says the slot and the position — `slot 3 · 0.85, 0.15, +0.00` — for a placed
application, and for one in the bed it says `bed` and then why it is there: `full-screen`,
`no audio`, `idle`, `no tap`, `background`.

Two Behaviour settings decide who is in the list at all — applications with no audio, and
background processes with no window of their own. Both are on the
[Settings](settings.md#06-behaviour) page.

## 02 The room: plan and elevation

Everything on this page says a position in the same three numbers.

| Axis | Runs from | to |
|---|---|---|
| **x** | 0, the left of the room | 1, the right |
| **y** | 0, in front of you | 1, behind you |
| **z** | −1, the floor | +1, the ceiling, with 0 at ear level |

The **plan** is the room from above: front at the top, rear at the bottom, you as a diamond in
the middle, and the five bed speakers — L, R, C, Ls and Rs — drawn as small squares so a
placement has something to be near. The **elevation** is the room from the side: depth across it,
front at the left and rear at the right, height up it, ceiling at the top and floor at the bottom,
with ear level as the line through the middle. A stem drops from each marker to that line, which
is how height reads at a glance.

The two sit side by side when the centre column can give each of them 240 px, and stack when it
cannot; neither goes above 560 px, which is where the markers stop reading as a room. A narrow
window shrinks the room rather than pushing the right rail off the edge.

**Dragging.** Press a marker and it follows the mouse and tells the engine where it is on the way;
let go and it stays where you dropped it. A drag in the plan sets x and y and leaves height alone;
a drag in the elevation sets depth and height and leaves left-and-right alone. A marker that the
engine moved glides over about 90 ms rather than jumping, and a marker you are dragging does not
glide at all.

**Dropping a chip from the bed tray** places it. Dropped into the plan it lands where you dropped
it, at ear level. Dropped into the elevation it takes that depth and that height and goes to the
middle of the room left to right, because the elevation has no left-and-right axis to read.

**A split application draws three things**: a marker at the pair's centre, and one dot per side
labelled L and R. Each dot drags on its own and the centre follows the midpoint of the two;
dragging the centre moves both. Where a standard pair's two dots sit on top of each other in the
elevation, they are nudged apart so both stay reachable.

## The 3D view

The 3D picture is compiled in when the Qt kit that built Crucible has Quick 3D, and left out when
it does not; the **Plan + elevation / 3D** switch appears only where it is in. A build without it
has the two flat views, no switch, and nothing missing from placement, which stays in the plan and
the elevation either way. On Linux, Quick 3D is one of the optional pieces
[Install](install.md#what-you-get-and-what-you-do-not) names.

It draws you at the centre of the room and each placed application as a card carrying its own icon,
turned to face the camera, at its object position; a split pair is two cards. Around them is a
reference speaker layout — 5.1, 7.1 or 7.1.4, chosen under Settings → Appearance → 3D layout, with
Auto drawing 5.1 while the stream is the bed only and 7.1.4 once objects are on. The floor layer
is drawn as cabinets and the height layer as round in-ceiling units. Those speakers are there for
reference. They are not a reading of what your endpoint has.

The picture is worked with the mouse:

- drag an application to move it across the floor at its own height;
- hold `Shift`, or drag with the right button, to move it up and down instead;
- drag empty space to orbit the camera, which turns the room with the mouse;
- the wheel zooms;
- hovering a speaker names it in full.

The camera has no keyboard route, which [Keyboard and screen readers](accessibility.md#what-is-still-mouse-only)
records as a gap. The keys still move the chosen application and the picture follows them. The
camera you set survives a visit to the plan views: the 3D view stays loaded once it has been
shown.

The cards are 3D models, which no accessibility bridge can reach. The picture's frame reports how
many applications are placed and says that an application is chosen in the list and moved with the
arrow keys, so nothing about placing depends on reaching a model.

## The selected application

Choosing an application — a click on its row, a click on its marker, `Up` and `Down` in the list —
brings up a card under the room with its name, where it sits, and the buttons that act on it.

| Button | What it does |
|---|---|
| **Place in the room** | takes a slot and puts it in the centre; the same button reads **Send to bed** once it is placed |
| **Centre** | back to the middle of the room at ear level |
| **Split** / **Mono** | a stereo application as two objects, or as one again |
| **Standard stereo** | shown only for a split pair whose sides were dragged apart; puts them back either side of the centre |

Under them is a row of nine quick placements, for lining things up without a drag.

| Put | Goes to |
|---|---|
| in front, behind | the centre line, a tenth of the room from the front wall or the back |
| left, right | beside you, a tenth of the room from that wall, at ear level |
| overhead | the middle of the room, near the ceiling (z +0.80) |
| front left, front right, rear left, rear right | the four corners, at ear level |

**Size** is the last control on the card: a slider from a point to the whole room. It is the
extent carried in the object's metadata for the receiver's renderer to spread the object over.
The encoder's own bed render treats every object as a point, so size changes nothing you can hear
in the modes where Crucible decodes its own stream again instead of handing it to a receiver. On
the keyboard the slider steps 5% with `Left` and `Right`, 1% with `Shift`, and `Home` and `End`
take it to a point and to the whole room.

**Three ways back to the bed**: the Send to bed button, a double-click on the marker, or `Delete`
or `Backspace` while the room has the keyboard. The bed tray's own line also offers dragging a
marker back to it. The marker publishes no drag in the source as it stands, so that fourth route
does nothing today; this was read off the code rather than tried on a machine, and one of the
three above is the way to do it meanwhile.

## 03 The bed, and the ten slots

The encoder carries fifteen dynamic objects and no separate bed input: its 5.1 bed is rendered
*from* five of those fifteen, pinned to the L, R, C, Ls and Rs positions and snapped, so a
renderer sends each to its nearest speaker. That leaves **ten slots for placed applications**, and
the count is fixed when the stream starts rather than changing under it.

An application leaving the bed takes the lowest free slot. One returning to the bed frees its
slot, and an idle slot carries silence. A split application holds two consecutive slots, left then
right. An application that asks for a slot when none is free stays in the bed and its request is
remembered: it takes the next slot that frees, without being asked again. The `N of 10 slots
placed` line above the room is where the budget is visible.

The bed tray is a chip per unplaced application, with a lock drawn on one that is full-screen.
Drag a chip into either view to place it, or press `Enter` on it to put it in the centre of the
room and take the keyboard there with it.

Movement is smoothed rather than stepped. Each positioned slot approaches its target with a
first-order lag whose time constant is about three frames — roughly 100 ms at 32 ms frames — and a
slot that has just been given an application fades in rather than switching on. A 32 ms jump in
position is a click, and this is what stops it being one.

## What height and depth do, and what they do not

**With a signing key loaded, on an endpoint that takes E-AC-3 exclusively**, x, y, z and size
leave as object metadata and the receiver's renderer puts the application where you put it. Those
two conditions together are the Atmos row of the mode table on
[The signal path](signal-path.md#what-the-mode-line-means), and that table says what your
placements become in each of the other modes.

**Without a key**, the stream is the 5.1 bed alone. Your placements still pan: left and right
between the five bed speakers, and depth still pans front to rear, because the bed has rear
speakers to pan towards. Height does nothing and size does nothing, since both are carried only in
object metadata, and there is none.

The window says this in four places rather than leaving it to the receiver not to show: a notice
above the room, a dimmed and captioned elevation view, the line under the size slider, and the
`Page Up` announcement. It is a refusal rather than a degradation — an unsigned object container
is a hard error on a validating decoder, so Crucible sends no object metadata at all. The mode
table names that case DD+ 5.1, and
[Troubleshooting](troubleshooting.md#placements-pan-but-height-does-nothing) says how to tell.

## The full-screen rule

The application that is full-screen in front is the bed, whatever you asked for it, because a
full-screen game rendering 7.1 *is* the bed. It gives its slot back while it is full-screen and
takes one again when it leaves. Its row carries a **full-screen** tag, its chip carries a lock and
is not a tab stop, and a key that would move it says that it stays in the bed rather than doing
nothing.

This is the one platform seam that a platform can be unable to answer at all, and the window says
which case it is in rather than reporting that nothing is full-screen, which would be a different
claim.

| Platform | The rule |
|---|---|
| **Windows** | On. The shell is asked for the notification state, and the foreground window's process is taken from there. |
| **Linux, X11** | On. Crucible reads `_NET_WM_STATE` and `_NET_WM_PID` on the active window through libxcb. A build configured without libxcb has no reader and says so. |
| **Linux, Wayland** | Off. A Wayland client is given no way to ask about another client's windows, and no portal exposes it. |
| **Linux, no display** | Off. An ssh login or a container has no window manager to ask. |
| **macOS** | Designed — NSWorkspace answers for the frontmost application — and not built. |

The note under the applications list is the reason in the platform's own words, so the rule being
off is always attributed.

## The keyboard route through the room

The room is **one tab stop**, and it means the same thing whichever picture is on screen: the
plan, the elevation and the 3D view draw the same room, so no key asks which one you are looking
at.

The route through a placement is: `Tab` to the applications list, `Up` and `Down` to choose one,
`Enter` to place it in the centre of the room and hand the keyboard to the room, then the arrows
to move it.

| Key | In the room |
|---|---|
| `Left` `Right` | across the room, 0.05 of the width per press |
| `Up` `Down` | towards the front and towards the back |
| `Page Up` `Page Down` | up and down; it says so when height is carrying nothing |
| `Shift` + arrow | the fine step, 0.01 |
| `Ctrl` + arrow | the coarse step, 0.25 — a quarter of the room |
| `Home` | back to the centre |
| `Enter` | place one that is still in the bed; for one already placed, say where it is |
| `Delete` `Backspace` | back to the bed |
| `Plus` `Minus` | grow and shrink the object's size |

An application still in the bed starts from the centre, so one arrow press both places it and
moves it. A press does not wait for the engine to answer before the next one is allowed: the room
remembers where it last asked for this application to go, so two presses inside one poll add up
instead of the second undoing the first.

The rest of the map — the pages, the segmented controls, what is announced and in what order the
window is walked — is [Keyboard and screen readers](accessibility.md).
