"""Direction finding with circular arrays of monopoles (example circular_df).

1. Watson-Watt with a four-element Adcock: the east-west and north-south
   difference patterns against the sum give the bearing.
2. Beamscan (Bartlett) with an eight-element circular array: the beam is
   steered round the horizon and the received power peaks at the source.

Run it in the Python console with the example project open:
    exec(open(r"<examples>/circular_df/df_demo.py").read())
or without the window:
    lvdrfcad-cli df_demo.py

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
if project is None or project.name != "circular_df":
    project = lvdrfcad.open_project(HERE / "circular_df.lvdproj")


def wrap(angle):
    return (angle + 180) % 360 - 180


# 1. Watson-Watt. For a small Adcock the difference channels are
#    EW/sum = j sin(k R cos phi) / 2 and NS/sum = j sin(k R sin phi) / 2,
#    so the bearing is atan2 of their imaginary parts. The sine makes the
#    classic octantal "spacing error", which grows with the radius.
total = project.document("adcock_sum").array.evaluate()
ew = project.document("adcock_ew").array.evaluate()
ns = project.document("adcock_ns").array.evaluate()
radius = project.document("adcock_sum").array.design["geometry"]["rings"][0]["radius"]
print("1. Watson-Watt with a four-element Adcock of radius %.2f wavelength" % radius)
print("   true bearing | estimate | error")
worst = 0.0
for true in range(0, 360, 15):
    s = total.field(90, true)[0]
    x = (ew.field(90, true)[0] / s).imag
    y = (ns.field(90, true)[0] / s).imag
    estimate = math.degrees(math.atan2(y, x)) % 360
    error = wrap(estimate - true)
    worst = max(worst, abs(error))
    print("   %7.1f deg  | %7.2f  | %+6.2f deg" % (true, estimate, error))
kr = 2 * math.pi * radius
theory = max(abs(math.degrees(math.atan2(math.sin(kr * math.sin(math.radians(a))), math.sin(kr * math.cos(math.radians(a))))) - a)
             for a in range(0, 91))
print("   Largest error %.2f deg; the octantal spacing error of an ideal Adcock with k R = %.2f is %.2f deg." % (
    worst, kr, theory))

# 2. Beamscan with the eight-element ring: steer the beam round the horizon;
#    by reciprocity the power received from a source at phi_s with the beam
#    steered to phi is the gain at phi_s of the array steered to phi.
uca_doc = project.document("uca8_beam")
saved, was_modified = uca_doc.array.design, uca_doc.modified
try:
    uca = uca_doc.array
    sources = [(130.0, 0.0), (250.0, -6.0)]  # bearing (deg) and level (dB)
    scan = list(range(0, 360, 2))
    spectrum = []
    for phi in scan:
        uca.update({"excitation": {"steerTheta": 90, "steerPhi": phi}})
        pattern = uca.evaluate()
        power = sum(10 ** ((pattern.gain(90, bearing) + level) / 10) for bearing, level in sources)
        spectrum.append(10 * math.log10(power))
    top = max(spectrum)
    peaks = [scan[i] for i in range(len(scan))
             if spectrum[i] >= spectrum[i - 1] and spectrum[i] >= spectrum[(i + 1) % len(scan)] and spectrum[i] > top - 7]
    print()
    print("2. Beamscan with the eight-element ring (radius 0.5 wavelength)")
    print("   sources at %s; spectrum peaks at %s deg" % (
        ", ".join("%.0f deg (%+.0f dB)" % s for s in sources), ", ".join("%d" % p for p in peaks)))
    print("   (peaks within 7 dB of the strongest; the ring's side lobes are near -8 dB and would show up")
    print("   as false sources below that, so weaker sources need a larger ring or a high-resolution method)")
    print("   spectrum (dB below the peak, every 20 deg):")
    print("   " + " ".join("%3d:%5.1f" % (scan[i], spectrum[i] - top) for i in range(0, len(scan), 10)))
finally:
    uca_doc.array.design = saved
    if not was_modified:
        uca_doc.save()
