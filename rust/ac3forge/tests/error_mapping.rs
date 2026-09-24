//! `Error`: every variant's `Display` goes through `ac3forge_status_message()` with the right raw
//! code, the unknown-code fallback stays representable, and the C status codes the library
//! really returns land on the matching Rust variant (not on `Other`, not on a neighbour).

mod common;

use ac3forge::ac3;
use ac3forge::atmos::{AtmosConfig, AtmosEncoder, ObjectPlacement};
use ac3forge::eac3;
use ac3forge::stream;
use ac3forge::types::{Acmod, DecoderConfig};
use ac3forge::Error;
use common::tone;

/// Every named variant paired with the exact string `src/capi/src/common.cpp` gives its raw
/// code. A mismatch here means `Error::raw()` sends the wrong discriminant across the boundary.
const EXPECTED_MESSAGES: &[(Error, &str)] = &[
    (Error::InvalidArgument, "invalid argument"),
    (Error::OutOfMemory, "out of memory"),
    (Error::Internal, "internal error"),
    (Error::EncodeInvalidBitrate, "invalid bitrate"),
    (Error::EncodeInvalidDialnorm, "invalid dialnorm"),
    (Error::EncodeInvalidSubstream, "invalid substream"),
    (Error::EncodeInvalidChannelMap, "invalid channel map"),
    (Error::EncodeTooManyChannels, "too many channels"),
    (Error::EncodeInvalidMixLevel, "invalid mix level"),
    (Error::EncodeInvalidObjectAudio, "invalid object audio"),
    (Error::EncodeInvalidBsi, "invalid bit stream information"),
    (Error::DecodeTruncated, "truncated frame"),
    (Error::DecodeBadSyncWord, "bad sync word"),
    (Error::DecodeBadCrc, "bad CRC"),
    (Error::DecodeReservedValue, "reserved value"),
    (Error::DecodeUnsupported, "legal but unsupported syntax"),
    (Error::DecodeInvalidStream, "invalid stream"),
];

#[test]
fn every_variant_displays_its_c_status_message() {
    for (error, message) in EXPECTED_MESSAGES {
        assert_eq!(error.to_string(), *message, "{error:?}");
    }
}

#[test]
fn other_carries_raw_codes_including_ones_the_library_itself_does_not_know() {
    // 0 is AC3FORGE_OK: representable through Other, and the C side still names it.
    assert_eq!(Error::Other(0).to_string(), "ok");
    // The scan-specific codes (50..=55) have no named Rust variant; they surface as Other and
    // still print the library's own message.
    assert_eq!(Error::Other(50).to_string(), "empty stream");
    assert_eq!(Error::Other(51).to_string(), "lost sync");
    assert_eq!(Error::Other(54).to_string(), "truncated stream");
    assert_eq!(Error::Other(9999).to_string(), "unknown status");
}

#[test]
fn error_is_a_well_behaved_value_type() {
    let e = Error::DecodeBadCrc;
    let copy = e;
    assert_eq!(e, copy);
    assert_ne!(Error::Other(50), Error::Other(51));
    assert_ne!(Error::DecodeTruncated, Error::Other(30));
    assert_eq!(format!("{:?}", Error::Other(7)), "Other(7)");
    // Usable as a boxed std::error::Error (the `?`-into-Box<dyn Error> path).
    let boxed: Box<dyn std::error::Error + Send + Sync> = Box::new(Error::InvalidArgument);
    assert_eq!(boxed.to_string(), "invalid argument");
    assert!(boxed.source().is_none());
}

fn stereo_frame(config: &ac3::EncoderConfig) -> Result<ac3forge::Bytes, Error> {
    let mut encoder = ac3::Encoder::new(config)?;
    let left = tone(440.0, 48_000.0, 0.3, 1);
    let right = tone(660.0, 48_000.0, 0.3, 1);
    encoder.encode_frame(&[&left, &right])
}

