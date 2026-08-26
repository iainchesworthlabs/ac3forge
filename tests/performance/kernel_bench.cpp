// Per-kernel micro-benchmarks: the observability slice's other half, beside
// the Tracy zones threaded through the encoder for a real-time capture. Where
// ac3bench (bench_encoder.cpp) times a whole frame end to end, this isolates
// each hot kernel and answers which one actually costs what - without a
// profiler attached, and cheap enough to run on every change locally.
//
// Not a Catch2 binary, same reasoning as ac3bench: nothing here asserts
// anything. It writes one JSON record per kernel, {name, iters, ns_per_call},
// to --json-out, for tools/ci/append_kernel_history.py to append to the
// per-kernel trend data (docs/performance-trend.md) the same way ac3bench's
// output feeds append_performance_history.py - with one deliberate
// difference: the kernel series never fails CI, at any threshold (see that
// script's docstring for why).
//
// Every kernel is fed REAL audio run through the real per-block windowing +
// forward MDCT, never synthetic zeros or a single stationary tone: this
// project's own validation rule (silence and frame 0 give false passes on
// correctness checks - see docs on gold-reference validation) applies just as
// much to timing here, because a bit-allocation/mantissa-cost kernel run
// against one pure tone's degenerate, single-bin spectrum does not cost what
// it costs against broadband program material.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fmt/printf.h>
#include <fstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ac3/core/bitalloc.hpp"
#include "ac3/core/exponents.hpp"
#include "ac3/core/fft.hpp"
#include "ac3/core/mantissas.hpp"
#include "ac3/core/mdct.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/dsp/qmf.hpp"
#include "ac3/encoder/eac3_tools.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/oba/joc.hpp"
#include "ac3/oba/joc_tables.hpp"
#include "real_audio.hpp"

namespace {

// The largest legal fbw mantissa count (chbwcod = 60: 37 + 3*(60+12) = 253 -
// see exponents.cpp's own derivation). compute_bit_allocation/
// encode_exponents assert exps.size() <= this; the top few of a 256-bin MDCT
// output are never coded, so every exponent/bit-allocation/mantissa kernel
// below works on this many bins, not the full 256 mdct512_forward produces.
constexpr std::size_t kEndmant = 253;

// Sink every kernel's output value flows through before the run ends, so no
// compiler can prove a benched call's result is unused and elide it.
double g_sink = 0.0;

struct KernelResult {
    std::string name;
    std::uint64_t iters = 0;
    double ns_per_call = 0.0;
};

// Runs `fn` repeatedly until at least `min_ms` have elapsed (and at least 16
// iterations, for kernels fast enough that 16 calls still beat min_ms) rather
// than a hand-picked iteration count per kernel - the ~20x cost spread across
// these kernels (a single quantize_mantissa call versus a full
// ecpl_channel_spectrum) makes one fixed count either too slow for the cheap
// kernels or too noisy for the expensive ones.
template <typename Fn>
KernelResult time_kernel(std::string name, Fn&& fn, double min_ms = 200.0) {
    for (int i = 0; i < 3; ++i) {
        fn();
    }
    std::uint64_t iters = 0;
    const auto start = std::chrono::steady_clock::now();
    double elapsed_ms = 0.0;
    do {
        fn();
        ++iters;
        elapsed_ms = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - start)
                         .count();
    } while (elapsed_ms < min_ms || iters < 16);
    return {.name = std::move(name), .iters = iters,
            .ns_per_call = elapsed_ms * 1.0e6 / static_cast<double>(iters)};
}

// One channel's real PCM, windowed and forward-transformed exactly like the
// real encoder's per-block path (encoder.cpp / eac3_frame.cpp / atmos.cpp's
// band_energy all share this shape): 512 samples straddling the block
// boundary, zero-padded before the start of the buffer rather than reading a
// previous frame's history - a real-enough approximation for kernel timing,
// which does not depend on getting the very first block's overlap exactly
// right the way a bitstream would.
std::array<double, 512> block_windowed(std::span<const float> channel, int block_index) {
    std::array<double, 512> time{};
    for (int n = 0; n < 512; ++n) {
        const int index = block_index * 256 + n - 256;
        time[static_cast<std::size_t>(n)] =
            (index < 0 || static_cast<std::size_t>(index) >= channel.size())
                ? 0.0
                : static_cast<double>(channel[static_cast<std::size_t>(index)]);
    }
    std::array<double, 512> windowed{};
    ac3::apply_analysis_window(time, windowed);
    return windowed;
}

