"""SPICE from Python: a non-inverting amplifier with the generic op-amp.

Gain 1 + Rf/Rg = 11 for a 100 mV, 1 kHz sine; the operating point, the AC
response (gain and -3 dB bandwidth GBW / 11), a transient with its Fourier
analysis, and an imported netlist are checked.

Run by ctest through lvdrfcad-cli with a scratch folder as the only argument.
"""
import math
import pathlib
import shutil
import sys

import lvdrfcad

root = pathlib.Path(sys.argv[1])
shutil.rmtree(root, ignore_errors=True)
project = lvdrfcad.new_project(root / "amp")
amp = project.new_document("spice", "amp").schematic


def label(net, ref, pin):
    amp.add_label(net, amp.pin_position(ref, pin))


with amp.edit("Build amplifier"):
    amp.add_component("spice-ics:opamp", (0, 0), parameters={"gbw": "1MHz"}, reference="U1")
    amp.add_component("spice-sources:vsin", (-300, 0), parameters={"va": "100m", "freq": "1kHz", "ac": "1"},
                      reference="VIN")
    amp.add_component("spice-sources:vdc", (-300, 200), parameters={"dc": "15"}, reference="VP")
    amp.add_component("spice-sources:vdc", (-200, 200), parameters={"dc": "15"}, reference="VN")
    amp.add_component("basic:resistor", (200, 200), parameters={"r": "10k"}, reference="RF")
    amp.add_component("basic:resistor", (200, 300), parameters={"r": "1k"}, reference="RG")
    amp.add_component("basic:resistor", (300, 100), parameters={"r": "10k"}, reference="RL")
    label("in", "VIN", "p")
    label("in", "U1", "inp")
    label("out", "U1", "out")
    label("out", "RF", "p")
    label("out", "RL", "p")
    label("fb", "RF", "n")
    label("fb", "RG", "p")
    label("fb", "U1", "inn")
    label("vcc", "U1", "vcc")
    label("vcc", "VP", "p")
    label("vee", "U1", "vee")
    label("vee", "VN", "n")
    # Grounds
    for ref, pin, at in (("VIN", "n", (-300, 120)), ("VP", "n", (-300, 320)), ("VN", "p", (-200, 80)),
                         ("RG", "n", (300, 400)), ("RL", "n", (400, 200))):
        gnd = amp.add_component("basic:ground", at)
        amp.connect(ref, pin, gnd, "gnd")

op = amp.add_spice_analysis("op", name="Bias")
ac = amp.add_spice_analysis("ac", name="Response", start="10Hz", stop="10MHz", points=20)
tran = amp.add_spice_analysis("fourier", name="Distortion", step="2us", stop="5ms", maxStep="2us",
                              fundamental="1kHz", signals=["v(out)"])
assert [a["name"] for a in amp.spice_analyses] == ["Bias", "Response", "Distortion"]
assert ".ac dec 20 10 10000000" in amp.spice_netlist(ac)

bias = amp.spice(op)
assert abs(bias.value("v(vcc)") - 15) < 1e-9
assert abs(bias.value("v(out)")) < 1e-3, bias.value("v(out)")

response = amp.spice(ac)
freqs, gain = response.evaluate("db(v(out)/v(in))")
assert abs(gain[0] - 20 * math.log10(11)) < 0.01, gain[0]
# -3 dB bandwidth of a single-pole amplifier: GBW / noise gain.
corner = next(f for f, g in zip(freqs, gain) if g < gain[0] - 3)
assert 60e3 < corner < 120e3, corner

distortion = amp.spice(tran)
four = distortion.fourier[0]
assert four["error"] == "", four
assert abs(four["fundamental"] - 1000) < 1e-6
assert abs(four["harmonics"][0]["magnitude"] - 1.1) < 0.01, four["harmonics"][0]
assert four["thd_percent"] < 0.5, four["thd_percent"]
t, out = distortion.evaluate("v(out)", "tran")
assert len(t) == len(out) and max(out) > 1.05

