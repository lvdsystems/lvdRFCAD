"""Reference data for the rfdata tests (requirement NF-4), computed with scikit-rf.

Writes Touchstone files and JSON files with expected values into
src/rfdata/tests/data/. The C++ tests in src/rfdata/tests compare the lvd RF cad
implementation against them.

Regenerate (Windows example; any platform works the same way):

    python -m venv %TEMP%\\rfdata-venv
    %TEMP%\\rfdata-venv\\Scripts\\python -m pip install scikit-rf numpy scipy
    %TEMP%\\rfdata-venv\\Scripts\\python -I tools\\generate_rf_reference.py

The data was last generated with Python 3.14, scikit-rf 2.2.0, numpy 2.5 and
scipy 1.18. The random generator is seeded, so the output only changes when
this script or scikit-rf changes.

Contents:
  conversions.json   S -> Z, Y (N = 1..4) and ABCD, T (two-ports), non-50 ohm
                     per-port references; renormalisation
  interpolation.json magnitude / unwrapped phase interpolation onto a new grid
  cascade.json       cascade with mismatched junction references, de-embedding
  stability.json     K, Delta, mu, mu', MAG/MSG
  groupdelay.json    unwrapped phase and group delay on an uneven grid
  noise.json         noise figure for several source reflections, cascade of
                     two noisy two-ports (scikit-rf's ABCD correlation cascade)
  lines.json         microstrip (MLine), coax (Coaxial) and CPW (CPW) properties
                     (the CPW conductor loss is corrected for a scikit-rf 2.2
                     bug, see lines())
  touchstone.json    expected content of the Touchstone files below, as read by
                     scikit-rf
  *.s1p, *.s2p, *.s3p, *.s4p, *.ts  Touchstone files in varied formats
  roughness.json     microstrip conductor loss with surface roughness (MLine rough)
  gains.json         Mason's U, GUmax, MAG and noise-figure circles (scikit-rf)
  coupled_microstrip.json  quasi-static even/odd-mode Z0 and effective
                     permittivity of coupled microstrip from a 2D finite-
                     difference Laplace solver in this script (see coupled())
  discontinuities.json  microstrip open end, gap, step, bends, T-junction and
                     via evaluated from the published formulas in Python,
                     independently of the C++ code (see discontinuities())

Sections can be selected on the command line, for example
    python -I tools\\generate_rf_reference.py coupled discontinuities
(roughness, gains, coupled and discontinuities were added with scikit-rf
2.2.0, scipy 1.18.1 and numpy 2.5.5; they use no random numbers).
"""

import json
import math
import sys
import warnings
from pathlib import Path

import numpy as np
import scipy.special
import skrf as rf

OUT = Path(__file__).resolve().parent.parent / "src" / "rfdata" / "tests" / "data"
RNG = np.random.default_rng(20261116)


def c(value):
    value = complex(value)
    return [value.real, value.imag]


def mat(m):
    return [[c(x) for x in row] for row in np.asarray(m)]


def mats(ms):
    return [mat(m) for m in ms]


def write_json(name, data):
    path = OUT / name
    path.write_text(json.dumps(data, indent=None, separators=(",", ":")) + "\n", encoding="utf-8")
    print(f"wrote {path.name} ({path.stat().st_size} bytes)")


def random_passive(n, reciprocal=False, margin=0.9):
    a = RNG.normal(size=(n, n)) + 1j * RNG.normal(size=(n, n))
    if reciprocal:
        a = a + a.T
    return a * (margin / np.linalg.svd(a, compute_uv=False)[0])


def random_z0(n):
    return RNG.uniform(20.0, 120.0, size=n)


def smooth_network(f, n, z0, delays):
    """S(f) = sum of a few damped complex exponentials per element: smooth,
    passive after scaling, phase steps well below 90 degrees between points."""
    s = np.zeros((len(f), n, n), dtype=complex)
    for i in range(n):
        for j in range(n):
            amp = RNG.uniform(0.3, 0.6) * np.array([1.0, 0.15])  # dominant term: never near zero
            tau = RNG.uniform(-1, 1, size=2) * delays
            ph = RNG.uniform(-np.pi, np.pi, size=2)
            for a, t, p in zip(amp, tau, ph):
                s[:, i, j] += a * np.exp(1j * (p - 2 * np.pi * f * t)) * (1 + 0.2 * np.cos(f / f[-1] * 3))
    scale = max(np.linalg.svd(m, compute_uv=False)[0] for m in s)
    s *= 0.95 / scale
    return rf.Network(frequency=rf.Frequency.from_f(f, unit="Hz"), s=s, z0=z0)


# ---------------------------------------------------------------------------
def conversions():
    cases = []
    for n in (1, 2, 3, 4):
        for reciprocal in (False, True):
            s = random_passive(n, reciprocal)
            z0 = random_z0(n)
            z0_new = random_z0(n)
            s3 = s[None, :, :]
            case = {
                "z0": list(z0),
                "s": mat(s),
                "z": mat(rf.network.s2z(s3, z0)[0]),
                "y": mat(rf.network.s2y(s3, z0)[0]),
                "z0New": list(z0_new),
                "sRenormalized": mat(rf.network.renormalize_s(s3, z0, z0_new, "power", "power")[0]),
            }
            if n == 2:
                case["abcd"] = mat(rf.network.s2a(s3, z0)[0])
                case["t"] = mat(rf.network.s2t(s3)[0])
            cases.append(case)
    write_json("conversions.json", {"cases": cases})


