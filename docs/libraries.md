# Built-in component libraries

The application ships eight libraries in `libraries/`. Their file format is described in [file-format.md](file-format.md), section 4. This document lists their components and defines the **block and element types** that models refer to: the parameter names each solver reads and what they mean. It is the contract between the libraries and the analyses.

| Library | File | Purpose |
|---|---|---|
| `basic` | `libraries/basic.lvdlib` | Parts used by both RF and SPICE schematics: R, L, C, ground, hierarchical port |
| `rf-system` | `libraries/rf-system.lvdlib` | Behavioural system blocks for budget, spur and linear RF analyses, S-parameter blocks, ports and terminations |
| `rf-circuit` | `libraries/rf-circuit.lvdlib` | Linear RF circuit elements: ideal and physical transmission lines, stubs, coupled lines, transformer |
| `spice-sources` | `libraries/spice-sources.lvdlib` | SPICE independent, controlled and behavioural sources |
| `spice-semiconductors` | `libraries/spice-semiconductors.lvdlib` | Diodes, BJTs, MOSFETs, JFETs: generic parts with editable models and common real parts |
| `spice-ics` | `libraries/spice-ics.lvdlib` | Behavioural op-amps, comparators, regulators, shunt reference, logic gates |
| `spice-misc` | `libraries/spice-misc.lvdlib` | Potentiometer, magnetics, crystal, switches, transmission lines, waveform probes |
| `spice-instruments` | `libraries/spice-instruments.lvdlib` | Virtual instruments: oscilloscope, voltmeter, ammeter, function generator, spectrum analyser, Bode plotter |

