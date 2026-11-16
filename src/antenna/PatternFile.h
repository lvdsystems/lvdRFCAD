#pragma once

#include "antenna/ElementModel.h"

#include <QString>
#include <QStringList>

#include <optional>

namespace lvd::ant {

// Far-field pattern files (requirements AN-5, docs/file-format.md, section 7.4).
//
// Read:
// - the application's own CSV (written by writePatternCsv),
// - generic CSV or whitespace tables with a header naming the columns: theta,
//   phi, optional frequency, and either a total gain/directivity (dBi or
//   linear), magnitude and phase of E_theta and E_phi, or real and imaginary
//   parts of E_theta and E_phi,
// - CST "ASCII farfield" exports (Theta, Phi, Abs(Dir.), Abs(Theta),
//   Phase(Theta), Abs(Phi), Phase(Phi), Ax.Ratio),
// - FEKO .ffe files (one block per "#Frequency:").
// A total gain without phase is taken as x-polarised (Ludwig 3). Negative
// theta is folded to (|theta|, phi + 180).
struct PatternReadResult {
    PatternTable table;
    QStringList warnings;
    double peakDbi = 0; // largest |E|^2 in the file, in dB (dBi when the file holds gain or directivity)
};

std::optional<PatternReadResult> readPatternFile(const QString &path, QString *error, double defaultFrequency = 0);
std::optional<PatternReadResult> parsePatternText(const QString &text, const QString &sourceName, QString *error,
                                                  double defaultFrequency = 0);

// The application's own CSV: frequency_hz, theta_deg, phi_deg, etheta_re,
// etheta_im, ephi_re, ephi_im.
bool writePatternCsv(const PatternTable &table, const QString &path, QString *error);
QString patternCsvText(const PatternTable &table);

} // namespace lvd::ant
