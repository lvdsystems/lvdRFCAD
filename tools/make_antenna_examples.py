"""Builds the antenna example projects shipped with lvd RF cad and checks them.

Usage (from the repository root):
    lvdrfcad-cli tools/make_antenna_examples.py <examples folder>

Creates, replacing earlier copies:
  aesa_radar        X-band radar AESA: patch element document, circular
                    aperture on a triangular lattice, 6-bit T/R modules with
                    errors and failures, transmit and receive chains linked
                    from RF schematics (EIRP and G/T)
  satcom_panel      Ku-band receive panel with RHCP elements and sub-array
                    true-time delay against phase steering (beam squint)
  beamforming       pattern synthesis to a stepped mask, two simultaneous
                    beams with nulls, and a monopulse difference pattern
  element_patterns  element documents: patch, dipole over ground, GNSS RHCP
                    patch and a measured horn pattern imported from a file
  horn_amplitude_steering
                    ten squinted 17 GHz horns on a line and on an arc, fed
                    without steering phases: amplitude steering and
                    amplitude-comparison direction finding (with a script)
  circular_df       UHF direction finding with monopoles: a four-element
                    Adcock (Watson-Watt) and an eight-element circular array
                    scanned in azimuth (with a script)
"""
import math
import pathlib
import shutil
import sys

import lvdrfcad

root = pathlib.Path(sys.argv[1])
LIBRARIES = {lib["name"]: lib for lib in lvdrfcad.antenna_libraries()}
PATTERNS = pathlib.Path(LIBRARIES["antenna-patterns"]["path"]).parent / "patterns"
SCRIPTS = pathlib.Path(__file__).resolve().parent / "example_scripts"


def fresh(name):
    folder = root / name
    shutil.rmtree(folder, ignore_errors=True)
    return lvdrfcad.new_project(folder, name)


def chain(schematic, parts, name, frequency, power, title):
    """A straight RF chain: parts are (key, reference, parameters) left to right."""
    with schematic.edit("Build " + name):
        x = -600
        for key, ref, parameters in parts:
            schematic.add_component(key, (x, 0), parameters={k: str(v) for k, v in parameters.items()}, reference=ref)
            x += 200
        for (ka, ra, _), (kb, rb, _) in zip(parts, parts[1:]):
            out_pin = "p" if ka.endswith(":antenna") else "out"
            in_pin = "p" if kb.endswith((":antenna", ":termination")) else "in"
            schematic.connect(ra, out_pin, rb, in_pin)
        schematic.add_text(title, (-600, -110), 14)
    problems = [p["message"] for p in schematic.check() if p["severity"] != "info"]
    assert not problems, (name, problems)
    return schematic.add_budget(frequency=frequency, power=power, bandwidth="10MHz", spurs=False, name=name)


# ---------------------------------------------------------------------------
# 1. X-band radar AESA