#[test]
fn encoder_status_codes_map_to_their_variants() {
    let base = ac3::EncoderConfig::default();
    for bitrate in [0, 7, 33, 700] {
        let config = ac3::EncoderConfig {
            bitrate_kbps: bitrate,
            ..base.clone()
        };
        assert_eq!(
            stereo_frame(&config).err().unwrap(),
            Error::EncodeInvalidBitrate,
            "bitrate {bitrate}"
        );
    }
    for dialnorm in [0, -1, 32] {
        let config = ac3::EncoderConfig {
            dialnorm,
            ..base.clone()
        };
        assert_eq!(
            stereo_frame(&config).err().unwrap(),
            Error::EncodeInvalidDialnorm,
            "dialnorm {dialnorm}"
        );
    }
    // Boundaries of the legal ranges are accepted.
    for dialnorm in [1, 31] {
        let config = ac3::EncoderConfig {
            dialnorm,
            ..base.clone()
        };
        assert!(stereo_frame(&config).is_ok(), "dialnorm {dialnorm}");
    }
    // (32 kbps is a legal frmsizecod row but cannot fit stereo side info, so 64 is the floor
    // exercised here.)
    for bitrate in [64, 640] {
        let config = ac3::EncoderConfig {
            bitrate_kbps: bitrate,
            ..base.clone()
        };
        assert!(stereo_frame(&config).is_ok(), "bitrate {bitrate}");
    }

    // A substream identity a standalone E-AC-3 encoder cannot emit.
    let mut eac3_encoder = eac3::Eac3Encoder::new(&eac3::Eac3FrameConfig {
        substreamid: 9,
        ..Default::default()
    })
    .unwrap();
    let s = tone(440.0, 48_000.0, 0.3, 0);
    assert_eq!(
        eac3_encoder
            .encode_frame(&[&s, &s], None, None)
            .err()
            .unwrap(),
        Error::EncodeInvalidSubstream
    );

    // A dependent chanmap of 0 names no channels at all.
    let mut au = eac3::AccessUnitEncoder::new(
        &eac3::Eac3FrameConfig {
            acmod: Acmod::Channels3_2,
            lfe: true,
            bitrate_kbps: 448,
            ..Default::default()
        },
        &[eac3::Eac3FrameConfig {
            chanmap: Some(0),
            ..Default::default()
        }],
    )
    .unwrap();
    assert_eq!(au.channel_count(), 0);
    assert_eq!(
        au.encode(&[], None).err().unwrap(),
        Error::EncodeInvalidChannelMap
    );
}

#[test]
fn decoder_status_codes_map_to_their_variants() {
    let frame = stereo_frame(&ac3::EncoderConfig::default())
        .unwrap()
        .to_vec();
    let mut decoder = ac3::Decoder::new(&DecoderConfig::default()).unwrap();

    assert_eq!(
        decoder.decode_frame(&[]).err().unwrap(),
        Error::DecodeTruncated
    );
    assert_eq!(
        decoder
            .decode_frame(&frame[..frame.len() / 2])
            .err()
            .unwrap(),
        Error::DecodeTruncated
    );

    let mut bad_sync = frame.clone();
    bad_sync[0] ^= 0xFF;
    assert_eq!(
        decoder.decode_frame(&bad_sync).err().unwrap(),
        Error::DecodeBadSyncWord
    );

    let mut bad_crc = frame.clone();
    let middle = bad_crc.len() / 2;
    bad_crc[middle] ^= 0x5A;
    assert_eq!(
        decoder.decode_frame(&bad_crc).err().unwrap(),
        Error::DecodeBadCrc
    );

    // fscod '11' is reserved (A/52 Table 5.6).
    let mut reserved = frame.clone();
    reserved[4] = 0xFF;
    assert_eq!(
        decoder.decode_frame(&reserved).err().unwrap(),
        Error::DecodeReservedValue
    );

    // bsid 20 is neither AC-3 (<= 8) nor Annex E (16).
    let mut unsupported = frame.clone();
    unsupported[5] = (unsupported[5] & 0x07) | (20 << 3);
    assert_eq!(
        decoder.decode_frame(&unsupported).err().unwrap(),
        Error::DecodeUnsupported
    );

    // The decoder recovers: the untouched frame still decodes after all those failures.
    assert_eq!(decoder.decode_frame(&frame).unwrap().channel_count(), 2);

    let mut eac3_decoder = eac3::Eac3Decoder::new(&DecoderConfig::default()).unwrap();
    assert_eq!(
        eac3_decoder.decode_access_unit(&[]).err().unwrap(),
        Error::DecodeInvalidStream
    );
    assert_eq!(
        eac3_decoder.decode_substream(&[]).err().unwrap(),
        Error::DecodeTruncated
    );
}

