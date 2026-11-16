#pragma once

#include "rfsim/SignalChain.h"

#include <QString>
#include <QStringList>

namespace lvd::rf {

// Mixer spur analysis (requirement RF-8) along a signal chain.

struct SpurOptions {
    int maxOrder = 5;            // largest |m| + |n|
    double bandwidth = 1e6;      // Hz: output band around the wanted output frequency counted as in band
    double minimumLevelDbc = 150; // spurs more than this below the wanted output are not listed
    // Spurious responses are searched for input frequencies in [searchStart, searchStop];
    // NotSet: from 0 to 3 times the highest frequency in the chain.
    double searchStart = NotSet;
    double searchStop = NotSet;
    // Relative `spur_table` paths of mixers resolve against this directory
    // (normally the project directory, as given to the ChainEvaluator).
    QString projectDirectory;
};

struct Spur {
    enum class Kind {
        Product,          // an m x n product of a mixer, generated from the wanted input
        SpuriousResponse, // an input frequency that produces output inside the wanted band
        Image,            // the image response (a spurious response with |m| = |n| = 1)
        HalfIf,           // the half-IF response (|m| = |n| = 2)
        LoLeakage         // LO feedthrough at the output or back to the input
    };

    Kind kind = Kind::Product;
    QString mixer;          // path of the mixer
    int m = 0;              // RF harmonic (signed)
    int n = 0;              // LO harmonic (signed)
    double frequency = 0;   // Hz at the chain output (LoLeakage at the input: at the input)
    double inputFrequency = NotSet; // Hz at the chain input, for responses
    double levelDbm = NotSet;       // at the chain output (or input, for LO leakage to the input)
    double levelDbc = NotSet;       // relative to the wanted output (positive = below)
    double rejectionDb = NotSet;    // responses: how much weaker than a wanted signal of equal input power
    bool inBand = false;            // within bandwidth/2 of the wanted output frequency
    bool estimated = false;         // mixer level estimated (no spur table)
    QString note;
};

struct SpurResult {
    double wantedOutputFrequency = NotSet;
    double wantedOutputDbm = NotSet;
    std::vector<Spur> spurs; // sorted: in-band first, then by level, strongest first
    QStringList warnings;
    QStringList errors;

    bool ok() const { return errors.isEmpty(); }
};

// Analyses every mixer of the chain. inputPowerDbm is the wanted input level
// (normally chain.inputPowerDbm).
SpurResult analyseSpurs(ChainEvaluator &evaluator, const SpurOptions &options);

// Mixer spur table (the `spur_table` file of rf-system:mixer; format in
// docs/libraries.md): levels in dBc below the wanted output for RF at
// referencePowerDbm and LO at the nominal drive.
struct SpurTable {
    double referencePowerDbm = -10;
    struct Entry {
        int m = 0;
        int n = 0;
        double dbc = 0;
    };
    std::vector<Entry> entries;

    // Level in dBc for |m| x |n| at RF input power p, scaled by (|m| - 1) dB per
    // dB of input change; nullopt when the table has no entry.
    std::optional<double> levelDbc(int m, int n, double inputPowerDbm) const;
};

std::optional<SpurTable> readSpurTable(const QString &path, QString *error = nullptr);
std::optional<SpurTable> parseSpurTable(const QByteArray &text, QString *error = nullptr);

// Estimated level in dBc when there is no table (documented in docs/libraries.md).
double estimatedSpurDbc(int m, int n, double inputPowerDbm, double iip3Dbm);

} // namespace lvd::rf