def interpolation():
    cases = []
    for n, z0 in ((2, [50.0, 50.0]), (3, [50.0, 75.0, 35.0])):
        f = np.sort(np.concatenate([np.linspace(1e9, 10e9, 31), RNG.uniform(1e9, 10e9, 6)]))
        f = np.unique(np.round(f, -3))
        ntwk = smooth_network(f, n, z0, 0.4e-9)
        f_new = np.unique(np.concatenate([RNG.uniform(1e9, 10e9, 40), [1e9, 10e9]]))
        ntwk_new = ntwk.interpolate(rf.Frequency.from_f(f_new, unit="Hz"), basis="s", coords="polar", kind="linear")
        # The C++ implementation switches to real/imaginary interpolation for
        # phase steps beyond 90 degrees; make sure the data does not contain any.
        steps = np.abs(np.angle(ntwk.s[1:] / ntwk.s[:-1]))
        assert steps.max() < np.pi / 2, steps.max()
        cases.append({
            "z0": z0,
            "frequencies": list(f),
            "s": mats(ntwk.s),
            "newFrequencies": list(f_new),
            "sInterpolated": mats(ntwk_new.s),
        })
    write_json("interpolation.json", {"cases": cases})


def two_port(f, z0, gain=1.0):
    s = np.array([random_passive(2) for _ in f])
    s[:, 1, 0] *= gain
    return rf.Network(frequency=rf.Frequency.from_f(f, unit="Hz"), s=s, z0=z0)


def cascade_and_deembed():
    f = np.linspace(1e9, 5e9, 9)
    cases = []
    for za, zb in (([50.0, 50.0], [50.0, 50.0]), ([50.0, 35.0], [60.0, 75.0]), ([25.0, 80.0], [80.0, 40.0])):
        a = two_port(f, za)
        b = two_port(f, zb)
        ab = a ** b
        assert np.allclose(ab.z0, [za[0], zb[1]])
        cases.append({"f": list(f), "a": mats(a.s), "za": za, "b": mats(b.s), "zb": zb,
                      "ab": mats(ab.s), "zab": [za[0], zb[1]]})

    deembed_cases = []
    for zl, zd, zr in (([50.0, 50.0], [50.0, 50.0], [50.0, 50.0]), ([50.0, 30.0], [30.0, 70.0], [70.0, 50.0])):
        left = two_port(f, zl)
        dut = two_port(f, zd)
        right = two_port(f, zr)
        measured = left ** dut ** right
        recovered = left.inv ** measured ** right.inv
        assert np.allclose(recovered.s, dut.s, atol=1e-9)
        deembed_cases.append({"f": list(f), "left": mats(left.s), "zl": zl, "right": mats(right.s), "zr": zr,
                              "measured": mats(measured.s), "zm": [zl[0], zr[1]],
                              "dut": mats(recovered.s), "zd": zd})
    write_json("cascade.json", {"cascade": cases, "deembed": deembed_cases})


def stability():
    cases = []
    for k in range(12):
        s = random_passive(2, margin=RNG.uniform(0.5, 0.95))
        s[1, 0] *= RNG.uniform(1.0, 8.0)  # amplifier-like gain
        if k % 4 == 0:
            s[0, 1] *= 0.05  # nearly unilateral, usually K > 1
        n = rf.Network(frequency=rf.Frequency.from_f([1e9], unit="Hz"), s=s[None], z0=50)
        delta = s[0, 0] * s[1, 1] - s[0, 1] * s[1, 0]
        mu = (1 - abs(s[0, 0]) ** 2) / (abs(s[1, 1] - delta * np.conj(s[0, 0])) + abs(s[0, 1] * s[1, 0]))
        mu_p = (1 - abs(s[1, 1]) ** 2) / (abs(s[0, 0] - delta * np.conj(s[1, 1])) + abs(s[0, 1] * s[1, 0]))
        cases.append({"s": mat(s), "k": float(n.stability[0]), "delta": c(delta), "mu": float(mu),
                      "muPrime": float(mu_p), "magDb": float(10 * np.log10(n.max_gain[0]))})
    write_json("stability.json", {"cases": cases})


def group_delay():
    f = np.unique(np.concatenate([np.linspace(0.5e9, 6e9, 100), RNG.uniform(0.5e9, 6e9, 15)]))
    ntwk = smooth_network(f, 2, [50.0, 50.0], 1.5e-9)
    # an electrical length adds a large, known delay with several phase wraps
    ntwk.s[:, 1, 0] *= np.exp(-2j * np.pi * f * 2.5e-9)
    write_json("groupdelay.json", {
        "frequencies": list(f), "s": mats(ntwk.s),
        "phase21": list(ntwk.s_rad_unwrap[:, 1, 0]), "delay21": list(ntwk.group_delay[:, 1, 0].real),
        "phase11": list(ntwk.s_rad_unwrap[:, 0, 0]), "delay11": list(ntwk.group_delay[:, 0, 0].real),
    })


def noisy_amp(f, s, nfmin_db, gamma_opt, rn):
    n = rf.Network(frequency=rf.Frequency.from_f(f, unit="Hz"), s=s, z0=50)
    n.set_noise_a(rf.Frequency.from_f(f, unit="Hz"), nfmin_db=nfmin_db, gamma_opt=gamma_opt, rn=rn)
    return n


