# lvd RF cad

Electrical CAD for RF engineers: an RF system and circuit editor, a SPICE simulator and an antenna and array designer in one Qt 6 application.

The project is at an early stage. See [requirements.md](requirements.md) for the scope and the development plan, and [docs/file-format.md](docs/file-format.md) for the file formats.

## Status

Phases 0 and 1 are done:

- Projects with RF, SPICE and antenna documents, stored as versioned JSON
- Schematic editor for RF and SPICE documents: placing, wiring with automatic junctions, net labels, moving with wire dragging, rotate and mirror, clipboard, undo and redo, net highlighting, properties and variables panels
- Hierarchical schematics: any schematic can be placed as a block in another, with parameters passed down
- Values with SI prefixes, units and expressions (`2.4GHz`, `4n7`, `f_rf - f_if`)
- Built-in libraries (basic, rf-system, rf-circuit; 37 components), library browser, symbol editor and library manager
- Electrical rule check and a flattened hierarchical netlist
- Python console and scripting through the `lvdrfcad` module ([docs/python-api.md](docs/python-api.md))

Phase 2 (RF linear) is done:

- S-parameter and noise simulation of RF schematics: circuit elements, ideal and physical transmission lines (microstrip, stripline, coax, CPW), behavioural blocks and Touchstone N-ports; hierarchical sheets included
- Analyses stored in the schematic, run in the background, with optional auto-run after every edit
- Results as rectangular plots, Smith charts and tables: dB, phase, group delay, VSWR, input impedance, noise figure, noise parameters, stability factors, markers
- Touchstone 1 and 2 files: reading, writing, and an S-parameter editor (renormalise, renumber, resample, cascade, de-embed, passivity and reciprocity checks)
- Network maths and line models checked against scikit-rf ([tools/generate_rf_reference.py](tools/generate_rf_reference.py))

Phase 3 (RF system) is done:

- Signal chain traced automatically (or along an explicit path) from a source, port or antenna to the load, through splitters, couplers, switches, mixers and multipliers
- Frequency plan with LO tracing, budget cascade per stage and cumulative: gain, NF, IP3, P1dB, IP2, signal and noise levels, SNR, SFDR, compression headroom, ADC headroom, quantisation SNR and aliasing; mismatch between stages from the real S-parameters or an ideal matched cascade
- Swept budget, level diagram, and mixer spur analysis: m x n products, spurious responses, image, half-IF and LO leakage, from spur tables or an estimate

Phase 4 (RF design aids) is done; this is **release 0.1**:

- Matching-network synthesis: L, Pi, T, multi-section ladders, quarter-wave transformers, single and double stubs, with Smith chart paths, response plots and insertion into the schematic
- Live tuning of variables and component parameters with sliders, without touching the document until Apply
- Optimisation towards goals on any measurement over frequency bands (Nelder-Mead or Levenberg-Marquardt), with snapping to standard values
- Standard component values (E3 to E192) in the properties panel, the optimiser and the matching tool
- A self-contained Windows package (ZIP)

Phases 5 and 6 (SPICE) are done; this is **release 0.2**:

- ngspice 47 embedded as a shared library behind an engine interface, running in the background with progress, pause and stop; results checked against the standalone ngspice on reference circuits
- Analyses: operating point, DC sweep, AC, transient, noise, Fourier (THD), transfer function, pole-zero, sensitivity
- Netlister from schematics (hierarchy flattened, templates, shared subcircuits, model files), netlist view and export, import of existing netlists as simulation-only documents
- Waveform viewer: panes of trace expressions, cursors, measurements (rise time, RMS, frequency, THD, ...), FFT; values tables; operating point drawn on the schematic
- Default SPICE library of 82 parts (sources, diodes, transistors, op-amps, comparators, regulators, logic, switches, magnetics, transmission lines, probes) and six virtual instruments (oscilloscope, voltmeter, ammeter, function generator, spectrum analyser, Bode plotter)
- Import of vendor `.lib`/`.mod`/`.subckt` models as library components
- Parameter and temperature sweeps, Monte Carlo, worst case and optimisation, run in parallel worker processes
- Python-defined SPICE models, and the whole SPICE workflow in the Python API

Phase 7 (arrays and AESA) is done; this is **release 0.3**:

