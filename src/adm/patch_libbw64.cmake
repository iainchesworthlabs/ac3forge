# ---------------------------------------------------------------------------
# Patch step for the vendored libbw64, run by src/ac3adm/CMakeLists.txt's
# FetchContent_Populate(libbw64 ... PATCH_COMMAND), in libbw64's source directory,
# which `cmake -P` reports as CMAKE_CURRENT_SOURCE_DIR.
#
# Two patches against the pinned commit, both PRs proposed against the pinned fork
# (github.com/pwnified/libbw64) - see the CHANGELOG entry that added each for its PR link.
# Each stays until its PR lands, or until the pin moves to a commit that already carries it;
# either way, deleting this file and its PATCH_COMMAND wiring in src/ac3adm/CMakeLists.txt is
# the whole removal once both do.
#
# 1. Bw64Reader::parseChunkHeaders() (reader.hpp) refuses ANY chunk whose resolved size runs
#    past the end of the file - "chunk ends after end of file" - with no exception for <data>. A
#    recording truncated mid-<data> is an ordinary file (a transfer cut short, a disk that filled
#    up during capture), not a malformed one, and this project's own tests
#    (tests/adm/test_adm.cpp's "a file truncated inside its data chunk still parses") require it
#    to still read as far as it goes, the way every version of libbw64 before this one did. Every
#    OTHER chunk still throws - only <data> is exempted, and only from this ONE check.
# 2. FormatInfoChunk's constructor (chunks.hpp) accepts bitsPerSample 16, 24 or 32 only,
#    regardless of formatTag - so a 64-bit WAVE_FORMAT_IEEE_FLOAT <fmt > is refused at open time
#    even though this fork's own decodeFloatSamples/encodeFloatSamples (utils.hpp) both handle
#    64-bit float correctly; they are simply never reached. This module's own tests
#    (tests/adm/test_adm.cpp's "parses a float64 (double-precision) fmt chunk") need this widened
#    by one value to reach them.
#
# The empty-vector undefined behaviour an earlier libbw64 pin needed patching for
# (fuzz/CMakeLists.txt's instrumented set could not build ac3adm_objects without it) is already
# fixed here, upstream - see docs/threat-model.md's ADM section for the history; that is not a
# third patch.
#
# Matches a token rather than a line, so a CRLF checkout (core.autocrlf on
# Windows) patches the same way. Running it again on an already-patched tree
# changes nothing; a tree matching neither form means the pin moved without
# this script being updated, and stops the configure rather than silently
# doing nothing or patching the wrong thing.
# ---------------------------------------------------------------------------

function(ac3adm_patch_libbw64 file from to expected_count)
    set(path "${CMAKE_CURRENT_SOURCE_DIR}/include/bw64/${file}")
    file(READ "${path}" content)

    string(REPLACE "${from}" "" without "${content}")
    string(LENGTH "${content}" content_length)
    string(LENGTH "${without}" without_length)
    string(LENGTH "${from}" from_length)
    math(EXPR count "(${content_length} - ${without_length}) / ${from_length}")

    if(count EQUAL expected_count)
        string(REPLACE "${from}" "${to}" content "${content}")
        file(WRITE "${path}" "${content}")
        return()
    endif()
    string(FIND "${content}" "${to}" patched_at)
    if(count EQUAL 0 AND NOT patched_at EQUAL -1)
        return()
    endif()
    message(FATAL_ERROR
        "src/ac3adm/patch_libbw64.cmake: expected ${expected_count} of '${from}' in "
        "${path}, found ${count}. The script was written against the GIT_TAG pinned "
        "in src/ac3adm/CMakeLists.txt at the time; update it for whatever the pin is "
        "now, then delete the libbw64-src directory so the patch applies to a fresh "
        "checkout.")
endfunction()

ac3adm_patch_libbw64(reader.hpp
    "        if (chunk_end > end)
          throw std::runtime_error(\"chunk ends after end of file\");

        chunkHeaders_.push_back(chunkHeader);

        if (chunk_end < end) {
          if (chunk_size % 2 != 0)
            chunk_size = utils::safeAdd<std::streamoff>(chunk_size, 1);
        }
        fileStream_.seekg(chunk_size, std::ios::cur);"
    "        if (chunk_end > end) {
          // ac3forge's own patch (src/ac3adm/patch_libbw64.cmake): a recording
          // truncated mid-<data> is an ordinary file, not a malformed one -
          // clamp to what is actually there instead of refusing it. Every
          // other chunk still throws.
          if (chunkHeader.id != utils::fourCC(\"data\"))
            throw std::runtime_error(\"chunk ends after end of file\");
          chunk_size = end - fileStream_.tellg();
          chunkHeader.size = static_cast<uint64_t>(chunk_size);
          chunk_end = end;
        }

        chunkHeaders_.push_back(chunkHeader);

        if (chunk_end < end) {
          if (chunk_size % 2 != 0)
            chunk_size = utils::safeAdd<std::streamoff>(chunk_size, 1);
        }
        fileStream_.seekg(chunk_size, std::ios::cur);"
    1)

ac3adm_patch_libbw64(chunks.hpp
    "      if (bitsPerSample_ != 16u && bitsPerSample_ != 24u &&
          bitsPerSample_ != 32u) {"
    "      if (bitsPerSample_ != 16u && bitsPerSample_ != 24u &&
          bitsPerSample_ != 32u && bitsPerSample_ != 64u) {  // ac3forge's own patch: 64-bit float"
    1)
