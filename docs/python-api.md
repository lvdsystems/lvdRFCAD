# Python API

The `lvdrfcad` module is available in the application's Python console (already imported) and in scripts run without the GUI:

```powershell
lvdrfcad-cli.exe my_script.py [arguments...]
```

Changes made from Python go through the same editing path as the GUI: they appear in open editors immediately and can be undone with Ctrl+Z.

## Example

```python
import lvdrfcad

project = lvdrfcad.new_project("C:/work/receiver")

stage = project.new_document("rf", "stage").schematic
stage.variables = [("g", "10")]
with stage.edit("Build stage"):            # one undo step
    stage.add_component("basic:sheet_port", (-100, 0), parameters={"name": "in"})
    stage.add_component("basic:sheet_port", (150, 0), parameters={"name": "out"})
    stage.add_component("rf-system:amplifier", (0, 0), parameters={"gain": "g"})
    stage.connect("P1", "p", "A1", "in")
    stage.connect("A1", "out", "P2", "p")

top = project.new_document("rf", "top").schematic
top.add_component("rf-system:source", (-200, 0), parameters={"freq": "2.4GHz"})
top.add_component("sheet:stage", (0, 0), parameters={"g": "20"})
top.add_component("rf-system:termination", (200, 0))
top.connect("SRC1", "out", "X1", "in")
top.connect("X1", "out", "TERM1", "p")

for component in top.netlist()["components"]:
    print(component["path"], component["pins"], component["values"])
print(top.check())
project.save()
```

## Module functions

| Function | Returns |
|---|---|
| `project()` | The open `Project`, or `None` |
| `new_project(directory, name=None)` | Creates a project in a folder and opens it; the name defaults to the folder name |
| `open_project(path)` / `close_project()` | |
| `document_types()` | `['rf', 'spice', 'antenna', 'antenna-element']` |
| `libraries()` | Names of the available libraries |
| `library(name)` | `{name, description, path, scope, components}` |
| `component_info(key)` | Pins, parameters and models of a library component such as `'basic:resistor'` |
| `evaluate(expression, variables=None)` | Value of `'2.4GHz'`, `'4n7'` or `'db10(2) + g'` with `variables={'g': 3}` |
| `format_value(value, unit='', digits=4)` | `'2.4 GHz'` |
| `antenna_libraries()` | Antenna libraries: `[{name, description, path, entries: [{key, name, kind, description}]}]` |
| `read_pattern(path)` | Describes a far-field pattern file: `{frequencies, theta, phi, has_phase, peak, warnings}` |

Errors raise `RuntimeError`, `ValueError` or `KeyError` with a readable message.

## Project and Document

`Project`: `name`, `directory`, `path`, `documents`, `document(name)`, `new_document(type, name)`, `remove_document(document, delete_file=False)`, `save()` (project file and all modified documents).

`Document`: `name`, `type`, `path`, `modified`, `save()`, `schematic` for `rf` and `spice` documents, `array` for `antenna` documents and `element` for `antenna-element` documents (`None` otherwise).

Objects of a closed project or removed document raise `RuntimeError` when used.

## Schematic

Components can be given by item id (`'c3'`) or reference (`'A1'`). Points are `(x, y)` in drawing units on a 10-unit grid, Y pointing down. Parameter values are text, as typed in the editor; see [file-format.md](file-format.md), section 3.2, for the value syntax.