- Antenna element documents: isotropic, cos^n, dipoles, monopole, cavity-model patch and aperture/horn models with any polarisation (linear, slant, RHCP, LHCP), and patterns imported from CSV, CST and FEKO files
- Antenna array documents: linear, planar (rectangular and triangular lattices, circular or elliptical apertures), circular rings, conformal cylinders and custom element tables with per-element orientation
- Tapers (Taylor, circular Taylor, Dolph-Chebyshev, cosine, pedestal, Hamming, custom weights per element), phase or true-time-delay steering or none (amplitude steering, switched beams), sub-arrays with time delay, multiple beams, nulls and monopulse difference patterns; horizon (azimuth) cuts for direction-finding arrays
- AESA effects: phase-shifter, attenuator and delay quantisation, amplitude and phase errors, element failures, compared with the ideal array
- Pattern cuts, UV map with grating-lobe circles, 3D pattern (OpenGL), element layout; gain, integrated directivity, beamwidths, side lobes, cross-polarisation and axial ratio; scan and band sweeps (scan loss, squint)
- Pattern synthesis to a side-lobe mask (amplitude and phase, or phase only)
- T/R modules from typed values, library presets or live links to RF schematic budgets; EIRP, G/T and system temperature against scan angle
- Antenna libraries: element models, sample patterns, T/R module presets and array presets; the antenna workflow in the Python API

## Building on Windows

Requirements:

- Visual Studio 2022 with the C++ workload
- CMake 3.24 or newer
- Qt 6.5 or newer, MSVC 2022 64-bit kit
- Python 3.10 or newer, installed with its development files and on `PATH`

pybind11, Catch2, Eigen and ngspice 47 (shared library and console build, from SourceForge) are downloaded by CMake during configuration.

```powershell
$env:QT_ROOT = 'C:/Qt/6.11.1/msvc2022_64'   # your Qt kit folder
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
```

The binaries are in `build/bin/RelWithDebInfo`:

| Binary | Purpose |
|---|---|
| `lvdrfcad.exe` | The application |
| `lvdrfcad-cli.exe` | Runs a Python script against the application core without the GUI; also the worker process of parallel SPICE batches (`--spice-run`) |

### Packages

```powershell
cmake --build --preset windows --target portable    # build/package/lvdRFcad-<version>-win64.zip
cmake --build --preset windows --target installer   # build/package/lvdRFcad-<version>-setup.exe
```

- **Portable ZIP:** self-contained (the programs, the Qt and MSVC runtimes, an embedded Python with its standard library, the built-in libraries and the documentation). Unpack it anywhere and run `lvdrfcad.exe`.
- **Installer:** needs [Inno Setup 6](https://jrsoftware.org/isinfo.php) (found in the per-user or Program Files location; the `installer` target is missing otherwise). It installs for all users or only the current user (asked at start; `/CURRENTUSER` when silent), adds a Start-menu entry and opens `.lvdproj` files with the application. The uninstaller removes everything, including Python's run-time caches.

## Python API

The `lvdrfcad` module is available in the application's Python console and in scripts run by `lvdrfcad-cli`.

```python
import lvdrfcad

project = lvdrfcad.new_project("C:/work/receiver")
project.new_document("rf", "front_end")
for document in project.documents:
    print(document.name, document.type, document.path)
```

## Source layout

| Folder | Content |
|---|---|
| `src/core` | Data model, libraries, expressions, connectivity, netlist, rule check. Depends on QtCore only |
| `src/python` | Embedded interpreter and the `lvdrfcad` module |
| `src/widgets` | Symbol drawing and colours shared by the editors |
| `src/rfdata` | Network data, conversions, Touchstone files, network operations, line models |
| `src/rfsim` | RF element models, linear solver with noise, analyses, measurements |
| `src/plot` | Rectangular, Smith and polar plot widget; colour-map plot (UV maps, element layouts) |
| `src/plot3d` | 3D view (OpenGL, with a QPainter fallback) for antenna patterns and geometry |
| `src/spice`, `src/spice_ui` | ngspice driver, netlister, waveforms and batches; the SPICE simulation view |
| `src/antenna` | Element models, pattern files, array geometry, excitation and AESA effects, far-field engine, synthesis, T/R budget, antenna libraries. No GUI |
| `src/antenna_ui` | Array and element workspaces, antenna library panel |
| `src/simulation_ui` | Analysis dialog and results workspace |
| `src/sparam_ui` | S-parameter (Touchstone) editor |
| `src/schematic_editor` | Schematic canvas, properties and variables panels |
| `src/library_ui` | Library browser, symbol editor, library manager |
| `src/gui` | Main window, docks and dialogs |
| `src/app`, `src/cli` | The two executables |
| `libraries` | Built-in component and antenna libraries ([docs/libraries.md](docs/libraries.md)); check with `python -I tools/validate_libraries.py`; antenna libraries are generated by `tools/make_antenna_libraries.py` |
| `examples` | Example projects, built by `tools/make_examples.py`, `tools/make_spice_examples.py` and `tools/make_antenna_examples.py` |
| `tests` | Core, schematic, expression, GUI and Python API tests; the two GUI modules have their own tests in their folders |

Qt keywords are disabled because they clash with the Python headers: write `Q_SIGNALS`, `Q_SLOTS` and `Q_EMIT`.

## Licence

GNU General Public License, version 3 or later. See [LICENSE](LICENSE).