def noise():
    f = np.array([1e9, 2e9, 4e9])
    s_a = np.array([[[0.6 * np.exp(-1j * (1.0 + 0.3 * k)), 0.05 * np.exp(1j * 0.5)],
                     [4.0 * np.exp(1j * (2.0 - 0.4 * k)), 0.5 * np.exp(-1j * (0.7 + 0.2 * k))]] for k in range(3)])
    s_b = np.array([[[0.3 * np.exp(1j * 0.4), 0.1 * np.exp(-1j * 0.2)],
                     [2.5 * np.exp(1j * (1.0 - 0.3 * k)), 0.4 * np.exp(1j * 1.1)]] for k in range(3)])
    a = noisy_amp(f, s_a, np.array([0.8, 1.0, 1.3]), np.array([0.5 * np.exp(1j * 0.9), 0.45 * np.exp(1j * 1.4),
                                                                0.4 * np.exp(1j * 2.0)]), np.array([12.0, 10.0, 8.0]))
    b = noisy_amp(f, s_b, np.array([2.0, 2.2, 2.5]), np.array([0.3 * np.exp(-1j * 0.5), 0.35 * np.exp(-1j * 0.2),
                                                                0.38 * np.exp(1j * 0.3)]), np.array([25.0, 22.0, 20.0]))
    ab = a ** b

    def params(n):
        return {"fminDb": list(n.nfmin_db), "gammaOpt": [c(x) for x in n.g_opt], "rn": list(n.rn)}

    gammas = [0, 0.3, 0.5j, -0.4 + 0.2j, 0.7 * np.exp(1j * 2.5), 0.45 * np.exp(1j * 0.9)]
    nf = []
    for g in gammas:
        z = 50 * (1 + g) / (1 - g)
        nf.append({"gammaS": c(g), "nfDb": list(10 * np.log10(np.real(a.nf(z))))})
    write_json("noise.json", {"frequencies": list(f), "z0": 50.0,
                              "a": {"s": mats(a.s), **params(a)}, "b": {"s": mats(b.s), **params(b)},
                              "ab": {"s": mats(ab.s), **params(ab)}, "noiseFigure": nf})


def lines():
    # scikit-rf warns when t < 3 skin depths; the formulas are evaluated regardless
    warnings.simplefilter("ignore", RuntimeWarning)
    freqs = np.array([0.1e9, 1e9, 5e9, 10e9, 20e9, 40e9])
    fr = rf.Frequency.from_f(freqs, unit="Hz")
    micro = []
    for w, h, t, er, tand, sigma in ((1.8e-3, 1.0e-3, 35e-6, 4.4, 0.02, 5.8e7),
                                     (0.2e-3, 0.635e-3, 5e-6, 9.8, 0.0005, 4.1e7),
                                     (3.0e-3, 0.508e-3, 17e-6, 3.0, 0.002, 5.8e7),
                                     (0.6e-3, 0.254e-3, 18e-6, 2.2, 0.0009, 5.8e7)):
        kw = dict(w=w, h=h, t=t, ep_r=er, rho=1 / sigma, rough=0, model="hammerstadjensen",
                  disp="kirschningjansen", diel="frequencyinvariant")
        lossless = rf.media.MLine(fr, tand=0, **kw)
        lossy = rf.media.MLine(fr, tand=tand, **kw)
        micro.append({"w": w, "h": h, "t": t, "er": er, "tand": tand, "sigma": sigma, "frequencies": list(freqs),
                      "z0": list(np.real(lossless.z0_characteristic)), "eeff": list(np.real(lossless.ep_reff_f)),
                      "alphaC": list(lossless.alpha_conductor), "alphaD": list(np.real(lossy.alpha_dielectric))})

    coax = []
    for din, dout, er, tand, sigma in ((0.91e-3, 2.95e-3, 2.1, 3e-4, 5.8e7),
                                       (3.04e-3, 7.0e-3, 1.0, 0.0, 5.8e7),
                                       (0.5e-3, 1.68e-3, 2.2, 2e-3, 1.5e7)):
        m = rf.media.Coaxial(fr, Dint=din, Dout=dout, epsilon_r=er, tan_delta=tand, sigma=sigma)
        coax.append({"din": din, "dout": dout, "er": er, "tand": tand, "sigma": sigma, "frequencies": list(freqs),
                     "zc": [c(x) for x in m.z0_characteristic], "gamma": [c(x) for x in m.gamma]})

    cpw = []
    for w, s, h, t, er, tand, sigma, backed in ((0.6e-3, 0.3e-3, 1.0e-3, 35e-6, 4.4, 0.02, 5.8e7, False),
                                                (0.6e-3, 0.3e-3, 1.0e-3, 35e-6, 4.4, 0.02, 5.8e7, True),
                                                (50e-6, 30e-6, 0.4e-3, 1e-6, 9.8, 0.0005, 4.1e7, False),
                                                (0.25e-3, 0.15e-3, 0.2e-3, 18e-6, 3.0, 0.002, 5.8e7, True)):
        kw = dict(w=w, s=s, h=h, t=t, ep_r=er, rho=1 / sigma, has_metal_backside=backed, diel="frequencyinvariant")
        lossless = rf.media.CPW(fr, tand=0, **kw)
        lossy = rf.media.CPW(fr, tand=tand, **kw)
        # scikit-rf 2.2 evaluates K(k1) K'(k1) in the conductor loss as
        # scipy.special.ellipk(k1) * ellipk(sqrt(1 - k1^2)), but ellipk takes
        # the parameter m = k^2. The expected value is corrected for that.
        k1 = w / (w + 2 * s)
        as_skrf = scipy.special.ellipk(k1) * scipy.special.ellipk(math.sqrt(1 - k1 * k1))
        correct = scipy.special.ellipk(k1 * k1) * scipy.special.ellipk(1 - k1 * k1)
        cpw.append({"w": w, "s": s, "h": h, "t": t, "er": er, "tand": tand, "sigma": sigma, "backed": backed,
                    "frequencies": list(freqs), "z0": list(np.real(lossless.z0_characteristic)),
                    "eeff": list(np.real(lossless.ep_reff_f)),
                    "alphaC": list(lossless.alpha_conductor * as_skrf / correct),
                    "alphaCScikitRf": list(lossless.alpha_conductor),
                    "alphaD": list(np.real(lossy.alpha_dielectric))})
    write_json("lines.json", {"microstrip": micro, "coax": coax, "cpw": cpw})


