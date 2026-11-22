"""Builds the example projects in examples/ (except examples/upconverter, which is a
user design kept as drawn).

Run with the command-line runner from the repository root:

    build\\bin\\RelWithDebInfo\\lvdrfcad-cli.exe tools\\make_examples.py

Every example is checked: the rule check must be clean and its analyses must run.
"""
import json
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


# ---------------------------------------------------------------------------
# 4. 1.9 GHz receiver with a PLL local oscillator: phase noise and reciprocal mixing

def pll_receiver():
    project = fresh_project("pll_receiver")
    rx = project.new_document("rf", "pll_receiver").schematic
    rx.variables = [("f_rf", "1.9GHz"), ("f_if", "140MHz")]
    with rx.edit("Build PLL receiver"):
        rx.add_component("rf-system:antenna", (-700, 0), parameters={"temp": "290"}, reference="ANT1")
        rx.add_component("rf-system:filter", (-500, 0),
                         parameters={"fc": "f_rf", "bw": "60MHz", "il": "1.5", "order": "4"}, reference="F1")
        rx.add_component("rf-system:amplifier", (-300, 0),
                         parameters={"gain": "16", "nf": "1.2", "oip3": "30", "op1db": "18", "rl_in": "15",
                                     "rl_out": "15"}, reference="LNA1")
        rx.add_component("rf-system:mixer", (-100, 0),
                         parameters={"gain": "-7", "nf": "7", "iip3": "18", "product": "difference"}, reference="MX1")
        rx.add_component("rf-system:pll", (-420, 250),
                         parameters={"freq": "(f_rf - f_if) / 2", "power": "16", "ref_freq": "20MHz", "ref_pn": "-150",
                                     "fom": "-226", "loop_bw": "50kHz", "pn": "-112", "pn_offset": "100kHz",
                                     "pn_floor": "-160"}, reference="PLL1")
        rx.add_component("rf-system:multiplier", (-250, 250), parameters={"factor": "2", "gain": "-3"},
                         reference="MUL1")
        rx.add_component("rf-system:filter", (150, 0),
                         parameters={"fc": "f_if", "bw": "20MHz", "il": "2", "order": "4"}, reference="F2")
        rx.add_component("rf-system:amplifier", (350, 0),
                         parameters={"gain": "24", "nf": "4", "oip3": "35", "op1db": "20"}, reference="A2")
        rx.add_component("rf-system:adc", (550, 0), parameters={"fs": "250MHz", "bits": "14"}, reference="ADC1")
        rx.connect("ANT1", "p", "F1", "in")
        rx.connect("F1", "out", "LNA1", "in")
        rx.connect("LNA1", "out", "MX1", "rf")
        rx.connect("PLL1", "out", "MUL1", "in")
        rx.connect("MUL1", "out", "MX1", "lo")
        rx.connect("MX1", "if", "F2", "in")
        rx.connect("F2", "out", "A2", "in")
        rx.connect("A2", "out", "ADC1", "in")
        rx.add_text("1.9 GHz receiver, PLL LO doubled to 1.76 GHz, 140 MHz IF; blocker 3 MHz away",
                    (-700, -120), 14)
    require_clean(rx, "pll_receiver")

    budget = rx.add_budget(frequency="f_rf", power="-85", bandwidth="200kHz", blocker_offset="3MHz",
                           blocker_power="-30", spurs=True, spur_order=5, name="Phase-noise budget")
    result = rx.budget(budget)
    mixer = result["stages"][2]
    last = result["stages"][-1]
    assert result["frequency_plan"][2]["lo_multiplication"] == 2
    assert result["phase_noise"]["has_lo_phase_noise"]
    assert math.isfinite(mixer["lo_phase_noise"]) and math.isfinite(last["reciprocal_mixing_snr"])
    assert last["snr"] < last["thermal_snr"]

    # The receiver sheet has no simulation ports (it starts at an antenna and ends in an ADC), so the
    # S-parameters of the RF front end are on their own sheet, between ports 1 and 2.
    fe = project.new_document("rf", "front_end").schematic
    fe.variables = [("f_rf", "1.9GHz")]
    with fe.edit("Build front end"):
        fe.add_component("rf-system:port", (-500, 20), parameters={"num": 1}, reference="P1")
        fe.add_component("rf-system:filter", (-300, 0),
                         parameters={"fc": "f_rf", "bw": "60MHz", "il": "1.5", "order": "4"}, reference="F1")
        fe.add_component("rf-system:limiter", (-100, 0), parameters={"limit": "15", "il": "0.3", "rl_in": "20",
                                                                     "rl_out": "20"}, reference="LIM1")
        fe.add_component("rf-system:amplifier", (100, 0),
                         parameters={"gain": "16", "nf": "1.2", "oip3": "30", "op1db": "18", "rl_in": "15",
                                     "rl_out": "15"}, reference="LNA1")
        fe.add_component("rf-system:port", (300, 20), parameters={"num": 2}, reference="P2")
        fe.connect("P1", "p", "F1", "in")
        fe.connect("F1", "out", "LIM1", "in")
        fe.connect("LIM1", "out", "LNA1", "in")
        fe.connect("LNA1", "out", "P2", "p")
        fe.add_text("RF front end of pll_receiver: S-parameters and noise figure", (-500, -100), 14)
    require_clean(fe, "front_end")
    analysis = fe.add_analysis("1.7GHz", "2.1GHz", 201, noise=True, name="S-parameters")
    sparams = fe.simulate(analysis)
    gain = dict(zip(sparams.frequencies, sparams.measure("dB(S21)")))
    centre = min(sparams.frequencies, key=lambda f: abs(f - 1.9e9))
    assert 13 < gain[centre] < 15, gain[centre]  # 16 - 1.5 - 0.3 dB and a little mismatch
    project.save()
    print("pll_receiver: LO phase noise %.1f dBc (%.2f deg rms), SNR thermal %.1f / phase noise %.1f / "
          "reciprocal mixing %.1f / total %.1f dB" % (
              mixer["lo_phase_noise"], mixer["lo_rms_phase"], last["thermal_snr"], last["phase_noise_snr"],
              last["reciprocal_mixing_snr"], last["snr"]))


