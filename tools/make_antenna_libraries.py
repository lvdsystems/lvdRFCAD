#!/usr/bin/env python3
"""Write the built-in antenna libraries (libraries/antenna-*.lvdantlib) and
their sample pattern files (libraries/patterns/*.csv).

    python -I tools/make_antenna_libraries.py

The sample patterns are synthetic: computed from aperture and patch models
with added ripple and cross-polarisation, so they look like measured data
and can be redistributed. File format: docs/file-format.md, section 7.
"""

import cmath
import json
import math
import pathlib
import random

ROOT = pathlib.Path(__file__).resolve().parent.parent
LIB = ROOT / "libraries"
C0 = 299792458.0


def library(name, description, entries):
    return {"format": "lvdrfcad-antenna-library", "version": 1, "name": name,
            "description": description, "entries": entries}


def entry(id_, name, kind, description, data):
    return {"id": id_, "name": name, "kind": kind, "description": description, "data": data}


def write(path, obj):
    path.write_text(json.dumps(obj, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    print("wrote", path.relative_to(ROOT))


# ---- Element models -------------------------------------------------------

elements = [
    entry("isotropic", "Isotropic", "element", "Ideal isotropic radiator (array factor only).",
          {"model": "isotropic", "polarisation": "x", "frequency": 10e9, "efficiency": 0}),
    entry("cos15", "Generic array element cos^1.5", "element",
          "Typical embedded element pattern of a well-matched planar array element (about 7.0 dBi, scan loss cos^1.5).",
          {"model": "cosn", "polarisation": "x", "frequency": 10e9, "efficiency": -0.5, "exponent": 1.5, "backLobe": -25}),
    entry("cos12-slant", "cos^1.2 element, slant 45", "element",
          "Wide-beam base-station style element, slant +45 polarisation.",
          {"model": "cosn", "polarisation": "slant45", "frequency": 3.5e9, "efficiency": -0.3, "exponent": 1.2, "backLobe": -25}),
    entry("dipole-half-wave", "Half-wave dipole", "element", "Free-space half-wave dipole along x, 2.15 dBi.",
          {"model": "dipole", "polarisation": "x", "frequency": 1e9, "efficiency": 0, "length": 0}),
    entry("dipole-over-ground", "Dipole a quarter wave over ground", "element",
          "Half-wave dipole a quarter wavelength over an infinite ground plane; classic array element.",
          {"model": "dipole-ground", "polarisation": "x", "frequency": 3e9, "efficiency": -0.2, "length": 0, "height": 0}),
    entry("monopole-quarter-wave", "Quarter-wave monopole", "element",
          "Vertical monopole on an infinite ground plane, vertically polarised (E-theta), 5.16 dBi at the horizon.",
          {"model": "monopole", "frequency": 1e9, "efficiency": 0, "length": 0}),
    entry("patch-x-ro4003", "Patch 10 GHz, RO4003C 0.813 mm", "element",
          "Rectangular patch for 10 GHz on Rogers RO4003C (er 3.55, 0.813 mm), cavity model, dimensions from the design equations.",
          {"model": "patch", "polarisation": "x", "frequency": 10e9, "efficiency": -0.8, "patchWidth": 0, "patchLength": 0,
           "substrateHeight": 0.813e-3, "permittivity": 3.55, "backLobe": -20}),
    entry("patch-c-fr4", "Patch 5.8 GHz, FR-4 1.6 mm", "element",
          "Rectangular patch for 5.8 GHz on FR-4 (er 4.4, 1.6 mm); FR-4 losses lower the efficiency.",
          {"model": "patch", "polarisation": "x", "frequency": 5.8e9, "efficiency": -2.5, "patchWidth": 0, "patchLength": 0,
           "substrateHeight": 1.6e-3, "permittivity": 4.4, "backLobe": -18}),
    entry("patch-gnss-rhcp", "GNSS patch L1, RHCP", "element",
          "Ceramic patch for 1575.42 MHz (er 6.15, 4 mm), dual fed for right-hand circular polarisation.",
          {"model": "patch", "polarisation": "rhcp", "frequency": 1.57542e9, "efficiency": -1.5, "patchWidth": 0,
           "patchLength": 0, "substrateHeight": 4e-3, "permittivity": 6.15, "backLobe": -15}),
    entry("open-waveguide-wr90", "Open waveguide WR-90", "element",
          "Open-ended WR-90 waveguide (22.86 x 10.16 mm), TE10 aperture model.",
          {"model": "aperture", "polarisation": "x", "frequency": 10e9, "efficiency": -0.1, "apertureA": 22.86e-3,
           "apertureB": 10.16e-3, "distribution": "cosine"}),
    entry("horn-x-15dbi", "Pyramidal horn 10 GHz, 15 dBi", "element",
          "Pyramidal horn with a 60 x 46 mm aperture; aperture model without phase error.",
          {"model": "aperture", "polarisation": "x", "frequency": 10e9, "efficiency": -0.2, "apertureA": 60e-3,
           "apertureB": 46e-3, "distribution": "cosine"}),
    entry("horn-ku-20dbi", "Pyramidal horn 12 GHz, 20 dBi", "element",
          "Pyramidal horn with a 92 x 72 mm aperture for 12 GHz; aperture model without phase error.",
          {"model": "aperture", "polarisation": "x", "frequency": 12e9, "efficiency": -0.2, "apertureA": 92e-3,
           "apertureB": 72e-3, "distribution": "cosine"}),
]

# ---- Sample patterns ------------------------------------------------------


def sinc(x):
    return 1.0 if abs(x) < 1e-12 else math.sin(x) / x


def cos_aperture(x):
    d = 1 - (2 * x / math.pi) ** 2
    return math.pi / 4 if abs(d) < 1e-9 else math.cos(x) / d


def write_pattern(path, frequencies, field):
    lines = ["# lvd RF cad far-field pattern; synthetic sample data (see tools/make_antenna_libraries.py)",
             "frequency_hz,theta_deg,phi_deg,etheta_re,etheta_im,ephi_re,ephi_im"]
    for f in frequencies:
        for it in range(0, 181, 2):
            for ip in range(0, 360, 5):
                et, ep = field(math.radians(it), math.radians(ip), f)
                lines.append("%.6g,%d,%d,%.6g,%.6g,%.6g,%.6g" % (f, it, ip, et.real, et.imag, ep.real, ep.imag))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print("wrote", path.relative_to(ROOT))


rng = random.Random(7)
ripple = {(i, j): (1 + 0.06 * rng.uniform(-1, 1)) for i in range(0, 181, 2) for j in range(0, 360, 5)}


def horn_field(theta, phi, f):
    k = 2 * math.pi * f / C0
    a, b = 0.060, 0.046
    st, ct = math.sin(theta), math.cos(theta)
    fx = sinc(k * b / 2 * st * math.cos(phi))
    fy = cos_aperture(k * a / 2 * st * math.sin(phi))
    h = (1 + ct) / 2
    amp = h * fx * fy * ripple[(int(round(math.degrees(theta))) // 2 * 2, int(round(math.degrees(phi))) % 360 // 5 * 5)]
    # Spill-over floor behind the horn and a little cross-polarisation.
    amp = amp if abs(amp) > 0.01 else 0.01 * (1 if amp >= 0 else -1)
    phase = cmath.exp(1j * 0.4 * st * st)
    co_t = amp * math.cos(phi) * phase
    co_p = -amp * math.sin(phi) * phase
    cross = 0.03 * amp * math.sin(2 * phi)
    return co_t + cross * math.sin(phi), co_p + cross * math.cos(phi)


def pcb_patch_field(theta, phi, f):
    # A patch on a small ground plane: beam tilted 8 degrees, ground-plane
    # ripple and back radiation, 1.5 % bandwidth shift with frequency.
    k = 2 * math.pi * f / C0
    w, le, h = 0.0158, 0.0135, 1.6e-3
    tilt = math.radians(8)
    t = abs(theta - tilt * math.cos(phi))
    st, ct = math.sin(t), math.cos(t)
    x = k * h / 2 * st * math.cos(phi)
    y = k * w / 2 * st * math.sin(phi)
    z = k * le / 2 * st * math.cos(phi)
    amp = sinc(x) * sinc(y) * math.cos(z)
    if theta > math.pi / 2:
        amp = 0.15 * math.sin(theta) ** 2 + 0.05
    amp *= 1 + 0.08 * math.sin(6 * theta)
    return amp * math.cos(phi), -amp * ct * math.sin(phi) + 0.02 * amp


(LIB / "patterns").mkdir(exist_ok=True)
write_pattern(LIB / "patterns" / "sample-horn-x.csv", [9.5e9, 10e9, 10.5e9], horn_field)
write_pattern(LIB / "patterns" / "sample-pcb-patch-5g8.csv", [5.7e9, 5.8e9, 5.9e9], pcb_patch_field)

patterns = [
    entry("sample-horn-x", "Sample measured horn, X band", "pattern",
          "Synthetic 'measured' pattern of a 15 dBi X-band horn, 9.5 to 10.5 GHz, with measurement ripple and cross-polarisation.",
          {"model": "imported", "frequency": 10e9, "efficiency": -0.3, "patternFile": "patterns/sample-horn-x.csv"}),
    entry("sample-pcb-patch-5g8", "Sample PCB patch 5.8 GHz", "pattern",
          "Synthetic 'measured' pattern of a patch on a small ground plane: tilted beam, back radiation, 5.7 to 5.9 GHz.",
          {"model": "imported", "frequency": 5.8e9, "efficiency": -2.0, "patternFile": "patterns/sample-pcb-patch-5g8.csv"}),
]

# ---- T/R module presets ---------------------------------------------------


def tr(id_, name, description, tx_power, tx_gain, rx_gain, nf, phase_bits, att_bits, att_step, amp_err, phase_err, front):
    return entry(id_, name, "tr-module", description,
                 {"tx": {"power": tx_power, "gain": tx_gain}, "rx": {"gain": rx_gain, "nf": nf},
                  "phaseBits": phase_bits, "attenuatorBits": att_bits, "attenuatorStep": att_step,
                  "amplitudeError": amp_err, "phaseError": phase_err, "frontLoss": front})


tr_modules = [
    tr("x-gan-10w", "X-band GaN, 10 W", "Generic X-band radar T/R module: GaN HPA, 6-bit phase and attenuation.",
       40, 30, 25, 3.0, 6, 6, 0.5, 0.4, 3.0, 0.8),
    tr("x-gaas-2w", "X-band GaAs, 2 W", "Generic X-band GaAs T/R module for lighter arrays.",
       33, 28, 24, 2.5, 6, 6, 0.5, 0.3, 2.5, 0.7),
    tr("s-gan-50w", "S-band GaN, 50 W", "Generic S-band surveillance radar T/R module.",
       47, 35, 28, 2.0, 6, 6, 0.5, 0.4, 3.0, 0.6),
    tr("ku-satcom", "Ku-band satcom Rx/Tx channel", "Satcom flat-panel channel: low-noise receive, modest transmit.",
       24, 28, 30, 1.4, 5, 5, 0.5, 0.5, 4.0, 0.4),
    tr("ka-bf-ic", "Ka-band beamformer IC channel", "Silicon beamformer channel at 28 GHz with external LNA/PA.",
       18, 20, 22, 4.5, 5, 5, 0.5, 0.6, 5.0, 1.0),
    tr("n78-mmimo", "3.5 GHz massive-MIMO channel", "Base-station radio channel (digital beamforming, fine quantisation).",
       37, 30, 30, 2.2, 10, 10, 0.1, 0.2, 1.0, 0.5),
]

# ---- Array presets --------------------------------------------------------


def taper(kind, **kw):
    t = {"kind": kind}
    t.update(kw)
    return t


arrays = [
    entry("ula-16", "Linear, 16 elements, half wave", "array", "Uniform linear array of 16 elements at 0.5 wavelength.",
          {"geometry": {"kind": "linear", "unit": "lambda", "nx": 16, "dx": 0.5},
           "excitation": {"taperX": taper("uniform"), "taperY": taper("uniform")}}),
    entry("ula-32-cheb", "Linear, 32 elements, Chebyshev 30 dB", "array", "Dolph-Chebyshev line with 30 dB side lobes.",
          {"geometry": {"kind": "linear", "unit": "lambda", "nx": 32, "dx": 0.5},
           "excitation": {"taperX": taper("chebyshev", sll=30), "taperY": taper("uniform")}}),
    entry("ura-8x8", "Planar 8 x 8, uniform", "array", "Rectangular 8 x 8 array at 0.5 wavelength.",
          {"geometry": {"kind": "rectangular", "unit": "lambda", "nx": 8, "ny": 8, "dx": 0.5, "dy": 0.5, "aperture": "none"},
           "excitation": {"taperX": taper("uniform"), "taperY": taper("uniform")}}),
    entry("ura-16x16-taylor", "Planar 16 x 16, Taylor 30 dB", "array",
          "Rectangular 16 x 16 array with separable Taylor 30 dB, n-bar 5 tapers.",
          {"geometry": {"kind": "rectangular", "unit": "lambda", "nx": 16, "ny": 16, "dx": 0.5, "dy": 0.5, "aperture": "none"},
           "excitation": {"taperX": taper("taylor", sll=30, nbar=5), "taperY": taper("taylor", sll=30, nbar=5)}}),
    entry("aesa-circular-tri", "Circular AESA, triangular lattice", "array",
          "Radar AESA: equilateral triangular lattice (0.6 wavelength), circular aperture, radial Taylor 35 dB; scans to 60 degrees.",
          {"geometry": {"kind": "triangular", "unit": "lambda", "nx": 24, "ny": 28, "dx": 0.6, "dy": 0.5196,
                        "aperture": "circle", "apertureX": 6.9},
           "excitation": {"taperX": taper("taylor", sll=35, nbar=5), "taperY": taper("uniform"), "radial": True}}),
    entry("monopulse-16x16", "Monopulse 16 x 16, azimuth difference", "array",
          "16 x 16 array showing the azimuth difference pattern (switch to Sum for the sum beam).",
          {"geometry": {"kind": "rectangular", "unit": "lambda", "nx": 16, "ny": 16, "dx": 0.5, "dy": 0.5, "aperture": "none"},
           "excitation": {"taperX": taper("taylor", sll=30, nbar=5), "taperY": taper("taylor", sll=30, nbar=5),
                          "pattern": "delta-az"}}),
    entry("uca-16", "Ring of 16 elements", "array", "Uniform circular array: 16 elements on a ring of 1.3 wavelength radius.",
          {"geometry": {"kind": "circular", "unit": "lambda", "rings": [{"radius": 1.3, "count": 16, "rotation": 0}],
                        "centreElement": False},
           "excitation": {"taperX": taper("uniform"), "taperY": taper("uniform")}}),
    entry("cylinder-sector", "Cylinder sector, 24 x 8", "array",
          "Conformal sector on a cylinder of 3 wavelength radius: 24 columns over 120 degrees, 8 rows; steered to broadside (+x).",
          {"geometry": {"kind": "cylinder", "unit": "lambda", "cylinderRadius": 3, "columns": 24, "rows": 8, "arc": 120, "dz": 0.5},
           "excitation": {"taperX": taper("cosine-pedestal", pedestal=-12), "taperY": taper("uniform"),
                          "steerTheta": 90, "steerPhi": 0}}),
]

write(LIB / "antenna-elements.lvdantlib",
      library("antenna-elements", "Analytic antenna element models: isotropic, cos^n, dipoles, monopole, patches, apertures and horns.",
              elements))
write(LIB / "antenna-patterns.lvdantlib",
      library("antenna-patterns", "Pattern-file elements. The two samples are synthetic; add measured or vendor patterns here.",
              patterns))
write(LIB / "antenna-tr-modules.lvdantlib",
      library("antenna-tr-modules", "Generic T/R module presets (typical values, not vendor data).", tr_modules))
write(LIB / "antenna-arrays.lvdantlib",
      library("antenna-arrays", "Array presets: lines, planar, AESA, monopulse, ring and conformal.", arrays))