Four **antenna libraries** (`.lvdantlib`, [file-format.md](file-format.md), section 7.3) sit in the same folder; see [Antenna libraries](#antenna-libraries) at the end.

Check the libraries after editing them:

```
python -I tools/validate_libraries.py                    # all libraries/*.lvdlib
python -I tools/validate_libraries.py --render out/      # also write one SVG per library
python -I tools/check_spice_library.py                   # simulate every SPICE model with ngspice
```

The validator enforces the file format and also this document: a component whose model names a block or element type must define every parameter listed for that type below, and its pins must have the names and order given here.

## Conventions

- **Values** are expression text (file-format.md section 3.2). Values in dB, dBm, dBi, dBc and degrees are plain numbers in that unit: `"15"` with unit `dB` is 15 dB.
- **Losses are positive.** `il` (insertion loss), `isolation`, `rejection`, `coupling`, `directivity` are positive dB numbers; a transmission of `il = 3` is |S21| = -3 dB. Gains (`gain`) are signed dB, so a conversion loss is a negative `gain`.
- **Phase** is in degrees. A positive `phase` or electrical length `elen` is a phase *lag*: S21 = |S21| · exp(-j · phase · pi/180), as for a delay line.
- **`z0`** is a real impedance in ohms. On behavioural blocks it is the port impedance to which every port is ideally matched (S11 = S22 = ... = 0 unless stated otherwise); for lines it is the characteristic impedance.
- **`freq`** is a frequency in Hz. On a source it is the output frequency; on a lossy or dispersive part it is the frequency at which another parameter (Q, `elen`, `loss_per_m`) is specified.
- **Noise**: `nf` is a noise figure in dB at T0 = 290 K. A passive block without an `nf` parameter has noise figure equal to its loss at physical temperature T0.
- **Implicit ground**: RF circuit elements (`rf-circuit` library) and RF behavioural blocks draw only their signal pins. Every port voltage is referenced to an implicit ground, which is the global net `0` (the net of `basic:ground`). One-pin parts (stubs, ports, terminations, sources, antennas) therefore connect from their pin to ground.
- **Pin order** in a component is the order of the S-parameter ports and of the netlist.
- **Canonical parameter names** used wherever applicable: `gain` (dB), `nf` (dB), `oip3`, `iip3`, `op1db`, `oip2` (dBm), `il` (dB), `isolation` (dB), `z0` (ohm), `freq` (Hz), `power` (dBm), and `fmin` / `fmax` (Hz) for an operating range. `fmin` / `fmax` are reserved for this meaning; no built-in block uses them yet.
- **Components without a model**: `basic:ground` (`netName`) and `basic:sheet_port` (`role: "sheet-port"`) only define connectivity. Analyses must skip them instead of reporting them as unsupported.

## basic

Parts shared by RF and SPICE schematics. The two-terminal parts have pins `p` (left) and `n` (right) and work in both analyses: an `rf-element` model for linear RF analysis (a series two-terminal element between its pins; connect a pin to ground to use it as a shunt part) and a `spice` model.

### resistor (R)

Ideal resistor, ANSI zig-zag symbol. Pins `p`, `n`.

| Parameter | Unit | Default | Meaning |
|---|---|---|---|
| `r` | ohm | `1k` | Resistance (shown) |

Models: `rf-element` `resistor`; `spice` `R{ref} {pin:p} {pin:n} {r}`.

### capacitor (C)

Pins `p`, `n`.

| Parameter | Unit | Default | Meaning |
|---|---|---|---|
| `c` | F | `1pF` | Capacitance (shown) |
| `q` | | `0` | Unloaded Q at `freq`; `0` means ideal (lossless) |
| `freq` | Hz | `1GHz` | Frequency at which `q` applies |

Models: `rf-element` `capacitor`; `spice` `C{ref} {pin:p} {pin:n} {c}` (Q is ignored in SPICE).

### inductor (L)

Pins `p`, `n`.

| Parameter | Unit | Default | Meaning |
|---|---|---|---|
| `l` | H | `1nH` | Inductance (shown) |
| `q` | | `0` | Unloaded Q at `freq`; `0` means ideal (lossless) |
| `freq` | Hz | `1GHz` | Frequency at which `q` applies |

Models: `rf-element` `inductor`; `spice` `L{ref} {pin:p} {pin:n} {l}` (Q is ignored in SPICE).

### ground (GND)

One pin `gnd` at the origin; the symbol hangs below it. `"netName": "0"`: the pin is on the global net `0` in every sheet. No parameters, no model.

### sheet_port (P)

Hierarchical port, `"role": "sheet-port"`. One pin `p`. No model.

| Parameter | Unit | Default | Meaning |
|---|---|---|---|
| `name` | | `P1` | Port name (`"text": true`, shown); becomes the pin name of the sheet symbol |

## rf-system

Behavioural blocks, each with the model `{"kind": "rf-behavioural", "block": "<block>"}` where `<block>` equals the component id, except the S-parameter blocks, which use `{"kind": "sparameters", "file": "{file}"}`. Two-port blocks have `in` on the left and `out` on the right. Parameters marked (shown) are drawn next to the symbol.

| id | Ref | Pins | Parameters (shown first) | Model |
|---|---|---|---|---|
| `amplifier` | A | in, out | `gain` dB 15 (shown), `nf` dB 2 (shown), `oip3` dBm 30, `oip2` dBm 45, `op1db` dBm 18, `isolation` dB 40, `z0` ohm 50 | `amplifier` |
| `attenuator` | AT | in, out | `il` dB 3 (shown), `z0` ohm 50 | `attenuator` |
| `filter` | F | in, out | `type` bandpass (shown; lowpass, highpass, bandpass, bandstop), `fc` Hz 1GHz (shown), `response` butterworth (ideal, butterworth, chebyshev), `order` 5, `bw` Hz 100MHz, `il` dB 1, `ripple` dB 0.1, `rejection` dB 80, `z0` ohm 50 | `filter` |
| `mixer` | MX | rf, lo, if | `gain` dB -7 (shown), `nf` dB 7, `iip3` dBm 15, `lo_power` dBm 13, `lo_rf_isolation` dB 35, `lo_if_isolation` dB 30, `rf_if_isolation` dB 25, `side` low (low, high), `product` auto (shown; auto, difference, sum), `spur_table` file (empty), `z0` ohm 50 | `mixer` |
| `source` | SRC | out | `freq` Hz 1GHz (shown), `power` dBm -30 (shown), `z0` ohm 50 | `source` |
| `lo` | LO | out | `freq` Hz 2GHz (shown), `power` dBm 13 (shown), `phase_noise` file (empty), `z0` ohm 50 | `lo` |
| `splitter` | SPL | in, out1, out2 | `il` dB 0.5 (shown), `isolation` dB 20, `imbalance` dB 0.1, `phase_imbalance` deg 1, `z0` ohm 50 | `splitter` |
| `combiner` | CMB | in1, in2, out | `il` dB 0.5 (shown), `isolation` dB 20, `imbalance` dB 0.1, `phase_imbalance` deg 1, `z0` ohm 50 | `combiner` |
| `coupler` | DC | in, out, cpl, iso | `coupling` dB 20 (shown), `directivity` dB 25, `il` dB 0.3, `z0` ohm 50 | `coupler` |
| `switch_spdt` | SW | com, t1, t2 | `state` t1 (shown; t1, t2), `il` dB 0.5, `isolation` dB 30, `z0` ohm 50 | `switch_spdt` |
| `isolator` | ISO | in, out | `il` dB 0.5 (shown), `isolation` dB 20, `z0` ohm 50 | `isolator` |
| `circulator` | CIR | p1, p2, p3 | `il` dB 0.5 (shown), `isolation` dB 20, `z0` ohm 50 | `circulator` |
| `cable` | CBL | in, out | `length` m 1 (shown), `loss_per_m` dB/m 0.5, `freq` Hz 1GHz, `il` dB 0.1, `vf` 0.7, `z0` ohm 50 | `cable` |
| `antenna` | ANT | p | `gain` dBi 0 (shown), `temp` K 290, `z0` ohm 50 | `antenna` |
| `multiplier` | MUL | in, out | `factor` 2 (shown), `gain` dB -12, `rejection` dBc 20, `z0` ohm 50 | `multiplier` |
| `phase_shifter` | PS | in, out | `phase` deg 90 (shown), `il` dB 1, `z0` ohm 50 | `phase_shifter` |
| `adc` | ADC | in | `fs` Hz 100MHz (shown), `bits` 12 (shown), `full_scale` dBm 4, `nf` dB 30, `z0` ohm 50 | `adc` |
| `nport1` | SP | p1 | `file` (file, shown, empty) | `sparameters` `{file}` |
| `nport2` | SP | p1, p2 | `file` (file, shown, empty) | `sparameters` `{file}` |
| `nport3` | SP | p1, p2, p3 | `file` (file, shown, empty) | `sparameters` `{file}` |
| `nport4` | SP | p1, p2, p3, p4 | `file` (file, shown, empty) | `sparameters` `{file}` |
| `port` | PORT | p | `num` 1 (shown), `z0` ohm 50 (shown) | `port` |
| `termination` | TERM | p | `z0` ohm 50 (shown) | `termination` |

S-parameter blocks: pin `pK` is Touchstone port K. The file is a Touchstone file with the matching number of ports, relative to the project folder; an empty `file` is an error at analysis time. On `nport4`, p1/p2 are on top (left/right) and p3/p4 below.

## rf-circuit

Linear circuit elements, each with the model `{"kind": "rf-element", "element": "<element>"}`. All are referenced to the implicit ground; only signal pins are drawn.

| id | Ref | Pins | Parameters (shown first) | Element |
|---|---|---|---|---|
| `tline` | TL | in, out | `z0` ohm 50 (shown), `elen` deg 90 (shown), `freq` Hz 1GHz | `tline` |
| `open_stub` | TL | p | `z0` ohm 50 (shown), `elen` deg 90 (shown), `freq` Hz 1GHz | `stub_open` |
| `short_stub` | TL | p | `z0` ohm 50 (shown), `elen` deg 90 (shown), `freq` Hz 1GHz | `stub_short` |
| `coupled_lines` | CL | p1, p2, p3, p4 | `z0e` ohm 70 (shown), `z0o` ohm 35 (shown), `elen` deg 90, `freq` Hz 1GHz | `coupled_lines` |
| `microstrip` | TL | in, out | `w` m 1.5mm (shown), `l` m 10mm (shown), `h` m 0.8mm, `er` 4.4, `tand` 0.02, `t` m 35um, `sigma` S/m 58M | `microstrip` |
| `stripline` | TL | in, out | `w` m 0.5mm (shown), `l` m 10mm (shown), `b` m 1.6mm, `er` 4.4, `tand` 0.02, `t` m 35um, `sigma` S/m 58M | `stripline` |
| `coax` | TL | in, out | `l` m 100mm (shown), `din` m 0.51mm, `dout` m 1.68mm, `er` 2.1, `tand` 0.0002, `sigma` S/m 58M | `coax` |
| `cpw` | TL | in, out | `w` m 1mm (shown), `l` m 10mm (shown), `s` m 0.2mm, `h` m 0.8mm, `er` 4.4, `tand` 0.02, `t` m 35um, `sigma` S/m 58M, `backed` no (no, yes) | `cpw` |
| `transformer` | TR | in, out | `n` 1 (shown) | `transformer` |

Substrate parameters are stored on each line element for now; a shared substrate definition may replace them later.

## SPICE libraries

The `spice-*` libraries hold the default parts for SPICE schematics (requirement SP-9). The target engine is ngspice (ngspice dialect: B sources, `.subckt ... params:`, VDMOS, S/W switches, T and LTRA lines, K coupling). Behavioural parts use B sources rather than XSPICE code models, so that every part works in operating point, DC, AC, transient and noise analyses. `basic` provides R, L, C and ground for SPICE as well; they are not repeated here.

### SPICE model contract

A component's SPICE model is `{ "kind": "spice", "template": "...", "subcircuit": "...", "include": "...", "probe": "..." }`; only `template` is required.

- **`template`**: one or more netlist lines separated by `\n`, emitted once per instance. Placeholders:
  - `{ref}`: unique sanitised instance name, for example `R1`, or `X1_R1` inside a hierarchy. Every element line starts with the SPICE device letter followed by `{ref}` (`R{ref} ...`, `X{ref} ...`). A part emitting several elements uses suffixes: `V{ref}_s ...` then `F{ref} ... V{ref}_s {gain}`.
  - `{pin:<name>}`: the node connected to that pin.
  - `{<parameter>}`: an expression parameter is substituted as a plain number (`1e-12`, never with units; the application evaluates `1pF` itself); a `choices` or `"text": true` parameter is substituted verbatim. Free SPICE text (a source specification, a PWL point list, a B-source expression) is therefore a text parameter written in SPICE syntax (in SPICE `M` is milli: use `meg`).
  - Lines starting with `.` are per-instance cards, for example `.model lvd_dm_{ref} D(is={is} n={n})` for a generic part with editable model parameters. Their model or subcircuit name contains `{ref}` so that instances do not clash.
- **`subcircuit`**: text emitted once per netlist, deduplicated by identical text, **verbatim** (no placeholder substitution, so it may use ngspice `{expr}` brace expressions). It holds the shared `.subckt ... .ends` and `.model` definitions. Every name defined at its top level starts with `lvd_` (`.model lvd_1N4148 D(...)`, `.subckt lvd_opamp ...`) to avoid clashes with user models; models local to a `.subckt` may use any name. Parameterised subcircuits use the ngspice `params:` syntax and the template passes every parameter: `X{ref} {pin:inp} {pin:inn} {pin:out} {pin:vcc} {pin:vee} lvd_opamp params: aol={aol} gbw={gbw} ...`.
- **`include`**: a model file to include; not used by the default libraries, which keep everything inline.
- **`probe`**: `voltage` (one pin `p`, empty template), `differential` (pins `p`, `n`, empty template) or `current` (pins `p`, `n` in series, template `V{ref} {pin:p} {pin:n} 0`). Probes have no effect on the circuit; they tell the waveform viewer what to plot (the current probe measures the current entering `p`).
- Templates may refer to the global ground node `0` (the ideal op-amp output is referenced to it). Internal nodes are only created inside shared subcircuits, never in templates.
- Inside ngspice B-source expressions every `{...}` brace expression of a subcircuit is written in parentheses, `({vdd/2})`: ngspice pastes the value into the expression text without grouping.

`tools/validate_libraries.py` enforces the contract statically: template present (empty only for a probe), each non-dot line starting with a letter and `{ref}`, placeholders naming a pin, a parameter or `ref`, per-instance `.model`/`.subckt` names containing `{ref}`, shared names starting with `lvd_`, balanced `.subckt`/`.ends`, no `{ref}`/`{pin:...}` in `subcircuit`, `probe` one of the three kinds, no unknown keys.

`tools/check_spice_library.py` simulates every component with a SPICE model (also basic R, L, C) with the console ngspice in batch mode (`build/_deps/ngspice_console-src/bin/ngspice_con.exe -b`). It renders the template with the default parameter values, places the part in a small test bench for its kind (sources into a 1 k load, diodes biased through a resistor, BJT/MOSFET/JFET common-emitter/common-source stages, op-amps as a non-inverting amplifier with gain 11, regulators with an input supply and a 100 mA load, gates through their truth table, switches driven, lines terminated in 50 ohm), runs `.op`, a short `.tran` and an `.ac` sweep where meaningful, and fails on ngspice errors or warnings, singular matrices, timestep or convergence failures, unknown models and out-of-range results (for example the 7805 output 5 V +-1 %, op-amp gain 20.8 dB, silicon diode drop 0.5 to 0.8 V at about 1 mA, zener 4.95 to 5.25 V at 5 mA, logic levels). `--keep DIR` keeps the netlists, `--verbose` prints them.

### Conventions of the SPICE parts

- **Sources** are drawn vertically, `p` on top for voltage sources (V(p) - V(n) is the source value). Current sources follow SPICE: the current flows from `p` through the source to `n`; the symbol puts `p` at the bottom and `n` at the top, so the arrow points up and the current leaves at the top into the circuit. Controlled sources have the output `p`/`n` on top/bottom and the control pins `cp`/`cn` on the left; F and H have a 0 V sense branch from `cp` to `cn`, placed in series with the controlling current.
- **Generic parts** (diode, zener, NPN, PNP, NMOS, PMOS, N/P-JFET) carry their model parameters as component parameters and emit a per-instance `.model lvd_..._{ref}` card. **Named parts** (1N4148, 2N3904, IRF540N, ...) have no parameters and share one `.model lvd_<PART>` card. Their parameters are typical values published by manufacturers or fitted to typical datasheet curves, as stated in each description; they are not copies of vendor or third-party library files.
- **Semiconductor pins**: diodes `a`, `k`; BJTs `c`, `b`, `e`; MOSFETs and JFETs `d`, `g`, `s` (level-1 MOSFETs have the bulk tied to the source; the named MOSFETs are ngspice VDMOS models with body diode). P-type parts are drawn with the source or emitter on top. JFET `vto` is negative for both N and P types (SPICE convention).
- **ICs** (reference `U`) are behavioural macromodels, named "...-like" when the defaults follow a real part's datasheet typicals: they are generic models, not vendor models. Op-amps and comparators have pins `inp`, `inn`, `out`, `vcc`, `vee`; regulators `in`, `gnd` (or `adj`), `out`; logic gates `a`, `b`, `y` with an implicit ground-referenced supply `vdd` (1 Gohm input leakage to ground keeps unconnected inputs solvable).
- **Magnetics** have pins `p1`, `n1` (primary) and `p2`, `n2` (secondary), dots at `p1` and `p2`; each winding has 1 Gohm to ground inside the model so floating windings remain solvable.
- **Probes** use the reference prefix `PR` (`P` is already used by `basic:sheet_port`).

### spice-sources

SPICE sources: independent voltage and current sources, controlled sources (E, F, G, H) and behavioural sources.

| id | Ref | Pins | Parameters (default) | Description and model |
|---|---|---|---|---|
| `vdc` | V | p, n | `dc` 5 V, `ac` 0 V | DC voltage source with optional AC magnitude for AC analysis. `V{ref} {pin:p} {pin:n} DC {dc} AC {ac}` |
| `idc` | I | p, n | `dc` 1m A, `ac` 0 A | DC current source; current enters at p (bottom) and leaves at n (top), into the circuit. `I{ref} {pin:p} {pin:n} DC {dc} AC {ac}` |
| `vsin` | V | p, n | `vo` 0 V, `va` 1 V, `freq` 1k Hz, `td` 0 s, `theta` 0 1/s, `phase` 0 deg, `ac` 1 V | Damped sine SIN(vo va freq td theta phase); the DC value for operating point is the offset. `V{ref} {pin:p} {pin:n} DC {vo} AC {ac} SIN({vo} {va} {freq} {td} {theta} {phase})` |
| `vpulse` | V | p, n | `v1` 0 V, `v2` 5 V, `td` 0 s, `tr` 10n s, `tf` 10n s, `pw` 0.5m s, `per` 1m s, `ac` 0 V | Trapezoidal pulse PULSE(v1 v2 td tr tf pw per). `V{ref} {pin:p} {pin:n} DC {v1} AC {ac} PULSE({v1} {v2} {td} {tr} {tf} {pw} {per})` |
| `vpwl` | V | p, n | `points` `0 0 1m 1 2m 1 3m 0` | Piecewise-linear source; points are SPICE text 't1 v1 t2 v2 ...' (SPICE number syntax: M is milli, use meg). `V{ref} {pin:p} {pin:n} PWL({points})` |
| `vexp` | V | p, n | `v1` 0 V, `v2` 1 V, `td1` 0 s, `tau1` 100u s, `td2` 1m s, `tau2` 200u s | Exponential pulse EXP(v1 v2 td1 tau1 td2 tau2). `V{ref} {pin:p} {pin:n} EXP({v1} {v2} {td1} {tau1} {td2} {tau2})` |
| `vsffm` | V | p, n | `vo` 0 V, `va` 1 V, `fc` 10k Hz, `mdi` 5, `fs` 1k Hz | Single-frequency FM: SFFM(vo va fc mdi fs), v = vo + va sin(2 pi fc t + mdi sin(2 pi fs t)). `V{ref} {pin:p} {pin:n} SFFM({vo} {va} {fc} {mdi} {fs})` |
| `vac` | V | p, n | `mag` 1 V, `phase` 0 deg, `dc` 0 V | Small-signal source for AC analysis (magnitude and phase) with a DC value. `V{ref} {pin:p} {pin:n} DC {dc} AC {mag} {phase}` |
| `vsrc` | V | p, n | `spec` `DC 0 AC 1 SIN(0 1 1k)` | Voltage source with a free SPICE specification, for example 'DC 0 AC 1 SIN(0 1 1k)'. `V{ref} {pin:p} {pin:n} {spec}` |
| `isin` | I | p, n | `io` 0 A, `ia` 1m A, `freq` 1k Hz, `td` 0 s, `theta` 0 1/s, `phase` 0 deg, `ac` 0 A | Damped sine current SIN(io ia freq td theta phase); current leaves at n (top). `I{ref} {pin:p} {pin:n} DC {io} AC {ac} SIN({io} {ia} {freq} {td} {theta} {phase})` |
| `ipulse` | I | p, n | `i1` 0 A, `i2` 1m A, `td` 0 s, `tr` 10n s, `tf` 10n s, `pw` 0.5m s, `per` 1m s | Trapezoidal current pulse PULSE(i1 i2 td tr tf pw per); current leaves at n (top). `I{ref} {pin:p} {pin:n} DC {i1} PULSE({i1} {i2} {td} {tr} {tf} {pw} {per})` |
| `isrc` | I | p, n | `spec` `DC 1m AC 1` | Current source with a free SPICE specification, for example 'DC 1m AC 1'. `I{ref} {pin:p} {pin:n} {spec}` |
| `vcvs` | E | p, n, cp, cn | `gain` 10 V/V | Voltage-controlled voltage source: V(p,n) = gain * V(cp,cn). `E{ref} {pin:p} {pin:n} {pin:cp} {pin:cn} {gain}` |
| `vccs` | G | p, n, cp, cn | `gm` 1m S | Voltage-controlled current source: current gm * V(cp,cn) flows from p through the source to n. `G{ref} {pin:p} {pin:n} {pin:cp} {pin:cn} {gm}` |
| `cccs` | F | p, n, cp, cn | `gain` 10 A/A | Current-controlled current source: current gain * I(cp->cn) flows from p through the source to n; cp-cn is a 0 V sense branch placed in series with the controlling current. `V{ref}_s {pin:cp} {pin:cn} 0` ... |
| `ccvs` | H | p, n, cp, cn | `rm` 1k ohm | Current-controlled voltage source: V(p,n) = rm * I(cp->cn); cp-cn is a 0 V sense branch. `V{ref}_s {pin:cp} {pin:cn} 0` ... |
| `bvsrc` | B | p, n | `expr` `2.5+2.5*sin(2*pi*1k*time)` | B source V(p,n) = expression (ngspice syntax: time, V(<net>), I(<vsource>), sin, exp, ...). `B{ref} {pin:p} {pin:n} V={expr}` |
| `bisrc` | B | p, n | `expr` `1m*sin(2*pi*1k*time)` | B source: current = expression flows from p through the source to n (ngspice syntax). `B{ref} {pin:p} {pin:n} I={expr}` |

### spice-semiconductors

SPICE diodes, BJTs, MOSFETs and JFETs: generic parts with editable models and common real parts.

| id | Ref | Pins | Parameters (default) | Description and model |
|---|---|---|---|---|
| `diode` | D | a, k | `is` 4n A, `n` 1.9, `rs` 0.6 ohm, `cjo` 2p F, `vj` 0.75 V, `m` 0.33, `tt` 5n s, `bv` 100 V, `ibv` 100u A | Generic junction diode with editable SPICE model parameters (defaults: small-signal silicon diode). `D{ref} {pin:a} {pin:k} lvd_dm_{ref}` ... |
| `zener` | D | a, k | `bv` 5.1 V, `izt` 5m A, `rz` 5 ohm, `cjo` 100p F | Generic zener: breakdown bv at test current izt; rz sets the slope above the knee. `D{ref} {pin:a} {pin:k} lvd_dz_{ref}` ... |
| `1n4148` | D | a, k | none | Typical parameters for the 1N4148/1N914 fast switching diode (widely published manufacturer model). `D{ref} {pin:a} {pin:k} lvd_1N4148` |
| `1n4007` | D | a, k | none | Typical parameters for the 1N4007 1 A / 1000 V general-purpose rectifier. `D{ref} {pin:a} {pin:k} lvd_1N4007` |
| `1n5819` | D | a, k | none | Typical parameters for the 1N5819 1 A / 40 V Schottky rectifier, fitted to datasheet forward curves. `D{ref} {pin:a} {pin:k} lvd_1N5819` |
| `bat54` | D | a, k | none | Typical parameters for the BAT54 small-signal Schottky diode, fitted to datasheet forward voltage (0.32 V at 1 mA). `D{ref} {pin:a} {pin:k} lvd_BAT54` |
| `bzx84c5v1` | D | a, k | none | Typical parameters for the BZX84C5V1 5.1 V zener (5.1 V at 5 mA). `D{ref} {pin:a} {pin:k} lvd_BZX84C5V1` |
| `led_red` | D | a, k | none | Typical red LED: about 1.75 V at 1 mA and 1.9 V at 20 mA. `D{ref} {pin:a} {pin:k} lvd_LED_RED` |
| `npn` | Q | c, b, e | `bf` 200, `is` 10f A, `vaf` 100 V, `br` 1, `ikf` 100m A, `rb` 10 ohm, `rc` 1 ohm, `re` 0.5 ohm, `cje` 10p F, `cjc` 5p F, `tf` 400p s, `tr` 50n s | Generic NPN Gummel-Poon transistor with editable model parameters. `Q{ref} {pin:c} {pin:b} {pin:e} lvd_qn_{ref}` ... |
| `pnp` | Q | c, b, e | `bf` 200, `is` 10f A, `vaf` 100 V, `br` 1, `ikf` 100m A, `rb` 10 ohm, `rc` 1 ohm, `re` 0.5 ohm, `cje` 10p F, `cjc` 5p F, `tf` 400p s, `tr` 50n s | Generic PNP Gummel-Poon transistor with editable model parameters. `Q{ref} {pin:c} {pin:b} {pin:e} lvd_qp_{ref}` ... |
| `2n3904` | Q | c, b, e | none | Typical parameters for the 2N3904 small-signal NPN (classic manufacturer-published model). `Q{ref} {pin:c} {pin:b} {pin:e} lvd_2N3904` |
| `2n3906` | Q | c, b, e | none | Typical parameters for the 2N3906 small-signal PNP (classic manufacturer-published model). `Q{ref} {pin:c} {pin:b} {pin:e} lvd_2N3906` |
| `2n2222a` | Q | c, b, e | none | Typical parameters for the 2N2222A general-purpose NPN (classic manufacturer-published model). `Q{ref} {pin:c} {pin:b} {pin:e} lvd_2N2222A` |
| `bc547b` | Q | c, b, e | none | Typical parameters for the BC547B low-noise NPN (hFE group B), after the manufacturer-published model. `Q{ref} {pin:c} {pin:b} {pin:e} lvd_BC547B` |
| `bc557b` | Q | c, b, e | none | Typical parameters for the BC557B low-noise PNP (hFE group B), after the manufacturer-published model. `Q{ref} {pin:c} {pin:b} {pin:e} lvd_BC557B` |
| `tip31c` | Q | c, b, e | none | Typical parameters for the TIP31C 3 A / 100 V power NPN, fitted to datasheet typical hFE, VBE(on) and fT. `Q{ref} {pin:c} {pin:b} {pin:e} lvd_TIP31C` |
| `tip32c` | Q | c, b, e | none | Typical parameters for the TIP32C 3 A / 100 V power PNP (complement of TIP31C), fitted to datasheet typicals. `Q{ref} {pin:c} {pin:b} {pin:e} lvd_TIP32C` |
| `nmos` | M | d, g, s | `vto` 1 V, `kp` 20u A/V^2, `lambda` 0.02 1/V, `w` 100u m, `l` 1u m, `cgso` 0.4n F/m, `cgdo` 0.4n F/m | Generic level-1 enhancement NMOS, bulk tied to source; VTO, KP, LAMBDA, W and L editable. `M{ref} {pin:d} {pin:g} {pin:s} {pin:s} lvd_mn_{ref} w={w} l={l}` ... |
| `pmos` | M | d, g, s | `vto` -1 V, `kp` 10u A/V^2, `lambda` 0.02 1/V, `w` 100u m, `l` 1u m, `cgso` 0.4n F/m, `cgdo` 0.4n F/m | Generic level-1 enhancement PMOS, bulk tied to source; VTO (negative), KP, LAMBDA, W and L editable. `M{ref} {pin:d} {pin:g} {pin:s} {pin:s} lvd_mp_{ref} w={w} l={l}` ... |
| `2n7000` | M | d, g, s | none | Typical parameters for the 2N7000 small-signal N-MOSFET (VDMOS model fitted to datasheet VGS(th), RDS(on), capacitances). `M{ref} {pin:d} {pin:g} {pin:s} lvd_2N7000` |
| `bss138` | M | d, g, s | none | Typical parameters for the BSS138 logic-level N-MOSFET (VDMOS model fitted to datasheet typicals). `M{ref} {pin:d} {pin:g} {pin:s} lvd_BSS138` |
| `bss84` | M | d, g, s | none | Typical parameters for the BSS84 small-signal P-MOSFET (VDMOS model fitted to datasheet typicals). `M{ref} {pin:d} {pin:g} {pin:s} lvd_BSS84` |
| `irf540n` | M | d, g, s | none | Typical parameters for the IRF540N 100 V / 33 A power N-MOSFET (VDMOS model fitted to datasheet RDS(on), VGS(th), Ciss/Crss, body diode). `M{ref} {pin:d} {pin:g} {pin:s} lvd_IRF540N` |
| `njf` | J | d, g, s | `vto` -2 V, `beta` 1m A/V^2, `lambda` 10m 1/V, `rd` 1 ohm, `rs` 1 ohm, `cgs` 3p F, `cgd` 1.5p F, `is` 10f A | Generic N-channel JFET (Shichman-Hodges) with editable parameters. `J{ref} {pin:d} {pin:g} {pin:s} lvd_jn_{ref}` ... |
| `pjf` | J | d, g, s | `vto` -2 V, `beta` 1m A/V^2, `lambda` 10m 1/V, `rd` 1 ohm, `rs` 1 ohm, `cgs` 3p F, `cgd` 1.5p F, `is` 10f A | Generic P-channel JFET (Shichman-Hodges) with editable parameters. `J{ref} {pin:d} {pin:g} {pin:s} lvd_jp_{ref}` ... |
| `j310` | J | d, g, s | none | Typical parameters for the J310 VHF N-JFET (IDSS about 30 mA, VGS(off) about -3 V). `J{ref} {pin:d} {pin:g} {pin:s} lvd_J310` |
| `2n5457` | J | d, g, s | none | Typical parameters for the 2N5457 general-purpose N-JFET (IDSS about 3 mA, VGS(off) about -1.5 V). `J{ref} {pin:d} {pin:g} {pin:s} lvd_2N5457` |

### spice-ics

Behavioural SPICE macromodels: op-amps, comparators, regulators, shunt reference and logic gates.

| id | Ref | Pins | Parameters (default) | Description and model |
|---|---|---|---|---|
| `opamp_ideal` | U | inp, inn, out | `gain` 1meg V/V | Ideal op-amp: VCVS with very high gain from V(inp,inn) to out (referenced to ground), no supply pins or rails. `E{ref} {pin:out} 0 {pin:inp} {pin:inn} {gain}` |
| `opamp` | U | inp, inn, out, vcc, vee | `aol` 100k V/V, `gbw` 1meg Hz, `sr` 1 V/us, `vohr` 1.5 V, `volr` 1.5 V, `rout` 50 ohm, `ilim` 25m A, `iq` 1m A, `rin` 1T ohm, `vos` 0 V | Behavioural single-pole op-amp with supply pins: open-loop gain, GBW, slew rate, output swing and current limit. `X{ref} {pin:inp} {pin:inn} {pin:out} {pin:vcc} {pin:vee} lvd_opamp params: aol={aol} gbw={gbw} sr={sr} vos={vos} rin={rin} rout={rout} ilim={ilim} vohr={vohr} volr={volr} iq={iq}` |
| `opamp_lm358` | U | inp, inn, out, vcc, vee | `aol` 100k V/V, `gbw` 1meg Hz, `sr` 0.3 V/us, `vohr` 1.5 V, `volr` 20m V, `rout` 100 ohm, `ilim` 40m A, `iq` 350u A, `rin` 10meg ohm, `vos` 0 V | Generic preset with LM358 datasheet-typical figures: 100 dB gain, 1 MHz GBW, 0.3 V/us, output to about 20 mV above vee and 1.5 V below vcc (not a vendor model). `X{ref} {pin:inp} {pin:inn} {pin:out} {pin:vcc} {pin:vee} lvd_opamp params: aol={aol} gbw={gbw} sr={sr} vos={vos} rin={rin} rout={rout} ilim={ilim} vohr={vohr} volr={volr} iq={iq}` |
| `opamp_tl072` | U | inp, inn, out, vcc, vee | `aol` 200k V/V, `gbw` 3meg Hz, `sr` 13 V/us, `vohr` 1.5 V, `volr` 1.5 V, `rout` 100 ohm, `ilim` 25m A, `iq` 1.4m A, `rin` 1T ohm, `vos` 0 V | Generic preset with TL072 datasheet-typical figures: 106 dB gain, 3 MHz GBW, 13 V/us, output 1.5 V from the rails (not a vendor model). `X{ref} {pin:inp} {pin:inn} {pin:out} {pin:vcc} {pin:vee} lvd_opamp params: aol={aol} gbw={gbw} sr={sr} vos={vos} rin={rin} rout={rout} ilim={ilim} vohr={vohr} volr={volr} iq={iq}` |
| `opamp_opa2134` | U | inp, inn, out, vcc, vee | `aol` 1meg V/V, `gbw` 8meg Hz, `sr` 20 V/us, `vohr` 1 V, `volr` 1 V, `rout` 50 ohm, `ilim` 40m A, `iq` 4m A, `rin` 10T ohm, `vos` 0 V | Generic preset with OPA2134 datasheet-typical figures: 120 dB gain, 8 MHz GBW, 20 V/us, output 1 V from the rails (not a vendor model). `X{ref} {pin:inp} {pin:inn} {pin:out} {pin:vcc} {pin:vee} lvd_opamp params: aol={aol} gbw={gbw} sr={sr} vos={vos} rin={rin} rout={rout} ilim={ilim} vohr={vohr} volr={volr} iq={iq}` |
| `opamp_rrio` | U | inp, inn, out, vcc, vee | `aol` 300k V/V, `gbw` 10meg Hz, `sr` 5 V/us, `vohr` 10m V, `volr` 10m V, `rout` 50 ohm, `ilim` 30m A, `iq` 1m A, `rin` 1T ohm, `vos` 0 V | Generic rail-to-rail output CMOS op-amp preset: 110 dB gain, 10 MHz GBW, 5 V/us, output within 10 mV of the rails. `X{ref} {pin:inp} {pin:inn} {pin:out} {pin:vcc} {pin:vee} lvd_opamp params: aol={aol} gbw={gbw} sr={sr} vos={vos} rin={rin} rout={rout} ilim={ilim} vohr={vohr} volr={volr} iq={iq}` |
| `comparator` | U | inp, inn, out, vcc, vee | `vhys` 10m V, `tpd` 100n s, `rout` 100 ohm, `iq` 0.5m A | Behavioural comparator with hysteresis, propagation delay and rail-to-rail push-pull output: out = vcc when inp > inn. `X{ref} {pin:inp} {pin:inn} {pin:out} {pin:vcc} {pin:vee} lvd_comparator params: vhys={vhys} tpd={tpd} rout={rout} iq={iq}` |
| `comparator_lm393` | U | inp, inn, out, vcc, vee | `vhys` 0 V, `tpd` 300n s, `ron` 50 ohm, `isink` 16m A, `iq` 0.4m A | Open-collector comparator preset with LM393 datasheet-typical figures: no hysteresis, 300 ns delay, 16 mA sink; needs a pull-up (not a vendor model). `X{ref} {pin:inp} {pin:inn} {pin:out} {pin:vcc} {pin:vee} lvd_comparator_oc params: vhys={vhys} tpd={tpd} ron={ron} isink={isink} iq={iq}` |
| `regulator` | U | in, gnd, out | `vout` 5 V, `vdrop` 2 V, `ilim` 1.5 A, `iq` 5m A | Behavioural fixed positive linear regulator: output voltage, dropout, current limit and quiescent current. `X{ref} {pin:in} {pin:gnd} {pin:out} lvd_linreg params: vout={vout} vdrop={vdrop} ilim={ilim} iq={iq}` |
| `reg_7805` | U | in, gnd, out | `vout` 5 V, `vdrop` 2 V, `ilim` 2.2 A, `iq` 4.2m A | Behavioural 7805-like +5 V regulator with datasheet-typical 2 V dropout and 2.2 A peak current (not a vendor model). `X{ref} {pin:in} {pin:gnd} {pin:out} lvd_linreg params: vout={vout} vdrop={vdrop} ilim={ilim} iq={iq}` |
| `reg_7812` | U | in, gnd, out | `vout` 12 V, `vdrop` 2 V, `ilim` 2.2 A, `iq` 4.3m A | Behavioural 7812-like +12 V regulator with datasheet-typical 2 V dropout and 2.2 A peak current (not a vendor model). `X{ref} {pin:in} {pin:gnd} {pin:out} lvd_linreg params: vout={vout} vdrop={vdrop} ilim={ilim} iq={iq}` |
| `reg_lm1117_33` | U | in, gnd, out | `vout` 3.3 V, `vdrop` 1.1 V, `ilim` 1 A, `iq` 5m A | Behavioural LM1117-3.3-like LDO: 3.3 V, 1.1 V dropout, 1 A (not a vendor model). `X{ref} {pin:in} {pin:gnd} {pin:out} lvd_linreg params: vout={vout} vdrop={vdrop} ilim={ilim} iq={iq}` |
| `reg_lm317` | U | in, adj, out | `vref` 1.25 V, `vdrop` 2 V, `ilim` 2.2 A, `iadj` 50u A | Behavioural LM317-like adjustable regulator: V(out) - V(adj) = 1.25 V, 50 uA adjust-pin current, 2 V dropout (not a vendor model). `X{ref} {pin:in} {pin:adj} {pin:out} lvd_adjreg params: vref={vref} vdrop={vdrop} ilim={ilim} iadj={iadj}` |
| `tl431` | U | k, a, r | `vref` 2.495 V, `gm` 5 S, `imin` 0.4m A | Behavioural TL431-like programmable shunt reference: 2.495 V reference, about 0.2 ohm dynamic impedance (not a vendor model). `X{ref} {pin:k} {pin:a} {pin:r} lvd_tl431 params: vref={vref} gm={gm} imin={imin}` |
| `logic_inv` | U | a, y | `vdd` 5 V, `rout` 50 ohm, `tpd` 10n s | Behavioural inverter: input threshold vdd/2, output 0 or vdd through rout after tpd; supply is implicit (ground-referenced), works in all analyses. `X{ref} {pin:a} {pin:y} lvd_inv params: vdd={vdd} rout={rout} tpd={tpd}` |
| `logic_buf` | U | a, y | `vdd` 5 V, `rout` 50 ohm, `tpd` 10n s | Behavioural buffer: input threshold vdd/2, output 0 or vdd through rout after tpd; supply is implicit (ground-referenced), works in all analyses. `X{ref} {pin:a} {pin:y} lvd_buf params: vdd={vdd} rout={rout} tpd={tpd}` |
| `logic_and2` | U | a, b, y | `vdd` 5 V, `rout` 50 ohm, `tpd` 10n s | Behavioural and gate: input threshold vdd/2, output 0 or vdd through rout after tpd; supply is implicit (ground-referenced), works in all analyses. `X{ref} {pin:a} {pin:b} {pin:y} lvd_and2 params: vdd={vdd} rout={rout} tpd={tpd}` |
| `logic_nand2` | U | a, b, y | `vdd` 5 V, `rout` 50 ohm, `tpd` 10n s | Behavioural nand gate: input threshold vdd/2, output 0 or vdd through rout after tpd; supply is implicit (ground-referenced), works in all analyses. `X{ref} {pin:a} {pin:b} {pin:y} lvd_nand2 params: vdd={vdd} rout={rout} tpd={tpd}` |
| `logic_or2` | U | a, b, y | `vdd` 5 V, `rout` 50 ohm, `tpd` 10n s | Behavioural or gate: input threshold vdd/2, output 0 or vdd through rout after tpd; supply is implicit (ground-referenced), works in all analyses. `X{ref} {pin:a} {pin:b} {pin:y} lvd_or2 params: vdd={vdd} rout={rout} tpd={tpd}` |
| `logic_nor2` | U | a, b, y | `vdd` 5 V, `rout` 50 ohm, `tpd` 10n s | Behavioural nor gate: input threshold vdd/2, output 0 or vdd through rout after tpd; supply is implicit (ground-referenced), works in all analyses. `X{ref} {pin:a} {pin:b} {pin:y} lvd_nor2 params: vdd={vdd} rout={rout} tpd={tpd}` |
| `logic_xor2` | U | a, b, y | `vdd` 5 V, `rout` 50 ohm, `tpd` 10n s | Behavioural xor gate: input threshold vdd/2, output 0 or vdd through rout after tpd; supply is implicit (ground-referenced), works in all analyses. `X{ref} {pin:a} {pin:b} {pin:y} lvd_xor2 params: vdd={vdd} rout={rout} tpd={tpd}` |
| `logic_schmitt` | U | a, y | `vdd` 5 V, `vtp` 3 V, `vtn` 2 V, `rout` 50 ohm, `tpd` 10n s | Behavioural Schmitt-trigger inverter with rising/falling thresholds; supply is implicit (ground-referenced). `X{ref} {pin:a} {pin:y} lvd_schmitt params: vdd={vdd} vtp={vtp} vtn={vtn} rout={rout} tpd={tpd}` |

### spice-misc

SPICE passives and magnetics, switches, transmission lines and waveform probes.

| id | Ref | Pins | Parameters (default) | Description and model |
|---|---|---|---|---|
| `potentiometer` | RV | a, w, b | `r` 10k ohm, `pos` 0.5 | Three-terminal potentiometer; position 0 puts the wiper at a, 1 at b. `X{ref} {pin:a} {pin:w} {pin:b} lvd_pot params: rt={r} pos={pos}` |
| `coupled_inductors` | TR | p1, n1, p2, n2 | `lp` 1m H, `ls` 1m H, `k` 0.99 | Two coupled inductors (primary lp, secondary ls, coupling k); dots mark p1 and p2. `X{ref} {pin:p1} {pin:n1} {pin:p2} {pin:n2} lvd_xfmr params: lp={lp} ls={ls} k={k}` |
| `transformer` | TR | p1, n1, p2, n2 | `n` 2, `lp` 10m H, `k` 0.999 | Transformer from primary inductance, turns ratio n = Ns/Np and coupling k (secondary inductance lp * n^2). `X{ref} {pin:p1} {pin:n1} {pin:p2} {pin:n2} lvd_xfmr_n params: lp={lp} ratio={n} k={k}` |
| `ideal_transformer` | TR | p1, n1, p2, n2 | `n` 2 | Ideal transformer (controlled sources): V(p2,n2) = n V(p1,n1), works down to DC. `X{ref} {pin:p1} {pin:n1} {pin:p2} {pin:n2} lvd_ideal_xfmr params: ratio={n}` |
| `crystal` | Y | a, b | `rs` 10 ohm, `ls` 10.13m H, `cs` 25f F, `cp` 5p F | Butterworth-Van Dyke crystal model; defaults are a typical 10 MHz fundamental AT-cut crystal. `X{ref} {pin:a} {pin:b} lvd_crystal params: rs={rs} ls={ls} cs={cs} cp={cp}` |
| `vswitch` | S | p, n, cp, cn | `vt` 2.5 V, `vh` 0 V, `ron` 1 ohm, `roff` 1meg ohm | Voltage-controlled switch (S element): on when V(cp,cn) > vt + vh, off below vt - vh. `S{ref} {pin:p} {pin:n} {pin:cp} {pin:cn} lvd_sw_{ref}` ... |
| `iswitch` | W | p, n, cp, cn | `it` 1m A, `ih` 0 A, `ron` 1 ohm, `roff` 1meg ohm | Current-controlled switch (W element): on when the current from cp to cn exceeds it + ih; cp-cn is a 0 V sense branch. `V{ref}_s {pin:cp} {pin:cn} 0` ... |
| `tswitch_close` | S | p, n | `ton` 1m s, `ttr` 1n s, `ron` 10m ohm, `roff` 1G ohm | Switch that is open until time ton and closed afterwards (transient). `X{ref} {pin:p} {pin:n} lvd_tswitch_on params: ton={ton} ttr={ttr} ron={ron} roff={roff}` |
| `tswitch_open` | S | p, n | `toff` 1m s, `ttr` 1n s, `ron` 10m ohm, `roff` 1G ohm | Switch that is closed until time toff and open afterwards (transient). `X{ref} {pin:p} {pin:n} lvd_tswitch_off params: toff={toff} ttr={ttr} ron={ron} roff={roff}` |
| `tline` | T | p1, n1, p2, n2 | `z0` 50 ohm, `td` 1n s | Lossless transmission line (T element) given by z0 and delay td; n1/n2 are the return conductors. `T{ref} {pin:p1} {pin:n1} {pin:p2} {pin:n2} Z0={z0} TD={td}` |
| `tline_f` | T | p1, n1, p2, n2 | `z0` 50 ohm, `f` 100meg Hz, `nl` 0.25 wavelengths | Lossless transmission line (T element) given by z0 and its length nl in wavelengths at f. `T{ref} {pin:p1} {pin:n1} {pin:p2} {pin:n2} Z0={z0} F={f} NL={nl}` |
| `ltra` | T | p1, n1, p2, n2 | `r` 0.1 ohm/m, `l` 250n H/m, `g` 0 S/m, `c` 100p F/m, `len` 1 m | Lossy RLGC transmission line (ngspice LTRA, O element); defaults approximate a 50 ohm coax with v = 2e8 m/s. `O{ref} {pin:p1} {pin:n1} {pin:p2} {pin:n2} lvd_ltra_{ref}` ... |
| `vprobe` | PR | p | none | Marks a node voltage for the waveform viewer; no circuit effect. Probe `voltage`, empty template |
| `vdiffprobe` | PR | p, n | none | Marks the voltage V(p) - V(n) for the waveform viewer; no circuit effect. Probe `differential`, empty template |
| `iprobe` | PR | p, n | none | Ammeter placed in series: a 0 V source; positive current flows into p and out of n. Probe `current`, template `V{ref} {pin:p} {pin:n} 0` |

Model notes:

- **Op-amp macromodel** (`lvd_opamp`): a slew-limited transconductance (maximum current `sr` times 1 nF, tanh input) drives a 1 nF integrator with a resistor giving DC gain `aol`, so the unity-gain frequency is `gbw` and the slew rate `sr`. The internal node is clamped `vohr` below `vcc` and `volr` above `vee`; the output stage is a resistance `rout` with current limit `ilim`, sourcing from `vcc` and sinking into `vee`; `iq` flows from `vcc` to `vee`. Single pole; no common-mode range, CMRR or noise modelling.
- **Comparators**: tanh decision with hysteresis `vhys` through positive feedback of the delayed state, RC delay `tpd`; push-pull output between the rails through `rout`, or open-collector sink to `vee` (limit `isink`, `ron` when saturated).
- **Regulators**: a controlled voltage source sets the output to min(`vout`, V(in) - `vdrop`), not below ground, with a soft current limit at `ilim` (100 V/A above it) and at most about 10 mA of sink current; the output current is drawn from `in`, plus `iq` from `in` to `gnd`. LM317: V(out) - V(adj) = `vref`, `iadj` out of the adj pin. TL431: cathode current `gm` (V(r,a) - `vref`) above the reference, plus `imin`.
- **Logic gates**: each input is a soft threshold 0.5 (1 + tanh((V - vdd/2)/(vdd/40))); the values are combined with the gate's Boolean function, delayed by an RC of `tpd`, re-sharpened and driven 0..`vdd` through `rout`. No flip-flop is provided yet.
- **Timed switches** use an internal PWL control: `ton`/`toff` must be greater than 0.

### spice-instruments

Virtual instruments (requirement SP-5). Their SPICE model carries `"instrument"`; the simulation view shows each one in its Instruments tab from the latest results.

| id | Ref | Pins | Parameters (default) | Description and model |
|---|---|---|---|---|
| `oscilloscope` | SC | ch1, ch2, ref | `tdiv` 100us, `vdiv1` 1 V, `vdiv2` 1 V, `offset1` 0 V, `offset2` 0 V, `trigger` ch1 (ch1, ch2, none), `level` 0 V | Two channels against `ref` from the last transient; shows the latest rising-edge-triggered sweep of ten divisions. Empty template |
| `voltmeter` | VM | p, n | none | V(p) − V(n): operating-point value; DC, RMS and AC RMS of the transient's second half. Empty template |
| `ammeter` | AM | p, n | none | Current entering p, as the voltmeter. `V{ref} {pin:p} {pin:n} 0` |
| `function_generator` | FG | p, n | `shape` sine (sine, square, triangle, sawtooth), `freq` 1kHz, `amplitude` 1 V, `offset` 0 V, `phase` 0 deg, `ac` 1 V | V(p) − V(n) = offset + amplitude · shape(freq · t + phase/360) from a B source, in series with a 0 V source carrying the AC magnitude. Square is tanh-rounded. `V{ref}_ac {pin:p} fg_{ref} DC 0 AC {ac}` + `B{ref} fg_{ref} {pin:n} V=…`, shapes as `.func lvd_fg_<shape>` |
| `spectrum_analyzer` | SA | in, ref | `fmax` 0 (all), `window` hann | Amplitude spectrum in dBV of V(in) − V(ref) from the last transient. Empty template |
| `bode_plotter` | BP | in, out, ref | none | Gain and phase of (V(out) − V(ref)) / (V(in) − V(ref)) from the last AC analysis. Empty template |

## Block and element types

Every type below lists its pins in port order, the parameters the solver reads, and their meaning. "Budget" covers the cascade (gain, noise, linearity) and spur analyses; "linear" is the small-signal S-parameter analysis.

### rf-behavioural blocks

#### `amplifier` — pins `in`, `out`

| Parameter | Unit | Meaning |
|---|---|---|
| `gain` | dB | Small-signal gain; linear: S21 = undb20(gain) |
| `nf` | dB | Noise figure |
| `oip3` | dBm | Output third-order intercept point |
| `oip2` | dBm | Output second-order intercept point |
| `op1db` | dBm | Output 1 dB compression point |
| `isolation` | dB | Reverse isolation; linear: S12 = undb20(-isolation) |
| `z0` | ohm | Port impedance; S11 = S22 = 0 |

Phases of S21 and S12 are 0.

#### `attenuator` — pins `in`, `out`

`il` (dB): S21 = S12 = undb20(-il), matched to `z0`. Noise figure = `il`.

#### `filter` — pins `in`, `out`

| Parameter | Unit | Meaning |
|---|---|---|
| `type` | choice | `lowpass`, `highpass`, `bandpass`, `bandstop` |
| `response` | choice | `ideal`, `butterworth`, `chebyshev` |
| `order` | | Order of the low-pass prototype (number of resonators for bandpass/bandstop), integer >= 1; ignored for `ideal` |
| `fc` | Hz | Lowpass/highpass: passband edge. Bandpass/bandstop: centre frequency |
| `bw` | Hz | Bandpass/bandstop: bandwidth; ignored for lowpass/highpass |
| `il` | dB | Insertion loss added across the whole passband |
| `ripple` | dB | Passband ripple; used by `chebyshev` only |
| `rejection` | dB | Maximum attenuation relative to the passband: the stopband attenuation of `ideal`, and the floor at which `butterworth`/`chebyshev` attenuation is clipped. `il` comes on top, so the stopband transmission is -(il + rejection) dB |
| `z0` | ohm | Port impedance |

Band edges for bandpass/bandstop are f1 = fc - bw/2 and f2 = fc + bw/2. The frequency transformation of the low-pass prototype uses the geometric centre sqrt(f1 · f2) and the bandwidth f2 - f1. The passband edge is the -3 dB point for `butterworth` (relative to `il`), the ripple-band edge for `chebyshev` and the brick-wall edge for `ideal`. The `ideal` response has |S21| = undb20(-il) in the passband and undb20(-(il + rejection)) in the stopband, phase 0. For `butterworth` and `chebyshev`, |S21| (dB) = -il - A(f) with A the prototype attenuation, limited to `rejection`; the phase is that of the prototype response; S11 follows from losslessness of the prototype (|S11|^2 = 1 - |S21_prototype|^2). Noise figure equals the attenuation.

#### `mixer` — pins `rf`, `lo`, `if`

| Parameter | Unit | Meaning |
|---|---|---|
| `gain` | dB | Conversion gain, IF output power over RF input power (negative for a passive mixer) |
| `nf` | dB | Single-sideband noise figure |
| `iip3` | dBm | Input third-order intercept point (RF port) |
| `lo_power` | dBm | Nominal LO drive at which the other parameters apply |
| `lo_rf_isolation` | dB | Attenuation from `lo` to `rf` |
| `lo_if_isolation` | dB | Attenuation from `lo` to `if` |
| `rf_if_isolation` | dB | Attenuation of the RF signal leaking directly to `if` |
| `side` | choice | `low`: f_LO < f_RF and f_IF = f_RF - f_LO; `high`: f_LO > f_RF and f_IF = f_LO - f_RF |
| `product` | choice | The wanted output product, whichever pin the signal enters: `difference` \|f_in - f_LO\|, `sum` f_in + f_LO. `auto` (the default, and files made before this parameter existed): the difference when the signal enters at `rf`; when it enters at `if` (up-conversion), the sum for `side` = low and the difference for `side` = high |
| `spur_table` | file | Optional m×n spur table (format below); a relative path resolves against the project folder; empty: spur levels are estimated from `iip3` |
| `z0` | ohm | Port impedance |

The frequency of the signal at `lo` comes from what drives it (normally an `lo` block). A mixer is frequency-translating: the linear analysis treats it only through its isolations and reports the conversion path as unsupported.

**Spur table file.** A text file with the levels of the m×n products |m·f_RF ± n·f_LO| in dBc below the wanted (1×1) output, as printed in mixer datasheets, for an RF input at the reference power and the LO at `lo_power`. Signs of m and n do not matter (m is the RF harmonic, n the LO harmonic). Rules:

- `#` or `!` starts a comment, to the end of the line; blank lines are ignored. Values are separated by spaces, tabs, commas or semicolons.
- `reference_power <dBm>` (optionally followed by `dBm`): RF input power at which the levels were measured; default -10 dBm.
- Data as rows `m n dBc`, one product per line, or as a matrix: a line `lo_harmonics n1 n2 ...` gives the LO harmonics of the columns, and every following data line is `m level1 level2 ...` with one level per column. `-`, `--`, `x` or `n/a` marks a missing entry. A later entry for the same m×n replaces an earlier one; m = n = 0 is not allowed (skipped in a matrix).
- Errors (unknown keywords, wrong number of values, non-numeric values, no levels at all) are reported with the line number; the analysis then warns and uses the estimate below.

```
! RF harmonics down, LO harmonics across; dBc at -10 dBm RF
reference_power -10
lo_harmonics  0    1    2    3    4
0             -    24   18   30   35
1             28   0    35   12   40
2             65   60   65   58   70
3             80   70   75   68   80
```

Scaling: a level is used at the actual RF input power P of the mixer as dBc(P) = dBc_table - (|m| - 1)·(P - reference_power), because the m-th RF harmonic grows by m dB per dB while the wanted output grows by 1 dB per dB.

**Spur levels without a table** (and for products missing from the table), a conservative rule for a double-balanced mixer, with P the RF input power and IIP3 = `iip3`:

dBc = (|m| - 1)·max(0, IIP3 - P) + 10 dB if m is even + L(n), with L(n) = 20·log10|n| for odd n (the 1/n harmonic content of a switching LO), 20·log10|n| + 25 dB for even n ≥ 2 and 25 dB for n = 0 (balanced LO and RF ports suppress even harmonics). The 1×1 products are 0 dBc. The 1×0 RF feedthrough uses `rf_if_isolation` instead: dBc = `rf_if_isolation` + `gain`. Spurs whose level comes from this rule are marked as estimated.

**Spur analysis.** For every mixer the analysis lists: the m×n products (m ≥ 1, |m| + |n| up to the chosen order) of the wanted signal, carried to the chain output through the following stages (later mixers translate them like the wanted signal), in dBc below the wanted output; the spurious responses — input frequencies that land on the wanted output frequency through some m×n, among them the image (|m| = |n| = 1) and the half-IF response (|m| = |n| = 2, between the LO and the wanted input), with the rejection: the gain of the stages before the mixer at the wanted frequency minus that at the response frequency, plus the mixer's m×n level at the wanted input power; and the LO leakage, the LO drive minus `lo_if_isolation` towards the output and minus `lo_rf_isolation` back to the chain input (swapped for an up-converter, which is entered at `if`).

#### `source` — pin `out`

CW tone that starts a budget chain: `freq` (Hz), `power` (dBm, available power), `z0` (source impedance). In the linear analysis a source is a `z0` termination.

#### `lo` — pin `out`

Local oscillator: `freq` (Hz), `power` (dBm), `z0` (ohm), and `phase_noise`: optional text file with two columns, offset frequency in Hz and single-sideband phase noise L(f) in dBc/Hz, interpolated linearly in log-frequency; empty means no phase noise. In the linear analysis an LO is a `z0` termination.

#### `splitter` — pins `in`, `out1`, `out2`; `combiner` — pins `in1`, `in2`, `out`

Two-way in-phase splitter / combiner; the S-matrix is the same, the block type only sets the signal direction for the budget.

| Parameter | Unit | Meaning |
|---|---|---|
| `il` | dB | Excess insertion loss above the ideal 10·log10(2) = 3.01 dB split |
| `isolation` | dB | Between the two split ports (`out1`–`out2`, `in1`–`in2`) |
| `imbalance` | dB | Amplitude imbalance: port 1 path is +imbalance/2, port 2 path is -imbalance/2 |
| `phase_imbalance` | deg | Phase imbalance: port 1 path is +phase_imbalance/2, port 2 path is -phase_imbalance/2 |
| `z0` | ohm | Port impedance, all ports matched |

Transmission between the common port and split port k: -(3.0103 + il ± imbalance/2) dB. In the budget, a combiner passes each input to `out` with that loss (incoherent addition of different signals).

#### `coupler` — pins `in`, `out`, `cpl`, `iso`

| Parameter | Unit | Meaning |
|---|---|---|
| `coupling` | dB | Attenuation `in` → `cpl` (and `out` → `iso`) |
| `directivity` | dB | Isolation minus coupling: `in` → `iso` (and `out` → `cpl`) is coupling + directivity |
| `il` | dB | Main line loss `in` → `out` (and `cpl` → `iso`) |
| `z0` | ohm | Port impedance, all ports matched |

Reciprocal. Through paths have phase 0, coupled paths +90 degrees (quadrature coupler).

#### `switch_spdt` — pins `com`, `t1`, `t2`

`state` (choice `t1` or `t2`) selects the throw connected to `com` with loss `il` (dB). The other throw sees `isolation` (dB) to `com` and to the selected throw, and is terminated in `z0` (absorptive switch). Reciprocal.

#### `isolator` — pins `in`, `out`

`il` (dB) forward loss `in` → `out`, `isolation` (dB) reverse loss `out` → `in`, matched to `z0`.

#### `circulator` — pins `p1`, `p2`, `p3`

Circulation p1 → p2 → p3 → p1 with loss `il` (dB); the reverse direction (p2 → p1, p3 → p2, p1 → p3) has `isolation` (dB). Matched to `z0`.

#### `cable` — pins `in`, `out`

| Parameter | Unit | Meaning |
|---|---|---|
| `length` | m | Physical length |
| `loss_per_m` | dB/m | Cable loss per metre at `freq` |
| `freq` | Hz | Frequency at which `loss_per_m` is given |
| `il` | dB | Fixed loss independent of length and frequency (connectors) |
| `vf` | | Velocity factor (fraction of c0) |
| `z0` | ohm | Characteristic and port impedance (matched) |

Loss at frequency f: il + loss_per_m · length · sqrt(f / freq) dB (skin-effect scaling). Phase lag 360 · f · length / (vf · c0) degrees.

#### `antenna` — pin `p`

`gain` (dBi) antenna gain, `temp` (K) antenna noise temperature seen at the port when receiving, `z0` (ohm) port impedance. In the budget an antenna at the start of a chain is the input (its `temp` sets the input noise); at the end it is the radiating output (EIRP = output power + gain). In the linear analysis it is a `z0` termination.

#### `multiplier` — pins `in`, `out`

`factor` (integer >= 1): f_out = factor · f_in. `gain` (dB): output power at the wanted harmonic over input power. `rejection` (dBc, positive): level of the other harmonics below the wanted one. `z0`: port impedance. Phase noise increases by 20·log10(factor). Not supported by the linear analysis.

#### `phase_shifter` — pins `in`, `out`

`phase` (deg, positive = lag), `il` (dB), `z0`: S21 = S12 = undb20(-il) · exp(-j · phase · pi/180), matched.

#### `adc` — pin `in`

`fs` (Hz) sample rate, `bits` resolution, `full_scale` (dBm) power of a full-scale sine at the input, `nf` (dB) noise figure of the converter, `z0` (ohm) input impedance. Ends a budget chain; the budget reports the headroom to full scale and aliasing of the frequency plan into 0..fs/2. In the linear analysis it is a `z0` termination.

#### `port` — pin `p`

Simulation port for the linear analysis. `num` (integer >= 1, unique in the schematic) is the S-parameter port number, `z0` (ohm) the reference impedance. In the budget, port 1 marks the chain input.

#### `termination` — pin `p`

Load of impedance `z0` (ohm) to ground; noise at T0.

### sparameters

`nport1` to `nport4` use the `sparameters` model. Touchstone port K is component pin K in pin order. The reference impedance comes from the file.

### rf-element elements

Two-pin elements of the `basic` library are two-terminal elements between their pins (not referenced to ground). All other elements are referenced to the implicit ground.

#### `resistor` — pins `p`, `n`

`r` (ohm): impedance R.

#### `capacitor` — pins `p`, `n`

`c` (F), `q`, `freq` (Hz). Ideal (Z = 1/(j·2π·f·c)) if `q` = 0. Otherwise a series resistance Rs = 1 / (2π · freq · c · q), constant over frequency, so that the Q at `freq` is `q`.

#### `inductor` — pins `p`, `n`

`l` (H), `q`, `freq` (Hz). Ideal (Z = j·2π·f·l) if `q` = 0. Otherwise a series resistance Rs = 2π · freq · l / q, constant over frequency.

#### `tline` — pins `in`, `out`

Ideal lossless TEM line: `z0` (ohm) characteristic impedance, `elen` (deg) electrical length at `freq` (Hz). The electrical length scales linearly: θ(f) = elen · f / freq.

#### `stub_open` — pin `p`

Ideal open-circuited stub from the pin to ground: `z0`, `elen`, `freq` as `tline`. Input impedance -j · z0 · cot θ(f).

#### `stub_short` — pin `p`

Ideal short-circuited stub: input impedance j · z0 · tan θ(f).

#### `coupled_lines` — pins `p1`, `p2`, `p3`, `p4`

Ideal symmetric lossless coupled TEM lines. Line A runs p1 → p2, line B runs p3 → p4; p1 and p3 are at the same end. `z0e` and `z0o` (ohm) even- and odd-mode impedances, `elen` (deg) the electrical length of both modes at `freq` (Hz), scaling linearly with frequency.

#### `microstrip` — pins `in`, `out`

| Parameter | Unit | Meaning |
|---|---|---|
| `w` | m | Strip width |
| `l` | m | Length |
| `h` | m | Substrate height |
| `er` | | Substrate relative permittivity |
| `tand` | | Substrate loss tangent |
| `t` | m | Metal thickness (0: zero-thickness strip) |
| `sigma` | S/m | Conductor conductivity; 0 means no conductor loss |

Z0 and εeff from Hammerstad–Jensen with thickness correction, dispersion after Kirschning–Jansen; dielectric loss from `tand`, conductor loss from `sigma` (skin effect).

#### `stripline` — pins `in`, `out`

Symmetric stripline centred between two ground planes: `w`, `l`, `b` (m, ground-plane spacing), `er`, `tand`, `t`, `sigma` as for `microstrip`.

#### `coax` — pins `in`, `out`

`din` (m) inner conductor diameter, `dout` (m) inner diameter of the outer conductor, `l` (m) length, `er`, `tand`, `sigma`. Z0 = 60 / sqrt(er) · ln(dout / din) ohm; TEM, εeff = er.

#### `cpw` — pins `in`, `out`

Coplanar waveguide: `w` (m) centre strip width, `s` (m) gap to the side grounds, `l`, `h`, `er`, `tand`, `t`, `sigma` as for `microstrip`, and `backed` (choice `no` / `yes`): whether the substrate has a ground plane on its back (conductor-backed CPW). The side grounds are the implicit ground.

#### `transformer` — pins `in`, `out`

Ideal transformer with both windings referenced to ground. `n` = N_secondary / N_primary: V_out = n · V_in, I_out = -I_in / n, so a load Z_L at `out` appears at `in` as Z_L / n².

## Antenna libraries

Built by `tools/make_antenna_libraries.py` (run it again after editing the script; do not edit the files by hand). Used through the antenna library dock or `use_library()` in Python. The sample pattern files are in `libraries/patterns/`.

### antenna-elements

Analytic antenna element models: isotropic, cos^n, dipoles, monopole, patches, apertures and horns.

| Entry | Name | Description |
|---|---|---|
| `isotropic` | Isotropic | Ideal isotropic radiator (array factor only). |
| `cos15` | Generic array element cos^1.5 | Typical embedded element pattern of a well-matched planar array element (about 5.4 dBi, scan loss cos^1.5). |
| `cos12-slant` | cos^1.2 element, slant 45 | Wide-beam base-station style element, slant +45 polarisation. |
| `dipole-half-wave` | Half-wave dipole | Free-space half-wave dipole along x, 2.15 dBi. |
| `dipole-over-ground` | Dipole a quarter wave over ground | Half-wave dipole a quarter wavelength over an infinite ground plane; classic array element. |
| `monopole-quarter-wave` | Quarter-wave monopole | Vertical monopole on an infinite ground plane, 5.16 dBi at the horizon. |
| `patch-x-ro4003` | Patch 10 GHz, RO4003C 0.813 mm | Rectangular patch for 10 GHz on Rogers RO4003C (er 3.55, 0.813 mm), cavity model, dimensions from the design equations. |
| `patch-c-fr4` | Patch 5.8 GHz, FR-4 1.6 mm | Rectangular patch for 5.8 GHz on FR-4 (er 4.4, 1.6 mm); FR-4 losses lower the efficiency. |
| `patch-gnss-rhcp` | GNSS patch L1, RHCP | Ceramic patch for 1575.42 MHz (er 6.15, 4 mm), dual fed for right-hand circular polarisation. |
| `open-waveguide-wr90` | Open waveguide WR-90 | Open-ended WR-90 waveguide (22.86 x 10.16 mm), TE10 aperture model. |
| `horn-x-15dbi` | Pyramidal horn 10 GHz, 15 dBi | Pyramidal horn with a 60 x 46 mm aperture; aperture model without phase error. |
| `horn-ku-20dbi` | Pyramidal horn 12 GHz, 20 dBi | Pyramidal horn with a 92 x 72 mm aperture for 12 GHz; aperture model without phase error. |

### antenna-patterns

Pattern-file elements. The two samples are synthetic; add measured or vendor patterns here.

| Entry | Name | Description |
|---|---|---|
| `sample-horn-x` | Sample measured horn, X band | Synthetic 'measured' pattern of a 15 dBi X-band horn, 9.5 to 10.5 GHz, with measurement ripple and cross-polarisation. |
| `sample-pcb-patch-5g8` | Sample PCB patch 5.8 GHz | Synthetic 'measured' pattern of a patch on a small ground plane: tilted beam, back radiation, 5.7 to 5.9 GHz. |

### antenna-tr-modules

Generic T/R module presets (typical values, not vendor data).

| Entry | Name | Description |
|---|---|---|
| `x-gan-10w` | X-band GaN, 10 W | Generic X-band radar T/R module: GaN HPA, 6-bit phase and attenuation. |
| `x-gaas-2w` | X-band GaAs, 2 W | Generic X-band GaAs T/R module for lighter arrays. |
| `s-gan-50w` | S-band GaN, 50 W | Generic S-band surveillance radar T/R module. |
| `ku-satcom` | Ku-band satcom Rx/Tx channel | Satcom flat-panel channel: low-noise receive, modest transmit. |
| `ka-bf-ic` | Ka-band beamformer IC channel | Silicon beamformer channel at 28 GHz with external LNA/PA. |
| `n78-mmimo` | 3.5 GHz massive-MIMO channel | Base-station radio channel (digital beamforming, fine quantisation). |

### antenna-arrays

Array presets: lines, planar, AESA, monopulse, ring and conformal.

| Entry | Name | Description |
|---|---|---|
| `ula-16` | Linear, 16 elements, half wave | Uniform linear array of 16 elements at 0.5 wavelength. |
| `ula-32-cheb` | Linear, 32 elements, Chebyshev 30 dB | Dolph-Chebyshev line with 30 dB side lobes. |
| `ura-8x8` | Planar 8 x 8, uniform | Rectangular 8 x 8 array at 0.5 wavelength. |
| `ura-16x16-taylor` | Planar 16 x 16, Taylor 30 dB | Rectangular 16 x 16 array with separable Taylor 30 dB, n-bar 5 tapers. |
| `aesa-circular-tri` | Circular AESA, triangular lattice | Radar AESA: equilateral triangular lattice (0.6 wavelength), circular aperture, radial Taylor 35 dB; scans to 60 degrees. |
| `monopulse-16x16` | Monopulse 16 x 16, azimuth difference | 16 x 16 array showing the azimuth difference pattern (switch to Sum for the sum beam). |
| `uca-16` | Ring of 16 elements | Uniform circular array: 16 elements on a ring of 1.3 wavelength radius. |
| `cylinder-sector` | Cylinder sector, 24 x 8 | Conformal sector on a cylinder of 3 wavelength radius: 24 columns over 120 degrees, 8 rows; steered to broadside (+x). |

T/R module values are generic, not vendor data. The analytic element models are isolated elements; for arrays with strong coupling, import an embedded element pattern from a measurement or a full-wave simulation.
