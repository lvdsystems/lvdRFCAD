# lvd RF cad: requirements and development plan

Derived from [concept.md](concept.md) and the scoping answers of 2026-11-15.

## 1. Decisions taken

| Topic | Decision |
|---|---|
| First deliverable | RF editor; SPICE and antenna follow on the same editor core |
| Stack | C++20, Qt 6 Widgets, CMake |
| Simulation engines | Embed open-source engines: ngspice (SPICE), openEMS (full-wave EM); own solvers for RF linear, budget, spur and array factor |
| Distribution | Open source |
| RF editor level | System blocks and circuit elements mixed in one schematic |
| RF analyses | Budget cascade, linear S-parameter simulation, mixer spur analysis, matching and tuning |
| Antenna depth | Array factor and AESA design, plus full-wave 3D through openEMS |
| Antenna geometry | Parametric templates first, STEP/STL import; no general 3D modeller |
| Libraries | Generic parameterised parts plus importers; no redistributed vendor data |
| SPICE features | Standard analyses, virtual instruments, sweeps and statistics, annotated schematic |
| Scripting | Embedded Python from the first release |
| Team and platform | One developer with Claude Code; Windows first, code kept portable to Linux |

Consequence of these choices: openEMS is GPLv3, so the application is licensed **GPL-3.0-or-later**. This also allows GPL libraries such as QCustomPlot.

## 2. Scope decisions confirmed on 2026-11-15

1. **Project files**: a folder of JSON text files, one per document, so they diff and merge in git. No single-file archive.
2. **Nonlinear RF**: behavioural only. Compression, intermodulation and spurs come from block parameters (P1dB, IP3, spur tables). No harmonic balance; transistor-level nonlinear work is done in the SPICE document.
3. **PCB**: no layout, no footprints, no netlist export to PCB tools.
4. **Integration between document types**: file hand-offs only (Touchstone, far-field pattern files). No live links and no co-simulation.
5. **UI language**: English, with every visible string translatable through Qt's translation mechanism so languages can be added later.
6. **SPICE engine**: ngspice is the only engine shipped. It sits behind an engine interface so that a custom in-house engine can replace or join it later without reworking the editor, viewer or instruments.
7. **Component definitions**: a schematic references library components by name and also keeps a cached copy of each definition, used only when the library is missing (see [docs/file-format.md](docs/file-format.md)).
8. **Connectivity**: derived from wire geometry; nets are not stored.
9. **Symbols**: one symbol per component definition.

## 3. Functional requirements

### 3.1 Common platform (all document types)

- **PL-1** Multi-document main window: tabs, dockable panels (library browser, properties, project tree, Python console, messages).
- **PL-2** Project: a folder holding documents, local libraries, data files and results.
- **PL-3** Schematic canvas shared by RF and SPICE documents: place, move, rotate, mirror, wire with rubber-banding, net labels, buses not required, grid and snap, zoom and pan, copy and paste, unlimited undo and redo.
- **PL-4** Hierarchy: a schematic can be used as a sub-schematic block with ports, with parameters passed down.
- **PL-5** Symbol editor: draw graphics, define pins and parameters, attach one or more models (S-parameter file, behavioural parameters, SPICE model or subcircuit, Python code).
- **PL-6** Library manager: built-in, user and project libraries; search by name, category and parameter; libraries are plain files that can be shared.
- **PL-6a** A schematic uses the library version of each component when the library is available and its own cached copy otherwise. Parts drawn from the cache, and parts whose library definition differs from the cache, are flagged in the editor and in the rule check, because the same file can then behave differently on two machines.
- **PL-7** Plotting: rectangular, Smith chart, polar, tables; cursors, markers, multiple traces and axes; export to PNG, SVG and CSV.
- **PL-8** Parameters and equations: named variables and expressions usable in any component value; parameter sweeps over them.
- **PL-9** Python: embedded interpreter and console; API covering documents, components, simulations and results; component models written in Python; scripts runnable without the GUI.
- **PL-10** The core (data model, netlisting, solvers) is a library with no GUI dependency (QtCore only), used by both the GUI and the Python API.
- **PL-11** Electrical rule check: unconnected pins, shorted outputs, missing models, port impedance mismatches as warnings.
- **PL-12** Printing and PDF export of schematics.

