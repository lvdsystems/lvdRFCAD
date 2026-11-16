"""Builds the SPICE example projects shipped with lvd RF cad and checks them.

Usage (from the repository root):
    lvdrfcad-cli tools/make_spice_examples.py <examples folder>

Creates, replacing earlier copies:
  opamp_amplifier  non-inverting amplifier (gain 11) with a function generator,
                   oscilloscope and Bode plotter; operating point, transient,
                   AC, Fourier and a stored Monte Carlo setup
  power_supply     half-wave rectifier, reservoir capacitor and 7805 regulator
                   with voltmeter, ammeter and oscilloscope; a load sweep
  bjt_amplifier    common-emitter stage with operating-point annotation, AC,
                   noise and a temperature sweep, plus the same circuit
                   imported from a SPICE netlist
"""
import math
import pathlib
import shutil
import sys

import lvdrfcad

root = pathlib.Path(sys.argv[1])


def fresh(name):
    folder = root / name
    shutil.rmtree(folder, ignore_errors=True)
    return lvdrfcad.new_project(folder, name)


class Sheet:
    """Small helpers on a schematic: parts, wires between pins, labels, grounds."""

    def __init__(self, schematic):
        self.s = schematic

    def part(self, key, at, ref, rotate=0, **parameters):
        cid = self.s.add_component(key, at, parameters={k: str(v) for k, v in parameters.items()}, reference=ref)
        if rotate:
            self.s.rotate(cid, at, rotate)
        return ref

    def pin(self, ref, pin):
        return self.s.pin_position(ref, pin)

    def wire(self, *points):
        self.s.add_wire([tuple(p) for p in points])

    def link(self, a, pa, b, pb):
        self.s.connect(a, pa, b, pb)

    def label(self, net, ref, pin):
        self.s.add_label(net, self.pin(ref, pin))

    def ground(self, ref, pin, dy=40):
        x, y = self.pin(ref, pin)
        gid = self.s.add_component("basic:ground", (x, y + dy))
        self.wire((x, y), (x, y + dy))
        return gid


def check(condition, message):
    if not condition:
        raise SystemExit("make_spice_examples: " + message)


# ---------------------------------------------------------------- op-amp amplifier
project = fresh("opamp_amplifier")
doc = project.new_document("spice", "amplifier")
amp = doc.schematic
s = Sheet(amp)
with amp.edit("Build amplifier"):
    s.part("spice-instruments:function_generator", (0, 0), "FG1", freq="1kHz", amplitude="0.2")
    s.part("spice-ics:opamp_tl072", (300, -50), "U1")
    s.part("basic:resistor", (170, 60), "RG", r="1k")
    s.part("basic:resistor", (300, 60), "RF", r="10k")
    s.part("basic:resistor", (450, -50), "RL", r="10k")
    s.part("spice-sources:vdc", (-260, 0), "VP", dc="15")
    s.part("spice-sources:vdc", (-160, 0), "VN", dc="15")
    s.part("spice-instruments:oscilloscope", (640, 140), "SC1", tdiv="200us", vdiv1="0.5", vdiv2="1")
    s.part("spice-instruments:bode_plotter", (860, 140), "BP1")
    # Signal path
    x, y = s.pin("FG1", "p")
    xi, yi = s.pin("U1", "inp")
    s.wire((x, y), (x, yi), (xi, yi))
    s.ground("FG1", "n")
    xn, yn = s.pin("U1", "inn")
    xg, yg = s.pin("RG", "n")
    xf, yf = s.pin("RF", "p")
    s.wire((xg, yg), (xf, yf))
    s.wire((220, yg), (220, yn), (xn, yn))
    s.ground("RG", "p")
    xo, yo = s.pin("U1", "out")
    xr, yr = s.pin("RF", "n")
    s.wire((xr, yr), (370, yr), (370, yo))
    s.wire((xo, yo), s.pin("RL", "p"))
    s.ground("RL", "n")
    s.label("in", "U1", "inp")
    s.label("out", "U1", "out")
    s.label("vcc", "U1", "vcc")
    s.label("vee", "U1", "vee")
    # Supplies
    s.label("vcc", "VP", "p")
    s.ground("VP", "n")
    xp, yp = s.pin("VN", "p")
    gid = amp.add_component("basic:ground", (xp, yp - 40))
    amp.rotate(gid, (xp, yp - 40), 180)
    s.wire((xp, yp), (xp, yp - 40))
    s.label("vee", "VN", "n")
    # Instruments
    s.label("in", "SC1", "ch1")
    s.label("out", "SC1", "ch2")
    s.ground("SC1", "ref")
    s.label("in", "BP1", "in")
    s.label("out", "BP1", "out")
    s.ground("BP1", "ref")
    amp.add_text("Non-inverting amplifier, gain 1 + RF/RG = 11", (-260, -130), 14)