# ---------------------------------------------------------------------------
# Touchstone files. They are formatted here (to control the layout) and read
# back with scikit-rf, whose result is the expected content.

def fmt(x):
    return f"{x:.10g}"


def pair(value, form):
    if form == "RI":
        return f"{fmt(value.real)} {fmt(value.imag)}"
    if form == "MA":
        return f"{fmt(abs(value))} {fmt(math.degrees(np.angle(value)))}"
    return f"{fmt(20 * math.log10(abs(value)))} {fmt(math.degrees(np.angle(value)))}"


def to_y(s, z0):
    return rf.network.s2y(s[None], z0)[0]


def to_z(s, z0):
    return rf.network.s2z(s[None], z0)[0]


def touchstone_files():
    expected = {}

    def record(name, source, extra=None):
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            n = rf.Network(str(OUT / name))
        # scikit-rf must read back what was written (10 significant digits)
        assert np.allclose(n.s, np.asarray(source).reshape(n.s.shape), rtol=0, atol=1e-7), name
        entry = {"frequencies": list(n.f), "z0": list(np.real(n.z0[0])), "s": mats(n.s)}
        if extra:
            entry.update(extra)
        expected[name] = entry
        return n

    # 1-port, version 1, MHz, dB/angle, comments and a blank line
    f = np.linspace(100, 1000, 10)
    s = [0.8 * np.exp(-1j * (0.002 * x)) * (1 - 0.0003 * x) for x in f]
    lines = ["! one-port test file", "!  second comment line", "", "# MHz S DB R 50"]
    lines += [f"{fmt(x)} {pair(v, 'DB')}" for x, v in zip(f, s)]
    (OUT / "one_port.s1p").write_text("\n".join(lines) + "\n", encoding="utf-8")
    record("one_port.s1p", s)

    # 2-port with noise data, version 1, GHz, magnitude/angle, N21 before N12
    f = np.linspace(1, 6, 11)
    lines = ["! amplifier with noise parameters", "# GHz S MA R 50"]
    source = []
    for x in f:
        m = [[0.7 * np.exp(-1j * (0.6 * x)), 0.04 * np.exp(1j * (0.9 - 0.1 * x))],
             [5.0 / x * np.exp(1j * (2.4 - 0.5 * x)), 0.45 * np.exp(-1j * (0.3 * x))]]
        source.append(m)
        lines.append(f"{fmt(x)} {pair(m[0][0], 'MA')} {pair(m[1][0], 'MA')} {pair(m[0][1], 'MA')} {pair(m[1][1], 'MA')}"
                     "  ! inline comment")
    lines.append("! noise parameters")
    noise_rows = []
    for x in (1.0, 2.0, 3.5, 5.0):
        row = [x, 0.5 + 0.12 * x, 0.6 - 0.05 * x, 40 + 25 * x, 0.35 - 0.03 * x]
        noise_rows.append(row)
        lines.append(" ".join(fmt(v) for v in row))
    (OUT / "amp_noise.s2p").write_text("\n".join(lines) + "\n", encoding="utf-8")
    t = rf.io.touchstone.Touchstone(str(OUT / "amp_noise.s2p"))
    # scikit-rf's raw noise table: frequency in Hz, Rn still normalised to R
    noise = [{"frequency": r[0], "fminDb": r[1], "gammaOpt": c(r[2] * np.exp(1j * math.radians(r[3]))),
              "rn": r[4] * 50.0} for r in t.noise]
    assert len(noise) == len(noise_rows)
    record("amp_noise.s2p", source, {"noise": noise})

    # 3-port, version 1, Z-parameters (normalised to R = 75), Hz, real/imaginary
    f = np.array([1e8, 2e8, 3e8, 5e8, 8e8])
    lines = ["# Hz Z RI R 75"]
    source = []
    for x in f:
        s = random_passive(3, reciprocal=True)
        source.append(s)
        z = to_z(s, [75.0] * 3) / 75.0
        for i in range(3):
            prefix = fmt(x) if i == 0 else " " * len(fmt(x))
            lines.append(prefix + " " + " ".join(pair(z[i, j], "RI") for j in range(3)))
    (OUT / "three_port.s3p").write_text("\n".join(lines) + "\n", encoding="utf-8")
    record("three_port.s3p", source)

    # 4-port, version 1, kHz, dB/angle, one matrix row per line
    f = np.array([1e6, 1.5e6, 2e6, 3e6])
    lines = ["! four-port", "# kHz S DB R 50"]
    source = []
    for x in f:
        s = random_passive(4)
        source.append(s)
        for i in range(4):
            prefix = fmt(x) if i == 0 else "    "
            lines.append(prefix + " " + " ".join(pair(s[i, j], "DB") for j in range(4)))
    (OUT / "four_port.s4p").write_text("\n".join(lines) + "\n", encoding="utf-8")
    record("four_port.s4p", source)

    # Version 2 two-port with per-port references, 21_12 order and noise data (Rn in ohm)
    f = np.linspace(2, 4, 5)
    lines = ["! version 2 two-port", "[Version] 2.0", "# GHz S RI R 50", "[Number of Ports] 2",
             "[Two-Port Data Order] 21_12", "[Number of Frequencies] 5", "[Number of Noise Frequencies] 3",
             "[Reference] 50 75", "[Network Data]"]
    source = []
    for x in f:
        s = random_passive(2)
        source.append(s)
        lines.append(f"{fmt(x)} {pair(s[0, 0], 'RI')} {pair(s[1, 0], 'RI')} {pair(s[0, 1], 'RI')} {pair(s[1, 1], 'RI')}")
    lines.append("[Noise Data]")
    noise = []
    for x in (2.0, 3.0, 4.0):
        row = [x, 0.9 + 0.1 * x, 0.4, 60 + 10 * x, 14.0 - x]
        lines.append(" ".join(fmt(v) for v in row))
        noise.append({"frequency": x * 1e9, "fminDb": row[1], "gammaOpt": c(row[2] * np.exp(1j * math.radians(row[3]))),
                      "rn": row[4]})
    lines.append("[End]")
    (OUT / "two_port_v2.ts").write_text("\n".join(lines) + "\n", encoding="utf-8")
    record("two_port_v2.ts", source, {"noise": noise})

    # Version 2 three-port, Y-parameters (not normalised in version 2), per-port
    # references, upper-triangular matrix format with the rows wrapped
    z0 = [50.0, 25.0, 100.0]
    lines = ["[Version] 2.0", "# MHz Y MA", "[Number of Ports] 3", "[Number of Frequencies] 3",
             "[Reference] 50 25", "100", "[Matrix Format] Upper", "[Network Data]"]
    source = []
    for x in (900.0, 1800.0, 2400.0):
        s = random_passive(3, reciprocal=True)
        source.append(s)
        y = to_y(s, z0)
        lines.append(f"{fmt(x)} {pair(y[0, 0], 'MA')} {pair(y[0, 1], 'MA')} {pair(y[0, 2], 'MA')}")
        lines.append(f"  {pair(y[1, 1], 'MA')} {pair(y[1, 2], 'MA')}")
        lines.append(f"  {pair(y[2, 2], 'MA')}")
    lines.append("[End]")
    (OUT / "three_port_v2.ts").write_text("\n".join(lines) + "\n", encoding="utf-8")
    record("three_port_v2.ts", source)

    write_json("touchstone.json", expected)


