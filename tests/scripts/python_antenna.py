"""Antenna documents from Python: arrays, elements, libraries, metrics,
sweeps, budget, synthesis and pattern import."""

import math
import pathlib
import shutil
import sys

import lvdrfcad

root = pathlib.Path(sys.argv[1])
shutil.rmtree(root, ignore_errors=True)
project = lvdrfcad.new_project(root / "ant")

# Libraries
libraries = {lib["name"]: lib for lib in lvdrfcad.antenna_libraries()}
assert {"antenna-elements", "antenna-patterns", "antenna-tr-modules", "antenna-arrays"} <= set(libraries)
keys = {e["key"] for lib in libraries.values() for e in lib["entries"]}
assert "antenna-elements:patch-x-ro4003" in keys

# A uniform 16-element line of isotropic elements: textbook values.
doc = project.new_document("antenna", "line")
assert doc.element is None and doc.schematic is None
array = doc.array
array.update({"element": "", "inlineElement": {"model": "isotropic"},
              "geometry": {"kind": "linear", "nx": 16, "dx": 0.5}})
assert array.design["geometry"]["nx"] == 16
pattern = array.evaluate()
assert abs(pattern.gain(0) - 10 * math.log10(16)) < 1e-3
m = pattern.metrics(integrate=True)
assert abs(m["beamwidth"][0] - 6.35) < 0.05, m
assert abs(m["side_lobe_cuts"][0] + 13.15) < 0.1, m
assert abs(m["directivity"] - 12.04) < 0.05, m
cut = pattern.cut(phi=0, step=1.0)
assert len(cut["angle"]) == 361 and max(cut["gain"]) > 12
assert array.grating_lobe_free_scan(0) == 90

# Steering, squint and the band sweep.
array.update({"excitation": {"steerTheta": 30}, "sweeps": {"bandPoints": 5}})
band = array.band()
assert len(band) == 5 and band[0]["squint"] > 1, band[0]
array.update({"excitation": {"steering": "ttd"}})
assert max(p["squint"] for p in array.band()) < 0.05

# A planar AESA from the library, with a T/R module preset.
aesa = project.new_document("antenna", "aesa").array
aesa.use_library("antenna-arrays:ura-16x16-taylor")
aesa.use_library("antenna-elements:cos15")
aesa.use_library("antenna-tr-modules:x-gan-10w")
design = aesa.design
assert design["geometry"]["nx"] == 16 and design["aesa"]["phaseBits"] == 6
m = aesa.metrics()
assert m["side_lobe_cuts"][0] < -25, m
b = aesa.budget()
# 256 elements at 40 dBm minus 0.8 dB front loss: EIRP = 10 log(256^2 * 10 W * eta) + element gain.
assert 60 < b["eirp"] < 70, b
assert b["tx"]["origin"].startswith("preset"), b
scan = aesa.scan()
assert scan[len(scan) // 2]["gain"] > scan[0]["gain"] + 2  # scan loss
ideal = aesa.evaluate(ideal=True)
assert ideal.gain(0) >= aesa.evaluate().gain(0) - 0.5

# Synthesis writes custom weights.
line = project.document("line").array
line.update({"excitation": {"steerTheta": 0, "steering": "phase"}})
r = line.synthesise(sll=-25)
assert r["converged"] and r["side_lobe"] < -24.8, r
assert line.design["excitation"]["taperX"]["kind"] == "custom"
assert len(line.design["excitation"]["weights"]) == 16

# Elements: library models, pattern import and use by an array.
elem_doc = project.new_document("antenna-element", "horn")
element = elem_doc.element
assert elem_doc.array is None
element.use_library("antenna-elements:horn-x-15dbi")
assert 14 < element.directivity() < 17
source = pathlib.Path(libraries["antenna-patterns"]["path"]).parent / "patterns" / "sample-horn-x.csv"
info = lvdrfcad.read_pattern(source)
assert info["frequencies"] == [9.5e9, 10e9, 10.5e9] and info["has_phase"]
element.import_pattern(source)
assert element.design["element"]["model"] == "imported"
assert (root / "ant" / "patterns" / "sample-horn-x.csv").is_file()
assert 13 < element.directivity() < 17
assert element.gain(0) > element.gain(90) + 10

horn_array = project.new_document("antenna", "horn_array").array
horn_array.update({"element": "element:horn", "geometry": {"kind": "rectangular", "nx": 4, "ny": 4, "dx": 2.2, "dy": 2.2}})
p = horn_array.evaluate()
assert p.gain(0) > 25
assert horn_array.grating_lobe_free_scan(0) < 30

# Errors are Python exceptions.
for bad in (lambda: aesa.use_library("antenna-elements:nothing"),
            lambda: pattern.gain(0, component="sideways")):
    try:
        bad()
    except (KeyError, ValueError, RuntimeError):
        pass
    else:
        raise AssertionError("expected an exception")

project.save()
lvdrfcad.close_project()
reopened = lvdrfcad.open_project(root / "ant" / "ant.lvdproj")
assert reopened.document("aesa").array.design["geometry"]["nx"] == 16
print("python_antenna: ok")

# Horizon cut and steering mode none (DF arrays).
lvdrfcad.open_project(root / "ant" / "ant.lvdproj")
ring = lvdrfcad.project().new_document("antenna", "ring").array
ring.update({"element": "", "inlineElement": {"model": "monopole", "frequency": 4e8},
             "frequency": 4e8, "designFrequency": 4e8,
             "geometry": {"kind": "circular", "rings": [{"radius": 0.5, "count": 8, "rotation": 0}]},
             "excitation": {"steering": "phase", "steerTheta": 90, "steerPhi": 60}})
az = ring.evaluate().azimuth_cut(theta=90, step=1)
best = az["angle"][max(range(len(az["gain"])), key=lambda k: az["gain"][k])]
assert abs(best - 60) <= 1, best
print("python_antenna (DF): ok")

# Converting a lattice to a custom table keeps the pattern.
grid = lvdrfcad.project().new_document("antenna", "grid").array
grid.update({"geometry": {"kind": "triangular", "nx": 6, "ny": 5}, "excitation": {"steerTheta": 20}})
before = grid.evaluate().gain(20, 0)
grid.to_custom()
assert grid.design["geometry"]["kind"] == "custom" and len(grid.design["geometry"]["elements"]) == 30
assert abs(grid.evaluate().gain(20, 0) - before) < 1e-9
print("python_antenna (positions): ok")
