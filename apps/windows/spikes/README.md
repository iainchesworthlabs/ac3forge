# Phase 0 spikes: Windows Desktop Atmos Demo

Throwaway experiments for [docs/platforms/windows-demo.md](../../../docs/platforms/windows-demo.md),
one question each. They are standalone (not part of the root CMake build, not linked to the
library) and their answers are recorded here and on that page. Nothing in this directory is
reused as code.

```bash
cmake -S apps/windows/spikes -B D:/aa-wt-builds/spikes -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows.msvc.toolchain.cmake
cmake --build D:/aa-wt-builds/spikes
```

## S1: process-loopback taps (`s1_taps`, `tone_player`)

`tone_player <hz> [device-substring] [seconds]` renders one sine through WASAPI shared mode.
`s1_taps` spawns several of them at 250 Hz, 500 Hz, 750 Hz and so on, opens one Windows 11
process-loopback capture per PID, and reports each tap's frames per second, RMS and estimated
frequency once a second, so separation is proven by the frequency each tap sees. Flags:

```
s1_taps --list                          audio sessions on every active render endpoint
s1_taps --spawn N [--device SUBSTR]     tap N players (rendering to SUBSTR, default endpoint if omitted)
        [--mute-at S] [--unmute-at S]   mute tap 0's session through ISimpleAudioVolume
        [--exclusive-at S]              Initialize the default endpoint in exclusive mode
        [--probe-at S]                  IsFormatSupported(EXCLUSIVE) only, no Initialize
        [--taps-first]                  open the taps before the players start (spawn suspended)
        [--channels 2|6|8]              format requested from the tap
```

### Results, 2026-09-03

Windows 11 Pro for Workstations 10.0.26200, Realtek analogue endpoint as default, FxSound's
virtual endpoint present and idle.

| Question | Run | Answer |
|---|---|---|
| Do N taps separate N processes? | `--spawn 4`, `--spawn 16 --device FxSound` | **Yes.** Every tap read exactly its process's tone (250 Hz through 4000 Hz), 48 000 frames/s each, 16 taps at once with no drops. First packet within 20 ms of opening. |
| What format arrives? | all | Whatever the tap asks for; 48 kHz float32 stereo was granted, and an 8-channel request was granted too. `GetMixFormat` is not available on a process-loopback client, the caller states the format. |
| Does a tap survive the session being muted? | `--mute-at 3 --unmute-at 6` | **No.** RMS fell to silence within a second of muting and came back on unmute. Session mute is not a way to silence the direct path while tapping. |
| Does a tap survive the app rendering to another endpoint? | `--device FxSound` | **Yes.** Players rendering to the idle FxSound virtual device were tapped identically. This is the null-sink model the plan relies on. |
| Do taps survive us holding a *different* endpoint exclusively? | `--device FxSound --exclusive-at 4` | **Yes.** Exclusive Initialize on Realtek returned S_OK while the players rendered to FxSound, and the taps did not flinch. This is the demo's real configuration. |
| What happens if we open exclusive on the endpoint the apps are using? | `--exclusive-at 4` | **Refused with `AUDCLNT_E_DEVICE_IN_USE`, and the players' streams were invalidated anyway.** Both players exited, the taps ran on delivering silence. A refused exclusive open is destructive to whatever shares that endpoint. |
| Is the exclusive-mode *probe* safe on a live endpoint? | `--probe-at 3` | **Yes.** `IsFormatSupported(EXCLUSIVE)` for six PCM shapes answered (48k/16, 48k/24, 44.1k/16, 44.1k/24 yes; 32-bit no) and the players kept running. Only `Initialize` is destructive. |
| Does a tap opened before the app plays pick it up? | `--taps-first` | **Yes.** Players spawned suspended, taps opened, players resumed; first packet within 10 ms. |

### What this means for the plan

- Per-application separation needs no driver and scales past the encoder's 15-object budget.
- The direct path cannot be silenced by muting sessions. The null-sink endpoint is the way, and
  FxSound's idle endpoint stands in for it until the driver exists.
- The output stage must **never call `Initialize` in exclusive mode on an endpoint that carries
  live shared streams**. Probing with `IsFormatSupported` is fine and is what
  `enumerate_render_devices()` does. Before taking HDMI exclusively the app must confirm the
  default has moved to the null sink and the sessions have followed it.
- Taps can be opened for a session the moment it appears in the session list, before it plays.

## S4: encoder throughput (`s4_throughput`)

Links the real `ac3::forge` (the spike CMake pulls the repo root in the way the Android app
does) and runs, per frame, what the demo engine will run: fold 16 synthetic taps (15 stereo,
one 7.1) into 10 positioned mono objects and a 5-slot speaker-pinned bed, move the positioned
objects, `AtmosEncoder::encode_frame` with 15 objects, and wrap the access unit through
`Eac3BurstPacker`. Wall time per frame, sorted, for 20 s of audio per mode.

### Results, 2026-09-03

Quiet machine (the full-repo build had just finished), RelWithDebInfo, MSVC 14.51.

| Mode | Frame budget | p50 | p99 | max | p99 of budget | Stream |
|---|---|---|---|---|---|---|
| 6 blocks, 448 kb/s (the default) | 32.00 ms | 1.09 ms | 1.79 ms | 1.86 ms | 5.6 % | 448 kb/s |
| 6 blocks, 640 kb/s | 32.00 ms | 1.54 ms | 1.90 ms | 1.98 ms | 5.9 % | 640 kb/s |
| 3 blocks, 640 kb/s | 16.00 ms | 0.86 ms | 1.15 ms | 1.95 ms | 7.2 % | 640 kb/s |
| 2 blocks, 1024 kb/s | 10.67 ms | 0.63 ms | 1.00 ms | 1.15 ms | 9.3 % | 1023 kb/s |
| 1 block, 1536 kb/s | 5.33 ms | 0.44 ms | 0.72 ms | 0.90 ms | 13.5 % | 1536 kb/s |
| 1 block, 2048 kb/s | 5.33 ms | 0.44 ms | 0.73 ms | 0.98 ms | 13.7 % | 2046 kb/s |
| 1 block, 3072 kb/s | 5.33 ms | 0.43 ms | 0.70 ms | 0.79 ms | 13.1 % | 3072 kb/s |

A 1-block frame at 640 kb/s was **refused at frame 0**: the per-frame EMDF/OAMD/JOC container
for 15 objects does not fit a 256-sample frame at that rate. The floor lies somewhere between
640 and 1536 kb/s and is Phase 5's to pin down when low-latency mode gets built.

### What this means for the plan

- Real-time is not in question on this class of machine: the whole per-frame job is well
  under a tenth of the budget in normal mode, and the encoder is not the place to optimise.
- Low-latency mode is feasible at the encoder, at the cost of bitrate: the 1-block frame
  needs on the order of 1.5 Mb/s to carry 15 objects' metadata every 5.3 ms. Over HDMI that is
  fine (E-AC-3 bursts carry up to 6.144 Mb/s); it is the receiver's decode latency and the
  capture buffer, not the encoder, that will dominate what the user hears.