### 3.2 RF editor (MVP)

- **RF-1** System blocks with behavioural parameters: amplifier, attenuator, filter (ideal responses and from file), mixer, LO and source, splitter and combiner, coupler, switch, isolator and circulator, cable, antenna port, ADC and DAC boundary, generic N-port.
- **RF-2** Circuit elements: R, L, C with Q and parasitics, ideal and physical transmission lines (microstrip, stripline, coax, CPW), stubs, coupled lines, transformers, ports and terminations.
- **RF-3** S-parameter N-port from Touchstone (v1 and v2, any port count), with interpolation and stated extrapolation behaviour; noise parameters read where present.
- **RF-4** S-parameter editor: view and edit data as table and plots, change reference impedance, renumber ports, de-embed, cascade, convert between S, Y, Z, ABCD and T, check passivity and reciprocity, save as Touchstone.
- **RF-5** Linear simulation: frequency sweep of the whole network; outputs S-parameters, VSWR, group delay, stability factors and circles, gain circles, noise figure and noise circles.
- **RF-6** Budget cascade along a selected signal path: per stage and cumulative gain, noise figure, P1dB, IIP3 and OIP3, IIP2, signal and noise power, SNR, SFDR, compression headroom. Shown as table and level diagram, at one frequency or swept.
- **RF-7** Budget uses mismatch between stages when S-parameter data is available, and ideal matched cascade otherwise; the mode is shown to the user.
- **RF-8** Mixer spur analysis: m x n products up to a user-set order, image and half-IF, LO leakage, with filter rejection along the chain applied; spur chart and table with levels relative to the wanted signal.
- **RF-9** Frequency plan: each net segment carries its frequency band, translated through mixers and multipliers.
- **RF-10** Matching: L, pi, T and multi-section synthesis with lumped or distributed elements, drawn on the Smith chart and insertable into the schematic.
- **RF-11** Tuning: sliders on chosen parameters with live plot update.
- **RF-12** Optimisation: goals on any result (for example S11 below -15 dB over a band), gradient-free and gradient optimisers, results applied back to the schematic.
- **RF-13** Component values from standard series (E12, E24, E96) on request.

### 3.3 SPICE simulator

- **SP-1** ngspice embedded as a shared library, running in a worker thread with progress, pause and abort. The editor, waveform viewer and instruments use an engine interface (netlist generation, run control, result access), not ngspice directly, so a custom engine can be added later.
- **SP-2** Analyses: operating point, DC sweep, AC, transient, noise, Fourier, transfer function, pole-zero, sensitivity.
- **SP-3** Sweeps and statistics: parameter stepping, temperature, Monte Carlo with tolerances and distributions, worst case, optimisation toward goals.
- **SP-4** Waveform viewer: cursors, measurements (rise time, RMS, peak, frequency, THD), expressions on traces, FFT, multiple panes.
- **SP-5** Virtual instruments attached to nodes: oscilloscope, function generator, multimeter, spectrum analyser, Bode plotter.
- **SP-6** Annotated schematic: node voltages, branch currents and power on the drawing after a run; voltage, current and differential probes placed on wires.
- **SP-7** Models: import of vendor SPICE files (.lib, .mod, .cir, .subckt), automatic symbol proposal from a subcircuit pin list, encrypted models not supported.
- **SP-8** Custom components: subcircuit text, behavioural sources and expressions, Python-defined models, sub-schematics.
- **SP-9** Default library: passives, sources, generic and common discrete semiconductors, op-amps, comparators, regulators, logic, switches, transformers, transmission lines.
- **SP-10** Netlist view and export; import of an existing SPICE netlist as a simulation-only document.

### 3.4 Antenna design

