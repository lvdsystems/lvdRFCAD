#pragma once

#include "core/Netlist.h"
#include "rfdata/RfTypes.h"
#include "rfsim/ElementModels.h"

#include <QStringList>

#include <atomic>

namespace lvd::rf {

// Small-signal S-parameter and noise analysis of a flattened RF netlist
// (requirement RF-5).
//
// Every component is an N-port referenced to ground, with S-matrices at the
// common reference SolverZ0. Each net is an ideal junction of the component
// ports on it, S_junction = 2/m - I for m ports, so that with a = waves into
// the components and b = waves out of them:
//
//     b = S a + E a_ext + c        (S block diagonal, c the noise waves)
//     a = C b                      (C block diagonal, the junctions)
//
// Pins on the ground net are shorted (S = -1 each). Simulation ports are
// components that emit the external incident waves a_ext and absorb the
// waves arriving at them, which are the outgoing waves b_ext. Hence
//
//     (I - C S) a = C (E a_ext + c),   b_ext = E^T a,
//
// solved with a sparse LU per frequency. S and the noise correlation are
// finally referred to each port's z0.

struct LinearResult {
    NetworkData network;          // ports in order of their port numbers
    std::vector<int> portNumbers; // the `num` of each port, ascending
    QStringList portComponents;   // hierarchical reference of each port component
    QStringList warnings;
    QStringList errors;           // non-empty means no result
    bool cancelled = false;

    bool ok() const { return errors.isEmpty() && !cancelled; }
};

struct LinearOptions {
    bool noise = true;
    QString projectDirectory;
    TouchstoneCache *touchstones = nullptr; // optional; a private cache is used otherwise
    const std::atomic_bool *cancel = nullptr;
};

LinearResult simulateLinear(const Netlist &netlist, const std::vector<double> &frequencies,
                            const LinearOptions &options = {});

} // namespace lvd::rf
