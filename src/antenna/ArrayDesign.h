#pragma once

#include "antenna/ArrayGeometry.h"
#include "antenna/ElementModel.h"
#include "antenna/Excitation.h"
#include "antenna/Synthesis.h"

#include <QJsonObject>
#include <QString>

namespace lvd::ant {

// One side (transmit or receive) of the T/R module (requirements AN-3,
// decision 9.4): typed values, a library preset, or a live link to the
// budget analysis of an RF schematic in the same project.
struct TrSideSpec {
    QString source = QStringLiteral("typed"); // "typed", "preset", "linked"
    QString preset;    // antenna library key "library:entry"
    QString document;  // linked RF schematic
    QString analysis;  // its budget analysis (id or name); empty: the first one
    double powerDbm = 40; // transmit: output power per element
    double gainDb = 25;
    double nfDb = 3;      // receive: noise figure

    QJsonObject toJson() const;
    static TrSideSpec fromJson(const QJsonObject &object, bool transmit);
};

struct TrModuleSpec {
    TrSideSpec tx;
    TrSideSpec rx;
    double frontLossDb = 0.5;       // between the T/R module and the radiator (radome, feed, circulator)
    double beamformerLossDb = 6;    // receive combining loss after the LNA (excess over ideal)
    double antennaTemperatureK = 290;
    double ambientTemperatureK = 290;
    bool txTaper = false;           // false: every PA at full power (uniform transmit amplitude)

    QJsonObject toJson() const;
    static TrModuleSpec fromJson(const QJsonObject &object);
};

// Scan and band sweeps shown by the array workspace.
struct SweepSpec {
    double scanPlanePhiDeg = 0;
    double scanMaxDeg = 60;
    double scanStepDeg = 2;
    double bandStart = 0, bandStop = 0; // Hz; 0: +-10 % around the operating frequency
    int bandPoints = 21;

    QJsonObject toJson() const;
    static SweepSpec fromJson(const QJsonObject &object);
};

// Content of an antenna array document (docs/file-format.md, section 7.2).
struct ArrayDesign {
    // "element:<document>", "library:<library>:<entry>" or empty for the
    // inline element below.
    QString elementRef;
    ElementSpec inlineElement;
    double frequency = 10e9;        // operating frequency, Hz
    double designFrequency = 10e9;  // spacings in wavelengths and phase steering, Hz
    GeometrySpec geometry;
    ExcitationSpec excitation;
    AesaSpec aesa;
    TrModuleSpec trModule;
    SynthesisSpec synthesis;
    SweepSpec sweeps;
    QJsonObject view;               // user-interface state, kept as is

    QJsonObject toJson() const;
    static ArrayDesign fromJson(const QJsonObject &object);
};

// Content of an antenna element document (docs/file-format.md, section 7.1).
struct ElementDesign {
    ElementSpec element;
    QString notes;
    QJsonObject view;

    QJsonObject toJson() const;
    static ElementDesign fromJson(const QJsonObject &object);
};

} // namespace lvd::ant