# An imported netlist becomes a document with its analyses.
cir = root / "rc.cir"
cir.write_text("RC low-pass\nV1 in 0 AC 1\nR1 in out 1k\nC1 out 0 1u\n.ac dec 10 1 100k\n.end\n")
rc = project.import_spice_netlist(cir).schematic
assert [a["type"] for a in rc.spice_analyses] == ["spice-ac"]
result = rc.spice()
f, h = result.evaluate("mag(v(out))")
k = min(range(len(f)), key=lambda i: abs(f[i] - 1 / (2 * math.pi * 1e-3)))
assert abs(h[k] - 1 / math.sqrt(1 + (2 * math.pi * f[k] * 1e-3) ** 2)) < 1e-9

project.save()
print("python_spice: ok")

# A Python-defined SPICE model (SP-8): a resistor network computed in Python.
ladder = {
    "id": "ladder", "name": "R-2R ladder", "category": "Test", "referencePrefix": "U",
    "pins": [{"name": "in"}, {"name": "out"}],
    "parameters": [{"name": "r", "label": "R", "unit": "ohm", "default": "1k"},
                   {"name": "stages", "label": "Stages", "unit": "", "default": "3"}],
    "symbol": {"graphics": [{"type": "rect", "from": [-20, -10], "to": [20, 10]}],
               "pins": [{"pin": "in", "at": [-30, 0], "length": 10, "direction": "right"},
                        {"pin": "out", "at": [30, 0], "length": 10, "direction": "left"}],
               "referenceAt": [0, -20], "valueAt": [0, 20]},
    "models": [{"kind": "python", "source": '''
def spice(ref, nodes, params):
    # Series r between taps, 2r from each tap to ground: each stage halves.
    n = int(params["stages"])
    r = params["r"]
    taps = [nodes["in"]] + [f"{ref.lower()}_t{k}" for k in range(1, n)] + [nodes["out"]]
    lines = []
    for k in range(n):
        lines.append(f"R{ref}_s{k} {taps[k]} {taps[k + 1]} {r}")
        lines.append(f"R{ref}_g{k} {taps[k + 1]} 0 {2 * r if k < n - 1 else r * 2}")
    return "\\n".join(lines)
'''}],
}
key = lvdrfcad.save_component("scripted", ladder)
assert key == "scripted:ladder", key
net = project.new_document("spice", "ladder").schematic
with net.edit("Ladder"):
    net.add_component("spice-sources:vdc", (0, 0), parameters={"dc": "8"}, reference="V1")
    net.add_component("scripted:ladder", (200, -100), parameters={"stages": "1"}, reference="U1")
    net.add_label("in", net.pin_position("V1", "p"))
    net.add_label("in", net.pin_position("U1", "in"))
    net.add_label("out", net.pin_position("U1", "out"))
    g = net.add_component("basic:ground", (0, 120))
    net.connect("V1", "n", g, "gnd")
net.add_spice_analysis("op")
assert "RU1_s0 in out 1000" in net.spice_netlist(), net.spice_netlist()
# One stage: 1k in series, 2k to ground: out = 8 * 2/3.
assert abs(net.spice().value("v(out)") - 16 / 3) < 1e-9
print("python_spice: Python model ok")

# Batches (SP-3) on the ladder: a sweep, Monte Carlo, worst case and an optimisation.
sweep = net.spice_sweep([{"target": "V1.dc", "mode": "list", "list": ["3", "6"]}], ["v(out)"])
assert [round(r["measures"][0], 9) for r in sweep["runs"]] == [2.0, 4.0], sweep
mc = net.spice_monte_carlo([{"target": "V1.dc", "tolerance": 0.1, "distribution": "uniform"}], ["v(out)"], runs=8)
stats = mc["statistics"][0]
assert len(mc["runs"]) == 9 and abs(stats["nominal"] - 16 / 3) < 1e-9
assert 16 / 3 * 0.9 - 1e-9 <= stats["min"] and stats["max"] <= 16 / 3 * 1.1 + 1e-9, stats
wc = net.spice_worst_case([{"target": "V1.dc", "tolerance": 0.05}], ["v(out)"])[0]
assert abs(wc["max"] - 16 / 3 * 1.05) < 1e-9 and abs(wc["min"] - 16 / 3 * 0.95) < 1e-9, wc
opt = net.spice_optimize([{"target": "V1.dc", "min": 1, "max": 20, "log": False}],
                         [{"measure": {"expression": "v(out)"}, "type": "equal", "target": 4}], apply=True)
assert abs(opt["snapped"][0] - 6) < 0.01, opt
assert abs(net.spice().value("v(out)") - 4) < 0.01
print("python_spice: batches ok")
