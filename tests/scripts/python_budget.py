"""System budget from Python: a 2.45 GHz superheterodyne receiver.

Run by ctest through lvdrfcad-cli with a scratch folder as the only argument.
"""
import math
import pathlib
import shutil
import sys

import lvdrfcad

root = pathlib.Path(sys.argv[1])
shutil.rmtree(root, ignore_errors=True)
project = lvdrfcad.new_project(root / "receiver")
rx = project.new_document("rf", "rx").schematic
rx.variables = {"f_rf": "2.45GHz", "f_if": "70MHz"}

with rx.edit("Build receiver"):
    rx.add_component("rf-system:antenna", (0, 0), parameters={"temp": "100"})
    rx.add_component("rf-system:filter", (200, 0), parameters={"fc": "f_rf", "bw": "100MHz", "il": "1.5"})
    rx.add_component("rf-system:amplifier", (400, 0),
                     parameters={"gain": "18", "nf": "1", "oip3": "28", "op1db": "16"})
    rx.add_component("rf-system:mixer", (600, 0), parameters={"gain": "-7", "nf": "7", "iip3": "15"})
    rx.add_component("rf-system:lo", (600, 200), parameters={"freq": "f_rf - f_if", "power": "13"})
    rx.add_component("rf-system:filter", (800, 0),
                     parameters={"type": "lowpass", "response": "ideal", "fc": "100MHz", "il": "0.5"})
    rx.add_component("rf-system:adc", (1000, 0), parameters={"fs": "100MHz", "bits": "14"})
    rx.connect("ANT1", "p", "F1", "in")
    rx.connect("F1", "out", "A1", "in")
    rx.connect("A1", "out", "MX1", "rf")
    rx.connect("LO1", "out", "MX1", "lo")
    rx.connect("MX1", "if", "F2", "in")
    rx.connect("F2", "out", "ADC1", "in")

# The antenna has no frequency or power: give them to the analysis.
analysis = rx.add_budget(frequency="f_rf", power="-90", bandwidth="1MHz", sweep_start="2.3GHz",
                         sweep_stop="2.6GHz", sweep_points=31, spurs=False, name="Receiver")
assert rx.analyses == [], "budget analyses are not listed as S-parameter analyses"
budget = rx.budget(analysis)

plan = budget["frequency_plan"]
assert [round(p["output_frequency"]) for p in plan] == [2450000000, 2450000000, 70000000, 70000000], plan
assert plan[2]["lo_source"] == "LO1" and math.isclose(plan[2]["lo_frequency"], 2.38e9)

stages = budget["stages"]
assert [s["component"] for s in stages] == ["F1", "A1", "MX1", "F2", "ADC1"]
last = stages[-1]
assert abs(last["cumulative_gain"] - (-1.5 + 18 - 7 - 0.5)) < 1e-3, last["cumulative_gain"]
assert math.isclose(last["signal"], -90 + last["cumulative_gain"])
# Antenna at 100 K: input noise k * 100 K * 1 MHz.
assert abs(budget["input_noise"] - 10 * math.log10(1.380649e-23 * 100 * 1e6 * 1000)) < 1e-9
assert last["snr"] > 0
assert budget["adc"]["alias_frequency"] == 30e6
assert abs(budget["adc"]["quantisation_snr"] - (6.02 * 14 + 1.76)) < 1e-9
assert stages[1]["iip3"] < 28 - 18 + 1.5 + 1e-9  # LNA IIP3 referred to the input through the filter loss

assert len(budget["sweep"]) == 31
centre = budget["sweep"][15]["stages"][-1]["cumulative_gain"]
edge = budget["sweep"][0]["stages"][-1]["cumulative_gain"]
assert centre > edge + 10, (centre, edge)

# Spur analysis: the image of a low-side LO is at f_LO - f_IF = 2.31 GHz,
# attenuated by the RF bandpass filter.
with_spurs = rx.budget(frequency="f_rf", power="-90", spurs=True)
images = [s for s in with_spurs["spurs"] if s["kind"] == "image"]
assert len(images) == 1, images
assert abs(images[0]["input_frequency"] - 2.31e9) < 1e3, images[0]
assert images[0]["rejection"] > 20, images[0]
assert any(s["kind"] == "lo_leakage" for s in with_spurs["spurs"])

# Errors are exceptions.
try:
    rx.budget(path=["ANT1", "MX1"], frequency="f_rf")
except RuntimeError as error:
    assert "not connected" in str(error)
else:
    raise AssertionError("expected an error")

print("python_budget: all checks passed")
