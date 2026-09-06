# A Linux guest for the Crucible tray crash

Publishing a `StatusNotifierItem` from Crucible's window used to kill the process before it drew
a frame, on nine or ten launches out of ten, so the Linux build published no tray. This is the
machine that found out why.

**It was a type confusion in Qt.** `QDBusPlatformMenu` implements no `createSubMenu()`, so a
`Qt.labs.platform` `Menu` nested inside a tray icon's menu is handed Qt Labs Platform's QWidget
fallback and then `static_cast` to the D-Bus one; the panel's first request for the menu layout
reads a `QWidgetPlatformMenu` as a `QDBusPlatformMenu` and refcounts whatever is at the offset.
The tray's menu is flat now and the tray is back.
[`apps/crucible/ui/platform/linux/tray_support.cpp`](../../crucible/ui/platform/linux/tray_support.cpp)
carries the finding and
[docs/crucible/promotion.md](../../../docs/crucible/promotion.md) the measurements, under
Phase 4.

The crash was found on a Raspberry Pi 4B with 1844 MB of usable RAM. That machine cannot run an
address sanitiser over Qt and has no room for Debian's Qt debug archive, which is where the
investigation stopped, four wrong explanations in. This is a scripted, throwaway VMware
Workstation guest that can, the same shape as the Windows one in
[`apps/windows/driver-vm/`](../../windows/driver-vm/): it provisions itself into a desktop with
a StatusNotifier host, Qt 6.8.2, Qt's own `-dbgsym` packages, `gdb` and `valgrind`.

Debian 13 (trixie) because it carries **Qt 6.8.2**, the same Qt the Pi carries, and because
Debian publishes matching debug symbols for it. x86_64, so the architecture differs from the
Pi's aarch64 — deliberately: if a fault does not cross, that is a finding too. This one crossed,
and the signal differs only because the AArch64 read was an `ldaxr`, which faults unaligned as
`SIGBUS` where x86-64 faults unmapped as `SIGSEGV`.

It is kept because the bug is still in Qt: the constraint that the tray menu stays flat is only
as good as the machine that can show what happens when it does not.

## What you supply

Nothing. Unlike the Windows guest, which needs an install ISO from Microsoft,
`New-TrayTestVm.ps1` downloads Debian's own published cloud image (a 305 MB `tar.xz`) into
`D:\ISOs` the first time and verifies it against Debian's `SHA512SUMS`.

## The scripts

| Script | What it does |
|---|---|
| `New-TrayTestVm.ps1` | Creates the VM under `D:\Virtual Machines\Crucible Tray Test` (EFI, Secure Boot off, 8 vCPU, 16 GB, 60 GB NVMe, NAT, HD Audio, console on VNC port 5952), turns Debian's raw cloud image into a VMware disk, grows it, seeds it and starts it. |
| `New-SeedIso.ps1` | Writes the cloud-init seed CD from `seed/` and `guest/`, and the ssh key that goes with it. `-Reboot` stops the guest, re-seeds and starts it again — which is how a change to `guest/provision.sh` reaches an existing VM without recreating it. |
| `Wait-TrayTestVm.ps1` | Polls until provisioning has finished, prints what the guest ended up with **and which process owns `org.kde.StatusNotifierWatcher`**, then takes the `clean-install` snapshot. |
| `Sync-Source.ps1` | Pushes the working tree to `~/src` in the guest — `git ls-files -co --exclude-standard`, so uncommitted work goes too and build directories do not. |
| `Test-Tray.ps1` | The measurement: builds `ac3crucible` and launches it ten times in the guest's own session, reporting how many survived. `-NestSubmenu` adds a second arm with a submenu put back in the tray's menu, which is the Qt bug. `-Gdb` runs each launch under gdb and prints the backtrace and registers on death; `-Valgrind` runs one under memcheck; `-Asan` builds our half with AddressSanitizer. |
| `TrayVmCommon.ps1` | Dot-sourced by the others: where the guest is, its address, and `Invoke-TrayGuest` to run something in it over ssh. |
| `guest/provision.sh` | Runs in the guest at first boot (`sudo crucible-provision` re-runs it): the archives, the toolchain, the Qt kit, the desktop, PipeWire, the debug tools and symbols, autologin, and vcpkg. |
| `guest/build-crucible.sh` | `crucible-build` in the guest. `--nest-submenu` puts a submenu back in the tray's menu, `--tray` forces the tray on where the seam says the session has none, `--asan` instruments our half. Both edits are reverted from a pristine copy after every build, so the shipped source in the guest never drifts. |
| `guest/run-trials.sh` | `crucible-trials` in the guest: N launches, a survival count, and the gdb and valgrind modes. |

`apps/windows/driver-vm/guest_console.py` works against this guest too — `--port 5952` — for the
stretch before ssh is up.

## A run

```powershell
cd apps\linux\tray-vm
.\New-TrayTestVm.ps1     # 20-40 min unattended: a desktop, a Qt kit, Qt's debug symbols, vcpkg
```

```powershell
.\Wait-TrayTestVm.ps1    # snapshot when ready, and say what owns the watcher
```

```powershell
.\Sync-Source.ps1 ; .\Test-Tray.ps1 -NestSubmenu
```

## The reproducer