def set_plots(project, document_name, analysis, plots):
    """Replaces the default plots of a saved document by named plots: (name, kind, traces, labels)."""
    path = pathlib.Path(project.document(document_name).path)
    data = json.loads(path.read_text(encoding="utf-8"))
    data["content"]["simulation"]["plots"] = [
        dict({"id": "p%d" % (i + 1), "name": name, "kind": kind, "analysis": analysis, "traces": traces},
             **({"labels": labels} if labels else {}))
        for i, (name, kind, traces, labels) in enumerate(plots)]
    path.write_text(json.dumps(data, indent=4, sort_keys=True, ensure_ascii=False) + "\n", encoding="utf-8")


# ---------------------------------------------------------------------------
# 5. X-band down-converter: 10 GHz in, 9 GHz LO, 1 GHz IF, with simulation ports
#    so both the budget and the S-parameters (match, leakage) run on one sheet.

def downconverter():
    project = fresh_project("downconverter")
    dc = project.new_document("rf", "downconverter").schematic
    dc.variables = [("f_rf", "10GHz"), ("f_if", "1GHz"), ("lo_power", "13")]
    with dc.edit("Build down-converter"):
        dc.add_component("rf-system:port", (-800, 20), parameters={"num": "1"}, reference="PORT1")
        dc.add_component("rf-system:limiter", (-650, 0),
                         parameters={"limit": "20", "il": "0.4", "rl_in": "18", "rl_out": "18"}, reference="LIM1")
        dc.add_component("rf-system:amplifier", (-480, 0),
                         parameters={"gain": "20", "nf": "1.5", "oip3": "28", "op1db": "16", "rl_in": "12",
                                     "rl_out": "15"}, reference="LNA1")
        dc.add_component("rf-system:filter", (-310, 0),
                         parameters={"type": "bandpass", "fc": "f_rf", "bw": "800MHz", "order": "5", "il": "1.5"},
                         reference="F1")
        dc.add_component("rf-system:mixer", (-140, 0),
                         parameters={"gain": "-8", "nf": "8", "iip3": "18", "lo_power": "lo_power",
                                     "product": "difference", "rl_rf": "12", "rl_if": "12"}, reference="MX1")
        dc.add_component("rf-system:lo", (-140, 220), parameters={"freq": "f_rf - f_if", "power": "lo_power"},
                         reference="LO1")
        dc.add_component("rf-system:filter", (40, 0),
                         parameters={"type": "bandpass", "fc": "f_if", "bw": "200MHz", "order": "4", "il": "1.2"},
                         reference="F2")
        dc.add_component("rf-system:amplifier", (210, 0),
                         parameters={"gain": "22", "nf": "3", "oip3": "35", "op1db": "20", "rl_in": "15",
                                     "rl_out": "15"}, reference="A2")
        dc.add_component("rf-system:port", (380, 20), parameters={"num": "2"}, reference="PORT2")
        dc.connect("PORT1", "p", "LIM1", "in")
        dc.connect("LIM1", "out", "LNA1", "in")
        dc.connect("LNA1", "out", "F1", "in")
        dc.connect("F1", "out", "MX1", "rf")
        dc.connect("LO1", "out", "MX1", "lo")
        dc.connect("MX1", "if", "F2", "in")
        dc.connect("F2", "out", "A2", "in")
        dc.connect("A2", "out", "PORT2", "p")
        dc.add_text("X-band down-converter: 10 GHz RF, 9 GHz LO (low side), 1 GHz IF", (-800, -120), 14)
        dc.add_text("Budget: PORT1 to PORT2 with spurs. S-parameters: match at both ports and the RF-to-IF "
                    "leakage (the frequency conversion itself is not linear)", (-800, -90), 10)
    require_clean(dc, "downconverter")

    analysis = dc.add_analysis("100MHz", "12GHz", 1191, noise=True, name="S-parameters")
    sparams = dc.simulate(analysis)
    s11 = sparams.measure("dB(S(1,1))")
    s22 = sparams.measure("dB(S(2,2))")
    assert all(math.isfinite(v) for v in s11 + s22)
    budget = dc.add_budget(start="PORT1", frequency="f_rf", power="-60", bandwidth="10MHz",
                           sweep_start="9.6GHz", sweep_stop="10.4GHz", sweep_points=81, spurs=True, spur_order=5,
                           name="Budget")
    result = dc.budget(budget)
    last = result["stages"][-1]
    assert abs(result["frequency_plan"][-1]["output_frequency"] - 1e9) < 1
    project.save()
    set_plots(project, "downconverter", analysis, [
        ("Match and leakage", "rectangular", ["dB(S(1,1))", "dB(S(2,2))", "dB(S(2,1))"],
         {"dB(S(2,1))": "RF-to-IF leakage S21"}),
        ("Reflection", "smith", ["S(1,1)", "S(2,2)"], {}),
    ])
    print("downconverter: gain %.2f dB, NF %.2f dB, IIP3 %.1f dBm, %d spurs" % (
        last["cumulative_gain"], last["cumulative_nf"], last["iip3"], len(result["spurs"])))


