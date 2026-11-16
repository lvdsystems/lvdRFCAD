# File formats

All files are UTF-8 JSON objects with two header fields:

| Field | Meaning |
|---|---|
| `format` | What the file is, for example `lvdrfcad-project` |
| `version` | Integer format version. A file with a higher version than the application supports is refused; older versions are migrated on load |

Files are written with sorted keys and indentation so that they diff cleanly in git.

Sections 1 and 2 are implemented in phase 0. Sections 3 to 6 were agreed on 2026-11-15 and are implemented in phase 1.

## 1. Project: `<name>.lvdproj`

The project name is the file's base name. Paths are relative to the project folder.

```json
{
    "format": "lvdrfcad-project",
    "version": 1,
    "documents": [
        { "path": "front_end.lvdrf" },
        { "path": "bias.lvdspice" }
    ]
}
```

A referenced document that cannot be loaded is reported as a warning and its entry is kept when the project is saved.

## 2. Document: `.lvdrf`, `.lvdspice`, `.lvdant`, `.lvdelem`

The document name is the file's base name and is unique within a project, ignoring letter case.

```json
{
    "format": "lvdrfcad-document",
    "version": 1,
    "type": "rf",
    "content": {}
}
```

| `type` | Extension | Document | `content` |
|---|---|---|---|
| `rf` | `.lvdrf` | RF schematic | Schematic, section 5 |
| `spice` | `.lvdspice` | SPICE schematic | Schematic, section 5 |
| `antenna` | `.lvdant` | Antenna array | Array design, section 7.2 |
| `antenna-element` | `.lvdelem` | Antenna element | Element design, section 7.1 |

## 3. Conventions

### 3.1 Geometry

- **Coordinates** are integers in drawing units. The grid is 10 units; pin connection points and wire vertices lie on the grid. X grows to the right, Y grows downward.
- **Orientation** of a placed component: first the optional mirror about the Y axis (x becomes -x), then `rotation` of 0, 90, 180 or 270 degrees counter-clockwise **as seen on screen**. With Y pointing down, a 90 degree rotation maps symbol point (x, y) to (y, -x).
- **Identifiers** of placed items are short strings unique within the document and never reused, so that results and scripts can refer to an item after edits. The prefix gives the item kind: `c` component, `w` wire, `j` junction, `l` net label, `t` text. The schematic stores the next free number in `nextId`.

### 3.2 Values and expressions

Parameter and variable values are stored as text exactly as typed, for example `"2.4GHz"`, `"4n7"` or `"gain_lna + 3"`. A file never stores an evaluated number in place of the expression.

- **Numbers** take an optional SI prefix and unit directly after the digits: `2.4GHz`, `1.2pF`, `10mW`, `50ohm`, `15dBm`. Prefixes are case-sensitive: `a f p n u µ m k M G T`, plus `meg` in any letter case for SPICE compatibility. `m` is milli, `M` is mega. Letters after the prefix are a unit and are ignored for the value; the parameter definition carries the unit.
- **European notation**: a prefix between digits marks the decimal point: `4n7` is 4.7n, `2k2` is 2.2k, `1M5` is 1.5M.
- **Spaced units** are accepted when the word after the space is a known unit with an optional prefix, for example `2.4 GHz` or `10 dBm`.
- **Operators**: `+ - * / ^` and parentheses; `^` is right-associative and binds tighter than unary minus.
- **Functions**: `sqrt exp ln log10 sin cos tan asin acos atan atan2 sinh cosh tanh abs min max pow floor ceil round db10 db20 undb10 undb20`.
- **Constants**: `pi`, `c0` (speed of light, m/s), `kB` (Boltzmann, J/K), `T0` (290 K). Variables cannot use these names.
- **Variables** are identifiers `[A-Za-z_][A-Za-z0-9_]*` defined in the schematic's `variables` list and evaluated in dependency order; circular definitions are errors.

Values in dB, dBm and degrees are plain numbers in those units: a gain of `"15"` with unit `dB` means 15 dB.

### 3.3 Connectivity

Connectivity is not stored. It is derived from geometry:

- All segments of one wire belong to one net.
- A pin connection point, wire end point, junction or net label connects to every wire that passes through that point, at an end or in the middle of a segment.
- Two wires that cross in the middle of both segments do **not** connect unless a junction sits on the crossing.
- A pin connects to other pins at the same point.
- Net labels with the same name connect within one sheet.
- A component whose definition has `netName` connects its single pin to the global net of that name in every sheet. The ground symbol uses `"netName": "0"`.

