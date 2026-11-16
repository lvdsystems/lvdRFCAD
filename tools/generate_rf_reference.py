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


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    print(f"Python {sys.version.split()[0]}, scikit-rf {rf.__version__}, numpy {np.__version__}")
    conversions()
    interpolation()
    cascade_and_deembed()
    stability()
    group_delay()
    noise()
    lines()
    touchstone_files()


if __name__ == "__main__":
    main()