- **AN-1** Array designer: linear, planar (rectangular and triangular lattice), circular, conformal by coordinates, and arbitrary element positions from a table or script.
- **AN-2** Excitation: amplitude tapers (uniform, Taylor, Chebyshev, cosine, custom), phase steering to a direction, sub-arrays.
- **AN-3** AESA design: phase shifter and attenuator quantisation, element failures, amplitude and phase errors, true-time-delay versus phase steering and beam squint, scan loss, grating lobe diagram, per-element T/R module budget (EIRP, G/T) linked to RF-editor chains.
- **AN-4** Beamforming: multiple simultaneous beams, null placement, sum and difference patterns.
- **AN-5** Element patterns: isotropic, analytic (cos^n, dipole, patch), imported (CSV, common far-field formats), or computed by openEMS.
- **AN-6** Pattern outputs: 2D cuts, 3D pattern, UV plot, directivity, gain, beamwidth, side-lobe level, polarisation and axial ratio.
- **AN-7** Parametric antenna templates: patch (probe, inset and aperture fed), dipole and monopole, PCB inverted-F, slot, horn, Vivaldi, helix, with substrate and stack-up definition.
- **AN-8** 3D viewer for geometry, mesh, ports and fields; STEP and STL import with material and port assignment.
- **AN-9** openEMS run: automatic mesh from geometry with user refinement, port and boundary setup, run in a separate process with progress and abort, results as S-parameters, impedance, efficiency and far field.
- **AN-10** Small arrays simulated full-wave to obtain embedded element patterns and mutual coupling, usable in AN-5.

## 4. Non-functional requirements

- **NF-1** Windows 10 and 11 x64 first; no Windows-only APIs outside a platform layer, so a Linux build is a packaging task.
- **NF-2** Schematic editing stays responsive with 2,000 components; a linear RF sweep of 100 components over 1,001 points completes within one second, so tuning feels live.
- **NF-3** Simulations never block the UI and can always be cancelled.
- **NF-4** Solver results are checked against reference cases (analytic results, published examples, ngspice and scikit-rf output) in automated tests.
- **NF-5** File formats are versioned, with forward migration on load.
- **NF-6** Installer bundles Qt, ngspice, openEMS and Python; no separate installs.
- **NF-7** Crash safety: autosave and recovery.

## 5. Architecture

```
app (Qt Widgets GUI) ──┐
python module ─────────┼──> core library (no GUI)
command-line runner ───┘      ├─ model: project, documents, schematic, symbols, libraries
                              ├─ netlist and hierarchy flattening
                              ├─ rf: network solver, noise, budget, spur, matching, optimiser
                              ├─ spice: netlister, ngspice shared-library driver, raw data
                              ├─ antenna: array factor, templates, openEMS driver, far-field data
                              └─ io: Touchstone, SPICE models, STEP/STL, pattern files
```

| Need | Choice | Licence |
|---|---|---|
| GUI | Qt 6 Widgets, QGraphicsView canvas | LGPL/GPL |
| Linear algebra | Eigen | MPL-2.0 |
| 2D plots | QCustomPlot, own Smith chart item | GPL-3.0 |
| 3D view | Own QOpenGLWidget renderer (pattern surfaces, triangle meshes, grid lines, field slices) | LGPL (Qt) |
| STEP import | OpenCASCADE | LGPL-2.1 |
| SPICE | ngspice shared library | BSD |
| Full-wave EM | openEMS with CSXCAD, run as a separate process | GPL-3.0, LGPL-3.0 |
| Python | CPython embedded through pybind11 | PSF, BSD |
| Build and packages | CMake, MSVC; pybind11 and Catch2 through CMake FetchContent, vcpkg once heavier dependencies arrive | |
| Tests | Catch2, plus reference-data comparisons | BSL-1.0 |

Design rules:

- A component has one symbol and several models; each analysis picks the model it understands. A behavioural amplifier therefore supplies an S-matrix to the linear solver and gain, NF and IP3 to the budget.
- Mixers and other frequency-translating blocks split the network into frequency domains. The linear solver works within one domain; the budget and spur analyses work across them.
- Every GUI edit is a command object on the core model, which gives undo and the Python API the same path.

## 6. Development plan

Durations are rough estimates for one developer working with Claude Code. They are not based on measured velocity; revise after phase 1.