problems = [p for p in amp.check() if p["severity"] == "error"]
check(not problems, f"amplifier rule check: {problems}")
amp.add_spice_analysis("op", name="Bias")
amp.add_spice_analysis("tran", name="Waveforms", stop="5ms", step="2us", maxStep="2us")
ac = amp.add_spice_analysis(
    "ac", name="Frequency response", start="10Hz", stop="10MHz", points="20",
    monteCarlo={"tolerances": [{"target": "R*.r", "tolerance": 0.01, "distribution": "gaussian"}],
                "runs": 100, "seed": 1,
                "measures": [{"expression": "db(v(out)/v(in))", "function": "max", "name": "Gain"}]})
amp.add_spice_analysis("fourier", name="Distortion", stop="5ms", step="2us", maxStep="2us",
                       fundamental="1kHz", harmonics="9", signals=["v(out)"])

bias = amp.spice("Bias")
check(abs(bias.value("v(vcc)") - 15) < 1e-9, "supply")
check(abs(bias.value("v(out)")) < 0.05, f"output offset {bias.value('v(out)')}")
response = amp.spice("Frequency response")
f, gain = response.evaluate("db(v(out)/v(in))")
check(abs(gain[0] - 20 * math.log10(11)) < 0.02, f"gain {gain[0]}")
corner = next(fr for fr, g in zip(f, gain) if g < gain[0] - 3)
check(150e3 < corner < 400e3, f"bandwidth {corner}")  # 3 MHz / 11
distortion = amp.spice("Distortion").fourier[0]
check(abs(distortion["harmonics"][0]["magnitude"] - 2.2) < 0.02, f"output amplitude {distortion}")
check(distortion["thd_percent"] < 0.1, f"THD {distortion['thd_percent']}")
mc = amp.spice_monte_carlo([{"target": "R*.r", "tolerance": 0.01}],
                           [{"expression": "db(v(out)/v(in))", "function": "max"}], runs=30, analysis=ac)
check(0.01 < mc["statistics"][0]["sigma"] < 0.2, f"Monte Carlo sigma {mc['statistics'][0]}")
project.save()
print(f"opamp_amplifier: gain {gain[0]:.2f} dB, -3 dB at {corner / 1e3:.0f} kHz, "
      f"THD {distortion['thd_percent']:.4f} %, Monte Carlo sigma {mc['statistics'][0]['sigma']:.3f} dB")

# ---------------------------------------------------------------- power supply
project = fresh("power_supply")
doc = project.new_document("spice", "supply")
psu = doc.schematic
s = Sheet(psu)
with psu.edit("Build supply"):
    s.part("spice-sources:vsin", (0, 0), "VAC", va="17", freq="50Hz", ac="0")
    s.part("spice-semiconductors:1n4007", (120, -30), "D1")
    s.part("basic:capacitor", (260, 40), "C1", rotate=90, c="2200uF")
    s.part("spice-ics:reg_7805", (420, -30), "U1")
    s.part("basic:capacitor", (560, 40), "C2", rotate=90, c="10uF")
    s.part("spice-instruments:ammeter", (660, -30), "AM1")
    s.part("basic:resistor", (780, 40), "RL", rotate=90, r="50")
    s.part("spice-instruments:voltmeter", (900, 40), "VM1")
    s.part("spice-instruments:oscilloscope", (420, 260), "SC1", tdiv="5ms", vdiv1="1", vdiv2="0.5",
           offset1="15", offset2="5", trigger="ch1", level="15.5")
    x, y = s.pin("VAC", "p")
    xa, ya = s.pin("D1", "a")
    s.wire((x, y), (x, ya), (xa, ya))
    s.ground("VAC", "n")
    xk, yk = s.pin("D1", "k")
    xc, yc = s.pin("C1", "p") if s.pin("C1", "p")[1] < s.pin("C1", "n")[1] else s.pin("C1", "n")
    xin, yin = s.pin("U1", "in")
    s.wire((xk, yk), (xin, yin))
    s.wire((xc, yc), (xc, yk))
    s.ground("C1", "n" if s.pin("C1", "n")[1] > s.pin("C1", "p")[1] else "p")
    s.ground("U1", "gnd")
    xo, yo = s.pin("U1", "out")
    xm, ym = s.pin("AM1", "p")
    s.wire((xo, yo), (xm, ym))
    top2 = "p" if s.pin("C2", "p")[1] < s.pin("C2", "n")[1] else "n"
    x2, y2 = s.pin("C2", top2)
    s.wire((x2, y2), (x2, yo))
    s.ground("C2", "n" if top2 == "p" else "p")
    topl = "p" if s.pin("RL", "p")[1] < s.pin("RL", "n")[1] else "n"
    xl, yl = s.pin("RL", topl)
    xn, yn = s.pin("AM1", "n")
    s.wire((xn, yn), (xl, yn), (xl, yl))
    s.ground("RL", "n" if topl == "p" else "p")
    s.label("rect", "U1", "in")
    s.label("vout", "RL", topl)
    s.label("vout", "VM1", "p")
    s.ground("VM1", "n")
    s.label("rect", "SC1", "ch1")
    s.label("vout", "SC1", "ch2")
    s.ground("SC1", "ref")
    psu.add_text("Half-wave rectifier and 7805 regulator, 100 mA load", (0, -150), 14)