# ---------------------------------------------------------------------------
# Conductor roughness: scikit-rf's MLine with rough > 0 (Hammerstad-Jensen
# roughness factor on the conductor loss).

def roughness():
    warnings.simplefilter("ignore", RuntimeWarning)
    freqs = np.array([0.5e9, 2e9, 5e9, 10e9, 30e9])
    fr = rf.Frequency.from_f(freqs, unit="Hz")
    cases = []
    for w, h, t, er, sigma, rough in ((1.1e-3, 0.508e-3, 35e-6, 3.55, 5.8e7, 1.0e-6),
                                      (1.8e-3, 1.0e-3, 35e-6, 4.4, 5.8e7, 3.0e-6),
                                      (0.3e-3, 0.254e-3, 17e-6, 2.2, 5.8e7, 0.4e-6)):
        m = rf.media.MLine(fr, w=w, h=h, t=t, ep_r=er, rho=1 / sigma, rough=rough, tand=0,
                           model="hammerstadjensen", disp="kirschningjansen", diel="frequencyinvariant")
        cases.append({"w": w, "h": h, "t": t, "er": er, "sigma": sigma, "rough": rough, "frequencies": list(freqs),
                      "alphaC": list(m.alpha_conductor)})
    write_json("roughness.json", {"microstrip": cases})


# ---------------------------------------------------------------------------
# Gains and design circles. Mason's U and the noise-figure circles come from
# scikit-rf (Network.unilateral_gain, Network.nf_circle); the circles' centre
# and radius are recovered from scikit-rf's points.

def gains():
    f = np.array([1e9])
    cases = []
    s_list = [
        np.array([[0.6 * np.exp(-1j * 2.6), 0.05 * np.exp(1j * 0.4)], [3.2 * np.exp(1j * 1.4), 0.45 * np.exp(-1j * 0.8)]]),
        np.array([[0.7 * np.exp(-1j * 1.9), 0.11 * np.exp(1j * 0.6)], [5.0 * np.exp(1j * 1.9), 0.6 * np.exp(-1j * 0.5)]]),
        np.array([[0.3 * np.exp(1j * 2.2), 0.02 * np.exp(-1j * 0.1)], [1.8 * np.exp(-1j * 0.3), 0.2 * np.exp(1j * 2.9)]]),
    ]
    for s in s_list:
        n = rf.Network(frequency=rf.Frequency.from_f(f, unit="Hz"), s=s[None], z0=50)
        n.set_noise_a(rf.Frequency.from_f(f, unit="Hz"), nfmin_db=np.array([0.9]),
                      gamma_opt=np.array([0.55 * np.exp(1j * 1.2)]), rn=np.array([9.0]))
        circles = []
        for nf_db in (1.2, 2.0, 3.5):
            pts = n.nf_circle(nf_db, npoints=721)[:-1, 0]
            centre = np.mean(pts)
            circles.append({"nfDb": nf_db, "centre": c(centre), "radius": float(np.mean(np.abs(pts - centre)))})
        guMax = np.abs(s[1, 0]) ** 2 / ((1 - np.abs(s[0, 0]) ** 2) * (1 - np.abs(s[1, 1]) ** 2))
        cases.append({"s": mat(s), "uDb": float(10 * np.log10(np.real(n.unilateral_gain[0]))),
                      "guMaxDb": float(10 * np.log10(guMax)), "magDb": float(10 * np.log10(n.max_gain[0])),
                      "k": float(n.stability[0]), "fminDb": 0.9, "gammaOpt": c(0.55 * np.exp(1j * 1.2)), "rn": 9.0,
                      "nfCircles": circles})
    write_json("gains.json", {"cases": cases})