| Phase | Content | Exit criterion | Estimate |
|---|---|---|---|
| 0. Foundation (done 2026-11-15) | Repository, CMake and vcpkg, CI on Windows, core library skeleton, main window with docks, project and document framework, JSON persistence, embedded Python console | Empty documents can be created, saved, reopened and listed from Python | 4 to 6 weeks |
| 1. Schematic core (done 2026-11-15) | Canvas, wiring, nets, undo, properties panel, symbol editor, library manager, hierarchy, parameters and equations, rule check | A hierarchical schematic can be drawn from library parts and produces a correct netlist | 6 to 8 weeks |
| 2. RF linear (done 2026-11-16) | Touchstone I/O, network solver with noise, circuit elements and transmission lines, plots and Smith chart, S-parameter editor | Results match scikit-rf on a reference set within tolerance | 6 to 8 weeks |
| 3. RF system (done 2026-11-16) | Behavioural blocks, frequency domains, budget cascade, spur analysis, level diagram | A superheterodyne receiver example reproduces a hand-calculated budget and spur table | 4 to 6 weeks |
| 4. RF design aids (done 2026-11-16, release 0.1) | Matching synthesis, tuning, optimiser, standard-value snapping, generic RF library, installer | **Release 0.1: RF editor** | 4 to 6 weeks |
| 5. SPICE basics (done 2026-11-16) | ngspice driver, netlister, model import, waveform viewer, analyses SP-2, annotated schematic, default library | A set of reference circuits matches standalone ngspice | 8 to 10 weeks |
| 6. SPICE advanced (done 2026-11-16, release 0.2) | Virtual instruments, sweeps, Monte Carlo, worst case, optimisation, Python models | **Release 0.2: SPICE** | 4 to 6 weeks |
| 7. Arrays and AESA (done 2026-11-16, release 0.3) | Array designer, excitations, quantisation and errors, pattern plots in 2D, 3D and UV, imported element patterns, link to RF chains | **Release 0.3: array designer**; patterns match analytic cases | 6 to 8 weeks |
| 8. Full-wave | Templates, 3D viewer, stack-up, meshing, openEMS driver, far field, STEP/STL import, embedded element patterns | **Release 0.4**; a patch antenna matches a published openEMS example | 10 to 14 weeks |
| 9. Consolidation | Linux build, documentation, examples, performance, cross-document hand-offs | **Release 1.0** | 6 to 8 weeks |

Total: roughly 58 to 80 weeks of full-time work.

## 7. Main risks

1. **Scope.** This is three applications. The plan is only credible because each release is usable alone; phases 7 and 8 can be reordered or dropped without affecting earlier ones.
2. **Automatic FDTD meshing** is the hardest single item. Limiting geometry to parametric templates first lets each template carry its own meshing rules.
3. **openEMS on Windows**: packaging and process control need a spike early in phase 8, before the templates are built.
4. **Mixed system and circuit schematic**: the frequency-domain rule in section 5 must be prototyped in phase 2, before behavioural blocks are added, or phase 3 forces a solver rewrite.
5. **Python models inside frequency sweeps** can break NF-2. Models are called with whole frequency vectors, not point by point.
6. **ngspice shared-library threading** and its global state allow only one simulation per process; parallel Monte Carlo needs worker processes.
7. **Library size.** "Big default library" is met by generic parts and importers, not by vendor data; real-part coverage depends on users or a later community repository.

## 8. Follow-ups from phase 1

Small items found while building phase 1, to fold into later phases:

