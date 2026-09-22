#pragma once

// The exit codes every ac3cli command returns, and the only place their
// numbers are chosen.
//
// This CLI is positioned as a pipeline tool - "-" for stdin/stdout, `qc` as a
// CI step - but every failure used to return 1, so a script could not tell a
// usage error from a missing input file, a full disk, an absent capture
// device or a failed QC gate. It could only tell "worked" from "did not", and
// then had to scrape stderr to learn anything more, which is exactly the
// thing an exit code exists to avoid.
//
// The scheme is deliberately small: seven failure codes, each answering one
// question a script actually acts on differently.
//
//   0  success
//   1  the command line was wrong - a bad or missing argument, an unknown
//      command or option, or a configuration the encoder cannot express
//      (an illegal bitrate for a layout, more objects than a stream can
//      carry). Retrying with the same arguments cannot help. 1, not a
//      higher number, because that is what every other tool returns for a
//      usage error and what a shell's own "command failed" reflex expects.
//   2  the INPUT was the problem - unreadable, absent, not a valid AC-3/
//      E-AC-3/WAV/ADM file, or a stream that stopped decoding part-way.
//   3  the OUTPUT was the problem - the destination could not be created,
//      written or finalized. Distinct from 2 because the usual response is
//      different: free disk, fix permissions, choose another path.
//   4  this build or this machine cannot run the command at all - no audio
//      backend on the platform, no capture/render endpoint, a device that
//      refuses the format, or a library (ADM) this build was not configured
//      with. A different machine may well succeed with the identical
//      command line.
//   5  the run started and then failed for a reason that is none of the
//      above - a capture device that stopped delivering audio mid-session
//      (the live/record watchdog), a loudness measurement with nothing
//      above the gate to measure, a signing pass that could not complete.
//   6  a QC gate failed. `qc`'s own long-standing contract - "exit code is 0
//      only when every requested gate passes" - is unchanged; this just
//      names the non-zero half, so a CI step can tell "the stream is out of
//      spec" (which is a result) from "qc could not read the file" (which
//      is a fault).
//   7  an internal error: an exception escaped a command. Never expected;
//      worth reporting if it happens.
//
// Plain constants rather than an enum class: every run_* handler and main()
// itself return int (the C++ main() contract, and the function-pointer
// column in main.cpp's command table), so an enum would need a cast at all
// ~220 return sites and buy nothing. They live in their own header rather
// than in support.hpp because every command file needs them while most need
// nothing else from a shared header - and because misc-include-cleaner (a CI
// gate) wants the file that uses a name to include the file that declares it.
//
// Documented for users in docs/cli/commands.md#exit-codes; `ac3cli help
// exit-codes` prints the same table.
namespace ac3cli {

inline constexpr int kExitOk = 0;
inline constexpr int kExitUsage = 1;
inline constexpr int kExitInput = 2;
inline constexpr int kExitOutput = 3;
inline constexpr int kExitUnavailable = 4;
inline constexpr int kExitRuntime = 5;
inline constexpr int kExitQcGate = 6;
inline constexpr int kExitInternal = 7;

}  // namespace ac3cli