Net names, in order of precedence:

1. A global `netName`, so a grounded net is always `0`.
2. A net label name. Several different names on one net are reported by the rule check, and the alphabetically first wins.
3. The `name` of a sheet port on the net.
4. `Net-(<reference>-<pin>)` from the alphabetically first pin on the net; `Net-(<item id>)` for a net without pins.

In a flattened hierarchy, local net names are prefixed with the instance path, for example `X1/Net-(A1-out)`; global nets keep their name.

## 4. Component definition and library: `.lvdlib`

A component definition holds everything about a part that is not specific to one placement: pins, parameters, one symbol, and one model per analysis. A different drawing style for the same part (for example IEC and ANSI resistors) is a separate component.

```json
{
    "format": "lvdrfcad-library",
    "version": 1,
    "name": "rf-system",
    "description": "Behavioural RF system blocks",
    "components": [
        {
            "id": "amplifier",
            "name": "Amplifier",
            "category": "RF/Amplifiers",
            "description": "Behavioural amplifier",
            "referencePrefix": "A",
            "pins": [
                { "name": "in" },
                { "name": "out" }
            ],
            "parameters": [
                { "name": "gain", "label": "Gain", "unit": "dB", "default": "15", "shown": true },
                { "name": "nf", "label": "Noise figure", "unit": "dB", "default": "2" },
                { "name": "oip3", "label": "Output IP3", "unit": "dBm", "default": "30" },
                { "name": "op1db", "label": "Output P1dB", "unit": "dBm", "default": "18" },
                { "name": "z0", "label": "Port impedance", "unit": "ohm", "default": "50" }
            ],
            "symbol": {
                "graphics": [
                    { "type": "polygon", "points": [[-20, -20], [20, 0], [-20, 20]], "fill": "none" }
                ],
                "pins": [
                    { "pin": "in", "at": [-30, 0], "length": 10, "direction": "right" },
                    { "pin": "out", "at": [30, 0], "length": 10, "direction": "left" }
                ],
                "referenceAt": [0, -30],
                "valueAt": [0, 30]
            },
            "models": [
                { "kind": "rf-behavioural", "block": "amplifier" }
            ]
        }
    ]
}
```

### 4.1 Definition fields

| Field | Required | Meaning |
|---|---|---|
| `id` | yes | Identifier unique in the library, `[a-z0-9_]+` |
| `name` | yes | Display name |
| `category` | yes | Slash-separated path for the library browser |
| `description` | no | One sentence |
| `referencePrefix` | yes | Letters for references: `A` gives A1, A2 |
| `pins` | yes | Ordered pin list; the order is the netlist order |
| `parameters` | no | Editable parameters |
| `symbol` | yes | Drawing and pin placement |
| `models` | no | One model per analysis kind |
| `netName` | no | Global net this single-pin component connects to (ground: `"0"`) |
| `role` | no | `"sheet-port"` marks a hierarchical port; its `name` parameter is the port name |

The definition key used in schematics is `<library name>:<component id>`.

### 4.2 Parameters

`name` (identifier used in expressions and models), `label`, `unit`, `default` (expression text), `shown` (drawn next to the symbol), and optionally `choices` (list of allowed strings, for an enumeration), `"file": true` (a file path, relative to the project folder) or `"text": true` (plain text such as a port name). Values of `choices`, `file` and `text` parameters are not evaluated as expressions.

### 4.3 Symbol

- `graphics`: `line` (`points`), `polygon` (`points`, `fill`), `rect` (`from`, `to`, `fill`), `circle` (`center`, `radius`, `fill`), `arc` (`center`, `radius`, `startAngle`, `spanAngle`, degrees, counter-clockwise on screen from the +X axis) and `text` (`at`, `text`, `size`, `anchor`). `fill` is `none`, `outline` or `background`. `anchor` is `left`, `center` or `right`.
- `pins`: `at` is the connection point and must be on the 10-unit grid; `direction` (`left`, `right`, `up`, `down`) points from the connection point toward the body; a pin line of `length` units is drawn in that direction.
- `referenceAt` and `valueAt`: where the reference and the shown parameters are drawn.
- Symbols are drawn with the origin near their centre.

### 4.4 Models

An analysis uses the first model it understands and reports the component as unsupported otherwise.