| Method | Purpose |
|---|---|
| `components()`, `component(c)`, `wires()`, `labels()`, `junctions()` | Read items as dictionaries |
| `add_component(key, at, rotation=0, mirror=False, parameters=None, reference=None)` | Place a part; returns its id. Keys: `library:component` or `sheet:<document>` |
| `parameter(c, name)`, `set_parameter(c, name, value)`, `reset_parameter(c, name)` | Parameter text (own value or default) |
| `set_reference(c, reference)` | Rename |
| `pin_position(c, pin)` | Connection point of a pin |
| `connect(a, pin_a, b, pin_b)` | Wire between two pins, with one bend if needed |
| `add_wire(points)`, `add_label(net, at)`, `add_junction(at)`, `add_text(text, at, size=12)` | Other items |
| `remove(items)`, `move(items, dx, dy, drag_wires=True)`, `rotate(items, center, degrees=90)`, `mirror(items, center)` | Edit one item or a list |
| `variables` | List of `(name, expression)`; assign a list of pairs or a dict |
| `edit(description)` | Context manager grouping changes into one undo step |
| `undo()`, `redo()`, `can_undo`, `can_redo` | History |
| `nets()` | `{net name: ['A1.in', ...]}` for this sheet |
| `net_of(c, pin)` | Net name of one pin |
| `netlist()` | Flattened hierarchy: `{components, nets, errors, text}`; each component has `path`, `definition`, `document`, `pins`, `parameters` (text) and `values` (evaluated numbers) |
| `check()` | Rule check of this sheet and its sub-sheets: list of `{severity, document, item, message}` |
| `analyses` | Stored analysis setups (dictionaries) |
| `add_analysis(start, stop, points=201, sweep='linear', noise=True, name='', id='')` | Store an S-parameter analysis; returns its id |
| `simulate(analysis=None, start=None, stop=None, points=201, sweep='linear', noise=True)` | Run a stored analysis (id or name), an ad-hoc sweep, or the first stored one; returns a `Result` |

| `add_budget(path=None, start='', frequency='', power='', bandwidth='1MHz', mismatch=True, sweep_start='', sweep_stop='', sweep_points=101, spurs=True, spur_order=5, name='', id='')` | Store a system budget analysis; returns its id |
| `budget(analysis=None, path=None, frequency='', power='', bandwidth='1MHz', mismatch=True, spurs=True)` | Run a stored budget analysis (id or name) or an ad-hoc one; returns a dictionary |

| `optimize(variables, goals, analysis=None, method='nelder-mead', max_evaluations=500, apply=True, snapped=True)` | Optimise on a copy of the project; with `apply=True` the result is written as one undo step. Returns a dictionary |

### Optimisation

```python
outcome = lpf.optimize(
    variables=[{"variable": "C1", "min": "0.5pF", "max": "10pF", "series": "E96"},
               {"component": "L1", "parameter": "l", "min": "1nH", "max": "50nH"}],
    goals=[{"measurement": "dB(S11)", "below": -20, "band": ("2.4GHz", "2.5GHz")},
           {"measurement": "dB(S21)", "above": -0.5, "band": ("2.4GHz", "2.5GHz"), "weight": 2}],
    analysis="Passband", method="levenberg-marquardt")
print(outcome["best"], outcome["snapped"], outcome["goals"])
```

- A variable is a top-sheet variable (`variable`) or a component parameter (`component` reference, `parameter`, optional `document` for a sub-sheet; changing a sub-sheet component changes every instance). `min`/`max` are numbers or values such as `"0.5pF"`; `log` defaults to true for positive ranges; `series` (`E12`, `E24`, `E48`, `E96`, ...) snaps the final value.
- A goal has a `measurement` and one of `below`, `above`, `equal`, `minimize: True`, `maximize: True`, an optional `band` and `weight`.
- Methods: `nelder-mead` (robust, any goals) and `levenberg-marquardt` (`lm`; fast for target goals).
- The result has `variables`, `initial`, `best`, `snapped`, the costs, `goals` (worst value and whether met), `evaluations`, `converged` and `applied`.

### Budget dictionary

