"""Amplitude steering and amplitude-comparison direction finding with ten
squinted 17 GHz horns (example horn_amplitude_steering).

Run it in the Python console with the example project open:
    exec(open(r"<examples>/horn_amplitude_steering/amplitude_steering.py").read())
or without the window:
    lvdrfcad-cli amplitude_steering.py

The documents are put back as they were; nothing new is saved.
"""
import math
import pathlib

import lvdrfcad

try:
    HERE = pathlib.Path(__file__).resolve().parent
except NameError:  # run with exec() in the console
    HERE = pathlib.Path(lvdrfcad.project().directory) if lvdrfcad.project() else pathlib.Path.cwd()

project = lvdrfcad.project()
if project is None or project.name != "horn_amplitude_steering":
    project = lvdrfcad.open_project(HERE / "horn_amplitude_steering.lvdproj")

HORNS = [-45 + 10 * i for i in range(10)]  # squint of each horn, degrees
A, B = HORNS.index(5), HORNS.index(15)       # the pair used below
MID = (HORNS[A] + HORNS[B]) / 2


def weights(powers):
    return [[10 * math.log10(p) if p > 0 else -100, 0] for p in powers]


def set_powers(array, powers):
    array.update({"excitation": {"taperX": {"kind": "custom"}, "steering": "none", "weights": weights(powers)}})


def peak(array):
    cut = array.evaluate().cut(phi=0, step=0.1)
    i = max(range(len(cut["gain"])), key=lambda k: cut["gain"][k])
    return cut["angle"][i], cut["gain"][i]


documents = {name: project.document(name) for name in ("linear_fan", "arc_fan")}
saved = {name: (doc.array.design, doc.modified) for name, doc in documents.items()}
try:
    # 1. Steering by amplitude: move power from the 5 deg horn to the 15 deg horn.
    #    Coherent feeding (one transmitter, a variable power divider, equal
    #    feed lengths): the horns interfere, and only horns around a common
    #    centre add in phase between their beams.
    print("1. Amplitude steering between the %g and %g deg horns (coherent feed)" % (HORNS[A], HORNS[B]))
    print("   power to the %g deg horn | line: beam, gain     | arc: beam, gain" % HORNS[B])
    for fraction in (0.0, 0.05, 0.1, 0.25, 0.5, 0.75, 0.9, 0.95, 1.0):
        row = []
        for name in ("linear_fan", "arc_fan"):
            array = documents[name].array
            powers = [0.0] * len(HORNS)
            powers[A], powers[B] = 1 - fraction, fraction
            set_powers(array, powers)
            angle, gain = peak(array)
            row.append("%6.2f deg %6.2f dBi" % (angle, gain))
        print("   %5.0f %%                   | %s | %s" % (100 * fraction, row[0], row[1]))
    print("   On the line the pair interferes: the beam stays near broadside, then jumps.")
    print("   On the arc both horns are in phase along the bisector: the beam moves around %g deg." % MID)

    # 2. Amplitude-comparison direction finding: each horn has its own
    #    receiver, so there is no interference; the ratio of the two
    #    received powers gives the angle between the horn axes.
    line = documents["linear_fan"].array
    gains = {}
    angles = [HORNS[A] - 10 + a / 4 for a in range(0, 121)]  # 10 deg beyond each horn axis, 0.25 deg steps
    for horn in (A, B):
        powers = [0] * len(HORNS)
        powers[horn] = 1
        set_powers(line, powers)
        pattern = line.evaluate()
        gains[horn] = [pattern.gain(a if a >= 0 else -a, 0 if a >= 0 else 180) for a in angles]
    ratio = [ga - gb for ga, gb in zip(gains[A], gains[B])]

    def bearing(measured):
        """Invert the ratio curve (it falls monotonically across the range)."""
        for k in range(len(angles) - 1):
            a, b = ratio[k], ratio[k + 1]
            if (a - measured) * (b - measured) <= 0 and a != b:
                return angles[k] + (angles[k + 1] - angles[k]) * (a - measured) / (a - b)
        return float("nan")

    print()
    print("2. Amplitude-comparison DF with the %g and %g deg horns (separate receivers)" % (HORNS[A], HORNS[B]))
    print("   true angle | ratio GA - GB  | estimate | estimate with a 1 dB channel imbalance")
    for true in [HORNS[A] + 1.25 * k for k in range(9)]:
        k = angles.index(true)
        print("   %6.1f deg | %8.2f dB    | %6.2f   | %6.2f deg" % (true, ratio[k], bearing(ratio[k]), bearing(ratio[k] + 1)))
    slope = (ratio[angles.index(HORNS[A] + 2.5)] - ratio[angles.index(HORNS[B] - 2.5)]) / 5
    print("   Slope about %.2f dB per degree: 1 dB of imbalance moves the bearing by about %.1f deg." % (slope, 1 / slope))
finally:
    for name, doc in documents.items():
        design, was_modified = saved[name]
        doc.array.design = design
        if not was_modified:
            doc.save()
