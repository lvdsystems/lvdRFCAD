"""Builds the example projects in examples/ (except examples/upconverter, which is a
user design kept as drawn).

Run with the command-line runner from the repository root:

    build\\bin\\RelWithDebInfo\\lvdrfcad-cli.exe tools\\make_examples.py

Every example is checked: the rule check must be clean and its analyses must run.
"""
import math
import pathlib
import shutil
import sys

import lvdrfcad

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXAMPLES = ROOT / "examples"


def fresh_project(name):
    folder = EXAMPLES / name
    if folder.exists():
        shutil.rmtree(folder)
    return lvdrfcad.new_project(folder)


def require_clean(sheet, what):
    problems = [p for p in sheet.check() if p["severity"] != "info"]
    assert not problems, (what, problems)


def shunt(sheet, ref_key, at, parameters, node_component, node_pin, reference=None):
    """A two-terminal part from a node to ground, standing vertically below the node."""
    part = sheet.add_component(ref_key, at, parameters=parameters, reference=reference)
    sheet.rotate(part, at, 90)  # pin n at the top, p at the bottom
    ref = sheet.component(part)["reference"]
    sheet.connect(ref, "n", node_component, node_pin)
    ground = sheet.add_component("basic:ground", (at[0], at[1] + 70))
    sheet.connect(ref, "p", sheet.component(ground)["reference"], "gnd")
    return ref


# ---------------------------------------------------------------------------
# 1. Fifth-order Butterworth low-pass filter, 1 GHz, 50 ohm
#    Prototype g = 0.618, 1.618, 2.000, 1.618, 0.618 (C-L-C-L-C)

def lowpass_filter():
    project = fresh_project("lowpass_filter")
    lpf = project.new_document("rf", "lpf").schematic
    lpf.variables = [
        ("fc", "1GHz"),
        ("R0", "50"),
        ("w", "2*pi*fc"),
        ("C1", "0.618/(w*R0)"),
        ("L2", "1.618*R0/w"),
        ("C3", "2.000/(w*R0)"),
    ]
    with lpf.edit("Build low-pass filter"):
        lpf.add_component("rf-system:port", (-300, 20), parameters={"num": 1}, reference="P1")
        lpf.add_component("basic:inductor", (0, 0), parameters={"l": "L2"}, reference="L1")
        lpf.add_component("basic:inductor", (300, 0), parameters={"l": "L2"}, reference="L2")
        lpf.add_component("rf-system:port", (600, 20), parameters={"num": 2}, reference="P2")
        lpf.connect("P1", "p", "L1", "p")
        lpf.connect("L1", "n", "L2", "p")
        lpf.connect("L2", "n", "P2", "p")
        shunt(lpf, "basic:capacitor", (-150, 100), {"c": "C1"}, "L1", "p", reference="C1")
        shunt(lpf, "basic:capacitor", (150, 100), {"c": "C3"}, "L1", "n", reference="C2")
        shunt(lpf, "basic:capacitor", (450, 100), {"c": "C1"}, "L2", "n", reference="C3")
        lpf.add_text("5th-order Butterworth low-pass, fc = 1 GHz, 50 ohm", (-300, -100), 14)
    require_clean(lpf, "lowpass_filter")

    analysis = lpf.add_analysis("10MHz", "3GHz", 300, noise=False, name="Response")  # 10 MHz steps
    result = lpf.simulate(analysis)
    s21 = dict(zip(result.frequencies, result.measure("dB(S21)")))
    at_fc = min(result.frequencies, key=lambda f: abs(f - 1e9))
    assert abs(s21[at_fc] + 3.0103) < 0.05, s21[at_fc]
    project.save()
    print("lowpass_filter: |S21| at fc =", round(s21[at_fc], 3), "dB")


# ---------------------------------------------------------------------------
# 2. Input match of an amplifier described by a Touchstone file

def lna_match():
    project = fresh_project("lna_match")
    shutil.copy(ROOT / "src" / "rfdata" / "tests" / "data" / "amp_noise.s2p",
                pathlib.Path(project.directory) / "amp.s2p")
    lna = project.new_document("rf", "lna").schematic
    # Deliberately poor starting values: the tutorial tunes and optimises them.
    lna.variables = [("Lin", "2nH"), ("Cin", "1pF")]
    with lna.edit("Build LNA input match"):
        lna.add_component("rf-system:port", (-400, 20), parameters={"num": 1}, reference="P1")
        lna.add_component("basic:inductor", (-150, 0), parameters={"l": "Lin"}, reference="L1")
        lna.add_component("rf-system:nport2", (150, 0), parameters={"file": "amp.s2p"}, reference="AMP1")
        lna.add_component("rf-system:port", (450, 20), parameters={"num": 2}, reference="P2")
        lna.connect("P1", "p", "L1", "p")
        lna.connect("L1", "n", "AMP1", "p1")
        lna.connect("AMP1", "p2", "P2", "p")
        shunt(lna, "basic:capacitor", (-280, 100), {"c": "Cin"}, "L1", "p", reference="C1")
        lna.add_text("Amplifier from amp.s2p (with noise parameters), input L-match", (-400, -100), 14)
    require_clean(lna, "lna_match")

    analysis = lna.add_analysis("1GHz", "4GHz", 151, noise=True, name="Band")
    before = lna.simulate(analysis)
    # Find a match with the optimiser, but keep the poor values in the saved example.
    outcome = lna.optimize(
        variables=[{"variable": "Lin", "min": "0.3nH", "max": "30nH", "series": "E24"},
                   {"variable": "Cin", "min": "0.1pF", "max": "20pF", "series": "E24"}],
        goals=[{"measurement": "dB(S11)", "below": -15, "band": ("2.4GHz", "2.5GHz")}],
        analysis="Band", apply=False)
    assert outcome["goals"][0]["met"] or outcome["best_cost"] < 1, outcome
    project.save()
    print("lna_match: optimiser reaches", outcome["goals"][0]["worst"], "dB, values", outcome["snapped"])


