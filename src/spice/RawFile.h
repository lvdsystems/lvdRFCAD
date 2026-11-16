#pragma once

#include "spice/SpiceData.h"

#include <QByteArray>
#include <QList>
#include <QString>

namespace lvd::spice {

// SPICE raw files (the "-r" output of ngspice and other SPICE programs),
// ASCII and binary, possibly holding several plots. Vector names are
// normalised as SpiceVector documents (lower case, "v(node)", "i(source)").
std::optional<QList<SpicePlot>> readRawFile(const QString &path, QString *error = nullptr);
std::optional<QList<SpicePlot>> parseRawData(const QByteArray &data, QString *error = nullptr);

// Writes plots as a binary raw file that readRawFile reads back exactly
// (an extra "Plotid" header keeps the plot names).
bool writeRawFile(const QList<SpicePlot> &plots, const QString &path, QString *error = nullptr);
QByteArray rawData(const QList<SpicePlot> &plots);

// Plot type ("tran", "ac", ...) from a raw file's "Plotname" line.
QString plotTypeFromTitle(const QString &plotName);

} // namespace lvd::spice
