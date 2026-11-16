#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate lvd RF cad component libraries (.lvdlib).

Usage:
    python -I tools/validate_libraries.py [--render OUTDIR] [FILE ...]

Without FILE arguments every libraries/*.lvdlib of the repository is checked
(the repository root is found from this script's location). The checks follow
docs/file-format.md sections 3 and 4 and the block/element contract in
docs/libraries.md, including the SPICE model contract (template lines start
with the element letter and {ref}, placeholders name pins or parameters,
shared subcircuit text uses lvd_ names, probe kinds). Errors are printed as "file: component: field: message" and
the exit code is 1 if there is any error.

--render OUTDIR writes one SVG per library showing every symbol on a grid, with
pin connection points, pin names and the reference/value positions.

Standard library only.
"""

import argparse
import html
import json
import os
import re
import sys

FORMAT = "lvdrfcad-library"
VERSION = 1

ID_RE = re.compile(r"^[a-z0-9_]+$")
IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
PREFIX_RE = re.compile(r"^[A-Za-z]+$")
LIBNAME_RE = re.compile(r"^[A-Za-z0-9_.-]+$")

RESERVED = {
    "pi", "c0", "kB", "T0",
    "sqrt", "exp", "ln", "log10", "sin", "cos", "tan", "asin", "acos", "atan", "atan2",
    "sinh", "cosh", "tanh", "abs", "min", "max", "pow", "floor", "ceil", "round",
    "db10", "db20", "undb10", "undb20",
}

# Approximation of a number literal from section 3.2: digits, optional SI
# prefix and unit letters directly after or after one space, or European
# notation (4n7, 2k2, 1M5). Exponent notation is deliberately not accepted.
_PREFIX = r"(?:[Mm][Ee][Gg]|[afpnuµmkMGT])"
_UNIT = r"[A-Za-zµΩ°]*"
NUMBER_RE = re.compile(
    r"^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?: ?" + _UNIT + r")?$"
    r"|^[+-]?\d+" + _PREFIX + r"\d+" + _UNIT + r"$"
)

COMPONENT_KEYS = {"id", "name", "category", "description", "referencePrefix", "pins",
                  "parameters", "symbol", "models", "netName", "role"}
COMPONENT_REQUIRED = ["id", "name", "category", "referencePrefix", "pins", "symbol"]
PARAM_KEYS = {"name", "label", "unit", "default", "shown", "choices", "file", "text"}
SYMBOL_KEYS = {"graphics", "pins", "referenceAt", "valueAt"}
SYMBOL_PIN_KEYS = {"pin", "at", "length", "direction"}
DIRECTIONS = {"left", "right", "up", "down"}
FILLS = {"none", "outline", "background"}
ANCHORS = {"left", "center", "right"}
ROLES = {"sheet-port"}

# Fields per graphics primitive (all required; "type" is implied).
GRAPHICS = {
    "line": ["points"],
    "polygon": ["points", "fill"],
    "rect": ["from", "to", "fill"],
    "circle": ["center", "radius", "fill"],
    "arc": ["center", "radius", "startAngle", "spanAngle"],
    "text": ["at", "text", "size", "anchor"],
}

# Model kinds and their required fields (docs/file-format.md section 4.4).
MODEL_KINDS = {
    "rf-behavioural": ["block"],
    "rf-element": ["element"],
    "sparameters": ["file"],
    "spice": ["template"],
    "subschematic": ["document"],
    "python": [],
}
SPICE_INSTRUMENTS = {"oscilloscope", "voltmeter", "ammeter", "function-generator", "spectrum-analyzer", "bode-plotter"}

MODEL_OPTIONAL = {
    "rf-behavioural": set(),
    "rf-element": set(),
    "sparameters": set(),
    "spice": {"include", "subcircuit", "probe", "section", "instrument"},
    "subschematic": set(),
    "python": {"source", "file"},
}

# Contract with the solvers (docs/libraries.md, "Block and element types"):
# parameters each block or element reads. A component using the type must
# define all of them.
BLOCKS = {
    "amplifier": ["gain", "nf", "oip3", "oip2", "op1db", "isolation", "z0"],
    "attenuator": ["il", "z0"],
    "filter": ["type", "response", "order", "fc", "bw", "il", "ripple", "rejection", "z0"],
    "mixer": ["gain", "nf", "iip3", "lo_power", "lo_rf_isolation", "lo_if_isolation",
              "rf_if_isolation", "side", "product", "spur_table", "z0"],
    "source": ["freq", "power", "z0"],
    "lo": ["freq", "power", "phase_noise", "z0"],
    "splitter": ["il", "isolation", "imbalance", "phase_imbalance", "z0"],
    "combiner": ["il", "isolation", "imbalance", "phase_imbalance", "z0"],
    "coupler": ["coupling", "directivity", "il", "z0"],
    "switch_spdt": ["state", "il", "isolation", "z0"],
    "isolator": ["il", "isolation", "z0"],
    "circulator": ["il", "isolation", "z0"],
    "cable": ["length", "loss_per_m", "freq", "il", "vf", "z0"],
    "antenna": ["gain", "temp", "z0"],
    "multiplier": ["factor", "gain", "rejection", "z0"],
    "phase_shifter": ["phase", "il", "z0"],
    "adc": ["fs", "bits", "full_scale", "nf", "z0"],
    "port": ["num", "z0"],
    "termination": ["z0"],
}
BLOCK_PINS = {
    "amplifier": ["in", "out"], "attenuator": ["in", "out"], "filter": ["in", "out"],
    "mixer": ["rf", "lo", "if"], "source": ["out"], "lo": ["out"],
    "splitter": ["in", "out1", "out2"], "combiner": ["in1", "in2", "out"],
    "coupler": ["in", "out", "cpl", "iso"], "switch_spdt": ["com", "t1", "t2"],
    "isolator": ["in", "out"], "circulator": ["p1", "p2", "p3"], "cable": ["in", "out"],
    "antenna": ["p"], "multiplier": ["in", "out"], "phase_shifter": ["in", "out"],
    "adc": ["in"], "port": ["p"], "termination": ["p"],
}
ELEMENTS = {
    "resistor": ["r"],
    "capacitor": ["c", "q", "freq"],
    "inductor": ["l", "q", "freq"],
    "tline": ["z0", "elen", "freq"],
    "stub_open": ["z0", "elen", "freq"],
    "stub_short": ["z0", "elen", "freq"],
    "coupled_lines": ["z0e", "z0o", "elen", "freq"],
    "microstrip": ["w", "l", "h", "er", "tand", "t", "sigma"],
    "stripline": ["w", "l", "b", "er", "tand", "t", "sigma"],
    "coax": ["din", "dout", "l", "er", "tand", "sigma"],
    "cpw": ["w", "s", "l", "h", "er", "tand", "t", "sigma", "backed"],
    "transformer": ["n"],
}
ELEMENT_PINS = {
    "resistor": ["p", "n"], "capacitor": ["p", "n"], "inductor": ["p", "n"],
    "tline": ["in", "out"], "stub_open": ["p"], "stub_short": ["p"],
    "coupled_lines": ["p1", "p2", "p3", "p4"], "microstrip": ["in", "out"],
    "stripline": ["in", "out"], "coax": ["in", "out"], "cpw": ["in", "out"],
    "transformer": ["in", "out"],
}

PLACEHOLDER_RE = re.compile(r"\{([^{}]*)\}")

# SPICE model contract (docs/libraries.md, "SPICE libraries").
SPICE_PROBES = {"voltage", "differential", "current"}
SPICE_ELEMENT_RE = re.compile(r"^[A-Za-z]\{ref\}")
SPICE_CARD_RE = re.compile(r"^\.(model|subckt)\s+(\S+)", re.IGNORECASE)


def is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def is_num(v):
    return (isinstance(v, (int, float))) and not isinstance(v, bool)


def is_point(v):
    return isinstance(v, list) and len(v) == 2 and all(is_int(x) for x in v)


class Checker:
    def __init__(self):
        self.errors = 0

    def error(self, where, msg):
        self.errors += 1
        print("error: %s: %s" % (where, msg))

    # ------------------------------------------------------------ library
    def check_file(self, path):
        base = os.path.basename(path)
        try:
            with open(path, "r", encoding="utf-8") as f:
                lib = json.load(f)
        except (OSError, ValueError) as e:
            self.error(base, "cannot read JSON: %s" % e)
            return None
        if not isinstance(lib, dict):
            self.error(base, "top level must be an object")
            return None
        if lib.get("format") != FORMAT:
            self.error(base + ": format", "must be %r" % FORMAT)
        if lib.get("version") != VERSION or not is_int(lib.get("version")):
            self.error(base + ": version", "must be the integer %d" % VERSION)
        name = lib.get("name")
        stem = os.path.splitext(base)[0]
        if not isinstance(name, str) or not LIBNAME_RE.match(name):
            self.error(base + ": name", "missing or invalid library name")
        elif name != stem:
            self.error(base + ": name", "%r does not match the file base name %r" % (name, stem))
        if "description" in lib and not isinstance(lib["description"], str):
            self.error(base + ": description", "must be a string")
        for k in lib:
            if k not in {"format", "version", "name", "description", "components"}:
                self.error(base, "unknown top-level field %r" % k)
        comps = lib.get("components")
        if not isinstance(comps, list):
            self.error(base + ": components", "must be a list")
            return lib
        seen = set()
        for i, c in enumerate(comps):
            if not isinstance(c, dict):
                self.error("%s: components[%d]" % (base, i), "must be an object")
                continue
            cid = c.get("id")
            where = "%s: %s" % (base, cid if isinstance(cid, str) else "components[%d]" % i)
            if isinstance(cid, str):
                if cid in seen:
                    self.error(where + ": id", "duplicate component id")
                seen.add(cid)
            self.check_component(where, c)
        return lib

    # ------------------------------------------------------------ component
    def check_component(self, where, c):
        for k in COMPONENT_REQUIRED:
            if k not in c:
                self.error(where, "missing required field %r" % k)
        for k in c:
            if k not in COMPONENT_KEYS:
                self.error(where, "unknown field %r" % k)
        if "id" in c and (not isinstance(c["id"], str) or not ID_RE.match(c["id"])):
            self.error(where + ": id", "must match [a-z0-9_]+")
        for k in ("name", "category", "description"):
            if k in c and (not isinstance(c[k], str) or not c[k].strip()):
                self.error(where + ": " + k, "must be a non-empty string")
        if isinstance(c.get("category"), str):
            if any(not part.strip() for part in c["category"].split("/")):
                self.error(where + ": category", "empty path element")
        if "referencePrefix" in c and (not isinstance(c["referencePrefix"], str)
                                       or not PREFIX_RE.match(c["referencePrefix"])):
            self.error(where + ": referencePrefix", "must be letters only")

        # pins
        pins = []
        if isinstance(c.get("pins"), list):
            if not c["pins"]:
                self.error(where + ": pins", "a component needs at least one pin")
            for j, p in enumerate(c["pins"]):
                if not isinstance(p, dict) or not isinstance(p.get("name"), str) or not p["name"]:
                    self.error("%s: pins[%d]" % (where, j), "must be an object with a non-empty name")
                    continue
                for k in p:
                    if k != "name":
                        self.error("%s: pins[%d]" % (where, j), "unknown field %r" % k)
                if p["name"] in pins:
                    self.error(where + ": pins", "duplicate pin name %r" % p["name"])
                pins.append(p["name"])
        elif "pins" in c:
            self.error(where + ": pins", "must be a list")

        # parameters
        params = []
        plist = c.get("parameters", [])
        if not isinstance(plist, list):
            self.error(where + ": parameters", "must be a list")
            plist = []
        for j, p in enumerate(plist):
            pw = "%s: parameter %s" % (where, p.get("name") if isinstance(p, dict) else j)
            if not isinstance(p, dict):
                self.error(pw, "must be an object")
                continue
            self.check_parameter(pw, p)
            n = p.get("name")
            if isinstance(n, str):
                if n in params:
                    self.error(pw, "duplicate parameter name")
                params.append(n)

        # symbol
        if isinstance(c.get("symbol"), dict):
            self.check_symbol(where + ": symbol", c["symbol"], pins)
        elif "symbol" in c:
            self.error(where + ": symbol", "must be an object")

        # models
        models = c.get("models", [])
        if not isinstance(models, list):
            self.error(where + ": models", "must be a list")
            models = []
        kinds = set()
        for j, m in enumerate(models):
            mw = "%s: models[%d]" % (where, j)
            if not isinstance(m, dict):
                self.error(mw, "must be an object")
                continue
            k = m.get("kind")
            if k in kinds:
                self.error(mw, "more than one model of kind %r" % k)
            kinds.add(k)
            self.check_model(mw, m, pins, params)

        # netName / role
        if "netName" in c:
            if not isinstance(c["netName"], str) or not c["netName"]:
                self.error(where + ": netName", "must be a non-empty string")
            if len(pins) != 1:
                self.error(where + ": netName", "a netName component must have exactly one pin")
        if "role" in c:
            if c["role"] not in ROLES:
                self.error(where + ": role", "unknown role %r" % c["role"])
            elif c["role"] == "sheet-port":
                if len(pins) != 1:
                    self.error(where + ": role", "a sheet port must have exactly one pin")
                if "name" not in params:
                    self.error(where + ": role", "a sheet port needs a 'name' parameter")

    # ------------------------------------------------------------ parameter
    def check_parameter(self, pw, p):
        for k in p:
            if k not in PARAM_KEYS:
                self.error(pw, "unknown field %r" % k)
        n = p.get("name")
        if not isinstance(n, str) or not IDENT_RE.match(n):
            self.error(pw + ": name", "must be an identifier [A-Za-z_][A-Za-z0-9_]*")
        elif n in RESERVED:
            self.error(pw + ": name", "%r is a reserved constant or function name" % n)
        elif n == "ref":
            self.error(pw + ": name", "'ref' is reserved for the reference in model templates")
        if not isinstance(p.get("label"), str) or not p["label"].strip():
            self.error(pw + ": label", "missing or empty")
        if "unit" in p and not isinstance(p["unit"], str):
            self.error(pw + ": unit", "must be a string")
        for k in ("shown", "file", "text"):
            if k in p and not isinstance(p[k], bool):
                self.error(pw + ": " + k, "must be true or false")
        kinds = [k for k in ("choices", "file", "text") if p.get(k) not in (None, False)]
        if len(kinds) > 1:
            self.error(pw, "only one of choices, file and text may be given")
        d = p.get("default")
        if not isinstance(d, str):
            self.error(pw + ": default", "missing or not a string")
            return
        if "choices" in p:
            ch = p["choices"]
            if (not isinstance(ch, list) or not ch or not all(isinstance(x, str) and x for x in ch)
                    or len(set(ch)) != len(ch)):
                self.error(pw + ": choices", "must be a non-empty list of distinct non-empty strings")
            elif d not in ch:
                self.error(pw + ": default", "%r is not one of the choices" % d)
        elif p.get("file") or p.get("text"):
            pass  # any string, including empty
        elif not d.strip():
            self.error(pw + ": default", "empty default")
        elif not NUMBER_RE.match(d.strip()):
            self.error(pw + ": default", "%r does not look like a number with optional prefix/unit" % d)

    # ------------------------------------------------------------ symbol
    def check_symbol(self, sw, s, pins):
        for k in s:
            if k not in SYMBOL_KEYS:
                self.error(sw, "unknown field %r" % k)
        for k in ("referenceAt", "valueAt"):
            if k in s and not is_point(s[k]):
                self.error(sw + ": " + k, "must be an integer point [x, y]")
        g = s.get("graphics")
        if not isinstance(g, list):
            self.error(sw + ": graphics", "missing or not a list")
            g = []
        for j, item in enumerate(g):
            self.check_graphic("%s: graphics[%d]" % (sw, j), item)
        sp = s.get("pins")
        if not isinstance(sp, list):
            self.error(sw + ": pins", "missing or not a list")
            return
        used = {}
        points = {}
        for j, p in enumerate(sp):
            pw = "%s: pins[%d]" % (sw, j)
            if not isinstance(p, dict):
                self.error(pw, "must be an object")
                continue
            for k in p:
                if k not in SYMBOL_PIN_KEYS:
                    self.error(pw, "unknown field %r" % k)
            name = p.get("pin")
            if name not in pins:
                self.error(pw + ": pin", "%r is not a pin of the component" % (name,))
            elif name in used:
                self.error(pw + ": pin", "pin %r appears more than once in the symbol" % name)
            else:
                used[name] = True
            at = p.get("at")
            if not is_point(at):
                self.error(pw + ": at", "must be an integer point [x, y]")
            else:
                if at[0] % 10 or at[1] % 10:
                    self.error(pw + ": at", "%r is not on the 10-unit grid" % (at,))
                key = tuple(at)
                if key in points:
                    self.error(pw + ": at", "same connection point as pin %r" % points[key])
                points[key] = name
            if p.get("direction") not in DIRECTIONS:
                self.error(pw + ": direction", "must be one of left, right, up, down")
            ln = p.get("length")
            if not is_int(ln) or ln < 0:
                self.error(pw + ": length", "must be a non-negative integer")
        for name in pins:
            if name not in used:
                self.error(sw + ": pins", "pin %r has no symbol pin" % name)

    def check_graphic(self, gw, item):
        if not isinstance(item, dict):
            self.error(gw, "must be an object")
            return
        t = item.get("type")
        if t not in GRAPHICS:
            self.error(gw + ": type", "unknown graphics type %r" % (t,))
            return
        fields = GRAPHICS[t]
        for k in item:
            if k != "type" and k not in fields:
                self.error(gw, "field %r does not belong to a %s" % (k, t))
        for k in fields:
            if k not in item:
                self.error(gw, "%s needs field %r" % (t, k))
        if "points" in item:
            pts = item["points"]
            need = 3 if t == "polygon" else 2
            if not isinstance(pts, list) or len(pts) < need or not all(is_point(q) for q in pts):
                self.error(gw + ": points", "needs at least %d integer points" % need)
        for k in ("from", "to", "center", "at"):
            if k in item and not is_point(item[k]):
                self.error(gw + ": " + k, "must be an integer point [x, y]")
        if "radius" in item and (not is_num(item["radius"]) or item["radius"] <= 0):
            self.error(gw + ": radius", "must be a positive number")
        for k in ("startAngle", "spanAngle"):
            if k in item and not is_num(item[k]):
                self.error(gw + ": " + k, "must be a number of degrees")
        if "spanAngle" in item and is_num(item["spanAngle"]) and item["spanAngle"] == 0:
            self.error(gw + ": spanAngle", "must not be zero")
        if "fill" in item and item["fill"] not in FILLS:
            self.error(gw + ": fill", "must be none, outline or background")
        if "anchor" in item and item["anchor"] not in ANCHORS:
            self.error(gw + ": anchor", "must be left, center or right")
        if "size" in item and (not is_num(item["size"]) or item["size"] <= 0):
            self.error(gw + ": size", "must be a positive number")
        if "text" in item and (not isinstance(item["text"], str) or not item["text"]):
            self.error(gw + ": text", "must be a non-empty string")

    # ------------------------------------------------------------ model
    def check_placeholders(self, mw, text, pins, params):
        for ph in PLACEHOLDER_RE.findall(text):
            if ph.startswith("pin:"):
                if ph[4:] not in pins:
                    self.error(mw, "placeholder {%s} refers to an unknown pin" % ph)
            elif ph != "ref" and ph not in params:
                self.error(mw, "placeholder {%s} refers to an unknown parameter" % ph)

    def check_model(self, mw, m, pins, params):
        k = m.get("kind")
        if k not in MODEL_KINDS:
            self.error(mw + ": kind", "unknown model kind %r" % (k,))
            return
        allowed = {"kind"} | set(MODEL_KINDS[k]) | MODEL_OPTIONAL[k]
        for f in m:
            if f not in allowed:
                self.error(mw, "unknown field %r for a %s model" % (f, k))
        for f in MODEL_KINDS[k]:
            if k == "spice" and f == "template":
                continue  # checked in check_spice: may be empty for probes
            if not isinstance(m.get(f), str) or not m[f]:
                self.error(mw, "%s model needs a non-empty %r" % (k, f))
        if k == "rf-behavioural" and isinstance(m.get("block"), str):
            self.check_type(mw, "block", m["block"], BLOCKS, BLOCK_PINS, pins, params)
        elif k == "rf-element" and isinstance(m.get("element"), str):
            self.check_type(mw, "element", m["element"], ELEMENTS, ELEMENT_PINS, pins, params)
        elif k == "sparameters" and isinstance(m.get("file"), str):
            self.check_placeholders(mw + ": file", m["file"], [], params)
        elif k == "spice":
            self.check_spice(mw, m, pins, params)
        elif k == "python":
            if not (isinstance(m.get("source"), str) or isinstance(m.get("file"), str)):
                self.error(mw, "python model needs 'source' or 'file'")

    def check_spice(self, mw, m, pins, params):
        """SPICE model contract: template lines, placeholders, shared text and probe kind."""
        probe = m.get("probe")
        if "instrument" in m and m["instrument"] not in SPICE_INSTRUMENTS:
            self.error(mw + ": instrument", "must be one of %s" % ", ".join(sorted(SPICE_INSTRUMENTS)))
        if "probe" in m and probe not in SPICE_PROBES:
            self.error(mw + ": probe", "must be one of %s" % ", ".join(sorted(SPICE_PROBES)))
        for f in ("include", "subcircuit"):
            if f in m and not isinstance(m[f], str):
                self.error(mw + ": " + f, "must be a string")
        t = m.get("template")
        if not isinstance(t, str):
            self.error(mw, "spice model needs a 'template' string")
            return
        if not t:
            if "probe" not in m and "instrument" not in m:
                self.error(mw + ": template", "may be empty only for a probe or an instrument")
        else:
            tw = mw + ": template"
            for n, ln in enumerate(t.split("\n"), 1):
                lw = "%s line %d" % (tw, n)
                if not ln.strip():
                    self.error(lw, "empty line")
                elif ln != ln.strip():
                    self.error(lw, "leading or trailing white space")
                elif ln.startswith("."):
                    mc = SPICE_CARD_RE.match(ln)
                    if mc and "{ref}" not in mc.group(2):
                        self.error(lw, "per-instance .%s name %r must contain {ref}; put shared definitions "
                                   "in 'subcircuit'" % (mc.group(1).lower(), mc.group(2)))
                elif not SPICE_ELEMENT_RE.match(ln):
                    self.error(lw, "an element line must start with the SPICE element letter followed by {ref}")
            self.check_placeholders(tw, t, pins, params)
        if probe == "current" and isinstance(t, str) and not t:
            self.error(mw + ": template", "a current probe needs a 0 V source template")
        sc = m.get("subcircuit")
        if isinstance(sc, str):
            sw = mw + ": subcircuit"
            if re.search(r"\{(ref|pin:[^{}]*)\}", sc):
                self.error(sw, "shared text is emitted verbatim: it cannot use {ref} or {pin:...}")
            depth = 0
            for n, ln in enumerate(sc.split("\n"), 1):
                s = ln.strip()
                low = s.lower()
                mc = SPICE_CARD_RE.match(s)
                if mc and depth == 0 and not mc.group(2).lower().startswith("lvd_"):
                    self.error("%s line %d" % (sw, n), "shared name %r must start with 'lvd_'" % mc.group(2))
                if low.startswith(".subckt"):
                    depth += 1
                elif low.startswith(".ends"):
                    depth -= 1
                    if depth < 0:
                        self.error("%s line %d" % (sw, n), ".ends without .subckt")
                        depth = 0
            if depth:
                self.error(sw, ".subckt without .ends")

    def check_type(self, mw, what, name, table, pin_table, pins, params):
        if name not in table:
            self.error(mw + ": " + what, "unknown %s type %r (see docs/libraries.md)" % (what, name))
            return
        for p in table[name]:
            if p not in params:
                self.error(mw, "%s %r reads parameter %r, which the component does not define"
                           % (what, name, p))
        if pins != pin_table[name]:
            self.error(mw, "%s %r expects pins %s in this order, component has %s"
                       % (what, name, pin_table[name], pins))


# ---------------------------------------------------------------- rendering
CELL_W, CELL_H, COLS, SCALE = 180, 170, 5, 2


def _shown_text(comp):
    parts = []
    for p in comp.get("parameters", []):
        if isinstance(p, dict) and p.get("shown"):
            parts.append("%s=%s" % (p.get("name"), p.get("default")))
    return " ".join(parts)


def _arc_path(cx, cy, r, start, span):
    import math
    # angles counter-clockwise on screen, Y down: screen point = (cx + r cos, cy - r sin)
    a0 = math.radians(start)
    a1 = math.radians(start + span)
    x0, y0 = cx + r * math.cos(a0), cy - r * math.sin(a0)
    x1, y1 = cx + r * math.cos(a1), cy - r * math.sin(a1)
    large = 1 if abs(span) > 180 else 0
    sweep = 0 if span > 0 else 1  # SVG sweep=1 is clockwise on screen
    return "M %.2f %.2f A %g %g 0 %d %d %.2f %.2f" % (x0, y0, r, r, large, sweep, x1, y1)


def render_svg(lib, path):
    comps = [c for c in lib.get("components", []) if isinstance(c, dict)]
    rows = max(1, (len(comps) + COLS - 1) // COLS)
    w, h = COLS * CELL_W, rows * CELL_H + 30
    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">'
           % (w * SCALE, h * SCALE, w, h),
           '<rect width="100%" height="100%" fill="white"/>',
           '<text x="6" y="18" font-family="sans-serif" font-size="14" font-weight="bold">%s</text>'
           % html.escape(str(lib.get("name", "")))]
    for i, c in enumerate(comps):
        ox = (i % COLS) * CELL_W + CELL_W // 2
        oy = 30 + (i // COLS) * CELL_H + CELL_H // 2 - 10
        out.append('<g transform="translate(%d,%d)">' % (ox, oy))
        out.append('<rect x="%d" y="%d" width="%d" height="%d" fill="none" stroke="#ddd"/>'
                   % (-CELL_W // 2, -CELL_H // 2 + 10 - 10, CELL_W, CELL_H))
        for gx in range(-80, 81, 10):
            for gy in range(-60, 61, 10):
                out.append('<circle cx="%d" cy="%d" r="0.6" fill="#ccc"/>' % (gx, gy))
        out.append('<line x1="-4" y1="0" x2="4" y2="0" stroke="#f99" stroke-width="0.5"/>'
                   '<line x1="0" y1="-4" x2="0" y2="4" stroke="#f99" stroke-width="0.5"/>')
        sym = c.get("symbol", {}) if isinstance(c.get("symbol"), dict) else {}
        for g in sym.get("graphics", []):
            if not isinstance(g, dict):
                continue
            t = g.get("type")
            fill = {"outline": "black", "background": "#ffffe0"}.get(g.get("fill"), "none")
            st = 'stroke="black" stroke-width="1" fill="%s"' % fill
            try:
                if t == "line":
                    pts = " ".join("%d,%d" % tuple(p) for p in g["points"])
                    out.append('<polyline points="%s" stroke="black" stroke-width="1" fill="none"/>' % pts)
                elif t == "polygon":
                    pts = " ".join("%d,%d" % tuple(p) for p in g["points"])
                    out.append('<polygon points="%s" %s/>' % (pts, st))
                elif t == "rect":
                    (x0, y0), (x1, y1) = g["from"], g["to"]
                    out.append('<rect x="%d" y="%d" width="%d" height="%d" %s/>'
                               % (min(x0, x1), min(y0, y1), abs(x1 - x0), abs(y1 - y0), st))
                elif t == "circle":
                    out.append('<circle cx="%d" cy="%d" r="%g" %s/>'
                               % (g["center"][0], g["center"][1], g["radius"], st))
                elif t == "arc":
                    out.append('<path d="%s" stroke="black" stroke-width="1" fill="none"/>'
                               % _arc_path(g["center"][0], g["center"][1], g["radius"],
                                           g["startAngle"], g["spanAngle"]))
                elif t == "text":
                    anchor = {"left": "start", "center": "middle", "right": "end"}.get(g.get("anchor"), "middle")
                    out.append('<text x="%d" y="%d" font-family="sans-serif" font-size="%g" '
                               'text-anchor="%s" dominant-baseline="central">%s</text>'
                               % (g["at"][0], g["at"][1], g.get("size", 10), anchor, html.escape(g["text"])))
            except (KeyError, TypeError, ValueError):
                pass
        dirs = {"left": (-1, 0), "right": (1, 0), "up": (0, -1), "down": (0, 1)}
        for p in sym.get("pins", []):
            try:
                x, y = p["at"]
                dx, dy = dirs[p["direction"]]
                ln = p.get("length", 10)
                out.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="#036" stroke-width="1"/>'
                           % (x, y, x + dx * ln, y + dy * ln))
                out.append('<circle cx="%d" cy="%d" r="1.8" fill="#c00"/>' % (x, y))
                lx, ly = x - dx * 3 + (3 if dx == 0 else 0), y - dy * 3 - (3 if dy == 0 else 0)
                anchor = "end" if dx > 0 else ("start" if dx < 0 else "start")
                out.append('<text x="%d" y="%d" font-family="sans-serif" font-size="5" fill="#c00" '
                           'text-anchor="%s">%s</text>' % (lx, ly, anchor, html.escape(str(p["pin"]))))
            except (KeyError, TypeError, ValueError):
                pass
        if is_point(sym.get("referenceAt")):
            x, y = sym["referenceAt"]
            out.append('<text x="%d" y="%d" font-family="sans-serif" font-size="8" fill="#060" '
                       'text-anchor="middle" dominant-baseline="central">%s1</text>'
                       % (x, y, html.escape(str(c.get("referencePrefix", "?")))))
        if is_point(sym.get("valueAt")):
            x, y = sym["valueAt"]
            out.append('<text x="%d" y="%d" font-family="sans-serif" font-size="7" fill="#00a" '
                       'text-anchor="middle" dominant-baseline="central">%s</text>'
                       % (x, y, html.escape(_shown_text(c))))
        out.append('<text x="0" y="%d" font-family="sans-serif" font-size="8" text-anchor="middle" '
                   'fill="#555">%s</text>' % (CELL_H // 2 - 4, html.escape(str(c.get("id", "")))))
        out.append('</g>')
    out.append('</svg>')
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


# ---------------------------------------------------------------- main
def main(argv=None):
    ap = argparse.ArgumentParser(description="Validate lvd RF cad .lvdlib component libraries")
    ap.add_argument("files", nargs="*", help="library files (default: libraries/*.lvdlib)")
    ap.add_argument("--render", metavar="OUTDIR", help="write one SVG per library into OUTDIR")
    args = ap.parse_args(argv)

    files = args.files
    if not files:
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        libdir = os.path.join(root, "libraries")
        files = sorted(os.path.join(libdir, f) for f in os.listdir(libdir) if f.endswith(".lvdlib")) \
            if os.path.isdir(libdir) else []
        if not files:
            print("error: no .lvdlib files found in %s" % libdir)
            return 1

    chk = Checker()
    ncomp = 0
    for path in files:
        lib = chk.check_file(path)
        if isinstance(lib, dict) and isinstance(lib.get("components"), list):
            ncomp += len(lib["components"])
            if args.render:
                os.makedirs(args.render, exist_ok=True)
                stem = os.path.splitext(os.path.basename(path))[0]
                out = os.path.join(args.render, stem + ".svg")
                render_svg(lib, out)
                print("rendered %s" % out)
    print("%d libraries, %d components, %d errors" % (len(files), ncomp, chk.errors))
    return 1 if chk.errors else 0


if __name__ == "__main__":
    sys.exit(main())