# ---------------------------------------------------------------------------
# 6. Selectable filter path: an SP4T switch filter bank, 0.5 to 6 GHz in four
#    bands. The variable `band` (1 to 4) sets both switches.

def filter_bank():
    project = fresh_project("filter_bank")
    fb = project.new_document("rf", "filter_bank").schematic
    bands = [("1GHz", "500MHz"), ("2GHz", "1GHz"), ("3.5GHz", "1.5GHz"), ("5GHz", "2GHz")]
    fb.variables = [("band", "2")]
    with fb.edit("Build filter bank"):
        fb.add_component("rf-system:port", (-600, 20), parameters={"num": "1"}, reference="PORT1")
        fb.add_component("rf-system:switch_sp4t", (-400, 0),
                         parameters={"select": "band", "il": "0.8", "isolation": "35", "rl_in": "18",
                                     "rl_out": "18"}, reference="SW1")
        fb.add_component("rf-system:switch_sp4t", (200, 0), mirror=True,
                         parameters={"select": "band", "il": "0.8", "isolation": "35", "rl_in": "18",
                                     "rl_out": "18"}, reference="SW2")
        fb.add_component("rf-system:amplifier", (400, 0),
                         parameters={"gain": "15", "nf": "3", "oip3": "30", "op1db": "18", "rl_in": "12",
                                     "rl_out": "12"}, reference="A1")
        fb.add_component("rf-system:port", (600, 20), parameters={"num": "2"}, reference="PORT2")
        # Throws at y = -30, -10, 10, 30; filters at y = -180, -60, 60, 180. The wires are drawn
        # explicitly, with the outer throws' risers outside the inner ones, so no two nets touch.
        for k, (fc, bw) in enumerate(bands, start=1):
            y = -180 + 120 * (k - 1)
            fb.add_component("rf-system:filter", (-100, y),
                             parameters={"type": "bandpass", "fc": fc, "bw": bw, "order": "5", "il": "1.5",
                                         "response": "chebyshev", "ripple": "0.1"}, reference="F%d" % k)
            ty = -30 + 20 * (k - 1)
            outer = k in (1, 4)
            fb.add_wire([fb.pin_position("SW1", "t%d" % k), (-350 if outer else -330, ty),
                         (-350 if outer else -330, y), fb.pin_position("F%d" % k, "in")])
            fb.add_wire([fb.pin_position("F%d" % k, "out"), (150 if outer else 130, y),
                         (150 if outer else 130, ty), fb.pin_position("SW2", "t%d" % k)])
        fb.connect("PORT1", "p", "SW1", "com")
        fb.connect("SW2", "com", "A1", "in")
        fb.connect("A1", "out", "PORT2", "p")
        fb.add_text("Switched filter bank, 0.5 to 6 GHz: set the variable band = 1..4 (or tune it)",
                    (-600, -300), 14)
    require_clean(fb, "filter_bank")

    analysis = fb.add_analysis("200MHz", "7GHz", 681, noise=True, name="S-parameters")
    centres = []
    for k, (fc, _) in enumerate(bands, start=1):
        fb.variables = [("band", str(k))]
        sp = fb.simulate(analysis)
        gain = sp.measure("dB(S(2,1))")
        at = lambda f: gain[min(range(len(gain)), key=lambda i: abs(sp.frequencies[i] - f))]
        centres.append(at(lvdrfcad.evaluate(fc)))
        # the selected band passes (15 - 0.8 - 1.5 - 0.8 dB), the other bands' centres are rejected
        assert 10 < at(lvdrfcad.evaluate(fc)) < 13, (k, at(lvdrfcad.evaluate(fc)))
        for j, (other, _) in enumerate(bands, start=1):
            if j != k:
                assert at(lvdrfcad.evaluate(other)) < -10, (k, j, at(lvdrfcad.evaluate(other)))
    fb.variables = [("band", "2")]
    budget = fb.add_budget(start="PORT1", frequency="2GHz", power="-30", bandwidth="20MHz", spurs=False,
                           name="Budget (selected band)")
    result = fb.budget(budget)
    assert [s["component"] for s in result["stages"]][:4] == ["SW1", "F2", "SW2", "A1"], [s["component"] for s in result["stages"]]
    project.save()
    set_plots(project, "filter_bank", analysis, [
        ("Selected path", "rectangular", ["dB(S(2,1))", "dB(S(1,1))", "dB(S(2,2))", "NF"], {}),
        ("Group delay", "rectangular", ["GD(S(2,1))"], {}),
    ])
    print("filter_bank: gain at the band centres %s dB" % ", ".join("%.2f" % g for g in centres))