`crucible-build --nest-submenu` adds one `Platform.Menu` of literals to the tray's menu in
`apps/crucible/ui/qml/Main.qml`, and reverts it from a pristine copy after the build, so the
shipped source never drifts. A single launch is evidence of nothing; ten is the measurement.
Read on this guest, 2026-09-06:

```
=== shipped ===          10 of 10 survived, 0 died
=== nested-submenu ===    0 of 10 survived, 10 died      (SIGSEGV)
```

The window survived 20 of 20 as it ships, and valgrind reports no invalid read or write in
180 seconds.

## The precondition

The crash needs a desktop that owns `org.kde.StatusNotifierWatcher`. Reading a clean run on a
session with no such host as "it does not reproduce" is the first mistake this crash caused, and
it is recorded as such in `promotion.md`. So `run-trials.sh` refuses to report anything until
`busctl --user list` shows a host, and `Wait-TrayTestVm.ps1` prints which process owns the name.

The Pi's host is `wf-panel-pi` under `labwc`, a Raspberry Pi package with no amd64 build, so this
guest runs the same stack a step out: **labwc**, the same wlroots compositor, with **waybar** as
the panel. Debian's waybar is linked against `libdbusmenu-gtk3` — the library that makes the
`GetLayout` call the crash arrives on — so the client half of the protocol is the same code.
`agetty` logs the user in on tty1 and the login shell execs labwc; there is no display manager.

`Xwayland` and `qt6-wayland` are both installed, so the window can be put on either platform
plugin from the same session: `QT_QPA_PLATFORM=wayland` (the default `run-trials.sh` sets) or
`xcb`.

## Things learned the hard way

- **`Build-Iso.ps1` was writing UDF, not ISO 9660.** `ChooseImageDefaultsForMediaType` overwrites
  `FileSystemsToCreate` with its own choice for the media, so setting the filesystem *before* that
  call silently had no effect and the `-Udf` switch did nothing. Windows reads UDF without
  noticing, which is why the Windows guest never showed it; cloud-init's NoCloud datasource
  intersects "labelled cidata" with "type vfat or iso9660" and so found nothing at all. The guest
  booted to a login prompt with no user on it. Fixed in `Build-Iso.ps1` by calling
  `ChooseImageDefaultsForMediaType` first.
- **`ssh-keygen -N '""'` from PowerShell sets the passphrase to two literal quote characters**,
  not to nothing, and the key is then unusable. `New-SeedIso.ps1` strips it afterwards and
  asserts the result is unencrypted.
- **`vmrun getGuestIPAddress` needs VMware Tools**, and open-vm-tools is one of the things
  provisioning installs — so asking vmrun for the address deadlocks against the work that needs
  the address. `Get-TrayVmAddress` reads VMware's own `vmnetdhcp.leases` instead, keyed on the
  MAC in the VMX, which is there from the guest's first DHCP request.
- **`vmware-vdiskmanager` cannot read a raw image.** It can read a `monolithicFlat` descriptor,
  which is a dozen lines of text naming an extent file — and Debian's `disk.raw` is already the
  extent file. Write the descriptor beside it, convert, then grow.
- **cloud-init runs its per-instance modules once.** Re-seeding without changing `instance-id`
  gets you a guest that reads the new seed and does nothing with it; `New-SeedIso.ps1` bumps it
  on every write.
- **An ssh session is not in the desktop's session**, so it has neither the display nor the
  session bus the tray needs. An autostart entry writes the session's own environment to
  `~/.crucible-session-env` and `run-trials.sh` sources it.
- **`mks.enable3d = "TRUE"` in the VMX is load-bearing.** Without it the guest's `vmwgfx`
  reports "No 3D enabled", mesa cannot make a DRI2 screen, and labwc exits at "unable to create
  renderer" — no compositor, no panel, no host, no experiment.
- **…and `LIBGL_ALWAYS_SOFTWARE=1` is load-bearing for the window.** With the accelerated
  driver, wlroots cannot import the dmabufs Qt hands it and the window dies at once with
  "importing the supplied dmabufs failed" — a graphics-stack argument, not this bug. llvmpipe's
  buffers import fine and the window still runs the ordinary OpenGL scene graph rather than Qt's
  software fallback. This is the one place the guest is not the Pi.
- **`--no-install-recommends` does not give you a desktop.** `xserver-xorg` is only a
  *recommend* of the desktop metapackages, and `apt-get install` is all or nothing, so one name
  a release does not have takes the whole group with it. `install_group` filters on
  `apt-cache policy` and falls back to one at a time.
- **Xfce was tried first and does not work on this image**: `xfconfd` cannot resolve its own
  configuration directory, exits, and the panel comes up with no plugins. That is a session with
  no host on it, which reads exactly like "the crash does not reproduce".
- **This repository is developed on Windows with `core.autocrlf` on**, so a tar of the working
  tree carries CRLF into the guest. The compiler does not mind; an anchored `sed` — which is
  what the reproducer edit is — matches nothing. `Sync-Source.ps1` strips it from the build
  inputs on the way in.
- **`ac3crucible_lupdate` sees only the platform it runs on.** Running it in this guest deletes
  the *Windows* tray sentence from all six catalogues, translated, and replaces it with the
  Linux one, unfinished (docs/crucible/localisation.md).

The guest's credentials are deliberately trivial (`crucible` / `crucible`) and it is NAT-only; do
not put anything on it you care about.
