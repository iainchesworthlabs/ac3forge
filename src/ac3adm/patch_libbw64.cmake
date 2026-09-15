# ---------------------------------------------------------------------------
# Patch step for the vendored libbw64 0.10.0 - run by src/ac3adm/CMakeLists.txt's
# FetchContent_Populate(libbw64 ... PATCH_COMMAND), in libbw64's source directory,
# which `cmake -P` reports as CMAKE_CURRENT_SOURCE_DIR.
#
# 0.10.0 takes &buffer[0] of a std::vector<char> that can be empty in three places:
#
#   chunks.hpp  UnknownChunk(std::istream&, id, size) - a zero-length chunk whose id
#               libbw64 has no class for (a JUNK, a LIST, anything but ds64/fmt/axml/
#               chna/data)
#   reader.hpp  Bw64Reader::read() - a read of zero frames, which is what an empty
#               <data> chunk asks ac3adm's read_pcm for
#   writer.hpp  Bw64Writer::write() - a write of zero frames
#
# operator[] on an empty vector is undefined behaviour. An optimised libstdc++ build
# without assertions passes a null pointer and a zero length to stream.read()/write()
# and nothing happens, which is why this went unnoticed; UBSan reports it ("reference
# binding to null pointer of type 'char'"), and a standard library with its bounds
# checks on aborts on it (_GLIBCXX_ASSERTIONS, MSVC's debug iterators). fuzz_adm_parse
# reached the first one within a few hundred executions once ac3adm_objects was
# instrumented (fuzz/CMakeLists.txt).
#
# data() is defined on an empty vector, and every use here passes a length of zero
# alongside it, so the fix is the same token at each site. Upstream made this change to
# the first two in ebu/libbw64@0106b19 ("rework chunk reading", 2021), which no tagged
# release contains; the writer's is unchanged upstream. parser.hpp's parseAxmlChunk
# has the same spelling on a std::string, where &data[0] of an empty string is defined
# (it names the terminating null), so it is left alone.
#
# Each edit matches a token rather than a line, so a checkout with CRLF line endings
# (core.autocrlf on Windows) patches the same way. Running it again on an already
# patched tree changes nothing; a tree matching neither form means GIT_TAG moved
# without this script being updated, and stops the configure.
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
        "${path}, found ${count}. The script was written against libbw64 0.10.0; update "
        "it for the GIT_TAG in src/ac3adm/CMakeLists.txt, then delete the libbw64-src "
        "directory so the patch is applied to a fresh checkout.")
endfunction()

ac3adm_patch_libbw64(chunks.hpp "stream.read(&data_[0], size);" "stream.read(data_.data(), size);" 1)
ac3adm_patch_libbw64(reader.hpp "&rawDataBuffer_[0]" "rawDataBuffer_.data()" 2)
ac3adm_patch_libbw64(writer.hpp "&rawDataBuffer_[0]" "rawDataBuffer_.data()" 2)