problems = [p for p in psu.check() if p["severity"] == "error"]
check(not problems, f"supply rule check: {problems}")
tran = psu.add_spice_analysis(
    "tran", name="Start-up and ripple", stop="200ms", step="20us", maxStep="50us",
    sweep={"axes": [{"target": "RL.r", "mode": "list", "list": ["25", "50", "100"]}],
           "measures": [{"expression": "v(vout)", "function": "min", "from": "120ms", "name": "Minimum output"},
                        {"expression": "v(rect)", "function": "min", "from": "120ms", "name": "Minimum input"}]})
result = psu.spice(tran)
t, vout = result.evaluate("v(vout)", "tran")
late = [v for tt, v in zip(t, vout) if tt > 0.12]
check(4.9 < min(late) and max(late) < 5.1, f"regulated output {min(late)}..{max(late)}")
sweep = psu.spice_sweep([{"target": "RL.r", "mode": "list", "list": ["25", "50", "100"]}],
                        [{"expression": "v(rect)", "function": "min", "from": "120ms"}], analysis=tran)
mins = [r["measures"][0] for r in sweep["runs"]]
check(mins[0] < mins[1] < mins[2], f"heavier load, lower valley: {mins}")
project.save()
print(f"power_supply: output {min(late):.3f}..{max(late):.3f} V; valley of the input "
      f"{', '.join(f'{m:.2f}' for m in mins)} V at 25/50/100 ohm")