| `kind` | Used by | Fields |
|---|---|---|
| `rf-behavioural` | Budget, spur and linear RF analyses | `block`: built-in system block type; parameters are read by name |
| `rf-element` | Linear RF analysis | `element`: built-in circuit element type (resistor, transmission line, ...) |
| `sparameters` | Linear RF analysis; budget when available | `file`: Touchstone path, usually `"{file}"` taken from a parameter |
| `spice` | SPICE | `template`: netlist lines with `{ref}`, `{pin:<name>}` and `{<parameter>}` placeholders; optional `subcircuit`, `include`, `section`, `probe`, `instrument` (below) |
| `subschematic` | All | `document`: schematic whose sheet ports match the pin names |
| `python` | SPICE | `source` (code) or `file` (path relative to the project folder) defining `spice(ref, nodes, params)` |

**`spice` models.** The netlister renders the `template` for every placed part:

- `{ref}` is the part's unique name (the reference, with the sheet instance in front inside hierarchy: `X1_R1`); every element line starts with the SPICE device letter followed by `{ref}` (`R{ref}`, `X{ref}`), and a part with several elements adds suffixes (`L{ref}_p`).
- `{pin:<name>}` is the node of the pin. Nets named by labels keep the label in lower case; ground is `0`; a net called `gnd` that is not connected to a ground symbol becomes `gnd_net`.
- `{<parameter>}`: an expression parameter is written as a plain number (`1e-12`, never an SI suffix); a `choices` or `text` parameter is written as is.
- Template lines starting with `.` are per-instance cards (for example `.model DM_{ref} D(is={is})`).
- `subcircuit`: text added once per netlist, verbatim: braces in it are ngspice parameter expressions, not placeholders.
- `include`: a model file, relative to the project folder or absolute, added as `.include "<file>"`, or as `.lib "<file>" <section>` with `section`.
- `probe`: `voltage`, `differential` or `current` marks a probe for the waveform viewer; `instrument`: `oscilloscope`, `voltmeter`, `ammeter`, `function-generator`, `spectrum-analyzer` or `bode-plotter` marks a virtual instrument. Both may have an empty `template`.

**`python` models** (SPICE): the code defines `spice(ref, nodes, params)` and returns netlist lines as a string, following the same rules as rendered template lines. `nodes` maps pin names to nodes, `params` maps parameter names to numbers (expression parameters) or strings.

Block and element types, with their parameter names, are listed in [libraries.md](libraries.md).

### 4.5 Sheet symbols

Every schematic document in a project can be placed in another schematic as a block, under the key `sheet:<document name>`. Its definition is generated from the document: one pin per `sheet-port` component, a rectangle symbol with inputs on the left, and one parameter per schematic variable whose default is the variable's value. A value given on the placed block overrides the variable inside that instance.

## 5. Schematic content

The `content` of `rf` and `spice` documents.

```json
{
    "nextId": 6,
    "definitions": {
        "rf-system:amplifier": { "id": "amplifier", "name": "Amplifier", "...": "..." }
    },
    "variables": [
        { "name": "f0", "value": "2.4GHz" }
    ],
    "components": [
        {
            "id": "c1",
            "reference": "A1",
            "definition": "rf-system:amplifier",
            "at": [200, 100],
            "rotation": 0,
            "mirror": false,
            "parameters": { "gain": "17.5", "nf": "0.9" }
        }
    ],
    "wires": [
        { "id": "w2", "points": [[100, 100], [170, 100]] }
    ],
    "junctions": [
        { "id": "j3", "at": [150, 100] }
    ],
    "labels": [
        { "id": "l4", "net": "LO", "at": [150, 60] }
    ],
    "texts": [
        { "id": "t5", "text": "Receiver front end", "at": [100, 20], "size": 14 }
    ]
}
```

- `definitions` is a fallback cache holding a copy of every library component definition the schematic uses, keyed by `library:component`. Sheet symbols are not cached; they are generated from the project. Resolution on load:
  1. If the library is available, its definition is used, so a library fix reaches every design. The cache entry is refreshed when the document is next saved.
  2. If the library or the component is missing, the cached copy is used and the part is flagged as "from cache".
  3. If the library definition differs from the cached copy, the part is flagged as "changed in library" until the document is saved, and a pin that was removed or renamed is reported by the rule check.

  The same file can therefore behave differently on two machines with different libraries; the flags exist to make that visible.
- `components[].parameters` holds only values that differ from the definition's defaults.
- Ports, grounds and sources are ordinary components from a built-in library, not special item types.
- Items are written sorted by identifier.
- `simulation` (optional) holds analysis setups and plot definitions, section 5.1.

### 5.1 Simulation setup