# ---------------------------------------------------------------------------
# Coupled microstrip: a 2D finite-difference (finite-volume) Laplace solver
# for the quasi-static even- and odd-mode capacitances of two zero-thickness
# strips, with and without the dielectric. Half domain with a symmetry plane
# (Neumann for the even, Dirichlet for the odd mode), grounded box 100 h
# away, graded grid with spacing h/500 and h/1000 at the strip edges and the
# dielectric interface; Richardson extrapolation over the two grids (the
# error is first order in the edge spacing because of the edge singularity).
# The single microstrip solved the same way agrees with Hammerstad-Jensen
# (stated accuracy 0.2 % / 0.01 %) to about 0.1 %.

EPS0 = 1 / (1.25663706127e-6 * 299792458.0 ** 2)


def graded_grid(keys, dmin, ratio, dmax):
    keys = sorted(set(keys))
    pts = [keys[0]]
    for a, b in zip(keys[:-1], keys[1:]):
        left, right, d = [a], [b], dmin
        while left[-1] + d < right[-1] - d:
            left.append(left[-1] + d)
            right.append(right[-1] - d)
            d = min(d * ratio, dmax)
        pts.extend((left + right[::-1])[1:])
    return np.array(sorted(set(np.round(pts, 15))))


def laplace_capacitance(x, y, er, h, strip, dirichlet):
    import scipy.sparse as sparse
    import scipy.sparse.linalg as sparse_linalg
    nx, ny = len(x), len(y)
    jh = int(np.argmin(np.abs(y - h)))
    eps_cell = np.where(0.5 * (y[:-1] + y[1:]) < h, er, 1.0)
    dx, dy = np.diff(x), np.diff(y)
    index = np.arange(nx * ny).reshape(nx, ny)
    fixed = np.zeros(nx * ny, dtype=bool)
    value = np.zeros(nx * ny)
    fixed[index[:, 0]] = fixed[index[:, -1]] = fixed[index[-1, :]] = True
    if dirichlet:
        fixed[index[0, :]] = True
    on = (x >= strip[0] - 1e-15) & (x <= strip[1] + 1e-15)
    fixed[index[on, jh]] = True
    value[index[on, jh]] = 1.0
    # horizontal edges (i, j)-(i+1, j): cells below and above
    below = np.concatenate([[0.0], eps_cell * dy / 2])
    above = np.concatenate([eps_cell * dy / 2, [0.0]])
    ch = ((below + above)[None, :] / dx[:, None]).ravel()
    ph, qh = index[:-1, :].ravel(), index[1:, :].ravel()
    # vertical edges (i, j)-(i, j+1): cells left and right
    left = np.concatenate([[0.0], dx / 2])
    right = np.concatenate([dx / 2, [0.0]])
    cv = ((left + right)[:, None] * eps_cell[None, :] / dy[None, :]).ravel()
    pv, qv = index[:, :-1].ravel(), index[:, 1:].ravel()
    p, q, cc = np.concatenate([ph, pv]), np.concatenate([qh, qv]), np.concatenate([ch, cv])
    n = nx * ny
    a = sparse.coo_matrix((np.concatenate([cc, cc, -cc, -cc]),
                           (np.concatenate([p, q, p, q]), np.concatenate([p, q, q, p]))), shape=(n, n)).tocsr()
    free = ~fixed
    v = value.copy()
    v[free] = sparse_linalg.spsolve(a[free][:, free].tocsc(), -a[free][:, fixed] @ value[fixed])
    return EPS0 * float(np.sum(cc * (v[p] - v[q]) ** 2))


def quasi_static_mode(w, s, h, er, mode):
    def at(dmin):
        box = 100.0
        if mode == "single":
            strip, xkeys, dirichlet = (0.0, w / 2), [0.0, w / 2, w / 2 + box * h], False
        else:
            strip, xkeys, dirichlet = (s / 2, s / 2 + w), [0.0, s / 2, s / 2 + w, s / 2 + w + box * h], mode == "odd"
        x = graded_grid(xkeys, dmin * h, 1.12, h)
        y = graded_grid([0.0, h, box * h], dmin * h, 1.12, h)
        scale = 2.0 if mode == "single" else 1.0
        return (scale * laplace_capacitance(x, y, er, h, strip, dirichlet),
                scale * laplace_capacitance(x, y, 1.0, h, strip, dirichlet))
    c1, a1 = at(0.002)
    c2, a2 = at(0.001)
    cap, air = 2 * c2 - c1, 2 * a2 - a1
    return 1 / (299792458.0 * math.sqrt(cap * air)), cap / air


def coupled():
    cases = []
    for w, s, h, er in ((1.0e-3, 0.5e-3, 1.0e-3, 9.8), (0.5e-3, 0.2e-3, 1.0e-3, 9.8), (2.0e-3, 1.0e-3, 1.0e-3, 4.4),
                        (1.0e-3, 2.0e-3, 1.0e-3, 2.2), (0.3e-3, 0.1e-3, 1.0e-3, 12.9), (3.0e-3, 0.3e-3, 1.0e-3, 3.55)):
        ze, ee = quasi_static_mode(w, s, h, er, "even")
        zo, eo = quasi_static_mode(w, s, h, er, "odd")
        cases.append({"w": w, "s": s, "h": h, "er": er, "z0e": ze, "eeffEven": ee, "z0o": zo, "eeffOdd": eo})
        print(f"  coupled w/h {w / h:g} s/h {s / h:g} er {er}: Z0e {ze:.3f} ee {ee:.4f} Z0o {zo:.3f} eo {eo:.4f}")
    singles = []
    for w, h, er in ((1.0e-3, 1.0e-3, 9.8), (0.3e-3, 1.0e-3, 4.4)):
        z, e = quasi_static_mode(w, 0.0, h, er, "single")
        singles.append({"w": w, "h": h, "er": er, "z0": z, "eeff": e})
    write_json("coupled_microstrip.json", {"coupled": cases, "single": singles})


