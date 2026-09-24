//! Atmos encoder configuration/validation and the access-unit object accessors, the loudness
//! meter's validation and gating, the stream framing/scan helpers' error paths and AC-3/legacy
//! core detection, and `version()`.

mod common;

use ac3forge::atmos::{AtmosConfig, AtmosEncoder, ObjectPlacement};
use ac3forge::eac3::{Eac3Decoder, Eac3Encoder, Eac3FrameConfig, StreamType};
use ac3forge::meter::{dialnorm_from_lkfs, LoudnessMeter};
use ac3forge::stream::{self, StreamKind};
use ac3forge::types::{Acmod, DecoderConfig, SampleRate};
use ac3forge::{Error, SAMPLES_PER_FRAME};
use common::{rms, tone};

// --- Atmos ------------------------------------------------------------------------------------

#[test]
fn atmos_config_and_placement_defaults() {
    let config = AtmosConfig::default();
    assert_eq!(config.sample_rate, SampleRate::Hz48000);
    assert_eq!(config.bitrate_kbps, 448);
    assert_eq!(config.dialnorm, 31);
    assert_eq!(config.num_bands_idx, 4);
    assert!(!config.fine_quant);
    assert!(config.emit_object_metadata);
    assert_eq!(config.clone(), config);

    let placement = ObjectPlacement::default();
    assert_eq!(
        (placement.x, placement.y, placement.z),
        (0.5, 0.5, 0.0),
        "room centre"
    );
    assert_eq!(placement.gain, 1.0);
    assert_eq!(placement.lfe_send, 0.0);
}

#[test]
fn atmos_without_object_metadata_degrades_to_a_plain_bed() {
    let config = AtmosConfig {
        emit_object_metadata: false,
        ..Default::default()
    };
    let mut encoder = AtmosEncoder::new(&config, 1).unwrap();
    assert_eq!(encoder.latency(), encoder.bed_latency());
    assert_eq!(
        encoder.latency_samples(),
        encoder.bed_latency().total_samples()
    );

    let mut decoder = Eac3Decoder::new(&DecoderConfig::default()).unwrap();
    for frame_index in 0..2 {
        let object = tone(500.0, 48_000.0, 0.4, frame_index);
        let unit = encoder
            .encode_frame(&[&object], &[ObjectPlacement::default()])
            .unwrap();
        let decoded = decoder.decode_substream(&unit).unwrap().unwrap();
        assert!(!decoded.has_object_metadata());
        assert_eq!(decoded.dynamic_object_count(), 0);
        assert_eq!(decoded.object_audio_count(), 0);
        assert_eq!(decoded.program_bed(), 0);
        // The object still reaches the listener, panned into the 5.1 bed.
        assert_eq!(decoded.channel_count(), 6);
        if frame_index > 0 {
            let total: f32 = (0..6).map(|c| rms(decoded.channel_samples(c))).sum();
            assert!(total > 0.1, "bed energy {total}");
        }
    }
}

#[test]
fn atmos_object_path_costs_more_latency_than_the_bed() {
    let encoder = AtmosEncoder::new(
        &AtmosConfig {
            fine_quant: true,
            fast_mdct: false,
            ..Default::default()
        },
        3,
    )
    .unwrap();
    assert_eq!(encoder.dynamic_object_count(), 3);
    let object = encoder.latency();
    let bed = encoder.bed_latency();
    assert_eq!(object.frame_samples, bed.frame_samples);
    assert!(object.total_samples() > bed.total_samples());
    assert_eq!(encoder.latency_samples(), object.total_samples());
}