```json
"simulation": {
    "analyses": [
        {
            "id": "a1",
            "type": "rf-linear",
            "name": "S-parameters",
            "sweep": "linear",
            "start": "100MHz",
            "stop": "f0*3",
            "points": "201",
            "noise": true
        }
    ],
    "plots": [
        {
            "id": "p1",
            "name": "Response",
            "kind": "rectangular",
            "analysis": "a1",
            "traces": ["dB(S21)", "dB(S11)", "NF"]
        }
    ]
}
```

- Analyses are identified by `id` (`a1`, `a2`, ...). Values are expressions evaluated with the top sheet's variables.
- `rf-linear`: S-parameter and noise analysis. `sweep` is `linear`, `log` or `list`; a `list` sweep has `"list": ["1GHz", "2.4GHz"]` instead of `start`, `stop` and `points` (at most 100,000 points).
- `rf-budget`: system budget, frequency plan and spur analysis along a signal chain:

  ```json
  {
      "id": "a2",
      "type": "rf-budget",
      "name": "Receiver",
      "path": ["ANT1", "F1", "SPL1.out2", "A1", "MX1", "ADC1"],
      "start": "ANT1",
      "frequency": "f_rf",
      "power": "-90",
      "bandwidth": "1MHz",
      "mismatch": "actual",
      "sweep": { "start": "2.3GHz", "stop": "2.6GHz", "points": "31" },
      "spurs": { "enabled": true, "maxOrder": "5" }
  }
  ```

  `path` is optional (the chain is traced automatically from `start`, else a source, port 1 or an antenna); an entry `REF.pin` chooses the exit pin at a branch. `frequency` and `power` override the start component's values. `mismatch` is `actual` (cascade with the real S-parameters, RF-7) or `ideal` (matched cascade of |S21|). `sweep` is optional.
