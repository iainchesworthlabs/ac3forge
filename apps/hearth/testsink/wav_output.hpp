#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ac3/io/wav.hpp"
#include "ac3/sendspin/messages.hpp"

// A test sink's output for player@v1 PCM: each stream to a float WAV file in the output
// directory, with a play-time log beside it. The log's lines are
// `local_time_us,first_frame,frames` for each chunk, and `clear,<frames>` where stream/clear
// dropped what was buffered, so a group's alignment reads straight off two sinks' logs
// (planning/hearth-reference-player.md, The test sink). With no directory it counts only.
//
// Not thread-safe: the sink calls it from the session's callbacks, under the session's lock.

namespace ac3::hearth::testsink {

class WavOutput {
   public:
    // `prefix` starts every file's name.
    WavOutput(std::filesystem::path directory, std::string prefix);
    ~WavOutput();
    WavOutput(const WavOutput&) = delete;
    WavOutput& operator=(const WavOutput&) = delete;
    WavOutput(WavOutput&&) = delete;
    WavOutput& operator=(WavOutput&&) = delete;

    // A stream began, or changed format: a new file. False for a format this output cannot
    // write, whose audio is then counted and dropped.
    bool start(const sendspin::messages::PlayerStream& stream);
    void clear();
    void end();
    // One chunk to be played from `local_time`.
    void write(std::span<const std::uint8_t> frame, std::int64_t local_time);

    [[nodiscard]] std::uint64_t chunks() const { return chunks_; }
    [[nodiscard]] std::uint64_t frames() const { return frames_; }
    [[nodiscard]] std::uint32_t streams() const { return streams_; }
    // The file the current or last stream went to; empty without a directory.
    [[nodiscard]] const std::filesystem::path& file() const { return file_; }

   private:
    std::filesystem::path directory_;
    std::string prefix_;
    std::optional<sendspin::messages::AudioFormat> format_;
    io::WavStreamWriter writer_;
    std::ofstream log_;
    std::filesystem::path file_;
    std::vector<float> samples_;
    std::uint64_t chunks_ = 0;
    std::uint64_t frames_ = 0;
    std::uint64_t stream_frames_ = 0;
    std::uint32_t streams_ = 0;
};

}  // namespace ac3::hearth::testsink