#[test]
fn atmos_encode_frame_validates_counts_and_lengths() {
    let mut encoder = AtmosEncoder::new(&AtmosConfig::default(), 2).unwrap();
    let good = vec![0.0f32; SAMPLES_PER_FRAME];
    let short = vec![0.0f32; SAMPLES_PER_FRAME / 2];
    let two = [ObjectPlacement::default(); 2];
    let one = [ObjectPlacement::default(); 1];
    assert_eq!(
        encoder.encode_frame(&[&good], &two).err().unwrap(),
        Error::InvalidArgument
    );
    assert_eq!(
        encoder.encode_frame(&[&good, &good], &one).err().unwrap(),
        Error::InvalidArgument
    );
    assert_eq!(
        encoder.encode_frame(&[&good, &short], &two).err().unwrap(),
        Error::InvalidArgument
    );
    assert!(encoder.encode_frame(&[&good, &good], &two).is_ok());

    // An object count that does not fit the C int is refused before the FFI call.
    assert_eq!(
        AtmosEncoder::new(&AtmosConfig::default(), usize::MAX)
            .err()
            .unwrap(),
        Error::InvalidArgument
    );

    // Bad bitrate/dialnorm surface at encode time with their own codes.
    for (config, expected) in [
        (
            AtmosConfig {
                bitrate_kbps: 9_999,
                ..Default::default()
            },
            Error::EncodeInvalidBitrate,
        ),
        (
            AtmosConfig {
                dialnorm: 0,
                ..Default::default()
            },
            Error::EncodeInvalidDialnorm,
        ),
    ] {
        let mut encoder = AtmosEncoder::new(&config, 1).unwrap();
        assert_eq!(
            encoder
                .encode_frame(&[&good], &[ObjectPlacement::default()])
                .err()
                .unwrap(),
            expected
        );
    }
}

/// Objects decoded through the access-unit path: positions, gains and reconstructed audio on
/// `DecodedAccessUnit`, plus the scan-side complexity index an object stream advertises.
#[test]
fn atmos_access_unit_decode_and_scan() {
    let mut encoder = AtmosEncoder::new(
        &AtmosConfig {
            bitrate_kbps: 640,
            ..Default::default()
        },
        2,
    )
    .unwrap();
    let placements = [
        ObjectPlacement {
            x: 0.0,
            y: 0.0,
            z: 0.0,
            gain: 0.5,
            lfe_send: 0.0,
        },
        ObjectPlacement {
            x: 1.0,
            y: 1.0,
            z: 1.0,
            gain: 1.0,
            lfe_send: 0.5,
        },
    ];
    let mut elementary = Vec::new();
    for frame_index in 0..4 {
        let a = tone(300.0, 48_000.0, 0.4, frame_index);
        let b = tone(1_500.0, 48_000.0, 0.4, frame_index);
        elementary.extend_from_slice(&encoder.encode_frame(&[&a, &b], &placements).unwrap());
    }

    let scanned = stream::scan(&elementary).unwrap();
    assert_eq!(scanned.kind(), Some(StreamKind::Eac3));
    assert_eq!(scanned.acmod(), Some(Acmod::Channels3_2));
    assert!(scanned.lfe());
    assert_eq!(scanned.access_unit_count(), 4);
    assert!(scanned.oba_complexity_index().is_some());

    let units = stream::split_access_units(&elementary).unwrap();
    assert_eq!(units.len(), 4);
    let mut decoder = Eac3Decoder::new(&DecoderConfig::default()).unwrap();
    for (index, unit) in units.iter().enumerate() {
        let decoded = decoder.decode_access_unit(unit).unwrap().unwrap();
        assert_eq!(decoded.channel_count(), 6);
        assert!(decoded.has_object_metadata());
        assert_eq!(decoded.dynamic_object_count(), 2);
        let first = decoded.dynamic_object(0);
        let second = decoded.dynamic_object(1);
        assert!(first.x.abs() < 0.02 && first.y.abs() < 0.02);
        assert!((second.x - 1.0).abs() < 0.02 && (second.z - 1.0).abs() < 0.07);
        // The placement gain is folded into the reconstructed essence (src/forge/src/oba/
        // atmos.cpp, "object_gain can stay at 0 dB"), so OAMD always carries 0 dB...
        assert_eq!(first.gain_db, 0.0);
        assert_eq!(second.gain_db, 0.0);
        if index >= 2 {
            assert_eq!(decoded.object_audio_count(), 2);
            let levels: Vec<f32> = (0..2)
                .map(|object| {
                    let audio = decoded.object_audio(object);
                    assert_eq!(audio.len(), decoded.samples_per_channel());
                    rms(audio)
                })
                .collect();
            assert!(levels.iter().all(|&l| l > 0.005), "levels {levels:?}");
            // ...and the gain shows up in the audio instead: object 0 (gain 0.5, -6 dB) comes
            // back quieter than object 1 (unity), both having been fed at the same level.
            let ratio = levels[0] / levels[1];
            assert!((0.3..0.8).contains(&ratio), "level ratio {ratio}");
        }
    }
}