- SPICE analyses (`.lvdspice` documents) have the types `spice-op`, `spice-dc`, `spice-ac`, `spice-tran`, `spice-noise`, `spice-fourier`, `spice-tf`, `spice-pz` and `spice-sens`. Their fields are expression text (except names, choices and flags):

  | Type | Fields |
  |---|---|
  | `spice-op` | – |
  | `spice-dc` | `source` (a source's reference, or `temp`), `start`, `stop`, `step`; optional `source2`, `start2`, `stop2`, `step2` |
  | `spice-ac` | `sweep` (`dec`, `oct`, `lin`), `points`, `start`, `stop` |
  | `spice-tran` | `step`, `stop`; optional `start`, `maxStep`, `uic` (bool), `saveCurrents` (bool) |
  | `spice-noise` | `output` (net or `v(a,b)`), `source`, and the `spice-ac` fields |
  | `spice-fourier` | the `spice-tran` fields, `fundamental`, `harmonics`, `periods`, `signals` (list of trace expressions) |
  | `spice-tf` | `output` (net, `v(a,b)` or `i(V1)`), `source` |
  | `spice-pz` | `inputPositive`, `inputNegative`, `outputPositive`, `outputNegative` (nets), `transfer` (`voltage`, `current`), `find` (`both`, `poles`, `zeros`) |
  | `spice-sens` | `output`, `ac` (bool) and the `spice-ac` fields |

  All SPICE analyses may have `temperature` (°C), `options` (text of a `.options` card) and `cards` (extra SPICE text). Batch setups are stored in the analysis under `sweep` (`axes`, `measures`), `monteCarlo` (`tolerances`, `runs`, `seed`, `measures`), `worstCase` (`tolerances`, `measures`) and `optimization` (`variables`, `goals`, `maxEvaluations`, `tolerance`); a measure is `{"expression": "v(out)", "function": "max", "at": "", "from": "", "to": "", "level": "", "name": ""}`, a tolerance `{"target": "R*.r", "tolerance": 0.01, "distribution": "gaussian"}`.
- `spicePanes` (SPICE documents) holds the waveform panes per analysis id: `{"a2": [{"id": "p1", "name": "Pane 1", "traces": ["v(out)"], "logX": false, "fftSource": "", "fftWindow": "hann", "fftLinear": false, "complexParts": {}}]}`.
- `netlist` (SPICE documents imported from a netlist): the circuit text that is simulated instead of the drawing, and `netlistSource`, the file it came from.
- Plots: `kind` is `rectangular`, `smith` or `polar`; `traces` are measurement expressions (see the Python API documentation, `Result.measure`). An optional `"labels": {"dB(S21)": "Gain"}` gives traces a legend name. Unknown keys are kept when the application rewrites the object. Further analysis types (budget, spurs, SPICE) add their own `type`.
- Results are not stored in the document; they are recomputed.

## 6. Library search order

Libraries are found by name in this order; the first match wins:

1. The project folder's `libraries` subfolder
2. The user library folder (set in the application settings)
3. The built-in libraries shipped with the application

Antenna libraries (`.lvdantlib`, section 7.3) are found in the same folders, in the same order.

## 7. Antenna documents

Units: frequencies in Hz, lengths in metres (except geometry values in wavelengths, below), angles in degrees, levels in dB. Missing keys take the defaults shown; unknown keys are ignored. Angles follow the antenna convention: θ from +z, φ from +x towards +y; an element radiates towards +z with its reference polarisation along x.

### 7.1 Antenna element: `.lvdelem`, type `antenna-element`

```json
{
    "element": {
        "model": "patch",
        "polarisation": "x",
        "frequency": 10e9,
        "efficiency": -0.8,
        "patchWidth": 0, "patchLength": 0,
        "substrateHeight": 0.813e-3, "permittivity": 3.55,
        "backLobe": -20
    },
    "notes": "free text"
}
```

| Key | Meaning |
|---|---|
| `model` | `isotropic`, `cosn`, `dipole`, `dipole-ground`, `monopole`, `patch`, `aperture`, `imported` |
| `polarisation` | `x`, `y`, `slant45`, `slant-45`, `rhcp`, `lhcp` (ignored by `monopole` and `imported`) |
| `frequency` | Design frequency; zero lengths below are derived from it |
| `efficiency` | Radiation efficiency, dB (≤ 0) |
| `exponent`, `backLobe` | `cosn`: power pattern cos^n θ; level behind the ground plane (also `patch`), dB |
| `length` | `dipole`, `dipole-ground`: full length; `monopole`: height. 0: λ/2 and λ/4 |
| `height` | `dipole-ground`: height over the ground plane. 0: λ/4 |
| `patchWidth`, `patchLength`, `substrateHeight`, `permittivity` | `patch`, cavity model. Width and length 0: designed for the frequency |
| `apertureA`, `apertureB`, `distribution` | `aperture`: size along y (broad wall) and x; `cosine` (TE10) or `uniform` |
| `patternFile` | `imported`: pattern file, relative to the project folder (or to the library file for library entries), or absolute |

### 7.2 Antenna array: `.lvdant`, type `antenna`

```json
{
    "element": "element:patch",
    "frequency": 10e9,
    "designFrequency": 10e9,
    "geometry": { "kind": "triangular", "unit": "lambda", "nx": 24, "ny": 28,
                  "dx": 0.6, "dy": 0.5196, "aperture": "circle", "apertureX": 6.9 },
    "excitation": { "taperX": { "kind": "taylor", "sll": 35, "nbar": 5 }, "taperY": { "kind": "uniform" },
                    "radial": true, "steerTheta": 30, "steerPhi": 0, "steering": "phase" },
    "aesa": { "phaseBits": 6, "attenuatorBits": 6, "attenuatorStep": 0.5, "delayLsb": 0,
              "amplitudeError": 0.4, "phaseError": 3, "failedFraction": 0.02, "failed": [], "seed": 3 },
    "trModule": { "tx": { "source": "linked", "document": "tx_channel" },
                  "rx": { "source": "typed", "gain": 25, "nf": 3 },
                  "frontLoss": 0.8, "beamformerLoss": 6, "antennaTemperature": 150, "ambientTemperature": 290,
                  "txTaper": false },
    "synthesis": { "sll": -30, "mainBeam": 0, "phaseOnly": false, "iterations": 300, "segments": [] },
    "sweeps": { "scanPlane": 0, "scanMax": 60, "scanStep": 2, "bandStart": 0, "bandStop": 0, "bandPoints": 21 }
}
```

**Element**: `"element:<document>"` (an element document of the project), `"library:<library>:<entry>"`, or `""` with the element inline under `inlineElement` (section 7.1).

**Geometry** (`unit`: `lambda` at `designFrequency`, or `m`):

| `kind` | Keys |
|---|---|
| `linear` | `nx`, `dx` (along x) |
| `rectangular`, `triangular` | `nx`, `ny`, `dx`, `dy`; triangular: odd rows shifted by `dx`/2, `dy` is the row spacing. `aperture`: `none`, `circle`, `ellipse` with semi-axes `apertureX`, `apertureY` (0: the lattice extent) |
| `circular` | `rings`: `[{"radius", "count", "rotation"}]`, `centreElement` |
| `cylinder` | `cylinderRadius`, `columns`, `rows`, `arc` (degrees; 360: full), `dz`. Axis along z, elements face outward, broadside +x |
| `custom` | `elements`: `[{"x", "y", "z", "nx", "ny", "nz", "rotation", "amplitude", "phase", "enabled"}]`. Normal (default +z) and rotation of the element's x axis about it; amplitude (dB) and phase (degrees) for the custom taper |

**Excitation**:

| Key | Meaning |
|---|---|
| `taperX`, `taperY` | `kind`: `uniform`, `taylor` (`sll`, `nbar`), `chebyshev` (`sll`), `cosine` (`exponent`), `cosine-pedestal` (`pedestal`, dB), `hamming`, `custom` |
| `radial` | One taper over the radius (uses `taperX`) |
| `steerTheta`, `steerPhi` | Main beam direction |
| `steering` | `phase`, `ttd` (time delay per element), `subarray-ttd`, `none` (no steering phases: the taper and the custom phases only) |
| `subarrayX`, `subarrayY`, `subarrayTaper` | Sub-arrays of lattice elements; one amplitude per sub-array |
| `beams` | Extra beams: `[{"theta", "phi", "level", "phase"}]` |
| `nulls` | `[{"theta", "phi"}]` |
| `pattern` | `sum`, `delta-az`, `delta-el` |
| `weights` | Custom taper per element: `[[amplitude dB, phase deg], ...]` in element order; −100 dB or less switches an element off |

**AESA**: phase-shifter and attenuator bits (0: continuous), attenuator step (dB), delay resolution (ps), RMS amplitude (dB) and phase (degrees) errors, random failure fraction (0–1), failed element indices (from 0), random seed.

**T/R module**: `tx` and `rx` each have `source` (`typed`, `preset`, `linked`), `preset` (antenna library key), `document` and `analysis` (an RF schematic and its budget analysis, id or name; empty: the first), and the typed values `power` (dBm, transmit), `gain` (dB) and `nf` (dB, receive).

`view` holds user-interface settings: `cutMode` (`auto`, `planes`, `beam`, `horizon`) and `polar` are read by the array workspace; other keys are kept as they are.

### 7.3 Antenna library: `.lvdantlib`

```json
{
    "format": "lvdrfcad-antenna-library",
    "version": 1,
    "name": "antenna-elements",
    "description": "...",
    "entries": [
        { "id": "patch-x-ro4003", "name": "Patch 10 GHz, RO4003C 0.813 mm", "kind": "element",
          "description": "...", "data": { "model": "patch", "frequency": 10e9 } }
    ]
}
```

| `kind` | `data` |
|---|---|
| `element`, `pattern` | An element object (section 7.1). A pattern's `patternFile` is relative to the library file. |
| `tr-module` | `tx` (`power`, `gain`), `rx` (`gain`, `nf`), `phaseBits`, `attenuatorBits`, `attenuatorStep`, `amplitudeError`, `phaseError`, `frontLoss` |
| `array` | Any part of an antenna array (section 7.2); its keys replace the array's when applied |

Entries are referred to as `<library>:<id>`.

### 7.4 Pattern files

The application writes patterns as CSV:

```
# lvd RF cad far-field pattern; fields normalised so that |E|^2 is directivity or gain
frequency_hz,theta_deg,phi_deg,etheta_re,etheta_im,ephi_re,ephi_im
1e+10,0,0,1.2,0,0,0
```

It reads that format, CST "ASCII farfield" exports, FEKO `.ffe` files, and any table whose header names the columns: `theta`, `phi`, optionally `frequency` (unit from `_hz`, `_ghz` or a `[GHz]` bracket), and one of
- `etheta_re`, `etheta_im`, `ephi_re`, `ephi_im` (or `Re(Etheta)` …);
- `etheta_mag`, `etheta_phase`, `ephi_mag`, `ephi_phase` (magnitudes in dB when the name or unit says so);
- a total `gain`, `directivity` or `dBi` column (dB unless a unit says otherwise), taken as x-polarised without phase.

Separators are commas, semicolons, tabs or spaces; lines starting with `#`, `*`, `%` or `//` are comments, except `#Frequency: <Hz>` lines, which start a new frequency block. Negative θ is folded to (|θ|, φ + 180°). Patterns are normalised to directivity when used, so absolute levels do not matter.
