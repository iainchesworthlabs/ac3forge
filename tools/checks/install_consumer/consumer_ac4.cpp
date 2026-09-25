// The AC-4 consumer of an installed package, built by tools/checks/check_install_consumer.sh twice
// over: through find_package(ac3forge) for each exported decoder target (CMakeLists.txt here), and
// through `pkg-config --cflags --libs ac4dec` alone. It sees the installed headers and libraries
// and nothing of the build tree, so a header the decoder's includes and the install leave out, an
// archive a static decoder calls into and the package does not name, or a symbol the shared
// libraries do not export stops it here.
//
// It splits the stream given on its command line with the inspector's SyncFrameSplitter, reading
// it a block at a time as a network client would, and decodes every frame with ac4::Decoder,
// then reads the presentations and the metadata back: the calls a player makes, each across the
// boundary of the library that defines it. The stream is a committed DEE one
// (tests/golden/external-baseline/ac4-51-film-96/dee.ac4, 5.1 at 48 kHz) whose every frame
// decodes.

#include <cstddef>
#include <cstdio>
#include <fstream>
#include <ios>
#include <span>
#include <string_view>
#include <vector>

#include <ac4/ac4.hpp>
#include <ac4dec/decoder.hpp>

namespace {

int fail(const char* what) {
    std::fprintf(stderr, "consumer_ac4: %s\n", what);
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: consumer_ac4 <stream.ac4>\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        return fail("the stream does not open");
    }

    std::vector<std::byte> storage(ac4::kSplitterRecommendedBuffer);
    ac4::SyncFrameSplitter splitter{storage};
    ac4::Decoder decoder;
    std::size_t frames = 0;
    std::size_t decoded = 0;
    std::size_t samples = 0;
    std::size_t channels = 0;
    int rate = 0;

    for (;;) {
        const auto next = splitter.next();
        if (next.status == ac4::SyncFrameSplitter::Status::kNeedMoreInput) {
            // 4 KiB at a time, so that frames arrive in pieces.
            std::span<std::byte> space = splitter.writable();
            if (space.size() > 4096) {
                space = space.first(4096);
            }
            in.read(reinterpret_cast<char*>(space.data()),
                    static_cast<std::streamsize>(space.size()));
            const auto read = static_cast<std::size_t>(in.gcount());
            if (read == 0) {
                splitter.finish();
            } else {
                splitter.commit(read);
            }
            continue;
        }
        if (next.status != ac4::SyncFrameSplitter::Status::kFrame) {
            if (next.status != ac4::SyncFrameSplitter::Status::kEndOfStream) {
                return fail("the splitter stopped before the end of the stream");
            }
            break;
        }
        ++frames;
        const auto out = decoder.decode(next.frame.raw_ac4_frame);
        if (!out) {
            const std::string_view reason = decoder.refusal_reason();
            std::fprintf(stderr, "consumer_ac4: frame %zu did not decode: %.*s\n", frames,
                         static_cast<int>(reason.size()), reason.data());
            return 1;
        }
        if (!*out) {
            continue;
        }
        ++decoded;
        samples += (*out)->samples;
        channels = (*out)->channels.size();
        rate = (*out)->sample_rate_hz;
    }

    if (frames == 0 || decoded != frames) {
        return fail("not every frame of the stream decoded");
    }
    if (channels != 6 || rate != 48000) {
        return fail("the stream did not decode to 5.1 at 48 kHz");
    }
    if (splitter.resynchronised_bytes() != 0) {
        return fail("the splitter skipped bytes of a stream that is all frames");
    }
    const auto presentations = decoder.presentations();
    if (presentations.empty() || !presentations.front().decodable) {
        return fail("the decoder reported no presentation it decodes");
    }
    if (!decoder.metadata().loudness.dialnorm_dbfs) {
        return fail("the decoder reported no dialogue normalisation");
    }

    std::printf(
        "consumer_ac4: %zu frames, %zu channels at %d Hz, %zu samples, %zu presentation(s), "
        "dialnorm %.0f dBFS, latency %d samples\n",
        frames, channels, rate, samples, presentations.size(),
        *decoder.metadata().loudness.dialnorm_dbfs, decoder.latency_samples());
    return 0;
}