def aesa_radar():
    project = fresh("aesa_radar")
    rx = project.new_document("rf", "rx_channel").schematic
    rx_budget = chain(rx, [
        ("rf-system:antenna", "ANT1", {"gain": 0, "temp": 150}),
        ("rf-system:attenuator", "LIM1", {"il": 0.6}),
        ("rf-system:amplifier", "LNA1", {"gain": 24, "nf": 1.6, "oip3": 25, "op1db": 15}),
        ("rf-system:phase_shifter", "PS1", {"phase": 0, "il": 6}),
        ("rf-system:attenuator", "ATT1", {"il": 2}),
        ("rf-system:amplifier", "AMP1", {"gain": 14, "nf": 4, "oip3": 28, "op1db": 18}),
        ("rf-system:termination", "R1", {}),
    ], "Rx channel", "10GHz", "-100", "Receive channel of one T/R module: limiter, LNA, phase shifter, attenuator, driver")
    rx_result = rx.budget(rx_budget)
    tx = project.new_document("rf", "tx_channel").schematic
    tx_budget = chain(tx, [
        ("rf-system:source", "SRC1", {"freq": "10GHz", "power": 0}),
        ("rf-system:phase_shifter", "PS1", {"phase": 0, "il": 6}),
        ("rf-system:amplifier", "DRV1", {"gain": 22, "nf": 5, "oip3": 35, "op1db": 25}),
        ("rf-system:amplifier", "HPA1", {"gain": 25, "nf": 6, "oip3": 52, "op1db": 42}),
        ("rf-system:isolator", "CIR1", {"il": 0.4}),
        ("rf-system:termination", "R1", {}),
    ], "Tx channel", "10GHz", "0", "Transmit channel of one T/R module: phase shifter, driver, GaN HPA, circulator")
    tx_result = tx.budget(tx_budget)

    element = project.new_document("antenna-element", "patch").element
    element.use_library("antenna-elements:patch-x-ro4003")
    element.update({"notes": "Patch for 10 GHz on RO4003C, 0.813 mm; the AESA uses it as its element."})

    aesa = project.new_document("antenna", "aesa").array
    aesa.use_library("antenna-arrays:aesa-circular-tri")
    aesa.use_library("antenna-tr-modules:x-gan-10w")
    aesa.update({
        "element": "element:patch",
        "frequency": 10e9, "designFrequency": 10e9,
        "excitation": {"steerTheta": 30, "steerPhi": 0},
        "aesa": {"failedFraction": 0.02, "seed": 3},
        "trModule": {"tx": {"source": "linked", "document": "tx_channel"},
                     "rx": {"source": "linked", "document": "rx_channel"},
                     "frontLoss": 0.8, "beamformerLoss": 6, "antennaTemperature": 150},
        "sweeps": {"scanPlane": 0, "scanMax": 60, "scanStep": 2},
    })
    m = aesa.metrics()
    b = aesa.budget()
    project.save()
    assert b["tx"]["origin"].startswith("tx_channel") and b["rx"]["origin"].startswith("rx_channel"), b
    print("aesa_radar: %d elements, gain %.1f dBi at %.1f deg, SLL %.1f dB, EIRP %.1f dBW, G/T %.1f dB/K "
          "(Rx chain NF %.2f dB, Tx output %.1f dBm)" % (
              len(aesa.evaluate().weights), m["peak_gain"], m["peak_theta"], m["side_lobe"], b["eirp"], b["g_over_t"],
              rx_result["stages"][-1]["cumulative_nf"], tx_result["stages"][-1]["signal"]))


# ---------------------------------------------------------------------------
# 2. Ku-band satcom receive panel

def satcom_panel():
    project = fresh("satcom_panel")
    for name, steering in (("panel_phase", "phase"), ("panel_subarray_ttd", "subarray-ttd")):
        panel = project.new_document("antenna", name).array
        panel.use_library("antenna-tr-modules:ku-satcom")
        panel.update({
            "element": "",
            "inlineElement": {"model": "cosn", "exponent": 1.3, "polarisation": "rhcp", "frequency": 11.7e9,
                              "efficiency": -0.6, "backLobe": -25},
            "frequency": 11.7e9, "designFrequency": 11.7e9,
            "geometry": {"kind": "rectangular", "unit": "lambda", "nx": 32, "ny": 32, "dx": 0.55, "dy": 0.55,
                         "aperture": "none"},
            "excitation": {"taperX": {"kind": "cosine-pedestal", "pedestal": -10},
                           "taperY": {"kind": "cosine-pedestal", "pedestal": -10},
                           "steerTheta": 45, "steerPhi": 0, "steering": steering, "subarrayX": 4, "subarrayY": 4},
            "trModule": {"antennaTemperature": 60, "frontLoss": 0.4, "beamformerLoss": 3},
            "sweeps": {"bandStart": 10.7e9, "bandStop": 12.75e9, "bandPoints": 21, "scanMax": 60},
        })
        band = panel.band()
        squint = max(p["squint"] for p in band)
        m = panel.metrics(co="x")
        print("satcom_panel/%s: gain %.1f dBi, axial ratio %.2f dB, worst squint %.2f deg over 10.7-12.75 GHz" % (
            name, m["peak_gain"], m["axial_ratio"], squint))
    project.save()


# ---------------------------------------------------------------------------
# 3. Beamforming: synthesis, multiple beams and nulls, monopulse