# ---------------------------------------------------------------------------
# 3. 2.45 GHz superheterodyne receiver with a hierarchical IF strip

def receiver():
    project = fresh_project("receiver")

    strip = project.new_document("rf", "if_strip").schematic
    strip.variables = [("f_if", "70MHz"), ("gain", "30")]
    with strip.edit("Build IF strip"):
        strip.add_component("basic:sheet_port", (-350, 0), parameters={"name": "in"}, reference="P1")
        strip.add_component("rf-system:filter", (-150, 0),
                            parameters={"type": "bandpass", "fc": "f_if", "bw": "20MHz", "order": "4", "il": "2"},
                            reference="F1")
        strip.add_component("rf-system:amplifier", (100, 0),
                            parameters={"gain": "gain", "nf": "4", "oip3": "32", "op1db": "20"}, reference="A1")
        strip.add_component("basic:sheet_port", (300, 0), parameters={"name": "out"}, reference="P2")
        strip.connect("P1", "p", "F1", "in")
        strip.connect("F1", "out", "A1", "in")
        strip.connect("A1", "out", "P2", "p")
        strip.add_text("IF strip: 70 MHz band-pass and IF amplifier", (-350, -100), 14)
    require_clean(strip, "if_strip")

    rx = project.new_document("rf", "receiver").schematic
    rx.variables = [("f_rf", "2.45GHz"), ("f_if", "70MHz"), ("if_gain", "28")]
    with rx.edit("Build receiver"):
        rx.add_component("rf-system:antenna", (-700, 0), parameters={"temp": "150"}, reference="ANT1")
        rx.add_component("rf-system:filter", (-500, 0),
                         parameters={"fc": "f_rf", "bw": "100MHz", "il": "1.5", "order": "3"}, reference="F1")
        rx.add_component("rf-system:amplifier", (-300, 0),
                         parameters={"gain": "18", "nf": "1", "oip3": "28", "op1db": "16"}, reference="LNA1")
        rx.add_component("rf-system:mixer", (-100, 0),
                         parameters={"gain": "-7", "nf": "7", "iip3": "15", "product": "difference"},
                         reference="MX1")
        rx.add_component("rf-system:lo", (-100, 200), parameters={"freq": "f_rf - f_if", "power": "13"},
                         reference="LO1")
        rx.add_component("sheet:if_strip", (150, 0), parameters={"gain": "if_gain"}, reference="X1")
        rx.add_component("rf-system:adc", (400, 0), parameters={"fs": "100MHz", "bits": "14"}, reference="ADC1")
        rx.connect("ANT1", "p", "F1", "in")
        rx.connect("F1", "out", "LNA1", "in")
        rx.connect("LNA1", "out", "MX1", "rf")
        rx.connect("LO1", "out", "MX1", "lo")
        rx.connect("MX1", "if", "X1", "in")
        rx.connect("X1", "out", "ADC1", "in")
        rx.add_text("2.45 GHz receiver, 70 MHz IF, 14-bit ADC at 100 MS/s", (-700, -120), 14)
    require_clean(rx, "receiver")

    budget = rx.add_budget(frequency="f_rf", power="-90", bandwidth="1MHz",
                           sweep_start="2.3GHz", sweep_stop="2.6GHz", sweep_points=121,
                           spurs=True, spur_order=5, name="Receiver budget")
    result = rx.budget(budget)
    last = result["stages"][-1]
    assert [p["output_frequency"] for p in result["frequency_plan"]][-1] == 70e6
    assert result["adc"]["alias_frequency"] == 30e6
    project.save()
    print("receiver: gain %.2f dB, NF %.2f dB, SNR %.1f dB, %d spurs" % (
        last["cumulative_gain"], last["cumulative_nf"], last["snr"], len(result["spurs"])))


lowpass_filter()
lna_match()
receiver()
lvdrfcad.close_project()
print("examples written to", EXAMPLES)
