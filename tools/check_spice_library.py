#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Simulate every SPICE model of the lvd RF cad component libraries with ngspice.

Usage:
    python -I tools/check_spice_library.py [--ngspice EXE] [--keep DIR] [--verbose] [ID ...]

For every component with a {"kind": "spice"} model in libraries/*.lvdlib, the
template is rendered with the default parameter values as the netlister does
(docs/libraries.md, "SPICE libraries"): {ref} becomes the reference prefix
plus "1", {pin:x} the node of the test bench, expression parameters plain
numbers, text and choice parameters verbatim; the shared 'subcircuit' text is
emitted once. The part is placed in a small test bench for its kind and run
with the console ngspice in batch mode: operating point, a short transient and,
where meaningful, an AC sweep. A run fails on ngspice errors, warnings that are
not known to be harmless, singular matrices, timestep or convergence failures,
unknown models or subcircuits, failed measurements, and on numeric checks (for
example the 7805 output, the op-amp closed-loop gain, diode forward drops).

ID arguments restrict the run to components whose "<library>:<id>" or id
matches. Netlists go to a temporary folder (or --keep DIR). Exit code 1 on any
failure. Standard library only.
"""

import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_NGSPICE = os.path.join(ROOT, "build", "_deps", "ngspice_console-src", "bin", "ngspice_con.exe")

# ---------------------------------------------------------------- numbers
_PREFIX = {"a": 1e-18, "f": 1e-15, "p": 1e-12, "n": 1e-9, "u": 1e-6, "µ": 1e-6, "m": 1e-3,
           "k": 1e3, "M": 1e6, "G": 1e9, "T": 1e12}
_NUM_RE = re.compile(r"^([+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?) ?(.*)$")
_EURO_RE = re.compile(r"^([+-]?\d+)([afpnuµmkMGT]|[Mm][Ee][Gg])(\d+)(.*)$")


def _scale(rest):
    if rest[:3].lower() == "meg":
        return 1e6
    if rest and rest[0] in _PREFIX:
        return _PREFIX[rest[0]]
    return 1.0


def parse_number(text):
    """Number syntax of docs/file-format.md 3.2: SI prefix (m milli, M mega, meg), unit ignored, 4n7."""
    s = text.strip()
    m = _EURO_RE.match(s)
    if m:
        return float(m.group(1) + "." + m.group(3)) * _scale(m.group(2))
    m = _NUM_RE.match(s)
    if not m:
        raise ValueError("not a number: %r" % text)
    return float(m.group(1)) * _scale(m.group(2))


def spice_number(v):
    return "%.12g" % v


# ---------------------------------------------------------------- rendering
PH_RE = re.compile(r"\{([^{}]*)\}")


def spice_model(comp):
    for m in comp.get("models", []):
        if isinstance(m, dict) and m.get("kind") == "spice":
            return m
    return None


def render(comp, model, nodes, overrides=None):
    """Return (element lines, shared text) for one instance with default parameters."""
    values = {}
    for p in comp.get("parameters", []):
        v = (overrides or {}).get(p["name"], p["default"])
        if p.get("text") or p.get("choices") or p.get("file"):
            values[p["name"]] = v
        else:
            values[p["name"]] = spice_number(parse_number(v))
    ref = comp["referencePrefix"] + "1"

    def sub(mo):
        key = mo.group(1)
        if key == "ref":
            return ref
        if key.startswith("pin:"):
            return nodes[key[4:]]
        return values[key]

    text = PH_RE.sub(sub, model.get("template", ""))
    return text, model.get("subcircuit", ""), ref


# ---------------------------------------------------------------- test benches
# A bench gives the DUT pin nodes, extra circuit lines, control lines and
# checks {name: (lo, hi)} on values the control lines print as "name = value".

def tran_ac(tstep="10u", tstop="2m", ac=True, node="out"):
    c = ["tran %s %s" % (tstep, tstop)]
    if ac:
        c.append("ac dec 10 10 10meg")
    return c


def op_print(checks):
    lines = ["op"]
    for name, expr in checks:
        lines.append("let %s = %s" % (name, expr))
        lines.append("print %s" % name)
    return lines


def bench_for(lib, comp):
    cid = comp["id"]
    pins = [p["name"] for p in comp["pins"]]
    key = "%s:%s" % (lib, cid)

    # ---- basic passives
    if key == "basic:resistor":
        return dict(nodes={"p": "in", "n": "0"}, circuit=["Vs in 0 DC 1 AC 1"],
                    control=op_print([("c_i", "-i(vs)")]) + tran_ac(), checks={"c_i": (0.99e-3, 1.01e-3)})
    if key in ("basic:capacitor", "basic:inductor"):
        return dict(nodes={"p": "out", "n": "0"}, circuit=["Vs in 0 DC 1 AC 1 SIN(0 1 100meg)", "Rs in out 50"],
                    control=["op", "tran 0.1n 50n", "ac dec 10 1meg 10g"], checks={})

    # ---- probes
    m = spice_model(comp)
    if m.get("probe") == "voltage":
        return dict(nodes={"p": "in"}, circuit=["Vs in 0 DC 1", "Rl in 0 1k"], control=["op"], checks={})
    if m.get("probe") == "differential":
        return dict(nodes={"p": "in", "n": "mid"}, circuit=["Vs in 0 DC 1", "R1 in mid 1k", "R2 mid 0 1k"],
                    control=["op"], checks={})
    if m.get("probe") == "current":
        return dict(nodes={"p": "in", "n": "out"}, circuit=["Vs in 0 DC 5", "Rl out 0 1k"],
                    control=op_print([("c_i", "i(v%s1)" % comp["referencePrefix"].lower())]),
                    checks={"c_i": (4.99e-3, 5.01e-3)})

    if lib == "spice-sources":
        if pins == ["p", "n"]:
            ctl = ["op", "tran 10u 3m"] + (["ac dec 10 10 1meg"] if cid in ("vdc", "vsin", "vac", "vsrc", "idc", "isin", "isrc") else [])
            checks = {}
            if cid == "vdc":
                ctl = op_print([("c_v", "v(out)")]) + ctl[1:]
                checks = {"c_v": (4.999, 5.001)}
            if cid == "idc":
                # 1 mA leaves at n into the 1k load
                ctl = op_print([("c_v", "v(out)")]) + ctl[1:]
                checks = {"c_v": (0.999, 1.001)}
                return dict(nodes={"p": "0", "n": "out"}, circuit=["Rl out 0 1k"], control=ctl, checks=checks)
            if cid in ("isin", "ipulse", "isrc"):
                return dict(nodes={"p": "0", "n": "out"}, circuit=["Rl out 0 1k"], control=ctl, checks={})
            if cid == "vac":
                ctl += ["ac dec 10 10 1meg", "meas ac c_ac find vm(out) at=1k"]
                checks = {"c_ac": (0.999, 1.001)}
            if cid == "vpulse":
                ctl += ["meas tran c_hi find v(out) at=0.25m", "meas tran c_lo find v(out) at=0.75m"]
                checks = {"c_hi": (4.99, 5.01), "c_lo": (-0.01, 0.01)}
            if cid == "vsin":
                ctl.insert(2, "meas tran c_max max v(out)")  # right after tran, before ac
                checks = {"c_max": (0.99, 1.01)}
            if cid == "bvsrc":
                ctl += ["meas tran c_max max v(out)"]
                checks = {"c_max": (4.99, 5.01)}
            return dict(nodes={"p": "out", "n": "0"}, circuit=["Rl out 0 1k"], control=ctl, checks=checks)
        if pins == ["p", "n", "cp", "cn"]:
            if cid in ("cccs", "ccvs"):
                circuit = ["Vin in 0 DC 1 AC 1", "Rc in c 1k", "Rl out 0 1k"]
                nodes = {"p": "out", "n": "0", "cp": "c", "cn": "0"}
                # F: 10 * 1 mA flows p->n through the source, out of the load: -10 V; H: 1k * 1 mA = 1 V
                exp = (-10.01, -9.99) if cid == "cccs" else (0.999, 1.001)
            else:
                circuit = ["Vin in 0 DC 1 AC 1", "Rl out 0 1k"]
                nodes = {"p": "out", "n": "0", "cp": "in", "cn": "0"}
                exp = (9.99, 10.01) if cid == "vcvs" else (-1.001, -0.999)
            return dict(nodes=nodes, circuit=circuit,
                        control=op_print([("c_v", "v(out)")]) + ["ac dec 10 10 1meg", "tran 10u 1m"],
                        checks={"c_v": exp})

    if lib == "spice-semiconductors":
        cat = comp["category"]
        if pins == ["a", "k"]:
            name = comp["name"].lower()
            if "zener" in name or cid == "zener":
                return dict(nodes={"a": "0", "k": "k"}, circuit=["Vs in 0 DC 12 AC 1", "Rs in k 1.38k"],
                            control=op_print([("c_vz", "v(k)")]) + tran_ac(),
                            checks={"c_vz": (4.95, 5.25)})
            if cid.startswith("led"):
                return dict(nodes={"a": "a", "k": "0"}, circuit=["Vs in 0 DC 5 AC 1", "Rs in a 160"],
                            control=op_print([("c_vf", "v(a)")]) + tran_ac(), checks={"c_vf": (1.8, 2.1)})
            schottky = cid in ("1n5819", "bat54")
            return dict(nodes={"a": "a", "k": "0"}, circuit=["Vs in 0 DC 5 AC 1 SIN(5 1 1k)", "Rs in a 4.4k"],
                        control=op_print([("c_vf", "v(a)")]) + tran_ac(),
                        checks={"c_vf": (0.1, 0.4) if schottky else (0.5, 0.8)})
        p_type = bool(re.search(r"(^p|pnp|tip32|2n3906|bc557|bss84)", cid))
        if cat.endswith("BJT"):
            power = cid.startswith("tip")
            if not p_type:
                circ = ["Vcc vcc 0 DC 12", "R1 vcc b %s" % ("1k" if power else "100k"), "R2 b 0 %s" % ("220" if power else "22k"),
                        "Rc vcc c %s" % ("47" if power else "4.7k"), "Re e 0 %s" % ("10" if power else "1k"),
                        "Vin in 0 DC 0 AC 1 SIN(0 10m 1k)", "Cin in b 10u"]
                chk = ("c_vc", "v(c)")
            else:
                circ = ["Vcc vcc 0 DC 12", "R1 vcc b %s" % ("220" if power else "22k"), "R2 b 0 %s" % ("1k" if power else "100k"),
                        "Rc c 0 %s" % ("47" if power else "4.7k"), "Re vcc e %s" % ("10" if power else "1k"),
                        "Vin in 0 DC 0 AC 1 SIN(0 10m 1k)", "Cin in b 10u"]
                chk = ("c_vc", "v(c)")
            return dict(nodes={"c": "c", "b": "b", "e": "e"}, circuit=circ,
                        control=op_print([chk]) + tran_ac(), checks={"c_vc": (1.0, 11.0)})
        if cat.endswith("MOSFET"):
            power = cid == "irf540n"
            if not p_type:
                vg = {"nmos": "2", "2n7000": "3", "bss138": "2.2", "irf540n": "5"}[cid]
                circ = ["Vdd vdd 0 DC 12", "Vg gb 0 DC %s" % vg, "Vin in 0 DC 0 AC 1 SIN(0 10m 1k)", "Rg in g 1k",
                        "Eg g in gb 0 1", "Rd vdd d %s" % ("4" if power else ("4.7k" if cid == "nmos" else "100")),
                        "Rs s 0 %s" % ("1" if power else "1")]
            else:
                vg = {"pmos": "10", "bss84": "9"}[cid]
                circ = ["Vdd vdd 0 DC 12", "Vg gb 0 DC %s" % vg, "Vin in 0 DC 0 AC 1 SIN(0 10m 1k)", "Rg in g 1k",
                        "Eg g in gb 0 1", "Rd d 0 %s" % ("4.7k" if cid == "pmos" else "47"), "Rs vdd s 10"]
            return dict(nodes={"d": "d", "g": "g", "s": "s"}, circuit=circ,
                        control=op_print([("c_vd", "v(d)")]) + tran_ac(), checks={"c_vd": (0.5, 11.5)})
        if cat.endswith("JFET"):
            if not p_type:
                circ = ["Vdd vdd 0 DC 12", "Rd vdd d 1k", "Rs s 0 330", "Rg g 0 1meg", "Vin in 0 DC 0 AC 1 SIN(0 10m 1k)", "Cin in g 1u"]
            else:
                circ = ["Vdd vdd 0 DC 12", "Rd d 0 1k", "Rs vdd s 330", "Rg g vdd 1meg", "Vin in 0 DC 0 AC 1 SIN(0 10m 1k)", "Cin in g 1u"]
            return dict(nodes={"d": "d", "g": "g", "s": "s"}, circuit=circ,
                        control=op_print([("c_vd", "v(d)")]) + tran_ac(), checks={"c_vd": (0.5, 11.5)})

    if lib == "spice-ics":
        cat = comp["category"]
        if cat.endswith("Op-amps"):
            # non-inverting amplifier, gain 1 + 10k/1k = 11
            rrio = cid == "opamp_rrio"
            circ = ["Vin in 0 DC 0.1 AC 1 SIN(0.1 0.1 1k)", "Rf out fb 10k", "Rg fb 0 1k", "Rl out 0 10k"]
            nodes = {"inp": "in", "inn": "fb", "out": "out"}
            if "vcc" in pins:
                circ += ["Vp vcc 0 DC %s" % ("2.5" if rrio else "15"), "Vn vee 0 DC %s" % ("-2.5" if rrio else "-15")]
                nodes.update(vcc="vcc", vee="vee")
            ctl = op_print([("c_out", "v(out)")]) + ["ac dec 10 10 100meg", "meas ac c_g100 find vdb(out) at=100",
                                                      "tran 10u 3m", "meas tran c_max max v(out) from=1m to=3m"]
            checks = {"c_out": (1.089, 1.111), "c_g100": (20.6, 21.0), "c_max": (2.15, 2.25)}
            if "vcc" in pins:
                # saturation: input 2 V wants 22 V, output must stay inside the rails
                ctl += ["alter vin dc=2", "op", "let c_sat = v(out)", "print c_sat"]
                checks["c_sat"] = (1.0, 2.5) if rrio else (10.0, 15.0)
            return dict(nodes=nodes, circuit=circ, control=ctl, checks=checks)
        if cat.endswith("Comparators"):
            circ = ["Vcc vcc 0 DC 5", "Vin in 0 DC 0 PULSE(0 5 0 1m 1m 1u 2m)", "Vref ref 0 DC 2.5"]
            if "lm393" in cid:
                circ.append("Rpu vcc out 10k")
            else:
                circ.append("Rl out 0 10k")
            return dict(nodes={"inp": "in", "inn": "ref", "out": "out", "vcc": "vcc", "vee": "0"}, circuit=circ,
                        control=op_print([("c_low", "v(out)")]) +
                        ["tran 1u 2m", "meas tran c_high find v(out) at=0.9m", "meas tran c_low2 find v(out) at=1.9m",
                         "meas tran c_vth find v(in) when v(out)=2.5 rise=1"],
                        checks={"c_low": (-0.01, 0.3), "c_high": (4.5, 5.01), "c_low2": (-0.01, 0.3), "c_vth": (2.4, 2.65)})
        if cat.endswith("Regulators"):
            if cid == "tl431":
                return dict(nodes={"k": "k", "a": "0", "r": "k"}, circuit=["Vs in 0 DC 12 AC 1", "Rs in k 1k"],
                            control=op_print([("c_v", "v(k)")]) + tran_ac(), checks={"c_v": (2.49, 2.51)})
            if cid == "reg_lm317":
                circ = ["Vin in 0 DC 9 AC 1 PWL(0 0 1m 9)", "R1 out adj 240", "R2 adj 0 720", "Rl out 0 100", "Co out 0 1u"]
                return dict(nodes={"in": "in", "adj": "adj", "out": "out"}, circuit=circ,
                            control=op_print([("c_v", "v(out)")]) + tran_ac(tstop="3m"), checks={"c_v": (4.98, 5.10)})
            vout = parse_number(next(p["default"] for p in comp["parameters"] if p["name"] == "vout"))
            vin = vout + 4
            rl = vout / 0.1
            circ = ["Vin in 0 DC %g AC 1 PWL(0 0 1m %g)" % (vin, vin), "Rl out 0 %g" % rl, "Co out 0 1u"]
            return dict(nodes={"in": "in", "gnd": "0", "out": "out"}, circuit=circ,
                        control=op_print([("c_v", "v(out)")]) + tran_ac(tstop="3m") +
                        ["alter vin dc=%g" % (vout / 2), "op", "let c_drop = v(out)", "print c_drop"],
                        checks={"c_v": (vout * 0.99, vout * 1.01), "c_drop": (-0.01, vout / 2)})
        if cat.endswith("Logic"):
            return logic_bench(cid, pins)

    if lib == "spice-misc":
        cat = comp["category"]
        if cid == "potentiometer":
            return dict(nodes={"a": "vcc", "w": "w", "b": "0"}, circuit=["Vs vcc 0 DC 5 AC 1"],
                        control=op_print([("c_w", "v(w)")]) + tran_ac(), checks={"c_w": (2.49, 2.51)})
        if pins == ["p1", "n1", "p2", "n2"] and cat.startswith("Passive"):
            circ = ["Vs in 0 DC 0 AC 1 SIN(0 1 10k)", "Rs in p1 1", "Rl out 0 1k"]
            n = {"coupled_inductors": 1.0, "transformer": 2.0, "ideal_transformer": 2.0}.get(cid, 1.0)
            return dict(nodes={"p1": "p1", "n1": "0", "p2": "out", "n2": "0"}, circuit=circ,
                        control=["op", "tran 1u 300u", "ac dec 10 100 1meg", "meas ac c_gain find vm(out) at=10k"],
                        checks={"c_gain": (0.9 * n, 1.02 * n)})
        if cid == "crystal":
            return dict(nodes={"a": "a", "b": "b"}, circuit=["Vs in 0 DC 0 AC 1", "Rs in a 50", "Rl b 0 50"],
                        control=["op", "ac lin 2001 9.9meg 10.1meg", "meas ac c_peak max vm(b)",
                                 "meas ac c_fs when vm(b)=c_peak", "tran 1n 2u"],
                        checks={"c_fs": (9.99e6, 10.02e6), "c_peak": (0.3, 0.5)})
        if cid == "vswitch":
            return dict(nodes={"p": "out", "n": "0", "cp": "ctl", "cn": "0"},
                        circuit=["Vs vcc 0 DC 5", "Rs vcc out 1k", "Vc ctl 0 DC 0 PULSE(0 5 0.5m 1u 1u 1m 2m)"],
                        control=op_print([("c_off", "v(out)")]) + ["tran 1u 2m", "meas tran c_on find v(out) at=1m"],
                        checks={"c_off": (4.9, 5.01), "c_on": (0, 0.01)})
        if cid == "iswitch":
            return dict(nodes={"p": "out", "n": "0", "cp": "c", "cn": "0"},
                        circuit=["Vs vcc 0 DC 5", "Rs vcc out 1k", "Vc ctl 0 DC 0 PULSE(0 5 0.5m 1u 1u 1m 2m)", "Rc ctl c 1k"],
                        control=op_print([("c_off", "v(out)")]) + ["tran 1u 2m", "meas tran c_on find v(out) at=1m"],
                        checks={"c_off": (4.9, 5.01), "c_on": (0, 0.01)})
        if cid in ("tswitch_close", "tswitch_open"):
            closes = cid == "tswitch_close"
            return dict(nodes={"p": "out", "n": "0"}, circuit=["Vs vcc 0 DC 5", "Rs vcc out 1k"],
                        control=["op", "tran 1u 2m", "meas tran c_before find v(out) at=0.5m",
                                 "meas tran c_after find v(out) at=1.5m"],
                        checks={"c_before": (4.9, 5.01) if closes else (0, 0.01),
                                "c_after": (0, 0.01) if closes else (4.9, 5.01)})
        if cat.startswith("Transmission"):
            stop = "100n"
            return dict(nodes={"p1": "a", "n1": "0", "p2": "b", "n2": "0"},
                        circuit=["Vs in 0 DC 1 AC 1 PULSE(0 1 0 100p 100p 20n 40n)", "Rs in a 50", "Rl b 0 50"],
                        control=op_print([("c_v", "v(b)")]) + ["tran 0.05n %s" % stop, "ac dec 10 1meg 100meg"],
                        checks={"c_v": (0.45, 0.5001)})
    return None


GATE_FN = {
    "inv": lambda a: not a, "buf": lambda a: a, "schmitt": lambda a: not a,
    "and2": lambda a, b: a and b, "nand2": lambda a, b: not (a and b), "or2": lambda a, b: a or b,
    "nor2": lambda a, b: not (a or b), "xor2": lambda a, b: a != b,
}


def logic_bench(cid, pins):
    fn = GATE_FN[cid[len("logic_"):]]
    inputs = [p for p in pins if p != "y"]
    circ = ["Rl y 0 10k"]
    ctl = []
    checks = {}
    for i in inputs:
        circ.append("V%s %s 0 DC 0 PULSE(0 5 0 10n 10n %s %s)" % (i, i, "1u" if i == "a" else "2u", "2u" if i == "a" else "4u"))
    combos = [(0,), (1,)] if len(inputs) == 1 else [(0, 0), (0, 1), (1, 0), (1, 1)]
    for n, combo in enumerate(combos):
        for i, v in zip(inputs, combo):
            ctl.append("alter v%s dc=%d" % (i, 5 * v))
        ctl += ["op", "let c_tt%d = v(y)" % n, "print c_tt%d" % n]
        # 10k load on 50 ohm output: high level about 4.975 V
        checks["c_tt%d" % n] = (4.9, 5.01) if fn(*[bool(x) for x in combo]) else (-0.01, 0.1)
    ctl += ["tran 1n 8u", "ac dec 10 1k 100meg"]
    return dict(nodes={p: p for p in pins}, circuit=circ, control=ctl, checks=checks)


def smoke_bench(comp):
    pins = [p["name"] for p in comp["pins"]]
    circ = ["Vs drv 0 DC 1 AC 1", "Rdrv drv %s 1k" % ("n_" + pins[0])]
    circ += ["Rg_%s n_%s 0 1k" % (p, p) for p in pins]
    return dict(nodes={p: "n_" + p for p in pins}, circuit=circ, control=["op", "tran 1u 100u"], checks={})


# ---------------------------------------------------------------- running
ERROR_PATTERNS = [
    re.compile(p, re.IGNORECASE) for p in (
        r"\berror\b", r"singular matrix", r"timestep too small", r"unknown (model|subckt|subcircuit|device)",
        r"could not find", r"no convergence", r"not converge", r"\bfailed\b", r"\babort", r"can't find",
        r"unable to find", r"undefined", r"\bwarning\b",
    )
]
# Messages known to be harmless.
BENIGN = [
    re.compile(p, re.IGNORECASE) for p in (
        r"has no dc value, transient time 0 value used",
        r"dc value used for op instead of transient time=0 value",
    )
]
RESULT_RE = re.compile(r"^\s*(c_\w+)\s*=\s*([-+0-9.eE]+)")


def run_one(ngspice, workdir, lib, comp, bench, verbose):
    model = spice_model(comp)
    text, shared, ref = render(comp, model, bench["nodes"])
    title = "%s:%s" % (lib, comp["id"])
    lines = ["* lvd RF cad library check: " + title]
    if shared:
        lines.append(shared)
    lines += bench["circuit"]
    lines.append("* --- device under test")
    if text:
        lines.append(text)
    lines += [".control", "set noaskquit"] + bench["control"] + [".endc", ".end"]
    netlist = "\n".join(lines) + "\n"
    path = os.path.join(workdir, re.sub(r"[^A-Za-z0-9_.-]", "_", title) + ".cir")
    with open(path, "w", encoding="utf-8") as f:
        f.write(netlist)
    try:
        proc = subprocess.run([ngspice, "-b", path], capture_output=True, text=True, timeout=120,
                              cwd=workdir, errors="replace")
        out = proc.stdout + proc.stderr
    except subprocess.TimeoutExpired:
        return ["timeout"], {}
    problems = []
    for ln in out.splitlines():
        if any(b.search(ln) for b in BENIGN):
            continue
        if any(p.search(ln) for p in ERROR_PATTERNS):
            problems.append(ln.strip())
    results = {}
    for ln in out.splitlines():
        m = RESULT_RE.match(ln)
        if m:
            try:
                results[m.group(1).lower()] = float(m.group(2))
            except ValueError:
                pass
    for name, (lo, hi) in bench["checks"].items():
        v = results.get(name)
        if v is None:
            problems.append("%s: no result" % name)
        elif not (lo <= v <= hi) or math.isnan(v):
            problems.append("%s = %.6g outside [%g, %g]" % (name, v, lo, hi))
    if verbose or problems:
        print("---- %s (%s)" % (title, path))
        if verbose:
            print(netlist)
        for k, v in sorted(results.items()):
            print("   %s = %.6g" % (k, v))
    return problems, results


def main(argv=None):
    ap = argparse.ArgumentParser(description="Simulate the SPICE models of the component libraries with ngspice")
    ap.add_argument("ids", nargs="*", help="only components whose id or library:id matches")
    ap.add_argument("--ngspice", default=DEFAULT_NGSPICE, help="console ngspice executable")
    ap.add_argument("--keep", metavar="DIR", help="write netlists into DIR and keep them")
    ap.add_argument("--verbose", action="store_true", help="print every netlist and result")
    args = ap.parse_args(argv)

    if not os.path.isfile(args.ngspice):
        print("error: ngspice not found at %s (use --ngspice)" % args.ngspice)
        return 1
    libdir = os.path.join(ROOT, "libraries")
    files = sorted(f for f in os.listdir(libdir) if f.endswith(".lvdlib"))
    if args.keep:
        os.makedirs(args.keep, exist_ok=True)
        workdir = args.keep
    else:
        workdir = tempfile.mkdtemp(prefix="lvd_spice_check_")
    total = failed = smoke = 0
    failures = []
    try:
        for fn in files:
            with open(os.path.join(libdir, fn), encoding="utf-8") as f:
                lib = json.load(f)
            lname = lib.get("name", fn[:-7])
            for comp in lib.get("components", []):
                if spice_model(comp) is None:
                    continue
                key = "%s:%s" % (lname, comp["id"])
                if args.ids and comp["id"] not in args.ids and key not in args.ids:
                    continue
                total += 1
                try:
                    bench = bench_for(lname, comp)
                    if bench is None:
                        smoke += 1
                        print("note: %s: no dedicated test bench, running a smoke test" % key)
                        bench = smoke_bench(comp)
                    problems, _ = run_one(args.ngspice, workdir, lname, comp, bench, args.verbose)
                except (KeyError, ValueError) as e:
                    problems = ["cannot render template: %s" % e]
                if problems:
                    failed += 1
                    failures.append(key)
                    for p in problems:
                        print("FAIL %s: %s" % (key, p))
                else:
                    print("ok   %s" % key)
    finally:
        if not args.keep:
            shutil.rmtree(workdir, ignore_errors=True)
    print("%d components simulated, %d failed, %d smoke-tested only" % (total, failed, smoke))
    if failures:
        print("failed: " + ", ".join(failures))
    return 1 if failed or total == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