| Key | Content |
|---|---|
| `start`, `end`, `end_kind` | Chain ends |
| `input_frequency`, `input_power`, `bandwidth`, `input_noise`, `mismatch` | Conditions (input noise is k T B of the source, an antenna's `temp` or 290 K) |
| `stages` | One dictionary per stage: `component`, `kind`, `input_frequency`, `output_frequency`, stage `gain`, `nf`, `oip3`, `op1db`, `oip2`, and cumulative `cumulative_gain`, `cumulative_nf`, `iip3`, `cumulative_oip3`, `ip1db`, `cumulative_op1db`, `iip2`, `signal`, `noise`, `snr`, `sfdr`, `headroom`, `compressed` |
| `adc` | `full_scale`, `headroom`, `quantisation_snr`, `sample_rate`, `alias_frequency`, or `None` |
| `frequency_plan` | Per stage: frequencies, and for mixers `lo_source`, `lo_frequency`, `lo_power` |
| `sweep_frequencies`, `sweep` | Swept budget: one budget dictionary per input frequency |
| `spurs` | Spur analysis: `kind` (product, spurious_response, image, half_if, lo_leakage), `mixer`, `m`, `n`, `frequency`, `input_frequency`, `level`, `dbc`, `rejection`, `in_band`, `estimated` |
| `warnings` | Messages such as a branch taken by default or a compressed stage |

Levels in dBm, gains and figures in dB, frequencies in Hz. Values that do not apply (the IP3 of a passive stage) are infinite.

## Result

Result of an S-parameter and noise analysis. The circuit needs simulation ports (`rf-system:port`, numbered by their `num` parameter).

| Member | Purpose |
|---|---|
| `frequencies`, `ports`, `z0`, `warnings` | Sweep in Hz, port numbers in matrix order, port reference impedances |
| `s(row, col)` | Complex S-parameter by port number |
| `measure(expression)` | Floats, or complex numbers for complex measurements |
| `save_touchstone(path)` | Write the S-parameters as a Touchstone file |

Measurement expressions (port numbers as in the schematic):

| Expression | Meaning |
|---|---|
| `S21`, `S(2,1)`, `Z(1,1)`, `Y(2,1)` | Network parameters (complex) |
| `Zin(1)`, `Yin(1)`, `VSWR(1)`, `RL(1)` | Port input impedance (other ports terminated in their z0), VSWR, return loss in dB |
| `NF`, `NF(2,1)` | Noise figure in dB from port 1 to port 2 (sources and loads at the port impedances) |
| `NFmin`, `Rn`, `GammaOpt` | Two-port noise parameters |
| `K`, `Delta`, `mu`, `mup`, `MAG` | Two-port stability factors and maximum available (or stable) gain in dB |
| `dB(x)`, `dB10(x)`, `mag(x)`, `phase(x)`, `uphase(x)`, `re(x)`, `im(x)`, `conj(x)`, `GD(x)` | Functions; `dB` is 20 log10, phases in degrees, `GD` the group delay in seconds |
| `+ - * /`, numbers | Arithmetic, for example `dB(S21) - dB(S11)` |

## SPICE

SPICE documents (`spice`) are simulated with ngspice. Analyses are dictionaries in the file format ([file-format.md](file-format.md), section 5.1).

| Schematic method | Purpose |
|---|---|
| `spice_analyses` | Stored SPICE analyses (dictionaries) |
| `add_spice_analysis(type, name='', id='', **fields)` | Store an analysis; `type` is `op`, `dc`, `ac`, `tran`, `noise`, `fourier`, `tf`, `pz` or `sens`; fields as keyword arguments (`stop='5ms'`, `points=20`, `signals=['v(out)']`, `monteCarlo={...}`); returns its id |
| `spice_netlist(analysis=None)` | The netlist given to ngspice |
| `spice(analysis=None)` | Run an analysis (id or name; default the first); returns a `SpiceResult` |
| `spice_sweep(axes, measures, analysis=None, workers=0)` | Parameter or temperature sweep on a copy of the project |
| `spice_monte_carlo(tolerances, measures, runs=100, seed=1, analysis=None, workers=0)` | Monte Carlo; adds `statistics` (nominal, mean, sigma, min, max, count per measure) |
| `spice_worst_case(tolerances, measures, analysis=None, workers=0)` | Per measure: nominal, min, max and the corners |
| `spice_optimize(variables, goals, analysis=None, max_evaluations=200, apply=False)` | Nelder-Mead toward goals; `apply=True` writes the (series-rounded) values as one undo step |

`workers` is the number of parallel worker processes (0: one per processor core, 1: in this process). Batch results are dictionaries with `targets`, `measures`, `runs` (each `label`, `values`, `measures`; a failed measure is NaN) and `warnings`.

```python
amp = lvdrfcad.project().document("amplifier").schematic
ac = amp.add_spice_analysis("ac", name="Response", start="10Hz", stop="10MHz", points=20)
r = amp.spice(ac)
f, gain = r.evaluate("db(v(out)/v(in))")

sweep = amp.spice_sweep([{"target": "RF.r", "mode": "list", "list": ["4.7k", "10k", "22k"]},
                         {"target": "temperature", "mode": "lin", "start": "-40", "stop": "85", "points": "3"}],
                        [{"expression": "db(v(out)/v(in))", "function": "max"}], analysis=ac)
mc = amp.spice_monte_carlo([{"target": "R*.r", "tolerance": 0.01, "distribution": "gaussian"}],
                           ["v(out)"], runs=500, analysis="Bias")
best = amp.spice_optimize([{"target": "RF.r", "min": 1e3, "max": 1e5, "series": "E24"}],
                          [{"measure": {"expression": "db(v(out)/v(in))", "function": "max"},
                            "type": "equal", "target": 26}], analysis=ac, apply=True)
```

- Targets: a schematic variable (`"fc"`), a component parameter (`"R1.r"`), or `"temperature"`. Tolerance targets may use `*` in the reference (`"R*.r"`); `tolerance` is relative (0.01 = 1 %), absolute in °C for the temperature.
- Measures: a trace expression, or `{expression, function, at, from, to, level, name}` with `function` one of `value`, `max`, `min`, `mean`, `rms`, `pkpk`, `risetime`, `falltime`, `frequency`, `period`, `duty`, `overshoot`, `settling`, `crossing`, `thd`.
- Goals: `{measure, type, target, weight}` with `type` `below`, `above`, `equal`, `minimize` or `maximize`.

`SpiceResult`:

| Member | Purpose |
|---|---|
| `plots` | The result plots: `name`, `type` (`op`, `dc`, `ac`, `tran`, `noise`, `tf`, `pz`, `sens`), `scale`, `vectors` |
| `vector(name, plot=None)` | A vector such as `'v(out)'`, `'i(vv1)'`, `'time'` of a plot (name or type; default the first) |
| `evaluate(expression, plot=None)` | `(x, values)` of a trace expression; names may be written as on the schematic (`v(<label>)`, `i(V1)`) |
| `value(expression, plot=None)` | The first value, for operating points: `value('v(out)')` |
| `fourier` | Fourier analyses: per signal `fundamental`, `dc`, `thd_percent`, `harmonics` (`n`, `frequency`, `magnitude`, `phase`) |
| `netlist`, `log`, `warnings` | The netlist, the simulator output, warnings |

Trace expressions: `v(node)`, `v(a,b)`, `i(source)`, numbers with SI suffixes, `+ - * / ^`, and `db`, `db10`, `mag`, `ph`, `cph`, `real`, `imag`, `conj`, `sqrt`, `exp`, `ln`, `log10`, trigonometry, `deriv`, `integ`, `gd`, `mean`, `rms`, `max`, `min`, `pkpk`.

| Other functions | Purpose |
|---|---|
| `Project.import_spice_netlist(path, name='')` | A SPICE netlist file as a simulation-only document; its analysis cards become analyses |
| `lvdrfcad.save_component(library, definition, scope='project')` | Add a component (a dictionary in the library format) to a project or user library; returns its key |

A Python-defined SPICE part has a model `{"kind": "python", "source": "..."}` whose code defines `spice(ref, nodes, params)` and returns netlist lines:

```python
ladder = {"id": "ladder", "name": "RC ladder", "category": "Custom", "referencePrefix": "U",
          "pins": [{"name": "in"}, {"name": "out"}],
          "parameters": [{"name": "r", "label": "R", "unit": "ohm", "default": "1k"},
                         {"name": "c", "label": "C", "unit": "F", "default": "1n"},
                         {"name": "n", "label": "Sections", "unit": "", "default": "4"}],
          "symbol": {...},
          "models": [{"kind": "python", "source": '''
def spice(ref, nodes, params):
    n = int(params["n"])
    taps = [nodes["in"]] + [f"{ref.lower()}_{k}" for k in range(1, n)] + [nodes["out"]]
    lines = []
    for k in range(n):
        lines.append(f"R{ref}_{k} {taps[k]} {taps[k + 1]} {params['r']}")
        lines.append(f"C{ref}_{k} {taps[k + 1]} 0 {params['c']}")
    return "\n".join(lines)
'''}]}
lvdrfcad.save_component("custom", ladder)
```

Element names must start with the device letter followed by `ref`; parameters arrive as numbers (expression parameters) or strings.

## Antennas

Antenna documents hold a design dictionary ([file-format.md](file-format.md), section 7). Angles are degrees, frequencies Hz, levels dB. Changes appear at once in an open antenna tab.

`AntennaArray` (`document.array`):

| Member | Purpose |
|---|---|
| `design` | The whole design as a dict; assigning replaces it |
| `update(changes)` | Merges a partial design: `{'geometry': {'nx': 16}, 'excitation': {'steerTheta': 30}}` |
| `use_library(key)` | Applies an antenna library entry: element, pattern, T/R module (typed values and AESA settings) or array preset |
| `to_custom()` | Converts the geometry to a custom element table with the same positions and orientations, for editing single elements |
| `evaluate(frequency=None, ideal=False)` | An `ArrayPattern` at the operating frequency (or the one given); `ideal=True` without the AESA effects |
| `metrics(integrate=False, co='x')` | Beam metrics at the operating frequency (see below), with the side-lobe level over the UV plane |
| `budget()` | `{eirp, g_over_t, gain, system_temperature, total_power, active_elements, tx: {power, gain, origin}, rx: {nf, gain, origin}}` |
| `scan()` | Scan sweep from `design['sweeps']`: `[{scan, gain, peak_gain, pointing, eirp, g_over_t, grating_lobe}]` |
| `band()` | Band sweep: `[{frequency, gain, peak_gain, peak_theta, peak_phi, squint}]` |
| `grating_lobe_free_scan(phi=0)` | Largest scan angle without a grating lobe in that plane; `None` for non-periodic arrays |
| `synthesise(sll=None, phase_only=None, main_beam=None, segments=None, iterations=None, apply=True)` | Weights for a side-lobe mask (`segments`: `[(from_deg, level_db), ...]`); `{converged, side_lobe, worst_excess, iterations, main_beam, weights}`. With `apply=True` they become the custom taper. |

`ArrayPattern`:

| Member | Purpose |
|---|---|
| `gain(theta, phi=0, component='total', co='x')` | Gain in dBi; `component`: `total`, `co`, `cross`, `rhcp`, `lhcp`, `theta`, `phi`; `co`: Ludwig-3 reference `x` or `y` |
| `field(theta, phi=0)` | `(E_theta, E_phi)`, normalised so that \|E\|² is the gain |
| `cut(phi=0, step=0.5, component='total', co='x')` | `{angle, gain}` over signed θ in the plane φ |
| `azimuth_cut(theta=90, step=0.5, component='total', co='x')` | `{angle, gain}` over φ at constant θ (90: the horizon), for direction-finding arrays |
| `uv(size=101, component='total', co='x')` | `{axis, gain}`: rows of dBi over v (NaN outside the unit circle) |
| `metrics(integrate=False, co='x')` | Beam metrics |
| `weights`, `failed`, `positions`, `frequency` | Complex element weights, failed flags, positions in metres |

Beam metrics: `peak_gain`, `peak_theta`, `peak_phi`, `gain_at_steer`, `pointing_error`, `beamwidth` and `null_width` (two planes through the peak), `side_lobe_cuts` (two planes), `side_lobe` (over UV), `cross_pol`, `axial_ratio`, `directivity` (integrated; NaN unless `integrate=True`).

`AntennaElement` (`document.element`): `design`, `update(changes)`, `use_library(key)`, `import_pattern(path)` (copies the file into the project's `patterns` folder), `directivity(frequency=None)`, `gain(theta, phi=0, frequency=None, component='total', co='x')`, `cut(phi=0, step=0.5, frequency=None, component='total', co='x')`.

```python
p = lvdrfcad.project()
a = p.new_document("antenna", "radar").array
a.use_library("antenna-arrays:aesa-circular-tri")
a.use_library("antenna-elements:patch-x-ro4003")
a.update({"excitation": {"steerTheta": 45}, "aesa": {"phaseBits": 5}})
print(a.metrics()["side_lobe"], a.budget()["eirp"])
for s in a.scan():
    print(s["scan"], s["gain"], s["grating_lobe"])

e = p.new_document("antenna-element", "horn").element
e.import_pattern("horn.ffe")
print(e.directivity(), e.cut(phi=90)["gain"][:5])
```