# ---------------------------------------------------------------------------
# Microstrip discontinuities: the published closed formulas (references in
# src/rfdata/Discontinuities.cpp) evaluated here independently of the C++
# code, with the line impedance and effective permittivity from scikit-rf's
# MLine (the same Hammerstad-Jensen / Kirschning-Jansen model, checked in
# lines.json). The T-junction is assembled with scikit-rf's connect().

def _mline(fr, w, h, t, er):
    return rf.media.MLine(fr, w=w, h=h, t=t, ep_r=er, rho=1e-30, rough=0, tand=0,
                          model="hammerstadjensen", disp="kirschningjansen", diel="frequencyinvariant")


def _line_z_e(fr, w, h, t, er):
    m = _mline(fr, w, h, t, er)
    return np.real(m.z0_characteristic), np.real(m.ep_reff_f)


def _open_end_c(fr, w, h, t, er):
    z, e = _line_z_e(fr, w, h, t, er)
    u = w / h
    q1 = 0.434907 * (e ** 0.81 + 0.26) / (e ** 0.81 - 0.189) * (u ** 0.8544 + 0.236) / (u ** 0.8544 + 0.87)
    q2 = 1 + u ** 0.371 / (2.358 * er + 1)
    q3 = 1 + 0.5274 * np.arctan(0.084 * u ** (1.9413 / q2)) / e ** 0.9236
    q4 = 1 + 0.0377 * np.arctan(0.067 * u ** 1.456) * (6 - 5 * np.exp(0.036 * (1 - er)))
    q5 = 1 - 0.218 * np.exp(-7.5 * u)
    dl = h * q1 * q3 * q5 / q4
    return dl, dl * np.sqrt(e) / (299792458.0 * z)


