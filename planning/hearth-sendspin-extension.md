# Hearth over Sendspin: conformance and the `_ac3forge_player@v1` role

!!! note "Status as of 2026-09-15: draft for review"
    The first deliverable of [A4](hearth-reference-player.md#a4-sendspin): the conformance reading
    of the Sendspin specification that `src/sendspin` implements, how Hearth works with Music
    Assistant where Music Assistant's implementation differs from that text, and the normative
    definition of the application-specific role that carries AC-3 and E-AC-3 to Hearth sinks.
    Nothing here is built yet. Once the user has reviewed this page it is normative for
    `src/sendspin`, the test sink and `hearth_sink`, and [chip B](hearth-reference-player.md#chip-b-the-esp32-s3-sink)
    can start after A4 lands.

    Read from the Sendspin specification on `main` at commit
    `8fc2f8f8d8aa324cf385bd3332a284fd3a75520c` (2026-09-12). Where this page says "the
    specification", it means that commit. Section names below are the specification's own
    headings, in the file they appear in.

## What this page fixes

- **Which obligations Hearth meets**, as a server (`ac3hearth`'s engine) and as a player (the
  test sink and `hearth_sink`), in [the conformance tables](#conformance).
- **How Hearth works with Music Assistant**, whose Sendspin server is aiosendspin 9.1.1 and
  differs from the specification text in a few places ([Music Assistant](#music-assistant-and-aiosendspin-911)).
- **The role `_ac3forge_player@v1`**, byte for byte ([The role](#the-role-_ac3forge_playerv1)).
- **The questions the text leaves open**, recorded and not yet raised with the Sendspin project
  ([Open questions](#open-questions)).

## Sources

| Source | Version read | Used for |
|---|---|---|
| Sendspin specification, `github.com/Sendspin/spec` | `main` at `8fc2f8f8`, 2026-09-12; no release or tag of this text exists, and about forty normative changes landed between 2026-08-28 and 2026-09-12 | Everything normative on this page |
| aiosendspin | 9.1.1, 2026-08-25, the version Music Assistant's `dev` branch pins | [Music Assistant](#music-assistant-and-aiosendspin-911); the Python interoperability client |
| `sendspin` command-line player | 7.5.0, 2026-06-16, on aiosendspin 6.0.1 | Not used: it has no Noise and no CPace, so a conformant server cannot connect to it ([Decisions](#decisions), 2) |
| Sendspin time filter, `github.com/Sendspin/time-filter` | `39dd3f4a`, C++, Apache-2.0 | Vendored into `src/sendspin` for the player half |
| CPace, draft-irtf-cfrg-cpace-21 | Expires 2026-10-25 | The code-based pairing flows |
| IEC 61937 bursts | `ac3::iec61937::wrap_frame` and `Eac3BurstPacker` (`src/forge/include/ac3/iec61937/iec61937.hpp`) | What a burst chunk carries |

## Conformance

**Levels.** The specification uses BCP 14 key words, which count only in capitals. It also states
several obligations with a lowercase "must": that servers support both connection methods, both
cipher suites and every role, and that a client supports at least one suite. The specification
relies on those sentences (for example, having no suite negotiation), so Hearth treats them as
MUST, in keeping with the decision to be fully conformant
([hearth-reference-player.md, decision 17](hearth-reference-player.md#decisions)). In the tables,
**M** is MUST or MUST NOT, **m** is a lowercase must treated as MUST, **S** is SHOULD, and **Y** is
MAY.

**Columns.** *Server* is `ac3hearth`'s engine. *Player* is the player half of `src/sendspin`,
used by `ac3hearth-testsink` and `hearth_sink`. A dash means the row does not apply to that side.

### Transport and discovery

`connection.md`, Establishing a Connection.

| # | Obligation | Level | Server | Player |
|---|---|---|---|---|
| T1 | Plain `ws://` only; confidentiality comes from Noise inside the payloads | M | Implements | Implements |
| T2 | Support both connection methods: dial clients that advertise `_sendspin._tcp`, and accept clients that dial a server advertising `_sendspin-server._tcp` | m | Implements both | — |
| T3 | A client uses exactly one method at a time | M | — | Server-initiated only: advertises `_sendspin._tcp` and never dials |
| T4 | mDNS TXT `path` required; `name` optional and should match `client/hello` / `server/hello` | M / S | Advertises `path=/sendspin`, port 8927 | Advertises `path=/sendspin`, port 8928 |
| T5 | Admission between servers, ranked `playback` > `pairing` > empty, with the pairing and last-playback exceptions; the displaced connection gets `client/goodbye` `another_server` | M | Handles being displaced and reports it | Implements the ranking, the exceptions and the 30 s provisional timeout |
| T6 | Persist the last-playback server's `server_id` | m | — | Persists it |

### Encryption

`connection.md`, Encryption; `messaging.md`, Communication.

| # | Obligation | Level | Server | Player |
|---|---|---|---|---|
| E1 | Handshake order: `client/init`, `server/init`, then two `noise/handshake` messages, all as WebSocket text frames; everything after as binary frames of Noise transport ciphertext | M | Implements | Implements |
| E2 | Pattern `KKpsk2`; the server is the Noise initiator whichever side dialled | M | Initiator | Responder |
| E3 | Suites `25519_ChaChaPoly_SHA256` and `25519_AESGCM_SHA256`: servers support both, clients at least one | m | Both | Both on a computer; the boards' choice is chip B's, measured |
| E4 | `client_id` and `server_id` are the base64url (no padding) X25519 public keys; private keys from a CSPRNG, never a shared default | M | Implements; the key is created on first run | Implements; a board's from its hardware RNG |
| E5 | Prologue is the exact bytes of `client/init` then `server/init` as transmitted | M | Implements | Implements |
| E6 | Message 1's payload names the PSK: `psk_id` = base64url(SHA-256("sendspin-psk-id-v1" ‖ PSK)) and `psk_category` `lt`, `pr` or `sn` | M | Implements | Selects the PSK by it, after decrypting message 1 without a PSK |
| E7 | Sentinel PSK = SHA-256("sendspin-sentinel-psk-v1"); on a `psk_id` miss in the initial handshake the client completes with the Sentinel, and the server re-verifies message 2 under it | M | Implements; a Sentinel match while a pairing record exists activates no roles and offers re-pairing | Implements |
| E8 | After a `psk_id` match, the stored `server_id` must equal `server/init`'s | M | — | Implements |
| E9 | Init failures send `server/error` (`unsupported_version`, `unsupported_suite`, `malformed`, checked in that order) then close; every other handshake failure closes silently | M | Implements | Closes silently |
| E10 | Per-message handshake timeout of about 30 s | S | 30 s | 30 s |
| E11 | Re-handshake in transport mode: server-initiated, prologue is the previous handshake hash `h`, no new application messages between message 1 and the new `server/activate`, old-key messages tolerated | M | Used after pairing and to rotate keys on long sessions | Implements |
| E12 | One Noise transport message per WebSocket binary message; the first plaintext byte is the message ID; at most 65,518 bytes of payload after it | M | Implements | Implements |
| E13 | Fragmentation with ID 1: `[1][flags][orig_type][data]` then `[1][flags][data]`; flag bit 1 first, bit 0 last, others zero; one fragmented message in flight per direction; malformed sequences close the connection | M | Implements; also accepts aiosendspin 9.1.1's form on receive ([C1](#music-assistant-and-aiosendspin-911)) | Same |
| E14 | Ignore unrecognised payload fields; send no field the specification does not define, except `_`-prefixed role objects where a message permits them | M | Implements | Implements |

### Pairing

`pairing.md`. The flows run over a Sentinel-keyed connection until a PAKE round completes.

| # | Obligation | Level | Server | Player |
|---|---|---|---|---|
| P1 | Methods: pairing PSK (token `SP:0`), dynamic pairing code (6 digits or `SP:1` QR), static pairing code (8 digits, gesture-gated window) | m | All three | Pairing PSK always; the dynamic code on the test sink and the boards (serial console and status page); static code where a product prints one |
| P2 | A client lists at most one code method; a server that sees both ignores the static one | M | Implements | Lists one |
| P3 | CPace is CPACE-X25519-SHA512 per draft-21, initiator-responder with mutual confirmation; server is A, client is B; `sid` = "sendspin-pair-pake-v1" ‖ `h` ‖ u32be(pairing_index) ‖ u32be(round); empty CI; AD `server` and `client` | M | Implements; the 9.1.1 `sid` for peers that need it ([C2](#music-assistant-and-aiosendspin-911)) | Implements |
| P4 | `long_term_psk` is 32 CSPRNG bytes from the client, revealed only as `wrapped_psk` sealed with K_wrap = SHA-256(label ‖ `sid` ‖ ISK) under the connection suite's AEAD | M | Unwraps and stores | Generates and wraps |
| P5 | Pairing records: the client persists (PSK, `server_id`), replacing an older record for the server, keeps at least five and never evicts one backing an open connection; the server persists (PSK, `client_id`); both drop the record on `server/unpair` | M | In the settings directory, readable by the user only | In a file beside the test sink's config; in NVS on a board |
| P6 | Dynamic code: 20 rounds since the last verified `server_kc`, counted globally, then hold back until an operator action | M | — | Implements; the operator action is a button on the status page or a serial command |
| P7 | Static code: attempts only inside a device-gesture window, which closes on success, the fifth failed `server_kc`, a drop, a cancel or expiry | M | — | Where a static code exists |
| P8 | Protocol errors (malformed field, wrong share length, low-order point, commitment or unwrap failure) close silently and persist nothing | M | Implements | Implements |
| P9 | Unpaired access: an unapproved client gets no roles or playback; approval persists, is revocable and is dropped on pairing; unpaired, unapproved clients are shown distinctly and offered pairing | M / S | Implements; the Network page shows them as not paired | Offers `unpaired_access` off by default |

### Session, time, groups and streams

`messaging.md`, Core messages; `README` sections Role Versioning and Priority and Activation.

| # | Obligation | Level | Server | Player |
|---|---|---|---|---|
| S1 | `server/hello` → `client/hello` → `server/activate`; nothing else before the first activation except `client/goodbye` | M | Implements | Implements |
| S2 | Activate at most one version per role family, only roles the client listed, never a version whose support object is missing | M | Implements | — |
| S3 | Activity sets allowed per matched PSK (long-term: `[]` or `[playback]`; pairing: `[]` or `[pairing]`; Sentinel: `[]`, `[pairing]`, or `[playback]` with unpaired access); non-empty `active_roles` only on playback-capable connections | M | Implements | Rejects inadmissible activations with the reasons the table gives |
| S4 | Before removing a role: `stream/end` for stream roles, a null `server/state` object for state roles | M | Implements | — |
| S5 | `client/time` → `server/time` on the server's monotonic clock in microseconds | M | Implements | Sends often enough to keep the filter converged; feeds the vendored time filter |
| S6 | A player does not report `available: true` until its time filter has converged | M | — | Implements; the convergence threshold is Hearth's ([Q4](#open-questions)) |
| S7 | `client/state` carries `available` and full role objects; the server sends no role binary data before that role's `client/state` object, and a state change alone never starts a stream | M | Implements | Implements |
| S8 | `server/state` carries full role objects, promptly after a role is added and on every change; the first per role carries a past or present `timestamp` | M | Implements for the roles it activates ([S12](#session-time-groups-and-streams)) | — |
| S9 | `group/update` with all fields, promptly after the first activation and on any change | M | Implements | Implements |
| S10 | An unavailable client or `client/leave`: move it to a stopped solo group, send `group/update` and `stream/end`, never auto-rejoin | M | Implements | Sends `client/leave` or `available: false` when its output is taken |
| S11 | `stream/start` never to an unavailable client; `stream/clear` for seeks and track jumps; `stream/end` only when playback ends, never at a track transition | M | Implements, which is also how gapless works | Implements |
| S12 | "All servers must implement all versions of these roles", while a server "MAY omit a role at their discretion" | m / Y | Implements `player@v1` for playback and the other six roles as the conformance leg of A4, activated by policy ([Q1](#open-questions)) | — |
| S13 | Transmit timestamps (`server_transmitted`, `send_ahead`) are taken immediately before encryption, never at enqueue | M | Implements | — |

### `player@v1`

`roles/player/v1.md`.

| # | Obligation | Level | Server | Player |
|---|---|---|---|---|
| R1 | Servers support `opus`, `flac` and `pcm` | M | libFLAC and Opus through the vcpkg `hearth` feature; PCM at 16 and 24 bits | PCM always; FLAC and Opus on a computer, and on a board where measured to fit (chip B) |
| R2 | Choose the player's preferred `format` when producible, else the first producible `supported_formats` entry; always an entry the player listed | S / M | Implements | — |
| R3 | Chunk `[4][int64 BE timestamp µs][uint32 BE send_ahead µs][frame]`; PCM little-endian signed, whole frames; FLAC whole frames with `codec_header` carrying `fLaC` and STREAMINFO; Opus one packet per chunk | M | Implements | Implements |
| R4 | Chunks at most 150 ms, at least 15 ms except the last | M / S | Implements | — |
| R5 | `send_ahead` saturates at 0 and 4,294,967,295; a player never uses a saturated value as a delay sample and never schedules by `send_ahead` | M | Implements | Implements |
| R6 | First timestamp after a start from empty or a clear at least `min_buffer_ms + output_delay_ms` ahead; a group uses the largest send-ahead of its members; queued audio stays at or above `min_buffer_ms`; `buffer_capacity` is a hard byte cap | M | Implements, across `player@v1` and `_ac3forge_player@v1` members alike | — |
| R7 | Volume and mute are independent; amplitude `(volume/100)^1.5`, ramped | M / S | Sends commands only when listed in `supported_commands` | Implements |
| R8 | `output_delay_ms` 0 to 5,000, clamped and persisted | M | Honours it | Implements |
| R9 | Steady-state sync error within ±1 ms, aiming for ±0.5 ms; speed within ±0.5% over any 150 ms; inaudible corrections; no startup warble; late chunks dropped | M / S | — | Implements, correcting decoded PCM (the specification's suggested sample deletion and insertion) |
| R10 | Stereo and mono only: the role defines no channel order or layout | — | Standard players get stereo decoded by the engine, Lo/Ro | — |

### Other roles

`source@v1`, `controller@v1`, `metadata@v1`, `artwork@v1`, `visualizer@v1` and `color@v1` are part
of "all versions of these roles" ([S12](#session-time-groups-and-streams)). Hearth's server
implements each of them in A4, tested against the aiosendspin client, and activates them by
policy: `controller@v1` and `metadata@v1` for any client that lists them, `artwork@v1` when an item
carries artwork, `visualizer@v1` and `color@v1` from the same analysis the monitor already runs,
and `source@v1` only when the user adds a source in Settings. The obligations that matter for the
implementation are recorded in the role files and are not repeated here; the ones most easily
missed are the controller's group-volume arithmetic (rounded mean, delta applied and clamped
remainder redistributed), artwork's exact-size delivery without cropping, and colour's 4.5:1
contrast guarantee.

## Music Assistant and aiosendspin 9.1.1

Music Assistant's Sendspin server is aiosendspin 9.1.1, which follows the specification as it
read in late August 2026. Where it differs from `8fc2f8f8`, Hearth keeps the specification's
behaviour and adds the minimum needed to work with it
([Decisions](#decisions), 1).

| # | Difference | Specification (`8fc2f8f8`) | aiosendspin 9.1.1 | Hearth |
|---|---|---|---|---|
| C1 | Fragmentation | ID 1 with a flags byte | IDs 2 and 3 (first and continuation) | Sends the specification's form; accepts both on receive. IDs 2 and 3 are reserved in the specification, so accepting them cannot collide with anything defined |
| C2 | CPace `sid` and rounds | `sid` includes `u32be(round)`; `client/pair-retry` starts another round | No `round` in `sid`; no `client/pair-retry` | Uses the specification's `sid`; with a peer known to be aiosendspin 9.1.1 (below), uses its `sid` and ends a failed round with `pair/abort` rather than a retry |
| C3 | Sentinel fallback | Client completes with the Sentinel on a `psk_id` miss; server re-verifies | Neither side | Player: falls back as specified, which a 9.1.1 server sees as a failed handshake, the same result it expects. Server: re-verifies, which a 9.1.1 client never triggers |
| C4 | Messages | `server/error`, `client/leave`, `server/unpair`, `languages` defined | Absent | Sent as specified; a 9.1.1 peer ignores or does not send them, and nothing in Hearth waits for one |
| C5 | Extra messages | — | `stream/request-format`, management messages removed from the specification, WebRTC DataChannels | Ignored |

**Knowing a peer is 9.1.1.** Nothing on the wire names the implementation: both send core
`version: 1`. The `sid` is an input to CPace's generator, so the form has to be settled before a
round's shares are exchanged; a side cannot try both forms on one round. Hearth settles it per
peer: from the operator (a peer marked as Music Assistant in the test sink's configuration or in
the app's pairing dialog), and otherwise by using the specification's form, and after a failed
`server_kc` on an unmarked peer, the 9.1.1 form on the next attempt. The form that pairs is kept in
the pairing record. The Music Assistant run in A4's exit is made with the peer marked, and records
the form used. [Q3](#open-questions) asks whether the need goes away.

## The role `_ac3forge_player@v1`

A Hearth sink offers two playback roles in `client/hello`, in this order:
`["_ac3forge_player@v1", "player@v1"]`. Music Assistant, like any server that does not implement
the first, activates `player@v1`. `ac3hearth` activates `_ac3forge_player@v1` and does not activate
`player@v1` on the same connection. The two roles are different families
(`README`, Priority and Activation), so a server may activate both; one that does sends a stream
to at most one of them at a time.

The role follows `player@v1` wherever it can: the same clock, the same send-ahead rules, the same
state fields for volume, mute, output delay and timing, and the same stream lifecycle. What differs
is what a chunk carries, what the sink reports, and the settings the server can send.

### Names and IDs

| Item | Value |
|---|---|
| Role | `_ac3forge_player@v1`; family `_ac3forge_player` |
| Support object | `_ac3forge_player@v1_support` in `client/hello` |
| Object key in messages | `_ac3forge_player`, in `client/state`, `server/command`, `stream/start`; the string `_ac3forge_player` in the `roles` arrays of `stream/clear` and `stream/end` |
| Binary message IDs | 192: burst chunk, server to sink. 193 to 195 are reserved for this role's later versions. The role uses no other ID |
| Activation | As any role: only on a playback-capable connection ([S3](#session-time-groups-and-streams)). `ac3hearth` activates it only on a long-term PSK connection |

### Support object

`_ac3forge_player@v1_support`, required when the role is listed.

| Field | Type | Meaning |
|---|---|---|
| `data_types` | string[] | Bitstreams the sink decodes: a non-empty subset of `"ac3"`, `"eac3"`. `"eac3"` includes E-AC-3 JOC |
| `sample_rates` | integer[] | Coded sample rates the sink plays, in Hz; `[48000]` on the boards |
| `outputs` | object | `count`: output slots at the current setting; `bit_depth`: bits per slot at the current setting; `bit_depths`: the widths the sink can be set to on its own page. For display: the server does not change them |
| `layout_grammar` | integer | Version of the speaker-layout text grammar (`ac3::render::OutputLayout`) the sink parses: `1` |
| `management` | object | `routing`: boolean; `trim_db`: [minimum, maximum]; `delay_ms`: [0, maximum]; `crossover_hz`: [minimum, maximum]; `identify`: boolean |
| `decoder_settings` | string[] | Names of the decoder settings the sink accepts ([Settings](#settings)) |
| `buffer_capacity` | integer | Maximum bytes of burst chunks held and not yet played, counting each chunk's whole plaintext message |

### State object

`_ac3forge_player` in `client/state`. The first eight fields are `player@v1`'s and mean the same;
the rest are this role's.

| Field | Type | Meaning |
|---|---|---|
| `volume?` | integer | 0 to 100, required when `supported_commands` has `volume` |
| `muted?` | boolean | Required when `supported_commands` has `mute` |
| `output_delay_ms` | integer | 0 to 5,000; delay beyond the audio port, persisted |
| `required_lead_time_ms` | integer | As `player@v1`, including the sink's decoder warm-up |
| `min_buffer_ms` | integer | As `player@v1` |
| `supported_commands` | string[] | Subset of `volume`, `mute`, `set_output_delay`, `settings`, `identify` |
| `settings_revision` | integer | The `revision` of the last settings the sink applied; 0 before any |
| `settings_error?` | object | `{revision, why}` when the last settings were refused; absent otherwise |
| `decoder?` | object | What the decoder found in the current stream: `data_type`, `acmod`, `lfe`, `substreams`, `objects` (count carried), `objects_placed` (boolean), `dialnorm` (dB); absent with no stream |
| `levels?` | object[] | One `{output, peak_db, rms_db}` per output slot, `output` counted from 0; absent with no stream |
| `counters` | object | Since the connection opened: `bursts_played`, `underruns`, `late_chunks`, `dropped_chunks`, `invalid_chunks` |
| `why?` | string | A short sentence when the sink stopped playing for a reason of its own; absent otherwise |

The sink sends `client/state` whenever a field other than `levels` changes, and while a stream
plays it also sends one at most ten times a second with fresh `levels`. `levels` are measured on
the samples written to the outputs, after routing, trim and delay.

### Stream start, clear and end

**`stream/start`**, object `_ac3forge_player`:

| Field | Type | Meaning |
|---|---|---|
| `data_type` | string | `"ac3"` or `"eac3"`, one the sink listed |
| `sample_rate` | integer | Coded sample rate, one the sink listed |

A `stream/start` for a running stream updates it in place, as the specification defines. This is
how Hearth continues between queue items: from AC-3 to E-AC-3 it sends a new `stream/start` and
the timestamps run on. When the sample rate changes the sink may rebuffer, and the server treats
the start as one from empty ([R6](#playerv1)).

**`stream/clear`** with `"_ac3forge_player"` in `roles`: the sink drops every buffered chunk and
any decoded audio not yet played, resets its decoder state, and continues with chunks received
after the message.

**`stream/end`** with `"_ac3forge_player"` in `roles`, or with no `roles`: the sink stops output,
drops its buffers and its decoder, and clears `decoder` and `levels` from its state.

### Burst chunks

Binary message ID 192, server to sink, only during an active `_ac3forge_player` stream. Integers
are big-endian.

| Bytes | Field | Meaning |
|---|---|---|
| 0 | ID | `192` |
| 1 to 8 | `timestamp` | int64: server clock time in microseconds at which the first decoded sample of the burst leaves the audio port ([Timing](#timing)) |
| 9 to 12 | `send_ahead` | uint32: as `player@v1`, saturating at 0 and 4,294,967,295 |
| 13 to 14 | `Pc` | uint16: the burst's IEC 61937 burst-info word as the library writes it: data type in bits 0 to 4 (1 AC-3, 21 E-AC-3), bits 5 and 6 zero, error flag in bit 7, data-type-dependent bits 8 to 12 (bsmod in 8 to 10 for AC-3), data stream number in 13 to 15 |
| 15 to 16 | `Pd` | uint16: the IEC 61937 length code: payload length in bits for AC-3, in bytes for E-AC-3 |
| 17 to end | payload | The elementary-stream bytes the burst carries, in stream order: `Pd / 8` bytes for AC-3, `Pd` bytes for E-AC-3 |

What a burst is, exactly as the library packs it for a receiver:

- **AC-3**: one syncframe. 1,536 samples.
- **E-AC-3**: whole access units (the independent substream's syncframe followed by its dependent
  substreams' syncframes) until their blocks total six, as `Eac3BurstPacker` groups them. 1,536
  samples.

What is left out, compared with a burst for a receiver: the sync words `Pa` and `Pb`, the
byte-swapping of the payload into 16-bit words, the pad byte of an odd-length payload, and the
zero stuffing to the repetition period (6,144 bytes for AC-3, 24,576 for E-AC-3). The largest
chunk is 17 + 24,568 bytes, inside one frame's 65,518 ([E12](#encryption)), so a chunk is never
fragmented.

A chunk is 1,536 samples: 32 ms at 48 kHz, inside `player@v1`'s 15 to 150 ms.

**The sink rejects** a chunk whose `Pc` data type is not the stream's `data_type`, whose `Pd`
disagrees with the payload length, or whose payload does not start with a syncframe, and counts it
in `invalid_chunks`. Rejecting a chunk drops it; it does not close the connection.

### Timing

- The sink maps `timestamp` to its own clock through the time filter, subtracts
  `output_delay_ms`, and plays the burst's first decoded sample at its audio port at that time.
  Everything between the chunk and the port is the sink's to compensate: its decoder's frame
  hold-back (E-AC-3's §3.7 buffering), rendering, routing, delay lines and the DMA or device queue.
- "The burst's first decoded sample" is the first sample of the PCM the library's decoder
  attributes to the burst's first access unit. `ac3hearth` timestamps a standard player's PCM, FLAC
  or Opus from its own decode of the same stream on the same terms, so a sink and a standard player
  in one group play each sample at the same time.
- The server computes each timestamp from the stream's sample count, not by adding rounded chunk
  durations, so the timeline does not drift.
- Synchronisation corrections are applied to decoded PCM, never to bursts
  ([R9](#playerv1)).
- A late chunk is dropped before it is decoded, and counted in `late_chunks`. Because a decoder
  carries state from frame to frame, the sink then treats the next chunk as the start of a stream:
  it resets the decoder and conceals the gap as its `concealment` setting says.

### Settings

`server/command`, object `_ac3forge_player`, `command: "settings"`, with a `settings` object that
replaces the sink's current settings whole. The sink applies them at the next burst boundary and
reports `settings_revision`, or `settings_error` with a reason. Unknown keys in `settings` are
ignored, per [E14](#encryption); a known key with an invalid value refuses the whole object.

| Key | Type | Meaning |
|---|---|---|
| `revision` | integer | Increases with every settings command the server sends to this sink |
| `layout` | string | Speaker layout in the grammar `layout_grammar` names, e.g. `"5.1"` or `"L:small,C,R:small,Ls,Rs,LFE"` |
| `routing` | string | Output per rendered speaker, `ac3::render::Routing`'s text form: indices from 0, `-` for none, e.g. `"0,1,2,3,4,5"` |
| `trim_db` | number[] | One per output, inside `management.trim_db` |
| `delay_ms` | number[] | One per output, inside `management.delay_ms` |
| `crossover_hz` | number | Inside `management.crossover_hz` |
| `decoder` | object | Any of the keys below the sink listed in `decoder_settings` |

`decoder` keys, each mapping to a library setting:

| Key | Values | Library |
|---|---|---|
| `mode` | `"line"`, `"rf"`, `"custom"` | `OutputConfig::mode` |
| `drc_cut`, `drc_boost` | 0.0 to 1.0 | The separate cut and boost scales A3 adds to `DecoderConfig` |
| `heavy_compression` | boolean | `DecoderConfig::heavy_compression` |
| `dialnorm` | boolean | `OutputConfig::apply_dialnorm` |
| `downmix` | `"loro"`, `"ltrt"` | The fold a two-speaker layout gets (`ac3::render::serve`) |
| `ltrt_phase_shift`, `mix_lfe` | boolean | `OutputConfig` |
| `programme` | integer or `null` | `DecoderConfig::programme` |
| `objects` | `"auto"`, `"always"`, `"never"` | `ac3::render::ObjectsPolicy` |
| `concealment` | `"none"`, `"repeat_fade"`, `"mute"` | `DecoderConfig::concealment` |

### Identify

`server/command`, object `_ac3forge_player`, `command: "identify"`, with `identify` set to
`{output, level_db}` to start the tone on one output, or to `null` to stop it. While the tone
plays, every output carries `ac3::render::IdentifyTone::fill`'s result instead of the stream's
audio: the tone on `output` (its low band when the output carries the LFE) and silence elsewhere.
A running stream keeps its timeline and its chunks are decoded and discarded, so the stream
continues in sync when the tone stops. `level_db` is inside `IdentifyTone`'s −60 to −12 dB.

### Errors and limits

- A command whose `command` is not in the sink's latest `supported_commands` is ignored, as
  `player@v1` has it.
- `buffer_capacity` is a hard cap: the server never sends a chunk that would take the sink's held
  bytes above it ([R6](#playerv1)).
- A sink that cannot continue (a decoder that fails past its concealment, an output that goes
  away) stops its output, reports `why`, and reports `available: false` only when something outside
  Sendspin holds its output.

### Versions

The role is `v1` as defined here. Adding a value to `data_types` (AC-4, when
[chip D](hearth-reference-player.md#chip-d-the-ac-4-decoder) delivers a decoder) or a key to
`decoder` settings does not change the version, because a server only sends what a sink listed.
Anything that changes a field's meaning or the chunk layout is `_ac3forge_player@v2`, and a sink may
list both.

## Open questions

Recorded here and not yet raised with the Sendspin project ([Decisions](#decisions), 3).

| # | Question | Where | What Hearth does meanwhile |
|---|---|---|---|
| Q1 | Must a server implement every role, or may it leave roles out? `README`, Role Versioning says all servers "must implement all versions"; `messaging.md`, `server/activate` says servers MAY omit a role | `README`; `messaging.md` | Implements all seven and activates by policy |
| Q2 | What a receiver does with an unknown JSON message `type` or an unknown binary message ID. Only unknown payload fields have a rule. The extension relies on a server sending ID 192 only to a client that listed the role | `messaging.md`, Communication | Sends ID 192 only on connections where the role is active; ignores unknown IDs and types on receive |
| Q3 | Whether aiosendspin's fragment IDs 2 and 3, `sid` without `round` and missing Sentinel fallback will move to the specification's text, and when; and whether a peer can be told apart on the wire meanwhile | aiosendspin 9.1.1 against `pairing.md` and `messaging.md` | [C1 to C5](#music-assistant-and-aiosendspin-911) |
| Q4 | How converged the time filter must be before `available: true`; the library has `get_error()` and no convergence test | `messaging.md`, Clock Synchronization | Reports `available: true` once the filter's own error estimate has stayed under 1 ms for eight consecutive exchanges, a threshold chosen here and measured in A4 |
| Q5 | Whether `buffer_capacity` counts each chunk's 13-byte header, and how a server tracks what a player has consumed | `roles/player/v1.md`, support object and Server Audio Send Constraints | Counts whole plaintext messages; the server tracks consumption from timestamps |
| Q6 | CPace's confirmation tags: the MAC inputs and order are left to the draft, which suggests HMAC or CMAC; no Sendspin test vectors exist, and the draft expires 2026-10-25 | `pairing.md`, PAKE | Follows draft-21 §10 with HMAC-SHA-512; tests against the draft's vectors and against aiosendspin 9.1.1 |
| Q7 | Multichannel in `player@v1`: no channel order, layout or channel limit | `roles/player/v1.md` | Standard players get stereo; multichannel goes in this role |

## Test vectors

`src/sendspin`'s tests hold these as fixed bytes:

- The Sentinel PSK and its `psk_id`, from `connection.md`.
- The two pairing-token examples in `pairing.md`.
- CPace draft-21 Appendix B.1 (X25519, SHA-512).
- One recorded handshake and pairing exchange with aiosendspin 9.1.1 in each direction, with the
  keys used, so the Music Assistant path is tested without a network.
- For this role: a burst chunk from `wrap_frame` for `tests/golden`'s AC-3 fixture and one from
  `Eac3BurstPacker` for an E-AC-3 fixture with fewer than six blocks per syncframe, checked field by
  field against the table above.

## Decisions

Taken on 2026-09-15.

| # | Question | **Taken** |
|---|---|---|
| 1 | Which Sendspin Hearth implements, given aiosendspin 9.1.1 differs from `main` | **The specification at a pinned commit, with Music Assistant compatibility**: accept both fragment forms on receive, use 9.1.1's CPace `sid` with peers that need it, and list each difference |
| 2 | What stands in for "Sendspin's reference Python player" in A4's exit, since the released `sendspin` 7.5.0 command-line player has no encryption | **A scripted player on aiosendspin 9.1.1** (Apache-2.0), kept under `tools/` and run in the loopback test |
| 3 | How the open questions reach the Sendspin project | **Not yet**: recorded on this page only |
| 4 | Whether mbedTLS, libFLAC, Opus and the vendored time filter join `vcpkg.json` | **Yes, all four**, behind the manifest feature `hearth`, each in the generated notices |
