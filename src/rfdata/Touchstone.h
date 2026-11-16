#pragma once

#include "rfdata/RfTypes.h"

#include <QByteArray>
#include <QStringList>

namespace lvd::rf {

// Touchstone 1.x (.sNp) and 2.0 files (requirement RF-3).
// Reading accepts every format (RI, MA, DB), frequency unit and parameter
// type (S, Y, Z; G and H for two-ports), the 2-port column order convention,
// per-port reference impedances (2.0 [Reference]), and two-port noise data.
// Data is returned as S-parameters referred to the file's reference
// impedances, frequencies in Hz. Version 1 Y, Z, G, H data and noise Rn are
// normalised to R; version 2 data and Rn are in ohm/siemens (as scikit-rf).
// Full-line comments are collected into NetworkData::comment.

struct TouchstoneReadResult {
    std::optional<NetworkData> network;
    QString error;        // set when network is empty
    QStringList warnings; // recoverable oddities, with line numbers
};

// The port count comes from the file name extension (.sNp) for version 1
// files and from [Number of Ports] for version 2; `portCountHint` is used when
// neither is available (for example data read from a string), 0 = unknown.
TouchstoneReadResult readTouchstone(const QString &path);
TouchstoneReadResult parseTouchstone(const QByteArray &text, int portCountHint = 0);

enum class TouchstoneFormat { RealImaginary, MagnitudeAngle, DecibelAngle };

struct TouchstoneWriteOptions {
    TouchstoneFormat format = TouchstoneFormat::RealImaginary;
    QString frequencyUnit = QStringLiteral("GHz"); // Hz, kHz, MHz or GHz
    int version = 1; // 1 or 2; version 1 requires a single reference impedance for all ports
    int precision = 12;
};

// Writes S-parameters, and two-port noise parameters when present.
bool writeTouchstone(const QString &path, const NetworkData &network, const TouchstoneWriteOptions &options = {},
                     QString *error = nullptr);
QByteArray formatTouchstone(const NetworkData &network, const TouchstoneWriteOptions &options = {},
                            QString *error = nullptr);

} // namespace lvd::rf
