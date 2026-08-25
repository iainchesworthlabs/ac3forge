"""Design the 64-band complex QMF's prototype filter and emit it as a header.

TS 103 420 §7.1 says the JOC reconstruction runs in a 64-subband complex
filterbank. It does not print that filterbank's prototype coefficients, and no
public source for Dolby's does; this project is clean-room anyway. So the
prototype here is this project's OWN design, constrained to the structure
src/forge/src/dsp/qmf.cpp implements and to exact perfect reconstruction.

The structure, in the notation qmf.cpp uses: M = 64 subbands, hop M, prototype
length L = 640 = 5 * 2M, fold period 2M = 128 with an alternating sign, and an
odd-stacked (k + 1/2) modulation. Writing p's polyphase components on the
hop-64 lattice as q_j = p[n0 + 64 j], j = 0..9, analysis-then-synthesis
reconstructs the input exactly when, for every n0 in [0, 64),

    sum_j q_j q_j       == 1        (unit gain: no scaling)
    sum_j q_j q_{j+2m}  == 0        for m = 1, 2, 3, 4  (no time aliasing)

- the even-lag autocorrelations of each length-10 coset must vanish. Splitting
q into its even- and odd-indexed halves a and b, that is exactly
|A(w)|^2 + |B(w)|^2 == 1: a and b must be a POWER-COMPLEMENTARY pair. The
classic lossless (orthogonal) lattice produces such a pair from any four
rotation angles plus a leading one, so parametrizing each coset by five angles
puts perfect reconstruction beyond reach of the optimizer to break: it holds
identically, for every angle vector, to machine precision.

That leaves 64 * 5 = 320 free angles and one thing to spend them on -
selectivity. The objective is the prototype's stopband energy beyond
w_s = pi/64 (one subband width from DC; the passband is |w| < pi/128), a
quadratic form p' A p that Adam minimises directly.

w_s is itself the tuned parameter. Sweeping it trades far-stopband depth
against ADJACENT-band leakage, and adjacent-band leakage is what actually
limits a parametric per-band tool. Measured as single-band isolation - a tone
at a subband centre, in-band energy against every other band's - the sweep ran

    w_s = 0.50 * pi/64   ->  18.3 dB          w_s = 1.00 * pi/64  ->  34.7 dB
    w_s = 0.80 * pi/64   ->  24.9 dB          w_s = 1.20 * pi/64  ->  22.7 dB
    w_s = 1.50 * pi/64   ->  21.3 dB          w_s = 3.00 * pi/64  ->   8.8 dB

so pi/64 is a genuine optimum rather than an arbitrary pick, and it is what
WS_OVER_PI below encodes.

Deterministic: fixed initial point, no RNG, so re-running reproduces the
committed header byte for byte. Takes a couple of minutes.

Run from the repo root:  python tools/generators/gen_qmf_prototype.py
"""

from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent.parent
OUT = REPO / "src" / "forge" / "src" / "dsp" / "qmf_prototype.hpp"

M = 64  # subbands
L = 640  # prototype taps
TWO_M = 2 * M  # fold period
TAPS_PER_COSET = L // M  # 10
WS_OVER_PI = 1.0 / M  # stopband edge, in units of pi
ITERATIONS = 6000
LEARNING_RATE = 3e-3


def lattice(theta):
    """theta (..., 5) -> power-complementary pair a, b, each (..., 5)."""
    cos, sin = np.cos(theta), np.sin(theta)
    shape = theta.shape[:-1]
    a = np.zeros((*shape, 5))
    b = np.zeros((*shape, 5))
    a[..., 0] = cos[..., 0]
    b[..., 0] = sin[..., 0]
    for stage in range(1, 5):
        prev_a = a
        delayed_b = np.zeros_like(b)
        delayed_b[..., 1:] = b[..., :-1]
        a = cos[..., stage, None] * prev_a - sin[..., stage, None] * delayed_b
        b = sin[..., stage, None] * prev_a + cos[..., stage, None] * delayed_b
    return a, b


def prototype(theta):
    """theta (64, 5) -> the 640-tap prototype and its (64, 10) cosets."""
    a, b = lattice(theta)
    cosets = np.empty((M, TAPS_PER_COSET))
    cosets[:, 0::2] = a
    cosets[:, 1::2] = b
    taps = np.zeros(L)
    for offset in range(M):
        taps[offset::M] = cosets[offset]
    return taps, cosets