#[test]
#[should_panic(expected = "object index out of range")]
fn decoded_access_unit_object_out_of_range_panics() {
    let mut encoder = AtmosEncoder::new(&AtmosConfig::default(), 1).unwrap();
    let s = tone(400.0, 48_000.0, 0.3, 0);
    let unit = encoder
        .encode_frame(&[&s], &[ObjectPlacement::default()])
        .unwrap();
    let decoded = Eac3Decoder::new(&DecoderConfig::default())
        .unwrap()
        .decode_access_unit(&unit)
        .unwrap()
        .unwrap();
    assert_eq!(decoded.dynamic_object_count(), 1);
    let _ = decoded.dynamic_object(1);
}

#[test]
#[should_panic(expected = "object index out of range")]
fn decoded_access_unit_object_audio_out_of_range_panics() {
    let mut encoder = AtmosEncoder::new(&AtmosConfig::default(), 1).unwrap();
    let s = tone(400.0, 48_000.0, 0.3, 0);
    let unit = encoder
        .encode_frame(&[&s], &[ObjectPlacement::default()])
        .unwrap();
    let decoded = Eac3Decoder::new(&DecoderConfig::default())
        .unwrap()
        .decode_access_unit(&unit)
        .unwrap()
        .unwrap();
    let count = decoded.object_audio_count();
    let _ = decoded.object_audio(count);
}

#[test]
#[should_panic(expected = "channel index out of range")]
fn decoded_access_unit_channel_out_of_range_panics() {
    let mut encoder = Eac3Encoder::new(&Eac3FrameConfig::default()).unwrap();
    let s = tone(400.0, 48_000.0, 0.3, 0);
    let frame = encoder.encode_frame(&[&s, &s], None, None).unwrap();
    let decoded = Eac3Decoder::new(&DecoderConfig::default())
        .unwrap()
        .decode_access_unit(&frame)
        .unwrap()
        .unwrap();
    let _ = decoded.channel_samples(2);
}

// --- Loudness meter -------------------------------------------------------------------------

#[test]
fn meter_channel_counts_follow_the_layout() {
    for (acmod, lfe, expected) in [
        (Acmod::Mono, false, 1),
        (Acmod::Stereo, true, 3),
        (Acmod::DualMono, true, 3),
        (Acmod::Channels3_2, true, 6),
    ] {
        let meter = LoudnessMeter::new(SampleRate::Hz48000, acmod, lfe).unwrap();
        assert_eq!(meter.channel_count(), expected, "{acmod:?} lfe={lfe}");
    }
    let height = LoudnessMeter::for_chanmap(
        SampleRate::Hz48000,
        ac3forge_sys::AC3FORGE_CHANMAP_512_HEIGHT as u16,
    )
    .unwrap();
    assert_eq!(height.channel_count(), 2);
    assert_eq!(
        LoudnessMeter::for_chanmap(SampleRate::Hz48000, 0)
            .err()
            .unwrap(),
        Error::InvalidArgument
    );
}

#[test]
fn meter_push_validates_spans() {
    let mut meter = LoudnessMeter::new(SampleRate::Hz48000, Acmod::Stereo, false).unwrap();
    let a = vec![0.0f32; 480];
    let b = vec![0.0f32; 479];
    assert_eq!(meter.push(&[&a]).err().unwrap(), Error::InvalidArgument);
    assert_eq!(
        meter.push(&[&a, &a, &a]).err().unwrap(),
        Error::InvalidArgument
    );
    assert_eq!(meter.push(&[&a, &b]).err().unwrap(), Error::InvalidArgument);
    // Zero-length pushes are legal no-ops; odd lengths are fine as long as they agree.
    meter.push(&[&[], &[]]).unwrap();
    meter.push(&[&b, &b]).unwrap();
}