def beamforming():
    project = fresh("beamforming")
    line = project.new_document("antenna", "synthesised_line").array
    line.update({"element": "library:antenna-elements:cos15",
                 "geometry": {"kind": "linear", "nx": 24, "dx": 0.5},
                 "synthesis": {"sll": -30, "segments": [{"from": 35, "level": -40}]}})
    r = line.synthesise()
    assert r["worst_excess"] < 0.5, r
    print("beamforming/synthesised_line: side lobes %.1f dB after %d iterations" % (r["side_lobe"], r["iterations"]))

    beams = project.new_document("antenna", "two_beams_nulls").array
    beams.update({"element": "library:antenna-elements:cos15",
                  "geometry": {"kind": "linear", "nx": 32, "dx": 0.5},
                  "excitation": {"taperX": {"kind": "taylor", "sll": 30, "nbar": 5}, "steerTheta": 20, "steerPhi": 0,
                                 "beams": [{"theta": 25, "phi": 180, "level": -3, "phase": 0}],
                                 "nulls": [{"theta": 50, "phi": 0}, {"theta": 5, "phi": 180}]}})
    p = beams.evaluate()
    print("beamforming/two_beams_nulls: beams %.1f / %.1f dBi, nulls %.1f / %.1f dBi" % (
        p.gain(20, 0), p.gain(25, 180), p.gain(50, 0), p.gain(5, 180)))

    mono = project.new_document("antenna", "monopulse").array
    mono.use_library("antenna-arrays:monopulse-16x16")
    mono.update({"element": "library:antenna-elements:cos15"})
    p = mono.evaluate()
    print("beamforming/monopulse: boresight %.1f dBi, difference peaks %.1f dBi" % (p.gain(0), p.gain(4, 0)))
    project.save()


# ---------------------------------------------------------------------------
# 4. Element documents

def element_patterns():
    project = fresh("element_patterns")
    for name, key in (("patch_x", "antenna-elements:patch-x-ro4003"),
                      ("dipole_over_ground", "antenna-elements:dipole-over-ground"),
                      ("gnss_patch_rhcp", "antenna-elements:patch-gnss-rhcp")):
        e = project.new_document("antenna-element", name).element
        e.use_library(key)
        print("element_patterns/%s: %.2f dBi" % (name, e.directivity()))
    horn = project.new_document("antenna-element", "measured_horn").element
    horn.update({"element": {"frequency": 10e9, "efficiency": -0.3}})
    horn.import_pattern(PATTERNS / "sample-horn-x.csv")
    horn.update({"notes": "Imported from libraries/patterns/sample-horn-x.csv (synthetic sample data)."})
    print("element_patterns/measured_horn: %.2f dBi" % horn.directivity())
    project.save()


# ---------------------------------------------------------------------------
# 5. Horns: amplitude steering and amplitude-comparison DF

HORN_FREQUENCY = 17e9
HORN_ANGLES = [-45 + 10 * i for i in range(10)]            # -45 .. 45 deg in 10 deg steps
HORN_PITCH = 0.03                                          # m, more than the 27.1 mm aperture
ARC_RADIUS = HORN_PITCH / (2 * math.sin(math.radians(5)))   # adjacent horns 30 mm apart


def horn_elements(arc):
    elements = []
    for i, angle in enumerate(HORN_ANGLES):
        t = math.radians(angle)
        if arc:   # on a circle around a common centre behind the horns
            x, z = ARC_RADIUS * math.sin(t), ARC_RADIUS * (math.cos(t) - 1)
        else:     # on a straight line
            x, z = (i - (len(HORN_ANGLES) - 1) / 2) * HORN_PITCH, 0.0
        elements.append({"x": x, "y": 0, "z": z, "nx": math.sin(t), "ny": 0, "nz": math.cos(t)})
    return elements


def horn_weights(powers):
    """Relative powers per horn -> [amplitude dB, phase deg]; zero switches a horn off."""
    return [[10 * math.log10(p) if p > 0 else -100, 0] for p in powers]


def peak_angle(array):
    cut = array.evaluate().cut(phi=0, step=0.25)
    i = max(range(len(cut["gain"])), key=lambda k: cut["gain"][k])
    return cut["angle"][i], cut["gain"][i]