std::array<double, 256> block_coeffs(std::span<const float> channel, int block_index) {
    const auto windowed = block_windowed(channel, block_index);
    std::array<double, 256> coeffs{};
    ac3::mdct512_forward(windowed, coeffs);
    return coeffs;
}

std::array<std::uint8_t, 256> exps_from_coeffs(const std::array<double, 256>& coeffs) {
    std::array<std::int32_t, 256> fixed{};
    for (std::size_t i = 0; i < coeffs.size(); ++i) {
        fixed[i] = ac3::to_fixed25(coeffs[i]);
    }
    std::array<std::uint8_t, 256> exps{};
    ac3::extract_exponents(fixed, exps);
    return exps;
}

// Real fixture, loaded once: reference_51.wav's six channels, each long
// enough to supply several consecutive real blocks (AHT's six-block window,
// ecpl's prev/curr/next triple) without running off the end of the file.
// The loading and its no-synthetic-fallback rule live in real_audio.hpp,
// shared with ac3bench and ac3perf so the three benches cannot drift apart
// again on what a bench input is.
struct RealAudio {
    ac3::io::WavData wav;
    static constexpr std::size_t kMinChannels = 6;
    static constexpr int kMinBlocks = 8;  // covers every kernel's block reach

    [[nodiscard]] std::span<const float> channel(std::size_t index) const {
        return wav.channels[index % wav.channels.size()];
    }
};

RealAudio load_fixture(const std::string& path) {
    return RealAudio{
        .wav = perf::load_real_audio(path, RealAudio::kMinChannels,
                                     static_cast<std::size_t>(RealAudio::kMinBlocks + 1) *
                                         ac3::kSamplesPerBlock)};
}

