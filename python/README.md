# ICL Forge

Python bindings for [ICL Forge](https://github.com/iainchesworthlabs/iclforge), a clean-room
AC-3/E-AC-3 (Dolby Digital/Digital Plus) and AC-4 encoder and decoder written in C++23, including
the Atmos-in-DD+ object layer (OAMD + JOC). The AC-4 (ETSI TS 103 190) decoder and encoder, objects
included, are in the `iclforge.ac4` submodule.

```bash
pip install iclforge      # or, from a checkout of the repository: pip install ./python
```

The two pre-releases on PyPI, 0.9.0b1 and 0.10.0b1, are the project `ac3forge`, with the module `ac3forge`.
`iclforge` is the project from the first release made after the rename.

```python
import numpy as np
import iclforge as ac3

encoder = ac3.FrameEncoder(ac3.EncoderConfig(acmod=ac3.Acmod.k2_0, bitrate_kbps=192))
tone = 0.2 * np.sin(2 * np.pi * 440 * np.arange(ac3.SAMPLES_PER_FRAME) / 48000).astype(np.float32)
frame = encoder.encode_frame([tone, tone])

decoder = ac3.FrameDecoder()
decoded = decoder.decode_frame(frame)
print(decoded.channels[0].shape)  # (1536,)
```

`iclforge.ac4` is in a wheel built from a release that has it; the wheels for 0.10.0b1 and earlier
predate it, and `pip install ./python` from a checkout builds it. PyPI carries 0.10.0b1's wheels, as the
project `ac3forge`, for Windows x64, Linux x86_64 and macOS on Apple Silicon; on Linux aarch64 and macOS
Intel, where no release has carried a wheel, install from a checkout.

See [docs/library/python-api.md](https://iainchesworthlabs.github.io/iclforge/library/python-api/)
for the full surface (E-AC-3, Atmos object and AC-4 encode/decode included) and
[the main project README](https://github.com/iainchesworthlabs/iclforge) for what the codec
itself covers. Licensed GPL-3.0-or-later, same as the rest of the project — see
[LICENSE](https://github.com/iainchesworthlabs/iclforge/blob/main/LICENSE).
