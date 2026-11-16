#pragma once

// Formatting and parsing helpers of the S-parameter editor (requirement RF-4).

#include "rfdata/Touchstone.h"

#include <QString>

#include <optional>
#include <vector>

namespace lvd::sparam {

// "1.5 GHz", "250 MHz", "0 Hz" (significant digits as given).
QString formatFrequency(double hz, int digits = 6);
// The unit ("Hz", "kHz", "MHz", "GHz") and scale best suited to show a maximum frequency.
QString frequencyUnitFor(double maximumHz, double *scale = nullptr);
double frequencyUnitScale(const QString &unit); // 0 for an unknown unit

// Parses "1.5", "1.5G", "1.5 GHz", "100 MHz", "10k", "2e9 Hz". A number
// without a prefix is multiplied by defaultScale. Returns nullopt on bad input.
std::optional<double> parseFrequency(const QString &text, double defaultScale = 1.0);
// A whitespace, comma or semicolon separated list of frequencies.
std::optional<std::vector<double>> parseFrequencyList(const QString &text, double defaultScale = 1.0,
                                                      QString *error = nullptr);

// Linear or logarithmic grid of `points` frequencies from start to stop (inclusive).
std::vector<double> linearGrid(double start, double stop, int points);
std::vector<double> logGrid(double start, double stop, int points);

// "S21", or "S10,2" when the port count needs two digits.
QString parameterName(const QString &letter, int row, int column, int portCount);

// "50 Ω (all ports)" or "P1 50 Ω, P2 75 Ω".
QString referenceImpedanceText(const std::vector<double> &z0);
bool allEqual(const std::vector<double> &values);

// Touchstone file name extensions: ".s2p" for a version 1 two-port; version 2
// files may use ".ts" or the matching ".sNp".
QString touchstoneExtension(int portCount);
bool extensionMatches(const QString &path, int portCount, int version);
// The path with its extension replaced by the matching ".sNp".
QString withMatchingExtension(const QString &path, int portCount);

QString touchstoneFormatName(rf::TouchstoneFormat format);

} // namespace lvd::sparam