# ---------------------------------------------------------------------------
# 7. AGC module: two gain stages around a step attenuator that the variable
#    p_in sets so the output stays at p_out; a coupler feeds a detector.

def agc_module():
    project = fresh_project("agc_module")
    agc = project.new_document("rf", "agc_module").schematic
    def variables(p_in):
        return [("f0", "2.4GHz"), ("p_in", p_in), ("p_out", "5"), ("g_fixed", "20 + 20 + 20 - 1 - 0.3"),
                ("att", "max(0, min(31.5, round(2 * (p_in + g_fixed - p_out)) / 2))")]

    agc.variables = variables("-40")
    with agc.edit("Build AGC module"):
        agc.add_component("rf-system:port", (-600, 20), parameters={"num": "1"}, reference="PORT1")
        agc.add_component("rf-system:amplifier", (-420, 0),
                          parameters={"gain": "20", "nf": "2", "oip3": "28", "op1db": "16", "rl_in": "15",
                                      "rl_out": "15"}, reference="A1")
        agc.add_component("rf-system:attenuator", (-250, 0),
                          parameters={"il": "att", "rl_in": "20", "rl_out": "20"}, reference="ATT1")
        agc.add_component("rf-system:amplifier", (-80, 0),
                          parameters={"gain": "20", "nf": "4", "oip3": "30", "op1db": "18", "rl_in": "15",
                                      "rl_out": "15"}, reference="A2")
        agc.add_component("rf-system:amplifier", (90, 0),
                          parameters={"gain": "20", "nf": "5", "oip3": "38", "op1db": "24", "rl_in": "15",
                                      "rl_out": "15"}, reference="A3")
        agc.add_component("rf-system:filter", (260, 0),
                          parameters={"type": "bandpass", "fc": "f0", "bw": "200MHz", "order": "3", "il": "1"},
                          reference="F1")
        agc.add_component("rf-system:coupler", (430, 0), parameters={"coupling": "20", "directivity": "25",
                                                                     "il": "0.3"}, reference="CPL1")
        agc.add_component("rf-system:detector", (430, 200), parameters={"sensitivity": "-45",
                                                                        "dynamic_range": "40"}, reference="DET1")
        agc.add_component("rf-system:termination", (590, 200), reference="TERM1")
        agc.add_component("rf-system:port", (630, 20), parameters={"num": "2"}, reference="PORT2")
        agc.connect("PORT1", "p", "A1", "in")
        agc.connect("A1", "out", "ATT1", "in")
        agc.connect("ATT1", "out", "A2", "in")
        agc.connect("A2", "out", "A3", "in")
        agc.connect("A3", "out", "F1", "in")
        agc.connect("F1", "out", "CPL1", "in")
        agc.connect("CPL1", "out", "PORT2", "p")
        agc.connect("CPL1", "cpl", "DET1", "in")
        agc.connect("CPL1", "iso", "TERM1", "p")
        agc.add_text("AGC module, 2.4 GHz: the step attenuator att follows p_in so the output stays at p_out (+5 dBm) "
                     "for inputs from -50 to -25 dBm; the coupler and detector close the loop", (-600, -120), 14)
    require_clean(agc, "agc_module")

    budget = agc.add_budget(start="PORT1", frequency="f0", power="p_in", bandwidth="10MHz",
                            spurs=False, name="Main path")
    outputs = []
    for p in (-50, -40, -30, -25):
        agc.variables = variables(str(p))
        result = agc.budget(budget)
        outputs.append(result["stages"][-1]["signal"])
    assert max(outputs) - min(outputs) < 1.0, outputs
    agc.variables = variables("-40")
    agc.add_budget(path=["PORT1", "A1", "ATT1", "A2", "A3", "F1", "CPL1.cpl", "DET1"], frequency="f0", power="p_in",
                   bandwidth="10MHz", spurs=False, name="Detector path")
    analysis = agc.add_analysis("1.5GHz", "3.5GHz", 401, noise=True, name="S-parameters")
    sp = agc.simulate(analysis)
    assert all(math.isfinite(v) for v in sp.measure("dB(S(2,1))"))
    project.save()
    set_plots(project, "agc_module", analysis, [
        ("Gain and match", "rectangular", ["dB(S(2,1))", "dB(S(1,1))", "dB(S(2,2))"], {}),
        ("Noise figure", "rectangular", ["NF"], {}),
    ])
    print("agc_module: output %s dBm for input -50/-40/-30/-25 dBm" % ", ".join("%.2f" % o for o in outputs))


EXAMPLES_BY_NAME = {"lowpass_filter": lowpass_filter, "lna_match": lna_match, "receiver": receiver,
                    "pll_receiver": pll_receiver, "downconverter": downconverter, "filter_bank": filter_bank,
                    "agc_module": agc_module}
# Arguments choose examples by name; none: all of them.
for name in (sys.argv[1:] or list(EXAMPLES_BY_NAME)):
    EXAMPLES_BY_NAME[name]()
lvdrfcad.close_project()
print("examples written to", EXAMPLES)
