#pragma once

#include "rfsim/LinearSolver.h"

#include <QString>
#include <QStringList>

namespace lvd::rf {

// Measurement expressions evaluated on a linear result, for plots, tables
// and scripts. Port indices are port numbers (the `num` of the port
// components), so S21 means "into port 1, out of port 2".
//
//   S21  S(2,1)  Z(1,1)  Y(2,1)          network parameters (complex)
//   Zin(1)  Yin(1)  VSWR(1)  RL(1)        port quantities (Zin with the other ports matched)
//   NF  NF(2,1)  NFmin  Rn  GammaOpt      noise (two-ports for NFmin, Rn, GammaOpt); NF in dB
//   K  Delta  mu  mup  MAG                two-port stability and gain
//   GUmax  U                              maximum unilateral transducer gain, Mason's
//                                         unilateral gain (both in dB)
//   GammaMS  GammaML                      simultaneous conjugate match source and load
//                                         reflections (complex; NaN where K <= 1)
//   dB(x) dB10(x) mag(x) phase(x) uphase(x) re(x) im(x) conj(x) GD(x)
//   + - * / and numbers
//
// dB(x) is 20 log10 |x|; dB10(x) is 10 log10 |x|; phase in degrees; uphase
// unwrapped; GD(x) is the group delay -d(phase)/d(omega) in seconds.
struct Measurement {
    std::vector<double> frequencies;
    std::vector<Complex> values;
    bool isComplex = false; // false: only the real parts are meaningful
    QString unit;           // "dB", "deg", "s", "ohm", ... or empty
    QString error;

    bool ok() const { return error.isEmpty(); }
    std::vector<double> real() const;
};

Measurement evaluateMeasurement(const LinearResult &result, const QString &expression);

// Default traces for a result: dB of every S-parameter of up to 4 ports, plus NF for a two-port.
QStringList defaultMeasurements(const LinearResult &result);

} // namespace lvd::rf
