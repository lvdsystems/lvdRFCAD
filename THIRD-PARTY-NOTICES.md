# Third-party software in lvd RF cad

lvd RF cad is licensed under the GNU General Public License, version 3 or later (`LICENSE`). The packages and installers include the following third-party software, each under its own licence. The licence texts are in the `licenses` folder unless stated otherwise.

| Component | Version | Use | Licence | Text |
|---|---|---|---|---|
| Qt (Core, Gui, Widgets, OpenGL, OpenGL Widgets, Svg, Network, Print Support) | 6.11.1 | User interface, linked dynamically | LGPL-3.0 | `licenses/LGPL-3.0.txt` and `LICENSE` (GPL-3.0) |
| Python | 3.14 | Embedded interpreter and standard library | Python Software Foundation License 2 | `licenses/Python-LICENSE.txt` |
| pybind11 | 3.0.1 | Python bindings (compiled in) | BSD-3-Clause | `licenses/pybind11-LICENSE.txt` |
| Eigen | 3.4.0 | Linear algebra (compiled in) | MPL-2.0 | `licenses/MPL-2.0.txt` |
| HDF5 | 2.2.0 | Reading openEMS result files (statically linked) | BSD-3-Clause style | `licenses/HDF5-LICENSE.txt` |
| ngspice | 47 | SPICE simulator, in the `ngspice` folder (shared library) | Modified BSD, with exceptions listed in its COPYING | `licenses/ngspice-COPYING.txt` |
| libsndfile | as shipped with ngspice 47 | `ngspice/sndfile.dll` | LGPL-2.1 | `licenses/LGPL-2.1.txt` |
| libsamplerate | as shipped with ngspice 47 | `ngspice/samplerate.dll` | BSD-2-Clause | https://github.com/libsndfile/libsamplerate |
| openEMS, nf2ff | 0.37.0 | Full-wave solver, in the `openems` folder, run as separate programs | GPL-3.0-or-later | `LICENSE`; component list in `openems/NOTICE.txt` |
| CSXCAD, fparser | as shipped with openEMS 0.37.0 | Libraries of openEMS | LGPL-3.0 | `licenses/LGPL-3.0.txt` |
| Boost | 1.91 | Libraries of openEMS | Boost Software License 1.0 | `licenses/BSL-1.0.txt` |
| VTK, HDF5, zlib and others | as shipped with openEMS 0.37.0 | Libraries of openEMS | Permissive (BSD-3-Clause, zlib and similar) | `openems/NOTICE.txt` |
| Microsoft Visual C++ runtime, OpenMP runtime, DirectX shader compiler (`dxcompiler.dll`, `dxil.dll`) | | Run-time libraries | Microsoft redistribution terms | Visual Studio and Windows SDK licence terms |

Qt itself contains third-party code (for example FreeType, HarfBuzz, libpng, zlib, PCRE2); its licences are listed in the Qt documentation, "Licenses Used in Qt" (https://doc.qt.io/qt-6/licenses-used-in-qt.html).

## Sources

The source code of lvd RF cad is available from its authors under the GPL. The source code of the LGPL and GPL components is available from their projects:

- Qt: https://download.qt.io/official_releases/qt/6.11/
- ngspice: https://sourceforge.net/projects/ngspice/files/ng-spice-rework/47/
- libsndfile: https://github.com/libsndfile/libsndfile
- openEMS, CSXCAD, fparser: https://github.com/thliebig/openEMS-Project (tag v0.37.0)

Qt and the other LGPL libraries are linked dynamically; you may replace their DLLs with your own builds of compatible versions.

## Built-in library data

The component and antenna libraries are written for lvd RF cad and are distributed under its licence. Their values are generic and typical, not vendor data, with these exceptions: the SPICE models of the 2N3904, 2N2222A, BC547B and 1N4148 use parameter sets that circulate widely in public SPICE model collections; they are standard textbook models of these common parts. Measured or vendor data you add to your own libraries remain under their owners' terms.

## Development-only tools

Catch2 (BSL-1.0) is used for the tests and is not shipped. CMake, Inno Setup and the MSVC compiler build the packages and are not shipped.