def stopband_form(ws_over_pi):
    """The quadratic form whose value is p's energy beyond w_s."""
    lag = np.arange(L)[None, :] - np.arange(L)[:, None]
    form = -ws_over_pi * np.sinc(ws_over_pi * lag)
    form[lag == 0] += 1.0
    return form


def reconstruction_error(taps):
    """Worst violation of the two perfect-reconstruction conditions."""
    cosets = np.stack([taps[offset::M] for offset in range(M)], axis=0)
    worst = np.abs(np.sum(cosets * cosets, axis=1) - 1.0).max()
    for lag in range(2, TAPS_PER_COSET, 2):
        worst = max(
            worst,
            np.abs(np.sum(cosets[:, : TAPS_PER_COSET - lag] * cosets[:, lag:], axis=1)).max(),
        )
    return worst


def peak_stopband_db(taps, ws_over_pi):
    spectrum = np.abs(np.fft.rfft(taps, 32768))
    spectrum /= spectrum.max()
    freq = np.arange(len(spectrum)) * 2.0 / 32768
    return 20.0 * np.log10(spectrum[freq >= ws_over_pi].max())


def band_isolation_db(taps):
    """A tone at subband 20's centre: in-band energy against all other bands."""
    band = 20
    samples = np.arange(8192)
    tone = np.cos(2.0 * np.pi * ((band + 0.5) / TWO_M) * samples)
    pre = np.exp(-1j * np.pi * np.arange(TWO_M) / TWO_M)
    slots = (len(tone) - L) // M + 1
    energy = np.zeros(M)
    for slot in range(L // M, slots - 1):  # steady state only
        windowed = taps * tone[slot * M : slot * M + L]
        folded = np.zeros(TWO_M)
        for block in range(L // TWO_M):
            folded += ((-1) ** block) * windowed[block * TWO_M : (block + 1) * TWO_M]
        energy += np.abs(np.fft.fft(folded * pre)[:M]) ** 2
    return 10.0 * np.log10(energy[band] / max(energy.sum() - energy[band], 1e-300))


def project_onto_manifold(taps, iterations=300):
    """Min-norm Gauss-Newton onto the PR conditions, to seed the angles."""
    cosets = np.stack([taps[offset::M] for offset in range(M)], axis=0)
    lags = list(range(2, TAPS_PER_COSET, 2))
    for _ in range(iterations):
        rows = [np.sum(cosets * cosets, axis=1) - 1.0]
        for lag in lags:
            rows.append(np.sum(cosets[:, : TAPS_PER_COSET - lag] * cosets[:, lag:], axis=1))
        residual = np.stack(rows, axis=1)
        if np.max(np.abs(residual)) < 1e-15:
            break
        jac = np.zeros((M, residual.shape[1], TAPS_PER_COSET))
        jac[:, 0, :] = 2 * cosets
        for row, lag in enumerate(lags, start=1):
            for tap in range(TAPS_PER_COSET):
                if tap + lag < TAPS_PER_COSET:
                    jac[:, row, tap] += cosets[:, tap + lag]
                if tap - lag >= 0:
                    jac[:, row, tap] += cosets[:, tap - lag]
        gram = jac @ np.transpose(jac, (0, 2, 1)) + 1e-13 * np.eye(residual.shape[1])
        step = np.transpose(jac, (0, 2, 1)) @ np.linalg.solve(gram, residual[:, :, None])
        cosets = cosets - step[:, :, 0]
    out = np.zeros(L)
    for offset in range(M):
        out[offset::M] = cosets[offset]
    return out


def fit_angles(taps):
    """Read the lattice angles back off a prototype already on the manifold."""
    cosets = np.stack([taps[offset::M] for offset in range(M)], axis=0)
    even, odd = cosets[:, 0::2].copy(), cosets[:, 1::2].copy()
    theta = np.zeros((M, 5))
    for stage in range(4, 0, -1):
        angle = np.arctan2(odd[:, 0], even[:, 0])
        theta[:, stage] = angle
        cos, sin = np.cos(angle), np.sin(angle)
        rotated_even = cos[:, None] * even + sin[:, None] * odd
        rotated_odd = -sin[:, None] * even + cos[:, None] * odd
        even = rotated_even
        odd = np.zeros_like(rotated_odd)
        odd[:, :-1] = rotated_odd[:, 1:]
    theta[:, 0] = np.arctan2(odd[:, 0], even[:, 0])
    return theta


def design():
    centred = np.arange(L) - (L - 1) / 2.0
    cutoff = 1.0 / TWO_M
    seed = 2 * cutoff * np.sinc(2 * cutoff * centred) * np.kaiser(L, 9.0)
    seed *= np.sqrt(M / np.sum(seed * seed))  # coset energies -> ~1
    theta = fit_angles(project_onto_manifold(seed))

    form = stopband_form(WS_OVER_PI)
    first, second = np.zeros_like(theta), np.zeros_like(theta)
    taps, _ = prototype(theta)
    best = (peak_stopband_db(taps, WS_OVER_PI), theta.copy())
    for step in range(ITERATIONS):
        taps, cosets = prototype(theta)
        weighted = form @ taps
        by_tap = np.stack(
            [2.0 * weighted[np.arange(M) + M * tap] for tap in range(TAPS_PER_COSET)], axis=1
        )
        grad = np.zeros((M, 5))
        for angle in range(5):
            nudged = theta.copy()
            nudged[:, angle] += 1e-6
            a, b = lattice(nudged)
            moved = np.empty((M, TAPS_PER_COSET))
            moved[:, 0::2] = a
            moved[:, 1::2] = b
            grad[:, angle] = np.sum(by_tap * (moved - cosets) / 1e-6, axis=1)
        first = 0.9 * first + 0.1 * grad
        second = 0.999 * second + 0.001 * grad * grad
        theta = theta - LEARNING_RATE * (first / (1 - 0.9 ** (step + 1))) / (
            np.sqrt(second / (1 - 0.999 ** (step + 1))) + 1e-12
        )
        if step % 200 == 0 or step == ITERATIONS - 1:
            taps, _ = prototype(theta)
            score = peak_stopband_db(taps, WS_OVER_PI)
            if score < best[0]:
                best = (score, theta.copy())
    taps, _ = prototype(best[1])
    return taps


def main():
    taps = design()
    error = reconstruction_error(taps)
    peak = peak_stopband_db(taps, WS_OVER_PI)
    isolation = band_isolation_db(taps)
    # A prototype that misses the PR conditions would still filter plausibly
    # while quietly leaking time-domain aliasing into every reconstruction, so
    # refuse to write one rather than let the C++ identity test find it later.
    if error > 1e-13:
        raise SystemExit(f"perfect-reconstruction conditions violated by {error:.2e}")
    print(f"perfect-reconstruction residual: {error:.2e}")
    print(f"peak stopband beyond pi/64:      {peak:.2f} dB")
    print(f"single-band isolation:           {isolation:.2f} dB")

    lines = [
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstddef>",
        "",
        "// The 64-band complex QMF's prototype filter - 640 taps, 10 per subband.",
        "//",
        "// GENERATED by tools/generators/gen_qmf_prototype.py. Do not edit by hand;",
        "// that script carries the derivation, the design objective and the sweep",
        "// that chose it.",
        "//",
        "// This is NOT Dolby's prototype. TS 103 420 §7.1 fixes the filterbank's",
        "// SHAPE - 64 subbands, complex, odd-stacked - and does not publish its",
        "// coefficients; these are designed here, against that structure, for exact",
        "// perfect reconstruction. Measured on the committed values:",
        f"//     perfect-reconstruction residual   {error:.2e}",
        f"//     peak stopband beyond pi/64        {peak:.2f} dB",
        f"//     single-band isolation             {isolation:.2f} dB",
        "",
        "namespace ac3::dsp {",
        "",
        "inline constexpr std::size_t kQmfPrototypeTaps = 640;",
        "",
        "inline constexpr std::array<double, kQmfPrototypeTaps> kQmfPrototype = {{",
    ]
    for start in range(0, L, 4):
        row = ", ".join(f"{value: .17e}" for value in taps[start : start + 4])
        lines.append(f"    {row},")
    lines += ["}};", "", "}  // namespace ac3::dsp", ""]
    OUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {OUT.relative_to(REPO)}")


if __name__ == "__main__":
    main()