# ---------------------------------------------------------------- BJT amplifier
project = fresh("bjt_amplifier")
doc = project.new_document("spice", "common_emitter")
ce = doc.schematic
s = Sheet(ce)
with ce.edit("Build stage"):
    s.part("spice-sources:vdc", (-200, 100), "VCC", dc="12")
    s.part("spice-sources:vsin", (0, 200), "VIN", va="10m", freq="1kHz", ac="1m")
    s.part("basic:capacitor", (100, 100), "C1", c="10uF")
    s.part("basic:resistor", (180, 20), "R1", rotate=90, r="47k")
    s.part("basic:resistor", (180, 220), "R2", rotate=90, r="10k")
    s.part("spice-semiconductors:2n3904", (300, 100), "Q1")
    s.part("basic:resistor", (310, -40), "RC", rotate=90, r="4.7k")
    s.part("basic:resistor", (310, 220), "RE", rotate=90, r="1k")
    s.part("basic:capacitor", (420, 220), "CE", rotate=90, c="100uF")
    s.part("basic:capacitor", (450, 40), "CL", c="10uF")
    s.part("basic:resistor", (560, 120), "RL", rotate=90, r="100k")
    s.part("spice-misc:vprobe", (520, 0), "PR1")
    # Supply
    s.label("vcc", "VCC", "p")
    s.ground("VCC", "n")
    # Input
    x, y = s.pin("VIN", "p")
    xc, yc = s.pin("C1", "p")
    s.wire((x, y), (x, yc), (xc, yc))
    s.ground("VIN", "n")
    xb, yb = s.pin("Q1", "b")
    xc2, yc2 = s.pin("C1", "n")
    s.wire((xc2, yc2), (xb, yb))
    # Divider
    for r in ("R1", "R2"):
        a, b = s.pin(r, "p"), s.pin(r, "n")
        top, bottom = (a, b) if a[1] < b[1] else (b, a)
        if r == "R1":
            s.wire(bottom, (bottom[0], yb))
            s.s.add_label("vcc", top)
        else:
            s.wire(top, (top[0], yb))
            gid = ce.add_component("basic:ground", (bottom[0], bottom[1] + 40))
            s.wire(bottom, (bottom[0], bottom[1] + 40))
    # Collector and emitter
    for r, pin in (("RC", "c"), ("RE", "e")):
        a, b = s.pin(r, "p"), s.pin(r, "n")
        top, bottom = (a, b) if a[1] < b[1] else (b, a)
        xq, yq = s.pin("Q1", pin)
        if r == "RC":
            s.wire(bottom, (xq, yq))
            s.s.add_label("vcc", top)
        else:
            s.wire(top, (xq, yq))
            ce.add_component("basic:ground", (bottom[0], bottom[1] + 40))
            s.wire(bottom, (bottom[0], bottom[1] + 40))
    a, b = s.pin("CE", "p"), s.pin("CE", "n")
    top, bottom = (a, b) if a[1] < b[1] else (b, a)
    xe, ye = s.pin("Q1", "e")
    s.wire(top, (top[0], ye), (xe, ye))
    ce.add_component("basic:ground", (bottom[0], bottom[1] + 40))
    s.wire(bottom, (bottom[0], bottom[1] + 40))
    # Output
    xq, yq = s.pin("Q1", "c")
    xl, yl = s.pin("CL", "p")
    s.wire((xq, yq), (xq, yl), (xl, yl)) if yq != yl else s.wire((xq, yq), (xl, yl))
    a, b = s.pin("RL", "p"), s.pin("RL", "n")
    top, bottom = (a, b) if a[1] < b[1] else (b, a)
    xo, yo = s.pin("CL", "n")
    s.wire((xo, yo), (top[0], yo), top)
    ce.add_component("basic:ground", (bottom[0], bottom[1] + 40))
    s.wire(bottom, (bottom[0], bottom[1] + 40))
    s.s.add_label("out", top)
    xp, yp = s.pin("PR1", "p")
    s.wire((xp, yp), (xp, yo))
    s.s.add_label("in", (xc, yc))
    s.s.add_label("c", (xq, yq))
    ce.add_text("Common-emitter stage, 2N3904", (-200, -150), 14)

problems = [p for p in ce.check() if p["severity"] == "error"]
check(not problems, f"BJT rule check: {problems}")
ce.add_spice_analysis("op", name="Bias")
ce.add_spice_analysis("ac", name="Gain", start="10Hz", stop="100MHz", points="10",
                      sweep={"axes": [{"target": "temperature", "mode": "list", "list": ["-40", "27", "85"]}],
                             "measures": [{"expression": "db(v(out)/v(in))", "function": "max", "name": "Gain"}]})
ce.add_spice_analysis("noise", name="Noise", output="out", source="VIN", start="10Hz", stop="10MHz", points="10")
ce.add_spice_analysis("tran", name="Waveforms", stop="3ms", step="5us", maxStep="5us")
bias = ce.spice("Bias")
check(5 < bias.value("v(c)") < 8, f"collector {bias.value('v(c)')}")
gain = ce.spice("Gain").evaluate("db(v(out)/v(in))")[1]
check(40 < max(gain) < 50, f"mid-band gain {max(gain)}")
temperature = ce.spice_sweep([{"target": "temperature", "mode": "list", "list": ["-40", "27", "85"]}],
                             [{"expression": "db(v(out)/v(in))", "function": "max"}], analysis="Gain")
spread = [r["measures"][0] for r in temperature["runs"]]
check(max(spread) - min(spread) < 3, f"gain over temperature {spread}")

# The same stage as an imported netlist (simulation only).
cir = root / "bjt_amplifier" / "reference.cir"
shutil.copy(pathlib.Path(__file__).resolve().parent.parent / "src" / "spice" / "tests" / "reference" / "bjt_amplifier.cir", cir)
imported = project.import_spice_netlist(cir, "imported_netlist").schematic
check(len(imported.spice_analyses) == 5, "imported analyses")
check(imported.spice(imported.spice_analyses[0]["id"]).value("v(c)") > 0, "imported bias")
project.save()
print(f"bjt_amplifier: collector {bias.value('v(c)'):.2f} V, gain {max(gain):.1f} dB, "
      f"over -40/27/85 degC {', '.join(f'{g:.2f}' for g in spread)} dB")