#[test]
fn scan_status_codes_surface_as_other_with_their_raw_value() {
    let frame = stereo_frame(&ac3::EncoderConfig::default())
        .unwrap()
        .to_vec();
    // AC3FORGE_ERROR_SCAN_EMPTY / _LOST_SYNC / _TRUNCATED: no named Rust variant exists for the
    // scan family, so they must come through as Other(raw), never as a wrong named variant.
    let empty = stream::scan(&[]).err().unwrap();
    assert_eq!(empty, Error::Other(50));
    assert_eq!(empty.to_string(), "empty stream");
    assert_eq!(stream::scan(&[0x55; 64]).err().unwrap(), Error::Other(51));
    assert_eq!(
        stream::scan(&frame[..frame.len() - 10]).err().unwrap(),
        Error::Other(54)
    );
}

/// Out-of-range config fields and payloads the codec core only `assert()`s on must come back
/// as an `Err` through this crate's safe API. Before the C layer and core validated them, each
/// of these aborted the whole test process (SIGABRT) instead of failing.
#[test]
fn out_of_range_inputs_are_errors_not_aborts() {
    // AC-3 chbwcod: legal codes stop at 60 (61-63 are reserved in its six bits).
    for chbwcod in [61, 63] {
        let config = ac3::EncoderConfig {
            chbwcod: Some(chbwcod),
            ..Default::default()
        };
        assert_eq!(
            ac3::Encoder::new(&config).err().unwrap(),
            Error::InvalidArgument,
            "chbwcod {chbwcod}"
        );
    }
    let config = ac3::EncoderConfig {
        chbwcod: Some(60),
        ..Default::default()
    };
    assert!(stereo_frame(&config).is_ok());

    // E-AC-3 aux data rides block 0's skip field: 9 bits of byte count, so 511 bytes at most.
    let mut encoder = eac3::Eac3Encoder::new(&eac3::Eac3FrameConfig {
        acmod: Acmod::Stereo,
        bitrate_kbps: 640,
        ..Default::default()
    })
    .unwrap();
    let left = tone(440.0, 48_000.0, 0.3, 0);
    let right = tone(660.0, 48_000.0, 0.3, 0);
    let aux = vec![0x5Au8; 512];
    assert_eq!(
        encoder
            .encode_frame(&[&left, &right], None, Some(&aux))
            .err()
            .unwrap(),
        Error::EncodeInvalidObjectAudio
    );
    assert!(encoder
        .encode_frame(&[&left, &right], None, Some(&aux[..511]))
        .is_ok());
    let mut unit_encoder = eac3::AccessUnitEncoder::new(
        &eac3::Eac3FrameConfig {
            bitrate_kbps: 640,
            ..Default::default()
        },
        &[],
    )
    .unwrap();
    assert_eq!(
        unit_encoder
            .encode(&[&left, &right], Some(&aux))
            .err()
            .unwrap(),
        Error::EncodeInvalidObjectAudio
    );

    // Atmos num_bands_idx indexes Table 50's eight entries; refused at construction.
    for num_bands_idx in [-1, 8] {
        let config = AtmosConfig {
            num_bands_idx,
            ..Default::default()
        };
        assert_eq!(
            AtmosEncoder::new(&config, 1).err().unwrap(),
            Error::InvalidArgument,
            "num_bands_idx {num_bands_idx}"
        );
    }

    // With the object container on: at least one object, at most 15 (the LFE is the 16th).
    // 0, 17..30 and 31+ each used to hit a different assert in the payload writers.
    let object = tone(500.0, 48_000.0, 0.3, 0);
    for count in [0usize, 16, 17, 31, 40] {
        let mut encoder = AtmosEncoder::new(&AtmosConfig::default(), count).unwrap();
        let objects = vec![object.as_slice(); count];
        let placements = vec![ObjectPlacement::default(); count];
        assert_eq!(
            encoder.encode_frame(&objects, &placements).err().unwrap(),
            Error::EncodeInvalidObjectAudio,
            "{count} objects"
        );
    }
}
