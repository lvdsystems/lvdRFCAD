"""Phase 1 exit criterion from Python: a hierarchical schematic drawn from
library parts produces a correct netlist, and survives save and reopen.

Run by ctest through lvdrfcad-cli with a scratch folder as the only argument.
"""
import math
import pathlib
import shutil
import sys

import lvdrfcad

root = pathlib.Path(sys.argv[1])
shutil.rmtree(root, ignore_errors=True)


def expect_error(call, kind=Exception):
    try:
        call()
    except kind:
        return
    raise AssertionError("expected " + kind.__name__)


# Libraries and expressions
assert {"basic", "rf-system", "rf-circuit"} <= set(lvdrfcad.libraries())
amp_info = lvdrfcad.component_info("rf-system:amplifier")
assert amp_info["pins"] == ["in", "out"]
assert "gain" in [p["name"] for p in amp_info["parameters"]]
assert math.isclose(lvdrfcad.evaluate("2.4GHz"), 2.4e9)
assert math.isclose(lvdrfcad.evaluate("4n7"), 4.7e-9)
assert lvdrfcad.evaluate("g + 1", {"g": 2}) == 3
assert lvdrfcad.format_value(2.4e9, "Hz") == "2.4 GHz"
expect_error(lambda: lvdrfcad.evaluate("1 +"), ValueError)

project = lvdrfcad.new_project(root / "receiver")

# Sub-sheet: an amplifier stage with a variable gain.
stage = project.new_document("rf", "stage").schematic
stage.variables = [("g", "10")]
with stage.edit("Build stage"):
    stage.add_component("basic:sheet_port", (-100, 0), parameters={"name": "in"})
    stage.add_component("basic:sheet_port", (150, 0), parameters={"name": "out"})
    stage.add_component("rf-system:amplifier", (0, 0), parameters={"gain": "g", "nf": "1.2"})
    stage.connect("P1", "p", "A1", "in")
    stage.connect("A1", "out", "P2", "p")
assert stage.parameter("A1", "gain") == "g"
assert stage.parameter("A1", "oip3") == amp_info["parameters"][2]["default"]

# One undo step for the whole block.
assert len(stage.components()) == 3
stage.undo()
assert stage.components() == []
stage.redo()
assert len(stage.components()) == 3

# Top level: source -> filter -> X1 -> X2 -> termination.
top_doc = project.new_document("rf", "top")
top = top_doc.schematic
top.variables = {"f0": "2.4GHz", "boost": "20"}
top.add_component("rf-system:source", (-300, 0), parameters={"freq": "f0", "power": "-40"})
top.add_component("rf-system:filter", (-150, 0), parameters={"fc": "f0", "bw": "100MHz"})
top.add_component("sheet:stage", (0, 0), parameters={"g": "boost"})
top.add_component("sheet:stage", (200, 0))
top.add_component("rf-system:termination", (400, 0))
top.connect("SRC1", "out", "F1", "in")
top.connect("F1", "out", "X1", "in")
top.connect("X1", "out", "X2", "in")
top.connect("X2", "out", "TERM1", "p")
top.add_label("RF_OUT", top.pin_position("TERM1", "p"))

expect_error(lambda: top.add_component("rf-system:nothing", (0, 0)), KeyError)
expect_error(lambda: top.add_component("rf-system:amplifier", (0, 0), parameters={"bogus": 1}), KeyError)
assert len(top.components()) == 5  # the failed calls left nothing behind

nets = top.nets()
assert nets["RF_OUT"] == ["TERM1.p", "X2.out"], nets
assert top.net_of("X1", "out") == top.net_of("X2", "in")

netlist = top.netlist()
assert netlist["errors"] == [], netlist["text"]
by_path = {c["path"]: c for c in netlist["components"]}
assert sorted(by_path) == ["F1", "SRC1", "TERM1", "X1/A1", "X2/A1"], sorted(by_path)
assert by_path["X1/A1"]["values"]["gain"] == 20
assert by_path["X2/A1"]["values"]["gain"] == 10
assert math.isclose(by_path["F1"]["values"]["fc"], 2.4e9)
assert by_path["F1"]["parameters"]["type"] == "bandpass"  # an enumeration stays text
assert by_path["X1/A1"]["pins"]["out"] == by_path["X2/A1"]["pins"]["in"]
assert by_path["X2/A1"]["pins"]["out"] == "RF_OUT"
assert by_path["SRC1"]["pins"]["out"] == by_path["F1"]["pins"]["in"]

problems = [p for p in top.check() if p["severity"] != "info"]
assert problems == [], problems

# Save, reopen, compare.
text_before = netlist["text"]
project.save()
path = project.path
lvdrfcad.close_project()
project = lvdrfcad.open_project(path)
top = project.document("top").schematic
assert top.netlist()["text"] == text_before

# Rule check catches an unconnected part.
top.add_component("rf-system:amplifier", (0, 300))
messages = [p["message"] for p in top.check()]
assert any("not connected" in m for m in messages), messages
top.remove("A1")
assert all("A1" != c["reference"] for c in top.components())

print("python_schematic: all checks passed")