def horn_amplitude_steering():
    project = fresh("horn_amplitude_steering")
    horn = project.new_document("antenna-element", "horn").element
    # The 15 dBi X-band library horn scaled to 17 GHz (aperture times 10/17).
    horn.update({"element": {"model": "aperture", "polarisation": "x", "frequency": HORN_FREQUENCY, "efficiency": -0.2,
                             "apertureA": 35.3e-3, "apertureB": 27.1e-3, "distribution": "cosine"},
                 "notes": "15 dBi horn for 17 GHz (35.3 x 27.1 mm aperture model); ten of them form the fans."})
    for name, arc in (("linear_fan", False), ("arc_fan", True)):
        fan = project.new_document("antenna", name).array
        fan.update({
            "element": "element:horn", "frequency": HORN_FREQUENCY, "designFrequency": HORN_FREQUENCY,
            "geometry": {"kind": "custom", "unit": "m", "elements": horn_elements(arc)},
            # No steering phases: the direction comes from the horn amplitudes only.
            "excitation": {"taperX": {"kind": "custom"}, "steering": "none",
                           "weights": horn_weights([0.5 if a in (5, 15) else 0 for a in HORN_ANGLES])},
            "view": {"cutMode": "planes"},
        })
        angle, gain = peak_angle(fan)
        print("horn_amplitude_steering/%s: horns 5 and 15 deg at equal power -> beam at %.2f deg, %.2f dBi" % (
            name, angle, gain))
    shutil.copy(SCRIPTS / "amplitude_steering.py", root / "horn_amplitude_steering")
    project.save()


# ---------------------------------------------------------------------------
# 6. Circular arrays for direction finding

DF_FREQUENCY = 400e6


def circular_df():
    project = fresh("circular_df")
    mono = project.new_document("antenna-element", "monopole").element
    mono.update({"element": {"model": "monopole", "frequency": DF_FREQUENCY, "efficiency": -0.5, "length": 0},
                 "notes": "Quarter-wave monopole on a large ground plane, 400 MHz."})
    horizon = {"cutMode": "horizon", "polar": True}
    for name, pattern in (("adcock_sum", "sum"), ("adcock_ew", "delta-az"), ("adcock_ns", "delta-el")):
        adcock = project.new_document("antenna", name).array
        adcock.update({
            "element": "element:monopole", "frequency": DF_FREQUENCY, "designFrequency": DF_FREQUENCY,
            "geometry": {"kind": "circular", "unit": "lambda", "rings": [{"radius": 0.2, "count": 4, "rotation": 0}],
                         "centreElement": False},
            "excitation": {"taperX": {"kind": "uniform"}, "steering": "none", "pattern": pattern},
            "view": horizon,
        })
    uca = project.new_document("antenna", "uca8_beam").array
    uca.update({
        "element": "element:monopole", "frequency": DF_FREQUENCY, "designFrequency": DF_FREQUENCY,
        "geometry": {"kind": "circular", "unit": "lambda", "rings": [{"radius": 0.5, "count": 8, "rotation": 0}],
                     "centreElement": False},
        "excitation": {"taperX": {"kind": "uniform"}, "steering": "phase", "steerTheta": 90, "steerPhi": 60},
        "sweeps": {"bandStart": 350e6, "bandStop": 450e6, "bandPoints": 21},
        "view": horizon,
    })
    p = uca.evaluate()
    print("circular_df/uca8_beam: %.2f dBi towards phi = 60 deg, %.2f dBi towards phi = 240 deg" % (
        p.gain(90, 60), p.gain(90, 240)))
    ew = project.document("adcock_ew").array.evaluate()
    print("circular_df/adcock_ew: %.2f dBi at phi = 0, %.2f dBi at phi = 90 (figure eight)" % (ew.gain(90, 0), ew.gain(90, 90)))
    shutil.copy(SCRIPTS / "df_demo.py", root / "circular_df")
    project.save()


aesa_radar()
satcom_panel()
beamforming()
element_patterns()
horn_amplitude_steering()
circular_df()
lvdrfcad.close_project()
print("antenna examples written to", root)
