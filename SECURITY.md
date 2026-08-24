# Security Policy

## Supported versions

ac3forge is pre-1.0 and under active development. Security fixes are made against the
`main` branch and included in the next release; there are no long-term-support branches
to backport to yet.

## Threat model

Before linking this decoder against input you do not control, read
[docs/threat-model.md](docs/threat-model.md). It states what is treated as untrusted (elementary
streams, EMDF/OAMD/JOC payloads, WAV headers, ADM documents) and what is not, the memory-safety
posture and where the raw-pointer boundaries are (the C API, the WASM bindings, the JNI bridge),
the per-access-unit resource limits and what a hostile `frmsiz` does, and the gaps — including
the ones that are the embedder's to close rather than this project's.

## Reporting a vulnerability

Please report security vulnerabilities privately, not through a public GitHub issue.

Use [GitHub Security Advisories](https://github.com/iainchesworthlabs/ac3forge/security/advisories/new)
to open a private report. This reaches the maintainer directly and lets us coordinate a
fix before any details are made public.

Include, where relevant:

- The affected component (encoder, decoder, CLI, GUI, or a specific input format).
- Steps to reproduce, or a sample stream that triggers the issue.
- The potential impact (crash, memory corruption, incorrect output, etc.).

If you found it while embedding the library, two things speed a fix up most: the input itself
(a fuzzer corpus file is ideal — it becomes a permanent regression case under
`fuzz/regressions/`), and which entry point you called, since the allocating and `_into` decode
forms have different contracts. `ac3cli --version` prints the version, commit and toolchain, which
says whether what you hit is already fixed. See
[docs/threat-model.md](docs/threat-model.md#reporting-an-issue).

We aim to acknowledge new reports within 7 days and to agree a disclosure timeline once
the issue is confirmed.