#[test]
fn meter_reports_nothing_for_silence() {
    let mut meter = LoudnessMeter::new(SampleRate::Hz48000, Acmod::Mono, false).unwrap();
    let silence = vec![0.0f32; 48_000];
    meter.push(&[&silence]).unwrap();
    assert_eq!(meter.integrated_lkfs(), None);
    assert_eq!(meter.momentary_lkfs(), None);
    assert_eq!(meter.short_term_lkfs(), None);
    assert_eq!(meter.loudness_range(), None);
    assert_eq!(meter.true_peak_dbtp(), None);
}

/// A mono -20 dBFS-RMS 1 kHz tone for 3.5 s: every measurement becomes available, the
/// steady level gives a near-zero loudness range, the gated figures agree with one another,
/// and true peak sits at the sine's peak (0.1414 linear = about -17 dBTP).
#[test]
fn meter_measures_a_steady_tone() {
    let mut meter = LoudnessMeter::new(SampleRate::Hz48000, Acmod::Mono, false).unwrap();
    let amplitude = 0.1414f32;
    let samples: Vec<f32> = (0..(48_000 * 7 / 2))
        .map(|n| amplitude * (2.0 * std::f32::consts::PI * 1_000.0 * n as f32 / 48_000.0).sin())
        .collect();
    meter.push(&[&samples]).unwrap();

    let integrated = meter.integrated_lkfs().unwrap();
    let momentary = meter.momentary_lkfs().unwrap();
    let short_term = meter.short_term_lkfs().unwrap();
    let peak = meter.true_peak_dbtp().unwrap();
    // BS.1770: a 1 kHz sine at -20 dBFS RMS in one front channel reads -20 LKFS (the -0.691
    // offset and the K-weighting's ~+0.69 dB gain at 1 kHz cancel).
    assert!((integrated + 20.0).abs() < 0.3, "integrated {integrated}");
    assert!(
        (momentary - integrated).abs() < 0.5,
        "momentary {momentary}"
    );
    assert!(
        (short_term - integrated).abs() < 0.5,
        "short-term {short_term}"
    );
    assert!((peak - (-17.0)).abs() < 0.5, "true peak {peak}");
    if let Some(range) = meter.loudness_range() {
        assert!(
            range < 1.0,
            "a steady tone has no loudness range, got {range}"
        );
    }
}

#[test]
fn dialnorm_from_lkfs_clamps_into_the_legal_range() {
    assert_eq!(dialnorm_from_lkfs(-23.4), 23);
    assert_eq!(dialnorm_from_lkfs(-31.0), 31);
    assert_eq!(dialnorm_from_lkfs(-1.0), 1);
    assert_eq!(dialnorm_from_lkfs(0.0), 1);
    assert_eq!(dialnorm_from_lkfs(6.0), 1);
    assert_eq!(dialnorm_from_lkfs(-50.0), 31);
}

// --- Stream helpers -------------------------------------------------------------------------

fn ac3_stream(frames: usize) -> Vec<u8> {
    let mut encoder = ac3forge::ac3::Encoder::new(&Default::default()).unwrap();
    let mut out = Vec::new();
    for frame_index in 0..frames {
        let s = tone(440.0, 48_000.0, 0.3, frame_index);
        out.extend_from_slice(&encoder.encode_frame(&[&s, &s]).unwrap());
    }
    out
}

#[test]
fn split_and_bsid_error_paths() {
    assert!(stream::split_frames(&[]).unwrap().is_empty());
    assert_eq!(
        stream::split_frames(&[1, 2, 3, 4, 5]).err().unwrap(),
        Error::DecodeTruncated
    );
    assert_eq!(
        stream::split_access_units(&[0; 100]).err().unwrap(),
        Error::DecodeBadSyncWord
    );
    assert_eq!(
        stream::stream_bsid(&[0x0B, 0x77]).err().unwrap(),
        Error::DecodeTruncated
    );
    assert_eq!(
        stream::stream_bsid(&[]).err().unwrap(),
        Error::DecodeTruncated
    );
}

