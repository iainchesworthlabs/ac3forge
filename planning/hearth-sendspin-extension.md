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
  differs from the specification in more than forty places ([Music Assistant](#music-assistant-and-aiosendspin-911)).
- **The role `_ac3forge_player@v1`**, byte for byte ([The role](#the-role-_ac3forge_playerv1)).
- **The questions the text leaves open**, recorded and not yet raised with the Sendspin project
  ([Open questions](#open-questions)).

## Sources

| Source | Version read | Used for |
|---|---|---|
| Sendspin specification, `github.com/Sendspin/spec` | `main` at `8fc2f8f8`, 2026-09-12; no release or tag of this text exists, and about forty normative changes landed between 2026-08-28 and 2026-09-12 | Everything normative on this page |
| aiosendspin | 9.1.1, 2026-08-25, tag commit `5c024b42`, the version Music Assistant pins | [Music Assistant](#music-assistant-and-aiosendspin-911); the Python interoperability client |
| Music Assistant `server` | `62bb0289`, 2026-09-15 | The settings Music Assistant runs aiosendspin with |
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
| E6 | Message 1's payload names the PSK: `psk_id` = base64url(SHA-256("sendspin-psk-id-v1" ‖ PSK)) and `psk_category` `lt`, `pr` or `sn` | M | Implements | Selects the PSK by it, after decrypting message 1 without a PSK; also accepts 9.1.1's payload without `psk_category` ([C3](#music-assistant-and-aiosendspin-911)) |
| E7 | Sentinel PSK = SHA-256("sendspin-sentinel-psk-v1"); on a `psk_id` miss in the initial handshake the client completes with the Sentinel, and the server re-verifies message 2 under it | M | Implements; a Sentinel match while a pairing record exists activates no roles and offers re-pairing | Implements |
| E8 | After a `psk_id` match, the stored `server_id` must equal `server/init`'s | M | — | Implements |
| E9 | Init failures send `server/error` (`unsupported_version`, `unsupported_suite`, `malformed`, checked in that order) then close; every other handshake failure closes silently | M | Implements | Closes silently |
| E10 | Per-message handshake timeout of about 30 s | S | 30 s | 30 s |
| E11 | Re-handshake in transport mode: server-initiated, prologue is the previous handshake hash `h`, no new application messages between message 1 and the new `server/activate`, old-key messages tolerated | M | Used after pairing and to rotate keys on long sessions | Implements; after pairing with a 9.1.1 server, sends nothing else until it completes ([C6](#music-assistant-and-aiosendspin-911)) |
| E12 | One Noise transport message per WebSocket binary message; the first plaintext byte is the message ID; at most 65,518 bytes of payload after it | M | Implements | Implements |
| E13 | Fragmentation with ID 1: `[1][flags][orig_type][data]` then `[1][flags][data]`; flag bit 1 first, bit 0 last, others zero; one fragmented message in flight per direction; malformed sequences close the connection | M | Implements; accepts aiosendspin 9.1.1's form from any peer, and sends it to a 9.1.1 peer ([C1](#music-assistant-and-aiosendspin-911)) | Same |
| E14 | Ignore unrecognised payload fields; send no field the specification does not define, except `_`-prefixed role objects where a message permits them | M | Implements; to a 9.1.1 peer, sends 9.1.1's fields where the two differ ([Music Assistant](#music-assistant-and-aiosendspin-911)) | Same |

### Pairing

`pairing.md`. The flows run over a Sentinel-keyed connection until a PAKE round completes.

| # | Obligation | Level | Server | Player |
|---|---|---|---|---|
| P1 | Methods: pairing PSK (token `SP:0`), dynamic pairing code (6 digits or `SP:1` QR), static pairing code (8 digits, gesture-gated window) | m | All three; with a 9.1.1 client, in 9.1.1's forms ([C20 to C27](#music-assistant-and-aiosendspin-911)) | Pairing PSK always; the dynamic code on the test sink and the boards (serial console and status page); static code where a product prints one; with a 9.1.1 server, in 9.1.1's forms |
| P2 | A client lists at most one code method; a server that sees both ignores the static one | M | Implements | Lists one |
| P3 | CPace is CPACE-X25519-SHA512 per draft-21, initiator-responder with mutual confirmation; server is A, client is B; `sid` = "sendspin-pair-pake-v1" ‖ `h` ‖ u32be(pairing_index) ‖ u32be(round); empty CI; AD `server` and `client` | M | Implements; the 9.1.1 `sid` with a 9.1.1 peer ([C22](#music-assistant-and-aiosendspin-911)) | Same |
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
| S9 | `group/update` with all fields, promptly after the first activation and on any change | M | Implements; with a 9.1.1 client, not while it pairs ([C25](#music-assistant-and-aiosendspin-911)) | Implements; a 9.1.1 server's updates can leave fields out ([C15](#music-assistant-and-aiosendspin-911)) |
| S10 | An unavailable client or `client/leave`: move it to a stopped solo group, send `group/update` and `stream/end`, never auto-rejoin | M | Implements | Sends `client/leave` or `available: false` when its output is taken; to a 9.1.1 server, `available: false` only ([C12](#music-assistant-and-aiosendspin-911)) |
| S11 | `stream/start` never to an unavailable client; `stream/clear` for seeks and track jumps; `stream/end` only when playback ends, never at a track transition | M | Implements, which is also how gapless works | Implements |
| S12 | "All servers must implement all versions of these roles", while a server "MAY omit a role at their discretion" | m / Y | Implements `player@v1` for playback and the other six roles as the conformance leg of A4, activated by policy ([Q1](#open-questions)) | — |
| S13 | Transmit timestamps (`server_transmitted`, `send_ahead`) are taken immediately before encryption, never at enqueue | M | Implements | — |

### `player@v1`

`roles/player/v1.md`.

| # | Obligation | Level | Server | Player |
|---|---|---|---|---|
| R1 | Servers support `opus`, `flac` and `pcm` | M | libFLAC and Opus through the vcpkg `hearth` feature; PCM at 16 and 24 bits | PCM always; FLAC and Opus on a computer, and on a board where measured to fit (chip B) |
| R2 | Choose the player's preferred `format` when producible, else the first producible `supported_formats` entry; always an entry the player listed | S / M | Implements | — |
| R3 | Chunk `[4][int64 BE timestamp µs][uint32 BE send_ahead µs][frame]`; PCM little-endian signed, whole frames; FLAC whole frames with `codec_header` carrying `fLaC` and STREAMINFO; Opus one packet per chunk | M | Implements; a 9.1.1 client's header has no `send_ahead` ([C31](#music-assistant-and-aiosendspin-911)) | Implements; a 9.1.1 server's header has no `send_ahead` |
| R4 | Chunks at most 150 ms, at least 15 ms except the last | M / S | Implements | — |
| R5 | `send_ahead` saturates at 0 and 4,294,967,295; a player never uses a saturated value as a delay sample and never schedules by `send_ahead` | M | Implements | Implements |
| R6 | First timestamp after a start from empty or a clear at least `min_buffer_ms + output_delay_ms` ahead; a group uses the largest send-ahead of its members; queued audio stays at or above `min_buffer_ms`; `buffer_capacity` is a hard byte cap | M | Implements, across `player@v1` and `_ac3forge_player@v1` members alike | — |
| R7 | Volume and mute are independent; amplitude `(volume/100)^1.5`, ramped | M / S | Sends commands only when listed in `supported_commands` | Implements |
| R8 | `output_delay_ms` 0 to 5,000, clamped and persisted | M | Honours it; a 9.1.1 client reports `static_delay_ms` ([C29](#music-assistant-and-aiosendspin-911)) | Implements; reports `static_delay_ms` to a 9.1.1 server |
| R9 | Steady-state sync error within ±1 ms, aiming for ±0.5 ms; speed within ±0.5% over any 150 ms; inaudible corrections; no startup warble; late chunks dropped | M / S | — | Implements, correcting decoded PCM (the specification's suggested sample deletion and insertion) |
| R10 | Stereo and mono only: the role defines no channel order or layout | — | Standard players get stereo decoded by the engine, Lo/Ro | — |

### Other roles

`source@v1`, `controller@v1`, `metadata@v1`, `artwork@v1`, `visualizer@v1` and `color@v1` are part
of "all versions of these roles" ([S12](#session-time-groups-and-streams)). Hearth's server
implements each of them in A4 and activates them by policy: `controller@v1` and `metadata@v1` for
any client that lists them, `artwork@v1` when an item carries artwork, `visualizer@v1` and
`color@v1` from the same analysis the monitor already runs, and `source@v1` only when the user adds
a source in Settings. Each is tested against the specification's message and binary layouts, and
`controller@v1`, `metadata@v1` and `color@v1` also against the aiosendspin client; the server does
not activate the other three for a 9.1.1 client, whose forms of them differ
([C33 to C35](#music-assistant-and-aiosendspin-911)). The obligations that matter for the
implementation are recorded in the role files and are not repeated here; the ones most easily
missed are the controller's group-volume arithmetic (rounded mean, delta applied and clamped
remainder redistributed), artwork's exact-size delivery without cropping, and colour's 4.5:1
contrast guarantee.

## Music Assistant and aiosendspin 9.1.1

Music Assistant's Sendspin server is aiosendspin 9.1.1, which follows the specification as it read
in late August 2026. It differs from `8fc2f8f8` in more than forty places, and several of them
stop a specification peer and a 9.1.1 peer from working together at all: Music Assistant cannot
parse a specification player's `client/hello` (C8, C28), neither side reads the other's audio
chunks (C31), and no code-based pairing completes (C20 to C24). Hearth keeps the specification's
behaviour with a specification peer, and uses 9.1.1's forms with a peer it knows to be 9.1.1
([Decisions](#decisions), 1).

The differences were read from aiosendspin 9.1.1's source at its tag (commit `5c024b42`), with
Music Assistant's `server` repository at `62bb0289` (2026-09-15) for the settings it runs
aiosendspin with, and mashumaro 3.22, which parses aiosendspin's messages, for what becomes of a
message that does not fit: unknown keys are ignored, and a missing field, a wrong type or an
unknown enumeration value fails the whole message. Music Assistant closes the connection on a
client message it cannot parse. The 9.1.1 client drops a server message it cannot parse, except
while the connection starts and while it pairs, when it disconnects.

**Knowing a peer is 9.1.1.** Both send core `version: 1`, but in each direction a signal arrives
before anything that depends on the answer:

- **A player** knows from Noise message 1, whose payload the specification requires to carry
  `psk_category` and whose 9.1.1 form never does (C3).
- **A server** knows from `client/hello`, where 9.1.1 always sends `trust_level`, a field the
  specification does not define and so one a specification client does not send
  ([E14](#encryption)).

The answer holds for the connection, and the test sink's configuration can force either answer
for tests. Both signals describe 9.1.1 as released, so a later aiosendspin that sends
`psk_category`, or stops sending `trust_level`, needs this section read again
([Q3](#open-questions)). The Music Assistant run in A4's exit records which forms it used.

In the tables, **Player** is Hearth's player half with a 9.1.1 server, which is Music Assistant,
and **Server** is Hearth's server with a 9.1.1 client, such as the scripted player in `tools/`
([Decisions](#decisions), 2). A row that names only one side needs nothing from the other.

**Transport and handshake**

| # | Difference | Specification (`8fc2f8f8`) | aiosendspin 9.1.1 | Hearth |
|---|---|---|---|---|
| C1 | Fragmentation | ID 1 with a flags byte: `[1][flags][orig_type][data]`, then `[1][flags][data]` | IDs 2 and 3 with no flags byte: `[2][orig_type][data]`, then `[2][data]` while more follow and `[3][data]` for the last | Accepts both forms from any peer; IDs 2 and 3 are reserved in the specification, so accepting them collides with nothing defined. Sends ID 1 to a specification peer, and IDs 2 and 3 to a 9.1.1 peer, which drops an ID 1 message as unknown. Music Assistant fragments a `player@v1` chunk over 65,510 bytes, such as 25 ms of 8-channel 192 kHz 32-bit PCM, and an artwork image over the same size |
| C2 | Unencrypted sessions | Every connection begins with `client/init` and the Noise handshake | Music Assistant also takes `client/hello` as a first message and runs the session in cleartext, for a client ID not already paired or approved; its `allow_legacy_clients` setting, on by default, allows it | Not used: the player always begins with `client/init`, and the server accepts nothing else first |
| C3 | Noise message 1's payload | `psk_id` and `psk_category` | `psk_id` only | Server: sends both, and a 9.1.1 client ignores the field it does not know. Player: accepts a message 1 without `psk_category` and takes the category of the stored PSK whose `psk_id` matches (the identifier is a hash of one PSK, so it names one record), and from then on treats the server as 9.1.1 |
| C4 | Sentinel fallback | On a `psk_id` miss in the initial handshake, the client completes message 2 under the Sentinel PSK, and the server verifies message 2 again under it, which tells it the client has lost the record | The client aborts before message 2; the server reads message 2 only under the PSK it chose | Player: falls back as specified. Music Assistant then fails message 2, redials with backoff indefinitely and never learns why, so the player also reports, on its status page and in its log, that the server holds a pairing the sink no longer has and that the player has to be removed in Music Assistant before it can pair again. Server: verifies again as specified. A 9.1.1 client that has lost its record closes after message 1 on every attempt, which the Network page shows, offering to forget the pairing |
| C5 | `server/error` | Sent on an init failure, with its reason, before closing | Absent: the server closes without a message, and the client takes a `server/error` for a malformed `server/init` | Nothing to change: the player logs a 9.1.1 server's close as a handshake that ended without a reason, and a 9.1.1 client closes on a `server/error` as it would on any failure |
| C6 | Messages around the re-handshake after pairing | The server tolerates application messages under the old keys until the new message 2 | Music Assistant takes the client's next frame after pairing as message 2 and disconnects on anything else; the client reads message 1 directly and disconnects on anything else | Player: sends nothing after `client/pair-finalize` until the re-handshake completes. Server: sends message 1 straight after `server/pair-finalize`, before any other message |
| C7 | Pairing records without a server | Every long-term PSK is stored with its `server_id`, which the check after a `psk_id` match compares; a client at capacity evicts a record | A record's `server_id` is optional, and the check runs only when it is set. The client's bundled stores seed a record with none, and a full store pairs by delivering that shared PSK instead of a fresh one; `server/unpair` never removes it | Server: a shared PSK cannot be told apart on the wire; the 9.1.1 client's default stores are unbounded, so it does not arise with them. Player: generates a fresh PSK for every pairing, and stores every record with its `server_id` |

**Core messages**

| # | Difference | Specification (`8fc2f8f8`) | aiosendspin 9.1.1 | Hearth |
|---|---|---|---|---|
| C8 | `client/hello`'s `supported_pair_methods` | An object keyed by method. `pairing_psk` is always offered and at most one code method; the dynamic method's descriptor carries `out_channels` and `formats` | An array of `{method, out_channels?, min_pin_length?, locations?}`, which may leave out `pairing_psk` and list both code methods | Player: sends the array, with 9.1.1's method names (C20) and the `min_pin_length` Music Assistant requires on the dynamic method. Server: reads the array. Music Assistant cannot parse the object and closes the connection without redialling |
| C9 | `languages` | In `server/hello` | In `server/activate`'s `pairing` object, for the dynamic method only | Player: takes the language order for a spoken code from the activation. Server: sends it in the activation's `pairing` object as well |
| C10 | Management, and pairing on a paired connection | The activities are `playback` and `pairing`, and code pairing runs on a Sentinel connection | A `management` activity with `management/*` messages for pairing records and settings. Music Assistant enters it when its operator manages a paired client, and at the start of a code session on a paired client to open that client's pairing window; it runs code pairing and re-verification on the long-term connection | Player: answers either activation with `client/goodbye` reason `unauthorized`, as the specification has it for an activation the client does not admit. Pairing a paired sink again with a code from Music Assistant therefore fails; the operator removes the player there first. Server: does not use `management` |
| C11 | Activations the client admits | Per matched PSK: long-term `[]` or `['playback']`; the pairing PSK `[]` or `['pairing']`; Sentinel `[]`, `['pairing']`, or `['playback']` with unpaired access. An activation that leaves the connection unable to play clears the persisted roles | `['pairing']` under any PSK; `[]` refused under the pairing PSK; `source@v1` refused without a long-term PSK. Persisted roles count when `active_roles` is left out, so such an activation is refused rather than clearing them | Server: with a 9.1.1 client, cancels a pairing-PSK attempt by closing the connection, always sends `active_roles`, and does not activate `source@v1` (C33) |
| C12 | `client/leave` | A client asks to leave its group | Absent. Music Assistant closes the connection on it, and redials | Player: sends `client/state` with `available: false` instead, which moves the sink to a stopped solo group in Music Assistant, as leaving would |
| C13 | The first `client/state` | Every client with an active role sends one, even when its roles define no state object | Music Assistant waits up to 5 s for it from a client with the player, artwork, visualizer or source role, then carries on without it in its default lenient mode. The client sends one only with the player role active, or with the source role once its clock has converged | Player: sends its first `client/state` straight after activation, with `available: false` until its time filter has converged ([S6](#session-time-groups-and-streams)), so Music Assistant's wait does not run out. Server: does not wait for a first `client/state` from a 9.1.1 client whose active roles define no state object |
| C14 | `available` before the clock converges | A player does not report `available: true` until its time filter has converged | The client reports `available: true` on activation, and plays with a 500 ms lead until its filter converges | Nothing to change: a 9.1.1 client covers the gap itself |
| C15 | `group/update` | Carries `playback_state`, `group_id` and `group_name`, promptly after the first activation | Every field is optional and left out when unset, and queued updates are merged; aiosendspin has no way to set `group_name`. Music Assistant sends the first update after the client's first `client/state`, or after its 5 s wait | Player: takes a missing field as unchanged and a missing `group_name` as no name, and waits for no `group/update` |
| C16 | Replacing a stream | `stream/clear` for a seek or a track jump; `stream/end` only when playback ends, never at a track transition | In its default lenient mode, Music Assistant ends an active stream it replaces with `stream/end`, then starts a new one | Player: stops, and starts again from an empty buffer, as the messages say. Which Music Assistant operations do this is recorded in its run |
| C17 | A format change within a stream | A `stream/start` for a running stream keeps the buffered audio and continues the timeline | Music Assistant drops chunks queued in the old format, sends a new `stream/start`, and sends audio again from near the playhead, taking it that the client has discarded what it held | Player: on a `stream/start` from a 9.1.1 server that changes a running stream's format, discards the audio it holds in the old format. Server: does not change a 9.1.1 client's format during a stream |
| C18 | Refusal with `concurrent_attempt` | The server may retry later | After a `client/goodbye` with any reason except `restart`, Music Assistant does not dial the client again until it announces itself over mDNS | Player: announces its mDNS service again when the attempt that caused the refusal ends |
| C19 | `stream/request-format` | Changes to a stream's configuration travel in `client/state` | Carried by this message instead, which the 9.1.1 client never sends; its `client/state` has no player format, artwork or visualizer fields | Player: does not change its `player@v1` formats during a connection to a 9.1.1 server; a change it needs, such as a new output bit depth, sends `client/goodbye` reason `restart`, after which Music Assistant dials again |

**Pairing**

| # | Difference | Specification (`8fc2f8f8`) | aiosendspin 9.1.1 | Hearth |
|---|---|---|---|---|
| C20 | Method names and the activation's `pairing` object | Methods `pairing_psk`, `dynamic_pairing_code` and `static_pairing_code`. The activation's `pairing` is `{method, format}`, with `format` `digits` or `qr_code` for the dynamic method | Methods `pairing_psk`, `dynamic_pin` and `static_pin`. `pairing` is `{method, pin_length, languages}`: Music Assistant sends the larger of the client's `min_pin_length` and its own minimum of 4, and the client aborts with `pin_length_unacceptable` below its minimum or above 12 | With a 9.1.1 peer, both halves use 9.1.1's names and object. Player: lists `min_pin_length` 6, and accepts a `pin_length` from 6 to 12. Server: sends the client's `min_pin_length` as `pin_length`, and never less than 6 |
| C21 | Dynamic code derivation | `SHA-256("sendspin-pairing-code-derive-v1" ‖ h ‖ nonce_A ‖ nonce_B)`, reduced to 6 digits, or its first 24 bytes as an `SP:1` QR token | The label is `sendspin-pin-derive-v1`, reduced to `pin_length` digits; there is no QR form | With a 9.1.1 peer, both halves derive 9.1.1's code, and the QR form is not offered |
| C22 | CPace `sid` and rounds | `sid` = "sendspin-pair-pake-v1" ‖ `h` ‖ u32be(`pairing_index`) ‖ u32be(`round`). After a failed `server_kc` the client sends `client/pair-retry` for another round, up to 20 | `sid` without `round`. A failed `server_kc` ends the attempt with `pair/abort`, and the client counts failures across attempts, waiting for an operator action after 10 | With a 9.1.1 peer, both halves use its `sid` and end a failed round with `pair/abort`. The player keeps the specification's limit of 20 rounds since the last verified `server_kc`, counted across attempts |
| C23 | `nonce_B` | Revealed in `client/pair-confirm` only as `wrapped_nonce_B`, sealed under a key from the CPace output | Sent in the clear as `nonce_B` (43 characters) | With a 9.1.1 peer, both halves send and accept `nonce_B` in the clear, and accept it only from a 9.1.1 peer; the commitment and binding checks are unchanged |
| C24 | Server verification and abort reasons | The server checks `client_kc`, then the commitment, then the code binding. A failed `client_kc` is `pair/abort` reason `pairing_code_mismatch`; a failed commitment or binding closes the connection | The commitment is checked first, closing the connection on failure, then `client_kc` and the binding together, with `pair/abort` reason `pin_mismatch`. `pin_length_unacceptable` is an additional reason | Server: checks in the specification's order, and with a 9.1.1 client sends `pin_mismatch` where the specification has `pairing_code_mismatch`. Player: with a 9.1.1 server sends `pin_mismatch`, which Music Assistant parses where `pairing_code_mismatch` would disconnect it, and takes a `pair/abort` with `pin_mismatch` as the end of the attempt |
| C25 | Other messages during pairing | The sequence rules cover pairing messages only; `group/update` follows every activation promptly, and group membership continues through pairing | Once an attempt has begun, both treat any other frame as a failed attempt and disconnect; before that, Music Assistant drops them. The 9.1.1 client pauses its clock exchanges and state reports while it pairs | Player: with a 9.1.1 server, sends no `client/time` or `client/state` from the pairing activation until pairing ends. Server: with a 9.1.1 client, holds back `group/update` and every other message from the pairing activation until the attempt ends (C6) |
| C26 | Waiting and pairing windows | `client/pair-pending` may carry a `message`. A static-code window admits attempts until success, the fifth failure, a drop, a cancel or expiry | `client/pair-pending` carries `pairing_index` only, and Music Assistant disconnects on a second one in the same attempt. The 9.1.1 client's window admits one attempt per gesture | Player: sends at most one `client/pair-pending` per attempt to a 9.1.1 server. Server: shows a 9.1.1 client's waiting attempt without device text, and says that another static-code attempt needs the gesture again |
| C27 | Pairing tokens | Versions 0 and 1, with extra payload bytes ignored | Version 0 only, with exactly 64 payload bytes | Player: shows its `SP:0` token with no extra bytes. The `SP:1` form is not offered to a 9.1.1 peer (C21) |

**`player@v1`**

| # | Difference | Specification (`8fc2f8f8`) | aiosendspin 9.1.1 | Hearth |
|---|---|---|---|---|
| C28 | `player@v1_support` | `supported_formats` and `buffer_capacity` | Also requires `supported_commands`, limited to `volume` and `mute`; Music Assistant sends those commands only when they are listed here | Player: adds `supported_commands` to the support object, listing `volume` and `mute`. Server: takes volume and mute support from the support object. Music Assistant cannot parse a support object without the field |
| C29 | Player state and commands | `client/state` lists `supported_commands` from `volume`, `mute` and `set_output_delay`, and reports `output_delay_ms`; the command is `set_output_delay` | `supported_commands` may list only `set_static_delay`; the delay is `static_delay_ms`, set with the command `set_static_delay`. Music Assistant closes the connection on a `client/state` that lists anything else | Player: reports `static_delay_ms` and `supported_commands: ["set_static_delay"]`, and accepts `set_static_delay`. Server: reads `static_delay_ms` and sends `set_static_delay` |
| C30 | Timing fields | No upper bound on `required_lead_time_ms` or `min_buffer_ms` | Each 0 to 30,000 when parsed | Player: keeps both at or below 30,000 ms |
| C31 | Audio chunk header | `[4][int64 timestamp][uint32 send_ahead][frame]`, 13 bytes | `[4][int64 timestamp][frame]`, 9 bytes | With a 9.1.1 peer, both halves read and write the 9-byte header; the player then has no `send_ahead` samples, which it never schedules by ([R5](#playerv1)). Read with the other header, every frame gains or loses 4 bytes |
| C32 | Formats | Servers support `opus`, `flac` and `pcm`, and `client/state` can name a preferred `format`; `bit_depth` is ignored for Opus | Music Assistant encodes PCM at 16, 24 or 32 bits, FLAC at 16 or 24, and Opus only with `bit_depth` 16, 1 or 2 channels and 8, 12, 16, 24 or 48 kHz; it takes the first listed format it can encode, and a `bit_depth` of 0 or less fails parsing. The 9.1.1 client decodes PCM and FLAC only, and refuses Opus in its own `supported_formats` | Player: lists its formats in order of preference, with Opus entries at `bit_depth` 16. Server: sends a 9.1.1 client PCM or FLAC. A4's exit sends Opus to the scripted player, which the 9.1.1 client cannot receive as released; proposed for review: the scripted player lifts the SDK's codec check and decodes Opus with libopus itself |

**Other roles**

| # | Difference | Specification (`8fc2f8f8`) | aiosendspin 9.1.1 | Hearth |
|---|---|---|---|---|
| C33 | `source@v1` | Messages `client-stream/start` and `client-stream/end`; allowed on an approved unpaired connection | `client_stream/start` and `client_stream/end`; only on a long-term PSK connection | Server: does not activate `source@v1` for a 9.1.1 client |
| C34 | `artwork@v1` | Channels configured in `client/state`. An image travels as an announce with flags and total size, then parts, and can be cancelled | Channels configured in `artwork@v1_support` in `client/hello`. Each image travels whole as `[type][int64 timestamp][image]`, fragmented over 65,510 bytes (C1), and a `none` channel still needs `format`, `width` and `height` in `stream/start` | Server: does not activate `artwork@v1` for a 9.1.1 client. Player: the sinks do not list the role |
| C35 | `visualizer@v1` | `visualizer@v1_support` holds `buffer_capacity` only; `types`, `rate_max` and `spectrum` go in `client/state` | The support object also requires `rate_max` and `types`, and configuration changes use `stream/request-format`; `pitch` uses reserved ID 21, and Music Assistant turns it off | Server: does not activate `visualizer@v1` for a 9.1.1 client |
| C36 | `metadata@v1` and `color@v1` | Every `server/state` object carries the full state; fields are not nullable, and an omitted `progress` clears the position | After the first object, Music Assistant sends only the fields that changed, and cleared ones as `null`. The 9.1.1 client takes an omitted `progress` as unchanged, and drops a message with a `year` outside 1000 to 2040, a `track` below 1 or an invalid colour | Server: with a 9.1.1 client, sends a cleared field as `null`, and leaves out a `year` or `track` outside those ranges. Player: the sinks do not list these roles |
| C37 | `controller@v1` | Group volume and mute count only the players that support them, and an out-of-range seek is ignored | Music Assistant counts every player, at volume 100 when it has none, always offers `volume`, `mute` and `switch`, and closes the connection on a command it cannot validate, such as a negative `position_ms` | Nothing to change: the sinks do not list the role, and the 9.1.1 client's commands are within what Hearth's server accepts |
| C38 | Differences with no effect on Hearth | — | `stream/end` carries `server_transmitted`; `group/update` may say `paused`; artwork may be `bmp`; metadata also carries `repeat` and `shuffle`; a `visualizer@_draft_r1` role; a revoked approval sends the reduced `server/activate` before the role's `stream/end`; `stream/start` can reach an unavailable client; entering pairing moves the client to a solo group | Unknown fields and values are ignored ([E14](#encryption)), an unavailable sink plays nothing it is sent, and a sink Music Assistant pairs leaves its group for good |

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
| Q3 | Whether aiosendspin will move to the specification's text where they differ, or the text towards aiosendspin, and when. 9.1.1 was released on 2026-08-25, before the specification's changes of 2026-08-28 to 2026-09-12 ([Sources](#sources)) | aiosendspin 9.1.1 against the whole specification | Uses 9.1.1's forms with a peer it detects as 9.1.1, by two signals that describe 9.1.1 as released ([C1 to C38](#music-assistant-and-aiosendspin-911)) |
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
| 1 | Which Sendspin Hearth implements, given aiosendspin 9.1.1 differs from `main` | **The specification at a pinned commit, with Music Assistant compatibility**: use 9.1.1's forms with a peer detected as 9.1.1, and list each difference. When this was taken the known differences were fragmentation and the CPace `sid`; the full reading (C1 to C38) came after |
| 2 | What stands in for "Sendspin's reference Python player" in A4's exit, since the released `sendspin` 7.5.0 command-line player has no encryption | **A scripted player on aiosendspin 9.1.1** (Apache-2.0), kept under `tools/` and run in the loopback test |
| 3 | How the open questions reach the Sendspin project | **Not yet**: recorded on this page only |
| 4 | Whether mbedTLS, libFLAC, Opus and the vendored time filter join `vcpkg.json` | **Yes, all four**, behind the manifest feature `hearth`, each in the generated notices |
