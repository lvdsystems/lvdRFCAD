"""RF linear analysis from Python: a 3rd-order Butterworth LC low-pass filter.

The prototype values g = 1, 2, 1 at fc = 1 GHz and 50 ohm give
C = g / (2 pi fc R) and L = g R / (2 pi fc); the response is exactly -3.01 dB
at fc and -10 log10(1 + (f/fc)^6) elsewhere for a lossless ladder.

Run by ctest through lvdrfcad-cli with a scratch folder as the only argument.
"""
import math
import pathlib
import shutil
import sys

import lvdrfcad

root = pathlib.Path(sys.argv[1])
shutil.rmtree(root, ignore_errors=True)
project = lvdrfcad.new_project(root / "filters")
lpf = project.new_document("rf", "lpf").schematic

fc, r = 1e9, 50.0
c_value = 1 / (2 * math.pi * fc * r)
l_value = 2 * r / (2 * math.pi * fc)
lpf.variables = {"fc": "1GHz", "C1": repr(c_value), "L1": repr(l_value)}

with lpf.edit("Build filter"):
    lpf.add_component("rf-system:port", (-300, 0), parameters={"num": 1})
    lpf.add_component("rf-system:port", (300, 0), parameters={"num": 2})
    lpf.add_component("basic:inductor", (0, 0), parameters={"l": "L1"})
    for x, ref in ((-150, "C1"), (150, "C2")):
        cid = lpf.add_component("basic:capacitor", (x, 100), parameters={"c": "C1"}, reference=ref)
        lpf.rotate(cid, (x, 100), 90)
        gid = lpf.add_component("basic:ground", (x, 200))
    lpf.connect("PORT1", "p", "L1", "p")
    lpf.connect("L1", "n", "PORT2", "p")
    # Shunt capacitors: top pin to the inductor's pins, bottom pin to ground.
    lpf.connect("C1", "n", "L1", "p")
    lpf.connect("C2", "n", "L1", "n")
    for ref, gnd in (("C1", "GND1"), ("C2", "GND2")):
        lpf.connect(ref, "p", gnd, "gnd")

problems = [p for p in lpf.check() if p["severity"] != "info"]
assert problems == [], problems
nets = lpf.nets()
assert sorted(nets["0"]) == ["C1.p", "C2.p", "GND1.gnd", "GND2.gnd"], nets
assert len(nets) == 3, nets  # ground, input node, output node

analysis = lpf.add_analysis("100MHz", "3GHz", 291, name="Response")
assert lpf.analyses[0]["id"] == analysis
result = lpf.simulate(analysis)
assert result.ports == [1, 2]

freqs = result.frequencies
s21 = result.measure("dB(S21)")
for f, value in zip(freqs, s21):
    expected = -10 * math.log10(1 + (f / fc) ** 6)
    assert abs(value - expected) < 1e-6, (f, value, expected)

# Lossless: |S11|^2 + |S21|^2 = 1, and no added noise.
s11 = result.s(1, 1)
s21c = result.s(2, 1)
assert all(abs(abs(a) ** 2 + abs(b) ** 2 - 1) < 1e-9 for a, b in zip(s11, s21c))
assert max(result.measure("NF")) < 1e-6

# Complex measurements come back as complex numbers.
zin = result.measure("Zin(1)")
assert isinstance(zin[0], complex)

# Touchstone export, then the file used as an S-parameter block reproduces the filter.
result.save_touchstone(root / "filters" / "lpf.s2p")
text = (root / "filters" / "lpf.s2p").read_text()
assert text.count("\n") > 291, "one line per frequency expected"
copy = project.new_document("rf", "from_file").schematic
copy.add_component("rf-system:port", (-200, 0), parameters={"num": 1})
copy.add_component("rf-system:port", (200, 0), parameters={"num": 2})
copy.add_component("rf-system:nport2", (0, 0), parameters={"file": "lpf.s2p"})
copy.connect("PORT1", "p", "SP1", "p1")
copy.connect("SP1", "p2", "PORT2", "p")
again = copy.simulate(start="100MHz", stop="3GHz", points=291)
assert max(abs(a - b) for a, b in zip(again.measure("dB(S21)"), s21)) < 1e-9

# An ad-hoc sweep without storing it.
quick = lpf.simulate(start="1GHz", stop="1GHz", points=1)
assert abs(quick.measure("dB(S21)")[0] + 10 * math.log10(2)) < 1e-9

# Optimisation: detune the filter, then let the optimiser restore -3 dB at fc
# and a flat passband, snapping the capacitor to E96.
lpf.variables = {"fc": "1GHz", "C1": "2pF", "L1": repr(l_value)}
detuned = lpf.simulate(start="1GHz", stop="1GHz", points=1).measure("dB(S21)")[0]
assert abs(detuned + 3.0103) > 0.3, detuned
lpf.add_analysis("200MHz", "1GHz", 9, noise=False, name="Passband")
outcome = lpf.optimize(
    variables=[{"variable": "C1", "min": "0.5pF", "max": "10pF", "series": "E96"}],
    goals=[{"measurement": "dB(S21)", "equal": -3.0103, "band": ("1GHz", "1GHz")},
           # The ideal response is -0.07 dB at 500 MHz (and -0.48 dB at 700 MHz).
           {"measurement": "dB(S21)", "above": -0.2, "band": ("200MHz", "500MHz")}],
    analysis="Passband")
assert outcome["applied"] and outcome["goals"][1]["met"], outcome
assert abs(outcome["best"][0] - c_value) / c_value < 0.02, outcome
tuned = lpf.simulate("Passband").measure("dB(S21)")[-1]
assert abs(tuned + 3.0103) < 0.15, tuned  # E96 rounding leaves a small error
assert lpf.can_undo
lpf.undo()  # one undo step restores the detuned value
assert dict(lpf.variables)["C1"] == "2pF", lpf.variables

# Errors are exceptions.
for bad in (lambda: result.measure("S31"), lambda: lpf.simulate("nothing"),
            lambda: lpf.add_analysis("3GHz", "1GHz")):
    try:
        bad()
    except (ValueError, KeyError, RuntimeError):
        pass
    else:
        raise AssertionError("expected an error")

print("python_rf: all checks passed")