#[test]
fn ac3_stream_splits_and_scans_as_ac3() {
    let elementary = ac3_stream(3);
    let frames = stream::split_frames(&elementary).unwrap();
    assert_eq!(frames.len(), 3);
    assert!(frames.iter().all(|f| f.len() == 768));
    // The spans are views into the caller's buffer, contiguous and in order.
    assert_eq!(frames[1].as_ptr(), elementary[768..].as_ptr());
    assert_eq!(stream::stream_bsid(frames[0]).unwrap(), 8);
    assert_eq!(stream::split_access_units(&elementary).unwrap().len(), 3);

    let scanned = stream::scan(&elementary).unwrap();
    assert_eq!(scanned.kind(), Some(StreamKind::Ac3));
    assert_eq!(scanned.sample_rate(), Some(SampleRate::Hz48000));
    assert_eq!(scanned.acmod(), Some(Acmod::Stereo));
    assert_eq!(scanned.channels(), 2);
    assert_eq!(scanned.bsid(), 8);
    assert_eq!(scanned.substreams_per_unit(), 1);
    assert_eq!(scanned.access_unit_count(), 3);
    assert_eq!(scanned.access_unit(2), frames[2]);
    assert_eq!(scanned.access_unit_samples(0) as usize, SAMPLES_PER_FRAME);
    assert_eq!(scanned.oba_complexity_index(), None);
}

/// §E2.3.1.2 legacy-core delivery: an AC-3 5.1 frame followed by an E-AC-3 dependent substream
/// that widens it to 7.1.
#[test]
fn ac3_core_with_eac3_extension_is_detected() {
    let mut core = ac3forge::ac3::Encoder::new(&ac3forge::ac3::EncoderConfig {
        acmod: Acmod::Channels3_2,
        lfe: true,
        bitrate_kbps: 448,
        ..Default::default()
    })
    .unwrap();
    let mut extension = Eac3Encoder::new(&Eac3FrameConfig {
        acmod: Acmod::Channels2_2,
        bitrate_kbps: 192,
        strmtyp: StreamType::Dependent,
        chanmap: Some(ac3forge_sys::AC3FORGE_CHANMAP_71_REAR as u16),
        ..Default::default()
    })
    .unwrap();
    let mut elementary = Vec::new();
    for frame_index in 0..3 {
        let bed: Vec<Vec<f32>> = (0..6)
            .map(|c| tone(200.0 + 100.0 * c as f32, 48_000.0, 0.2, frame_index))
            .collect();
        let bed_spans: Vec<&[f32]> = bed.iter().map(|c| c.as_slice()).collect();
        elementary.extend_from_slice(&core.encode_frame(&bed_spans).unwrap());
        let rear: Vec<Vec<f32>> = (0..4)
            .map(|c| tone(900.0 + 100.0 * c as f32, 48_000.0, 0.2, frame_index))
            .collect();
        let rear_spans: Vec<&[f32]> = rear.iter().map(|c| c.as_slice()).collect();
        elementary.extend_from_slice(&extension.encode_frame(&rear_spans, None, None).unwrap());
    }
    assert_eq!(stream::split_frames(&elementary).unwrap().len(), 6);
    assert_eq!(stream::split_access_units(&elementary).unwrap().len(), 3);
    let scanned = stream::scan(&elementary).unwrap();
    assert_eq!(scanned.kind(), Some(StreamKind::Ac3CoreEac3Extension));
    assert_eq!(scanned.substreams_per_unit(), 2);
    assert_eq!(scanned.access_unit_count(), 3);
    assert_eq!(scanned.channels(), 8);
}

#[test]
#[should_panic(expected = "access unit out of range")]
fn scanned_access_unit_out_of_range_panics() {
    let elementary = ac3_stream(1);
    let scanned = stream::scan(&elementary).unwrap();
    let _ = scanned.access_unit(1);
}

#[test]
#[should_panic(expected = "access unit out of range")]
fn scanned_access_unit_samples_out_of_range_panics() {
    let elementary = ac3_stream(1);
    let scanned = stream::scan(&elementary).unwrap();
    let _ = scanned.access_unit_samples(1);
}

// --- version ----------------------------------------------------------------------------------

#[test]
fn version_is_consistent() {
    let version = ac3forge::version();
    assert!(version.major >= 0 && version.minor >= 0 && version.patch >= 0);
    let prefix = format!("{}.{}.{}", version.major, version.minor, version.patch);
    assert!(
        version.full.starts_with(&prefix),
        "{} vs {prefix}",
        version.full
    );
    assert_eq!(ac3forge::version(), version.clone());
}