def discontinuities():
    warnings.simplefilter("ignore", RuntimeWarning)
    freqs = np.array([1e9, 5e9, 10e9])
    fr = rf.Frequency.from_f(freqs, unit="Hz")
    h, t, er = 0.635e-3, 5e-6, 9.8
    out = {"h": h, "t": t, "er": er, "frequencies": list(freqs)}

    out["open"] = []
    for w in (0.2e-3, 0.6e-3, 1.5e-3):
        dl, cap = _open_end_c(fr, w, h, t, er)
        out["open"].append({"w": w, "dl": list(dl), "c": list(cap)})

    out["gap"] = []
    for w1, w2, s in ((0.6e-3, 0.6e-3, 0.1e-3), (0.6e-3, 1.2e-3, 0.2e-3), (1.0e-3, 0.4e-3, 0.05e-3)):
        narrow, wide = min(w1, w2), max(w1, w2)
        u1, r, g = narrow / h, wide / narrow, s / h
        q5 = 1.23 / (1 + 0.12 * (r - 1) ** 0.9)
        q1 = 0.04598 * (0.03 + u1 ** q5) * (0.272 + 0.07 * er)
        q2 = 0.107 * (u1 + 9) * g ** 3.23 + 2.09 * g ** 1.05 * (1.5 + 0.3 * u1) / (1 + 0.6 * u1)
        q3 = np.exp(-0.5978 * r ** 1.35) - 0.55
        q4 = np.exp(-0.5978 * r ** -1.35) - 0.55
        cs = 500e-12 * h * np.exp(-1.86 * g) * q1 * (1 + 4.19 * (1 - np.exp(-0.785 * np.sqrt(1 / u1) * r)))
        cn = _open_end_c(fr, narrow, h, t, er)[1] * (q2 + q3) / (q2 + 1)
        cw = _open_end_c(fr, wide, h, t, er)[1] * (q2 + q4) / (q2 + 1)
        c1, c2 = (cn, cw) if w1 <= w2 else (cw, cn)
        out["gap"].append({"w1": w1, "w2": w2, "s": s, "cs": float(cs), "c1": list(c1), "c2": list(c2)})

    out["step"] = []
    for w1, w2 in ((1.2e-3, 0.4e-3), (0.3e-3, 0.9e-3)):
        r = max(w1, w2) / min(w1, w2)
        le = np.log10(er)
        cs = max(0.0, np.sqrt(w1 * w2) * (r * (10.1 * le + 2.33) - 12.6 * le - 3.17) * 1e-12)
        ls = h * (40.5 * (r - 1) - 75 * np.log10(r) + 0.2 * (r - 1) ** 2) * 1e-9
        z1, e1 = _line_z_e(fr, w1, h, t, er)
        z2, e2 = _line_z_e(fr, w2, h, t, er)
        lw1, lw2 = z1 * np.sqrt(e1), z2 * np.sqrt(e2)
        out["step"].append({"w1": w1, "w2": w2, "c": cs, "l1": list(ls * lw1 / (lw1 + lw2)),
                            "l2": list(ls * lw2 / (lw1 + lw2))})

    out["bend"] = []
    for w in (0.3e-3, 0.6e-3, 2.0e-3):
        u = w / h
        out["bend"].append({"w": w,
                            "c": w * ((10.35 * er + 2.5) * u + (2.6 * er + 5.64)) * 1e-12,
                            "l": 220 * h * (1 - 1.35 * np.exp(-0.18 * u ** 1.39)) * 1e-9,
                            "cMitred": w * ((3.93 * er + 0.62) * u + (7.6 * er + 3.80)) * 1e-12,
                            "lMitred": 440 * h * (1 - 1.062 * np.exp(-0.177 * u ** 0.947)) * 1e-9})

    eta0 = 376.730313412
    out["tee"] = []
    for wa, wb, w2 in ((0.6e-3, 0.6e-3, 0.6e-3), (0.6e-3, 1.0e-3, 0.3e-3)):
        smat = []
        for k, f in enumerate(freqs):
            one = rf.Frequency.from_f([f], unit="Hz")
            za, ea = (x[0] for x in _line_z_e(one, wa, h, t, er))
            zb, eb = (x[0] for x in _line_z_e(one, wb, h, t, er))
            z2, e2 = (x[0] for x in _line_z_e(one, w2, h, t, er))
            da, db, d2w = eta0 / za * h / np.sqrt(ea), eta0 / zb * h / np.sqrt(eb), eta0 / z2 * h / np.sqrt(e2)
            fpa, fpb = 0.4e6 * za / h, 0.4e6 * zb / h
            lda, ldb = 299792458.0 / np.sqrt(ea) / f, 299792458.0 / np.sqrt(eb) / f
            dpa = 0.055 * d2w * za / z2 * (1 - 2 * za / z2 * (f / fpa) ** 2)
            dpb = 0.055 * d2w * zb / z2 * (1 - 2 * zb / z2 * (f / fpb) ** 2)
            la, lb = 0.5 * w2 - dpa, 0.5 * w2 - dpb
            r = np.sqrt(za * zb) / z2
            q = f * f / fpa / fpb
            dp2 = np.sqrt(da * db) * (0.5 - r * (0.05 + 0.7 * np.exp(-1.6 * r) + 0.25 * r * q - 0.17 * np.log(r)))
            l2 = 0.5 * max(wa, wb) - dp2
            ta2 = max(1e-30, 1 - np.pi * (f / fpa) ** 2 * ((za / z2) ** 2 / 12 + (0.5 - dp2 / da) ** 2))
            tb2 = max(1e-30, 1 - np.pi * (f / fpb) ** 2 * ((zb / z2) ** 2 / 12 + (0.5 - dp2 / db) ** 2))
            bt = (5.5 * np.sqrt(da * db / lda / ldb) * (er + 2) / er / z2 / np.sqrt(ta2 * tb2) * np.sqrt(dpa * dpb)
                  / d2w * (1 + 0.9 * np.log(r) + 4.5 * r * q - 4.4 * np.exp(-1.3 * r) - 20 * (z2 / eta0) ** 2))
            b = bt * 50.0
            n1 = ta2 * (1 + 1 / tb2 + 1j * b)
            n2 = tb2 * (1 + 1 / ta2 + 1j * b)
            n3 = 1 / ta2 + 1 / tb2 + 1j * b
            core = np.zeros((3, 3), complex)
            core[0, 0], core[1, 1], core[2, 2] = (1 - n1) / (1 + n1), (1 - n2) / (1 + n2), (1 - n3) / (1 + n3)
            core[0, 2] = core[2, 0] = 2 * np.sqrt(ta2) / (1 + n1)
            core[1, 2] = core[2, 1] = 2 * np.sqrt(tb2) / (1 + n2)
            core[0, 1] = core[1, 0] = 2 / (np.sqrt(ta2 * tb2) * (1 + 1j * b) + np.sqrt(ta2 / tb2) + np.sqrt(tb2 / ta2))
            junction = rf.Network(frequency=one, s=core[None], z0=50)
            for port, (w, length) in enumerate(((wa, la), (wb, lb), (w2, l2))):
                line = _line_network(_mline(one, w, h, t, er), one, length)
                # connect() keeps the line's outer port at the place of the connected port
                junction = rf.network.connect(junction, port, line, 1, num=1)
            smat.append(mat(junction.s[0]))
        out["tee"].append({"w1": wa, "w2": wb, "w3": w2, "s": smat})

    out["via"] = []
    mu0 = 1.25663706127e-6
    for d, hv, tv, sigma in ((0.3e-3, 0.635e-3, 20e-6, 5.8e7), (0.5e-3, 1.6e-3, 25e-6, 4.1e7)):
        rr = d / 2
        a = np.sqrt(rr * rr + hv * hv)
        l = mu0 / (2 * np.pi) * (hv * np.log((hv + a) / rr) + 1.5 * (rr - a))
        rdc = hv / (sigma * np.pi * (rr * rr - (rr - tv) ** 2))
        z = [c(rdc * np.sqrt(1 + f * np.pi * mu0 * sigma * tv * tv) + 2j * np.pi * f * l) for f in freqs]
        out["via"].append({"d": d, "h": hv, "t": tv, "sigma": sigma, "l": l, "rdc": rdc, "z": z})
    write_json("discontinuities.json", out)


def _line_network(media, frequency, length):
    """A line section of the media with the given (possibly negative) length,
    referred to 50 ohm on both ports."""
    zc = media.z0_characteristic[0]
    gl = media.gamma[0] * length
    a, b, cc, d = np.cosh(gl), zc * np.sinh(gl), np.sinh(gl) / zc, np.cosh(gl)
    abcd = np.array([[[a, b], [cc, d]]])
    return rf.Network(frequency=frequency, s=rf.network.a2s(abcd, z0=50), z0=50)


SECTIONS = {"conversions": conversions, "interpolation": interpolation, "cascade": cascade_and_deembed,
            "stability": stability, "groupdelay": group_delay, "noise": noise, "lines": lines,
            "touchstone": touchstone_files, "roughness": roughness, "gains": gains, "coupled": coupled,
            "discontinuities": discontinuities}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    print(f"Python {sys.version.split()[0]}, scikit-rf {rf.__version__}, numpy {np.__version__}")
    # Sections may be named on the command line (default: all). The random
    # sections draw from one seeded generator, so their output is only
    # reproducible when all of them run in the default order.
    names = sys.argv[1:] or list(SECTIONS)
    for name in names:
        if name not in SECTIONS:
            sys.exit(f"unknown section {name!r}; known: {', '.join(SECTIONS)}")
        SECTIONS[name]()


if __name__ == "__main__":
    main()
