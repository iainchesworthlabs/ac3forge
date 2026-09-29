# Object signing: `ac3::signing`

`ac3/signing/emdf_atmos_signer.hpp`, `signing_key.hpp` — a separate library, `ac3::signing`, not
part of `ac3::forge`: signing is an optional step a front end applies to already-encoded frames, so
the codec itself has no dependency on it. See [Object signing](../concepts/object-signing.md) for
what this is *for* — a licensed decoder treats the EMDF container's `emdf_protection` field as a
commitment to object decoding and refuses the whole stream if the tag does not check out; this
computes the keyed tag that satisfies it.

```cpp
const ac3::signing::SigningKey key{key_bytes};   // the operator's own, at runtime
const int signed_count = ac3::signing::sign_atmos_stream(stream, key);
```

Full program: [`examples/object_signing.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/object_signing.cpp)
— encodes a two-object Atmos stream, then signs it. The key used there is a literal stand-in so the
example needs nothing outside itself; no decoder accepts it. A key is always the operator's to
provision at runtime (an environment variable or a `signing-key=<path>` file in the CLI), never
compiled in.

## Signing

`sign_atmos_stream` signs, in place, every syncframe in a stream that carries an EMDF object
container — frames without one are left untouched — and returns how many it signed.
`sign_atmos_frame` does the same for one syncframe at a time, returning true when that frame
carried a container and was signed. Scope is narrow: the ac3forge "atmos" output (a single
independent 5.1 substream, frame-level exponent strategy and SNR, no coupling). A frame outside
that subset is left unsigned rather than signed wrong, the same answer a frame with no container
gets.

## Keys

`SigningKey` owns its bytes and zeroizes them on destruction, so a supplied key does not linger in
freed heap after signing finishes. `load_signing_key(explicit_path)` resolves one from, in order,
an explicit path, `$AC3FORGE_SIGNING_KEY_FILE`, then `$AC3FORGE_SIGNING_KEY` — the same resolution
the CLI's `signing-key=<path>` uses — and returns a `std::expected<SigningKey, KeyLoadError>`.
`KeyLoadError::kind` is one of `KeyErrorKind::kAbsent` (no source offered a key, which a caller
that did not ask to sign can treat as fine), `kUnreadable`, `kMalformed` or `kEmpty`.

`decode_signing_key` is the shared decode underneath all three. It tries base64 first (the
CI/secret-transport form: a GitHub secret is text and cannot carry a raw binary key), then a
comma- or whitespace-separated `0xHH` byte array (the shape a disassembler export has), then takes
the content as raw bytes. Content that is made up only of hex-array characters but parses as
neither of the first two returns `std::nullopt` rather than being taken as raw bytes, because a
truncated hex export used as a key would sign with the wrong secret without any error. Plain hex
without `0x` prefixes is not a separate format: it is valid base64, so the two cannot be told
apart.

## Verifying

`has_authenticity_tag(frame)` reports whether a syncframe carries a non-zero tag, without a key. A
true result says a tag is present, not that it is valid. `verify_atmos_frame(frame, key)` and
`verify_atmos_stream(stream, key)` recompute the tag over the same regions the signer authenticates
and compare it with what the frame holds, without modifying anything. A frame with no EMDF object
container has nothing to check, so the frame form returns a `VerifyResult` of `kNoContainer`,
`kValid` or `kMismatch`, and the stream form returns a `VerifySummary` of `valid`, `mismatch` and
`no_container` counts.

Verification checks a tag this signer wrote: it tests round trips, tampering with signed test
assets and delivery QC. It says nothing about whether a licensed decoder would accept the stream,
because that decoder's own check uses a key and scheme this library does not have. See
[Object signing](../concepts/object-signing.md).

## In the CLI

`atmos`, `atmos-path`, `atmos-encode` and `atmos-cbi` accept `sign-objects` and `signing-key=<path>`
(or the two environment variables above):

```bash
ac3cli atmos-encode in.wav out.ec3 448 0 paths.json sign-objects signing-key=/path/to/key
```

`decode`, `monitor` and `spatial` accept `verify-objects`, which checks every frame against the key
and refuses the whole command on a mismatch. `ac3cli probe` reports whether an authenticity tag is
present, with no key. There is no CLI command that signs an existing stream after encoding.

## Provenance

The HMAC-SHA-256 construction (RFC 2104 / FIPS 180-4) and the choice of which frame regions are
authenticated are clean-room: derived from this codec's own public container layout
(`ac3::emdf`, `ETSI TS 103 420`) and built on this project's own parsing primitives. The key is the
one piece never carried here, the same posture a licensed tool takes with its own (iLok-provisioned)
key.

---

See also: [Object signing](../concepts/object-signing.md) — the concept and why the key can
never be embedded; [Spatial & Atmos objects](spatial-and-atmos.md) — where the container being
signed comes from; [Header map](header-map.md).