void write_json(const std::vector<KernelResult>& results, const std::string& path) {
    std::ofstream out(path);
    out << "{\n  \"kernels\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        out << "    {\"name\": \"" << r.name << "\", \"iters\": " << r.iters
            << ", \"ns_per_call\": " << r.ns_per_call << "}"
            << (i + 1 < results.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string json_out;
    std::string wav_path = perf::kReference51Wav;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--json-out" && i + 1 < argc) {
            json_out = argv[++i];
        } else if (arg == "--wav" && i + 1 < argc) {
            wav_path = argv[++i];
        }
    }

    const RealAudio audio = load_fixture(wav_path);

    // Real per-block MDCT coefficients and their derived exponents, for a
    // handful of consecutive blocks on the first two real channels - enough
    // to cover every kernel below without recomputing the same transform
    // twice.
    std::vector<std::array<double, 256>> ch0_coeffs;
    std::vector<std::array<std::uint8_t, 256>> ch0_exps;
    for (int b = 0; b < RealAudio::kMinBlocks; ++b) {
        ch0_coeffs.push_back(block_coeffs(audio.channel(0), b));
        ch0_exps.push_back(exps_from_coeffs(ch0_coeffs.back()));
    }
    const auto windowed_block = block_windowed(audio.channel(0), 4);

    std::vector<KernelResult> results;

    // --- mdct512_forward -----------------------------------------------------
    results.push_back(time_kernel("mdct512_forward", [&] {
        std::array<double, 256> coeffs{};
        ac3::mdct512_forward(windowed_block, coeffs);
        g_sink += coeffs[64];
    }));

    // --- mdct512_forward, fast path (§7.9.4 N/4-FFT structure - what
    // EncoderConfig::fast_mdct / eac3::FrameConfig::fast_mdct, default on,
    // actually run; the direct row above is the reference form) - the number
    // this kernel's own fast/direct comparison exists to produce.
    results.push_back(time_kernel("mdct512_forward_fast", [&] {
        std::array<double, 256> coeffs{};
        ac3::mdct512_forward(windowed_block, coeffs, /*fast=*/true);
        g_sink += coeffs[64];
    }));

    // --- mdct256 pair (block-switched short transform) -----------------------
    results.push_back(time_kernel("mdct256_pair", [&] {
        const std::span<const double, 512> full(windowed_block);
        std::array<double, 128> first{};
        std::array<double, 128> second{};
        ac3::mdct256_forward_first(full.first<256>(), first);
        ac3::mdct256_forward_second(full.last<256>(), second);
        g_sink += first[32] + second[32];
    }));
    // The same pair down their own DCT-IV folds - what a block-switched
    // frame actually runs under the default fast_mdct; the direct row above
    // is the reference form.
    results.push_back(time_kernel("mdct256_pair_fast", [&] {
        const std::span<const double, 512> full(windowed_block);
        std::array<double, 128> first{};
        std::array<double, 128> second{};
        ac3::mdct256_forward_first(full.first<256>(), first, /*fast=*/true);
        ac3::mdct256_forward_second(full.last<256>(), second, /*fast=*/true);
        g_sink += first[32] + second[32];
    }));

    // --- imdct512_windowed ----------------------------------------------------
    // The inverse transform is where a decode frame's time actually goes,
    // and until roadmap PF1 only the direct form was benched here - while
    // the fast one became DecoderConfig::fast_imdct's default in 0.9.0, so
    // this series tracked a transform no decoder runs any more. Both forms
    // are timed for the same reason the forward pair above is: mode=reference
    // still selects the direct evaluation, and the ratio between these two
    // rows is what that choice costs.
    results.push_back(time_kernel("imdct512_windowed", [&] {
        std::array<double, 512> x{};
        ac3::imdct512_windowed(ch0_coeffs[4], x);
        g_sink += x[256];
    }));
    // The same inverse down its radix-2 step 3 - what DecoderConfig::fast_imdct
    // (default on since 0.9.0) actually runs, and so the row that matters for
    // decode throughput; the direct row above is the reference form. Added
    // with the PF5 vector kernels, which speed up this path and not the
    // direct one: without it the whole decode side of that work is invisible
    // to the trend tables. (ROADMAP PF1 wants more than this - E-AC-3 encode
    // series, decoder Tracy zones, real-audio timing inputs - and is
    // unaffected.)
    results.push_back(time_kernel("imdct512_windowed_fast", [&] {
        std::array<double, 512> x{};
        ac3::imdct512_windowed(ch0_coeffs[4], x, /*fast=*/true);
        g_sink += x[256];
    }));

    // --- imdct256_pair_windowed (block-switched inverse) ----------------------
    results.push_back(time_kernel("imdct256_pair_windowed", [&] {
        std::array<double, 512> x{};
        ac3::imdct256_pair_windowed(ch0_coeffs[4], x);
        g_sink += x[256];
    }));
    results.push_back(time_kernel("imdct256_pair_windowed_fast", [&] {
        std::array<double, 512> x{};
        ac3::imdct256_pair_windowed(ch0_coeffs[4], x, /*fast=*/true);
        g_sink += x[256];
    }));

    // --- dft512 (the FFT core at P = 512, its largest size) ------------------
    // §E3.5.5.1 step 5's full complex spectrum, the step-4 half of
    // ecpl_channel_spectrum below. Real input: one block's windowed PCM in
    // the real part, zero imaginary, the shape ecpl's own step 3 hands it.
    std::array<double, 512> dft_in_re = windowed_block;
    std::array<double, 512> dft_in_im{};
    results.push_back(time_kernel("dft512", [&] {
        std::array<double, 512> out_re{};
        std::array<double, 512> out_im{};
        ac3::dft512(dft_in_re, dft_in_im, out_re, out_im);
        g_sink += out_re[64];
    }));

    // --- compute_bit_allocation -----------------------------------------------
    const ac3::BitAllocCodes codes{};
    const std::span<const std::uint8_t> exps4{ch0_exps[4].data(), kEndmant};
    results.push_back(time_kernel("compute_bit_allocation", [&] {
        std::array<std::uint8_t, kEndmant> bap{};
        ac3::compute_bit_allocation(exps4, ac3::SampleRate::k48000, codes,
                                    /*csnroffst=*/10, /*fsnroffst=*/0, bap);
        g_sink += bap[64];
    }));

    // --- encode_exponents -------------------------------------------------
    results.push_back(time_kernel("encode_exponents", [&] {
        const auto encoded = ac3::encode_exponents(exps4, ac3::ExpStrategy::kD15);
        g_sink += encoded.absolute;
    }));

    // --- mantissa_bits_per_block (six real streams' worth of bap) ------------
    std::vector<std::array<std::uint8_t, kEndmant>> stream_bap(6);
    for (int ch = 0; ch < 6; ++ch) {
        const auto full_exps =
            exps_from_coeffs(block_coeffs(audio.channel(static_cast<std::size_t>(ch)), 4));
        const std::span<const std::uint8_t> exps{full_exps.data(), kEndmant};
        ac3::compute_bit_allocation(exps, ac3::SampleRate::k48000, codes, 10, 0,
                                    stream_bap[static_cast<std::size_t>(ch)]);
    }
    results.push_back(time_kernel("mantissa_bits_per_block", [&] {
        std::vector<std::span<const std::uint8_t>> views;
        views.reserve(stream_bap.size());
        for (const auto& bap : stream_bap) {
            views.push_back(bap);
        }
        g_sink += static_cast<double>(ac3::mantissa_bits_per_block(views));
    }));

    // --- quantize_mantissa loop (one real block's active bins) ---------------
    results.push_back(time_kernel("quantize_mantissa_loop", [&] {
        std::uint32_t checksum = 0;
        for (std::size_t bin = 0; bin < kEndmant; ++bin) {
            const int bap = stream_bap[0][bin];
            if (bap == 0) {
                continue;
            }
            const auto mantissa = ac3::to_fixed25(ch0_coeffs[4][bin]);
            checksum += ac3::quantize_mantissa(mantissa, bap);
        }
        g_sink += checksum;
    }));

    // --- aht_forward + aht_vector_quantize (real six-block window, one bin) --
    constexpr int kAhtBin = 64;
    std::array<double, ac3::eac3::kBlocksPerFrameSize> aht_blocks{};
    for (std::size_t b = 0; b < ac3::eac3::kBlocksPerFrameSize; ++b) {
        aht_blocks[b] = ch0_coeffs[b][kAhtBin];
    }
    results.push_back(time_kernel("aht_forward", [&] {
        std::array<double, ac3::eac3::kBlocksPerFrameSize> out{};
        ac3::eac3::aht_forward(aht_blocks, out);
        g_sink += out[0];
    }));

    std::array<double, ac3::eac3::kBlocksPerFrameSize> aht_coeffs{};
    ac3::eac3::aht_forward(aht_blocks, aht_coeffs);
    results.push_back(time_kernel("aht_vector_quantize", [&] {
        auto values = aht_coeffs;
        g_sink += ac3::eac3::aht_vector_quantize(values, /*hebap=*/4);
    }));

    // --- ecpl_channel_spectrum (real prev/curr/next 256-bin coefficient sets) -
    results.push_back(time_kernel("ecpl_channel_spectrum", [&] {
        std::array<double, 256> real_out{};
        std::array<double, 256> imag_out{};
        ac3::eac3::ecpl_channel_spectrum(ch0_coeffs[3], ch0_coeffs[4], ch0_coeffs[5], real_out,
                                         imag_out);
        g_sink += real_out[64];
    }));
    // The same call with its three step-1 inverses on the fast path - what a
    // decode (DecoderConfig::fast_imdct) and a default enhanced-coupling
    // encode (eac3::FrameConfig::fast_mdct) both run; the row above is the
    // reference form.
    results.push_back(time_kernel("ecpl_channel_spectrum_fast", [&] {
        std::array<double, 256> real_out{};
        std::array<double, 256> imag_out{};
        ac3::eac3::ecpl_channel_spectrum(ch0_coeffs[3], ch0_coeffs[4], ch0_coeffs[5], real_out,
                                         imag_out, /*fast=*/true);
        g_sink += real_out[64];
    }));

    // --- band_energy (one real 5.1 frame, default 9-band JOC layout) ---------
    const auto frame0 = audio.channel(0).subspan(0, static_cast<std::size_t>(ac3::kSamplesPerFrame));
    const auto& mapping = ac3::oba::joc::kSubbandToBand[4];  // idx 4 -> 9 bands, AtmosConfig's default
    results.push_back(time_kernel("band_energy", [&] {
        std::array<double, 9> energy{};
        ac3::oba::band_energy(frame0, mapping, energy, /*fast=*/false);
        g_sink += energy[0];
    }));
    // The same frame down the §7.9.4 fast path - what AtmosConfig::fast_mdct
    // (default on) actually runs; the direct row above is the reference form.
    results.push_back(time_kernel("band_energy_fast", [&] {
        std::array<double, 9> energy{};
        ac3::oba::band_energy(frame0, mapping, energy, /*fast=*/true);
        g_sink += energy[0];
    }));

    // --- the QMF-domain siblings (oba::joc::Domain::kQmf) --------------------------
    // Same frame, same band layout, read off §7.1's 64 complex subbands
    // instead of 256 MDCT bins. The filterbank rows underneath it are the
    // per-timeslot primitives both sides are built from - a frame is 24 of
    // them per signal.
    results.push_back(time_kernel("qmf_band_energy", [&] {
        static ac3::dsp::QmfAnalysis analysis;
        std::array<double, 9> energy{};
        ac3::oba::qmf_band_energy(frame0, mapping, energy, analysis);
        g_sink += energy[0];
    }));
    results.push_back(time_kernel("qmf_analysis_timeslot", [&] {
        static ac3::dsp::QmfAnalysis analysis;
        std::array<double, ac3::dsp::kQmfSubbands> real{};
        std::array<double, ac3::dsp::kQmfSubbands> imag{};
        analysis.push(std::span<const float, ac3::dsp::kQmfHop>{
                          frame0.data(), static_cast<std::size_t>(ac3::dsp::kQmfHop)},
                      real, imag);
        g_sink += real[7];
    }));
    results.push_back(time_kernel("qmf_synthesis_timeslot", [&] {
        static ac3::dsp::QmfSynthesis synthesis;
        std::array<double, ac3::dsp::kQmfSubbands> real{};
        std::array<double, ac3::dsp::kQmfSubbands> imag{};
        real[7] = 0.5;
        imag[9] = -0.25;
        std::array<float, ac3::dsp::kQmfHop> out{};
        synthesis.pull(real, imag, out);
        g_sink += static_cast<double>(out[3]);
    }));

    // --- oba::joc::reconstruct, both domains, one whole 4-object frame -------------
    // The decode-side cost the WASM demo and the Android app actually pay:
    // five downmix channels analysed and four objects synthesised, per frame.
    {
        std::vector<std::vector<float>> bed_storage(
            static_cast<std::size_t>(ac3::oba::joc::kNumChannels5X),
            std::vector<float>(static_cast<std::size_t>(ac3::kSamplesPerFrame)));
        for (std::size_t ch = 0; ch < bed_storage.size(); ++ch) {
            const auto source =
                audio.channel(ch).subspan(0, static_cast<std::size_t>(ac3::kSamplesPerFrame));
            std::copy(source.begin(), source.end(), bed_storage[ch].begin());
        }
        std::vector<std::span<const float>> bed;
        bed.reserve(bed_storage.size());
        for (const auto& channel : bed_storage) {
            bed.emplace_back(channel);
        }
        ac3::oba::joc::FrameParameters params{.objects = 4, .num_bands_idx = 4, .seq_count = 5};
        params.matrix.assign(params.coefficient_count(), 0.0);
        for (int object = 0; object < params.objects; ++object) {
            for (int channel = 0; channel < params.channels; ++channel) {
                for (int band = 0; band < params.bands(); ++band) {
                    params.at(object, channel, band) = 0.4 - 0.05 * ((object + channel) % 5);
                }
            }
        }
        results.push_back(time_kernel("joc_reconstruct_mdct_4obj", [&] {
            static ac3::oba::joc::ReconstructionState state;
            const auto out =
                ac3::oba::joc::reconstruct(bed, params, state, /*fast_mdct=*/true,
                                      /*fast_imdct=*/true, ac3::oba::joc::Domain::kMdctBand);
            g_sink += static_cast<double>(out[0][128]);
        }));
        // PF8: bed analysis' own forward transform, isolated from the object
        // synthesis inverse (left fast on both sides, matching
        // Eac3Decoder's real default) - what DecoderConfig::fast_mdct now
        // switches, against the direct §8.2.3.2 form it replaced.
        results.push_back(time_kernel("joc_reconstruct_mdct_4obj_direct", [&] {
            static ac3::oba::joc::ReconstructionState state;
            const auto out =
                ac3::oba::joc::reconstruct(bed, params, state, /*fast_mdct=*/false,
                                      /*fast_imdct=*/true, ac3::oba::joc::Domain::kMdctBand);
            g_sink += static_cast<double>(out[0][128]);
        }));
        results.push_back(time_kernel("joc_reconstruct_qmf_4obj", [&] {
            static ac3::oba::joc::ReconstructionState state;
            const auto out =
                ac3::oba::joc::reconstruct(bed, params, state, /*fast_mdct=*/true,
                                      /*fast_imdct=*/true, ac3::oba::joc::Domain::kQmf);
            g_sink += static_cast<double>(out[0][128]);
        }));

        // The SAME two workloads at 12 objects, and they exist because the
        // 4-object pair above could not see a real bug.
        //
        // FrameParameters::at() used to re-walk an O(objects) offset list on
        // every coefficient access, making a frame O(objects^2). At four
        // objects that term is small: fixing it moved these 4-object series
        // by ~26%, which reads like an ordinary optimisation. At twelve it
        // was 1.8x (kMdctBand) to 2.9x (kQmf) of a real decode - the fix was
        // worth more than every transform change in ROADMAP PF5 combined,
        // and this bench was structurally unable to say so.
        //
        // Twelve is also representative rather than arbitrary: TS 103 420
        // caps a programme at 15 dynamic objects plus the bed LFE, and 12 is
        // three clean batches of four for the batched transform paths. Any
        // cost that grows super-linearly in object count shows up here and
        // nowhere else in this bench.
        {
            ac3::joc::FrameParameters wide{.objects = 12, .num_bands_idx = 4, .seq_count = 5};
            wide.matrix.assign(wide.coefficient_count(), 0.0);
            for (int object = 0; object < wide.objects; ++object) {
                for (int channel = 0; channel < wide.channels; ++channel) {
                    for (int band = 0; band < wide.bands(); ++band) {
                        wide.at(object, channel, band) = 0.4 - 0.05 * ((object + channel) % 5);
                    }
                }
            }
            results.push_back(time_kernel("joc_reconstruct_mdct_12obj", [&] {
                static ac3::joc::ReconstructionState state;
                const auto out =
                    ac3::joc::reconstruct(bed, wide, state, /*fast_mdct=*/true,
                                          /*fast_imdct=*/true, ac3::joc::Domain::kMdctBand);
                g_sink += static_cast<double>(out[0][128]);
            }));
            results.push_back(time_kernel("joc_reconstruct_qmf_12obj", [&] {
                static ac3::joc::ReconstructionState state;
                const auto out =
                    ac3::joc::reconstruct(bed, wide, state, /*fast_mdct=*/true,
                                          /*fast_imdct=*/true, ac3::joc::Domain::kQmf);
                g_sink += static_cast<double>(out[0][128]);
            }));
        }
    }

    // --- one full bits_at evaluation ------------------------------------------
    // The SNR-offset binary search's per-iteration cost unit (see
    // eac3_frame.cpp's `bits_at`, ~line 2176, and encoder.cpp's AC-3-path
    // equivalent added alongside this bench's Tracy zone): every stream's
    // compute_bit_allocation, then one mantissa_bits_per_block combining
    // them. `bits_at` itself is a capturing lambda private to
    // FrameEncoder::encode_frame with no linkable symbol, so this
    // reconstructs its exact computational shape from the same public
    // building blocks it wraps, over six real streams' real exponents.
    std::vector<std::array<std::uint8_t, 256>> bits_at_exps_full(6);
    for (int ch = 0; ch < 6; ++ch) {
        bits_at_exps_full[static_cast<std::size_t>(ch)] =
            exps_from_coeffs(block_coeffs(audio.channel(static_cast<std::size_t>(ch)), 5));
    }
    results.push_back(time_kernel("bits_at_one_eval", [&] {
        std::vector<std::array<std::uint8_t, kEndmant>> bap(6);
        std::vector<std::span<const std::uint8_t>> views;
        views.reserve(6);
        for (int ch = 0; ch < 6; ++ch) {
            const std::span<const std::uint8_t> exps{
                bits_at_exps_full[static_cast<std::size_t>(ch)].data(), kEndmant};
            ac3::compute_bit_allocation(exps, ac3::SampleRate::k48000, codes, 10, 0,
                                        bap[static_cast<std::size_t>(ch)]);
            views.push_back(bap[static_cast<std::size_t>(ch)]);
        }
        g_sink += static_cast<double>(ac3::mantissa_bits_per_block(views));
    }));

    for (const auto& r : results) {
        fmt::printf("%-28s %10llu iters  %10.1f ns/call\n", r.name.c_str(),
                    static_cast<unsigned long long>(r.iters), r.ns_per_call);
    }
    // Not printed for its value - just to anchor g_sink as observably used.
    fmt::printf("(checksum %.6f)\n", g_sink);

    if (!json_out.empty()) {
        write_json(results, json_out);
        fmt::printf("wrote %s\n", json_out.c_str());
    }

    return 0;
}
