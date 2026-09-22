# ---------------------------------------------------------------------------
# Runs a command and captures its stdout into a file, as a `cmake -P` script.
#
# add_custom_command() does NOT run its COMMAND through a shell, so a plain
# `... man > ac3cli.1` in one would hand ">" and "ac3cli.1" to the process as
# two ordinary arguments rather than redirecting anything. This is the
# portable stand-in: execute_process()'s own OUTPUT_FILE does the redirect,
# and `cmake -P` is available by definition wherever CMake is.
#
# Used by apps/cli/CMakeLists.txt to generate the man page and the four shell
# completion scripts from `ac3cli man` / `ac3cli completions <shell>`.
#
#   cmake -DAC3_RUN_COMMAND=<exe>;<arg>... -DAC3_RUN_OUTPUT=<path>
#         -P cmake/RunToFile.cmake
#
# A non-zero exit is a hard error: a half-written or empty man page that the
# packaging step then installs is exactly the silent failure worth refusing.
# ---------------------------------------------------------------------------

if(NOT DEFINED AC3_RUN_COMMAND OR NOT DEFINED AC3_RUN_OUTPUT)
    message(FATAL_ERROR "RunToFile.cmake needs both AC3_RUN_COMMAND and AC3_RUN_OUTPUT")
endif()

execute_process(
    COMMAND ${AC3_RUN_COMMAND}
    OUTPUT_FILE "${AC3_RUN_OUTPUT}"
    RESULT_VARIABLE ac3_run_result
    ERROR_VARIABLE ac3_run_stderr)

if(NOT ac3_run_result EQUAL 0)
    # Remove the partial file first: leaving it behind would let a later
    # build of the same target see an up-to-date timestamp and skip the
    # regeneration that is still needed.
    file(REMOVE "${AC3_RUN_OUTPUT}")
    message(FATAL_ERROR
        "command failed (${ac3_run_result}): ${AC3_RUN_COMMAND}\n${ac3_run_stderr}")
endif()
