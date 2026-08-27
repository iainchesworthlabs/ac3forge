# Rust bindings

Roadmap item AP9: the first non-Python binding over [the C API](c-api.md) — a `-sys` crate
(`ac3forge-sys`, raw `bindgen`-generated FFI declarations) plus a safe wrapper (`ac3forge`), both
living in-tree at
[`rust/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/rust). See that directory's own
[README](https://github.com/iainchesworthlabs/ac3forge/blob/main/rust/README.md) for build
prerequisites, exactly what is and isn't wrapped yet, and the real defects this binding found in
the C header while crossing it as a genuine FFI boundary for the first time.

Python is pybind11-direct C++, WASM is Embind, Android is JNI over C++ — all three compiling the
same C++23 source this binding instead links as a black box, against a header `bindgen`
regenerates from at every build so drift between the two is caught immediately rather than
discovered later by a consumer.

```toml
[dependencies]
ac3forge = { path = "path/to/ac3forge/rust/ac3forge" }
```

## Encoding

Every config type calls the raw `ac3forge_*_config_init()` FFI function first and lets you
override only the fields you need — the same "`_init()` first, then selective overrides"
convention the C header itself documents, never a hand-written Rust-side guess at what the C++
defaults are:

```rust
use ac3forge::ac3::{Encoder, EncoderConfig};
use ac3forge::types::{Acmod, SampleRate};

let config = EncoderConfig {
    sample_rate: SampleRate::Hz48000,
    bitrate_kbps: 192,
    acmod: Acmod::Stereo,
    ..Default::default()
};
let mut encoder = Encoder::new(&config)?;
let frame = encoder.encode_frame(&[&left, &right])?;
```

`ac3forge::eac3::Eac3Encoder`/`Eac3FrameConfig` are the E-AC-3 equivalents (single, standalone
substream — see [What's covered](#whats-covered) below for the wide-layout/Atmos gap).

## Decoding

`Decoder`/`Eac3Decoder` mirror `ac3forge_decoder_t`/`ac3forge_eac3_decoder_t`; every accessor on
the returned `DecodedFrame`/`DecodedSubstream` borrows from `&self`, so a slice from
`channel_samples()` can't outlive the frame it came from — the borrow checker enforces the same
"valid until destroyed" contract the C header documents (see c-api.md's Decoding section) rather
than trusting the caller to respect it.

```rust
use ac3forge::eac3::Eac3Decoder;
use ac3forge::types::DecoderConfig;

let mut decoder = Eac3Decoder::new(&DecoderConfig::default())?;
match decoder.decode_substream(&frame_bytes)? {
    Some(decoded) => println!("{} channels", decoded.channel_count()),
    // Not an error - transient pre-noise processing (§3.7) is holding this frame's PCM back.
    // Rust's Option<T> is a strictly better fit for this than the C API's own
    // AC3FORGE_OK-plus-null-out-parameter convention.
    None => {}
}
```

## What's covered

AC-3 and E-AC-3 encode and decode, single substream — solid and tested against real synthesized
audio (several frames, distinct content per channel), not several surfaces half-covered. The
wide-layout access-unit encoder, Atmos/JOC object encode and decode, and the stream-framing
helpers are explicitly not wrapped yet — see `rust/README.md`'s own list, which also covers what
a future .NET, Node N-API or Android AAR binding over the same header would need to answer for
itself (linking against `forge_c_shared`, the `_config_init` convention, treating C enums as open
sets).

## Header defects found while building this

- `ac3forge_object_placement_t` had no `_init()`, unlike every other config struct — a
  zero-initialized one silently encoded a muted object (`gain` 0.0) rather than the documented
  default of unity gain. Fixed (`ac3forge_object_placement_init()` added).
- Four decoded-audio accessors (`ac3forge_decoded_substream_channel_samples()` and three others)
  were missing the pointer-lifetime documentation their AC-3 sibling has. Fixed (doc-only).
- No status/enum in the header says whether it may gain new values in a future minor release —
  reported, not changed; `ac3forge::Error` is deliberately open (`Error::Other(i32)`) rather than
  a closed set for exactly this reason.

Full detail in `rust/README.md`'s own section — the primary point of AP9 was finding these, not
the crate itself.