1. Core: a change notification for sheet definitions (so library previews and placed sheet blocks refresh when a sheet's ports change) and a document-rename signal so `sheet:<name>` references can follow.
2. Core: cache `Project::resolve` results instead of comparing full definitions on every call; a spatial index for connectivity and junction detection beyond about 2,000 parts.
3. Core: move the symbol editor's extra checks into `ComponentDefinition::validate()` (graphics that `fromJson` rejects, empty name, reserved parameter names), so there is one rule set.
4. Core: make the junction, label and text JSON writers public, as `componentToJson` is; the editor's clipboard copies them.
5. Format: an orientation for net labels (they are always drawn to the right).
6. Widgets: `SymbolPainter::paintPreview` with options (pin names, orientation), and a text-measuring helper shared by the editor.
7. Editor: rotating or mirroring a selection does not drag attached wires; plural strings need an English translation file.
8. Python: `Schematic.connect()` routes with a single bend, so a wire can cross the target symbol when the pin points up or down (the port symbol's pin is on top); route according to the pin direction.

Follow-ups from phase 2:

9. Smith chart: gain and noise circles (stability circles exist); a shared stability-circle helper in `NetworkOps` instead of the simulation UI's local one.
10. Plot widget: a public legend double-click signal, `setTraceUnit`, traces excluded from the legend and shaded x-ranges (the S-parameter editor draws check highlights as overlay traces).
11. Network operations: de-embedding does not carry noise data; no mixed-mode Touchstone; no surface-roughness model for lines.
12. Results table column choices are not saved with the document.
13. Upstream: scikit-rf 2.2 calls `ellipk(k)` with the modulus instead of the parameter in its CPW conductor loss (20 to 25 % too low); its reading of normalised version 1 Y data also looks wrong. Worth reporting to the scikit-rf project.

Follow-ups from phase 3:

14. Budget: shunt parts on the signal path (a stub or a shunt capacitor between blocks) are skipped with a warning instead of being folded into the cascade; mixer P1dB is estimated as IIP3 - 10 dB; the LO drive level uses passband losses of the LO path only.
15. Spurs: the estimate for mixers without a spur table is a documented heuristic (docs/libraries.md); example spur tables for common mixers would make results more realistic.
16. Plot widget: an integer or minimum tick step, so the level diagram's stage axis has no half-stage ticks.

Follow-ups from phase 4:

17. Installer: only the portable ZIP is built; `packaging/lvdrfcad.iss.in` makes a setup.exe once Inno Setup is installed. No code signing.
18. Optimisation: budget measurements (gain, NF, IP3 of a chain) are not goals yet; `evaluateDesign` and `optimize` check for cancellation between simulations only; the tuning picker offers the analysed sheet's own parts, not sub-sheet parts.
19. Library: the generic library is the 37 parameterised parts plus importers; more generic parts (DC block, bias tee, balun) would help.

Found while making the manual (release 0.1.0):

20. The mixer symbol's pin names "rf"/"if" are drawn over the circle's diagonal lines.
21. Each plot pane has a "Traces ×" header row; it costs height and the × looks like closing the plot.
22. The manual's rule-check screenshot shows a clean check; an example with a deliberate problem would illustrate findings better.

Found while building phases 5 and 6 (release 0.2.0):

23. ngspice runs without a compatibility mode (`ngbehavior`); some PSpice- or LTspice-only constructs in vendor models need editing. A per-project or per-model compatibility setting would help.
24. The ngspice 47 shared-library package lacks `sndfile.dll` and `samplerate.dll`; they are taken from the console package of the same release (`cmake/Ngspice.cmake`). Worth reporting upstream.
25. Annotation and probes cover the drawn sheet only; currents inside subcircuit instances (op-amp macromodels, imported subcircuits) are not shown.
26. Worker processes for batches start ngspice for every run; persistent workers would make batches of short simulations faster. Optimisation runs in-process and sequentially.
27. Tolerance globs (`R*.r`) and batch targets reach the analysed sheet only, not sub-sheets.
28. Waveform viewer: a trace scaled by a number keeps its unit (`i(V1)*1000` is labelled A); frequency and period need two rising crossings in the measured range.
29. Logic is behavioural (gates, Schmitt trigger); no flip-flops and no XSPICE digital simulation yet.
30. Python models exist for SPICE only; the RF linear solver does not call Python models yet (the file format and symbol editor already offer the kind).

Found while building phase 7 (release 0.3.0):

31. Mutual coupling enters only through the element pattern; the analytic element models are isolated elements. Embedded element patterns from openEMS (AN-10, phase 8) will close this.
32. Difference patterns use the sum taper with the sign flipped; there is no Bayliss taper yet.
33. Synthesis works on the array factor of the positions projected onto the xy-plane with isotropic elements; nulls are placed in the array factor too, so rotated (conformal) elements fill them partly.
34. A linked T/R chain uses the first stage-to-end values of its budget analysis for every element; there is no per-element spread of the chain and no amplifier compression model in the EIRP.
35. The integrated directivity is computed for arrays of up to 4096 elements (it integrates the full sphere).
36. Edits made from Python to antenna documents are not on the workspace's undo stack.
37. The 3D view needs OpenGL on screen (images and screenshots fall back to QPainter); `opengl32sw.dll` is not deployed.
38. Pattern files: no NSI, MI or HFSS `.ffd` readers yet, and computed array patterns are exported only through the plot actions (PNG, CSV).

## 9. Antenna design decisions (2026-11-16)

Agreed before phase 7:

1. **Integration.** Antenna design lives in the same application and project, with its own workspace (as the SPICE view has), not a separate executable or a separate mode.
2. **Documents.** Two document types: *Antenna element* (`.lvdelem`; analytic or imported now, full-wave templates in 0.4) and *Antenna array* (`.lvdant`; geometry, excitation, AESA settings, T/R module). An array references an element by name, as a schematic references a sheet; several arrays can share one element.
3. **Antenna library.** A new library kind with element models (isotropic, cos^n, dipoles, monopole, cavity-model patch, aperture/horn), measured or vendor pattern wrappers (user-filled, one or two samples), T/R module presets (generic values) and array presets (ULA, URA rectangular and triangular, circular, with tapers). Substrates and full-wave templates follow in 0.4.
4. **RF link.** Each array has Tx and Rx T/R module sections; each is typed values or a library preset, or a live link to an RF schematic's budget analysis in the same project, re-evaluated when the chain changes. Beamformer and radome losses are typed. EIRP and G/T are computed against scan angle.
5. **3D view.** Own QOpenGLWidget renderer instead of VTK: no new dependency or installer growth; enough for pattern surfaces in 0.3 and for triangle geometry (templates, STL, STEP tessellated by OpenCASCADE), mesh lines and field slices in 0.4.
6. **Release 0.3 scope.** The phase 7 core (AN-1, AN-2, AN-3, AN-5, AN-6 outputs, Python API) plus beamforming (AN-4: multiple beams, nulls, sum and difference), full polarisation (vector element patterns, Ludwig-3 co/cross, axial ratio, RHCP/LHCP), conformal arrays with per-element orientation, and pattern synthesis against a mask (amplitude and phase, or phase only).

## 10. Full-wave decisions (2026-11-16)

Agreed before phase 8, after a spike on openEMS 0.37.0 for Windows (official MSVC build, GPL/LGPL):

1. **Solver.** The official openEMS Windows release is bundled and run as separate processes: `openEMS.exe` on an XML model written by the application, `nf2ff.exe` for the far field. Only the files they need are shipped (about 56 MB: no AppCSXCAD, Qt copy or tutorials).
2. **HDF5.** The HDF5 C library (BSD-3) is built with FetchContent and linked statically; a small C++ reader loads far fields and field dumps.
3. **Documents.** Full-wave models live in antenna element documents as a new model, *Full-wave (openEMS)*: template or imported geometry, stack-up, mesh and run settings. The computed far field becomes the element's pattern, so arrays use it unchanged; S11 and input impedance show in the element workspace. An *embedded in an N × M array* option gives embedded element patterns (AN-10).
4. **CAD import.** STL in 0.4 (ASCII and binary, unit and material per body); STEP (OpenCASCADE) later.
5. **Templates.** All of AN-7 in 0.4: probe-, inset- and aperture-fed patches; dipole and monopole; PCB inverted-F and slot; horn, Vivaldi and helix.

## 11. Immediate next steps

1. Phase 8, full-wave, with the decisions of section 10: finish the openEMS spike (process control, result files, run time), then the HDF5 reader, XML writer and mesher with the probe-fed patch checked against the openEMS Simple Patch Antenna tutorial, then the other templates, STL import and embedded element patterns — towards release 0.4.
