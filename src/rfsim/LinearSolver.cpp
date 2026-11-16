#include "rfsim/LinearSolver.h"

#include "rfdata/NetworkMath.h"

#include <Eigen/Sparse>

#include <QHash>
#include <QMap>
#include <QSet>

#include <algorithm>

namespace lvd::rf {

namespace {

using SparseMatrix = Eigen::SparseMatrix<Complex>;
using Triplet = Eigen::Triplet<Complex>;

const QString GroundNet = QStringLiteral("0");

} // namespace

LinearResult simulateLinear(const Netlist &netlist, const std::vector<double> &frequencies, const LinearOptions &options)
{
    LinearResult result;
    if (frequencies.empty()) {
        result.errors.append(QStringLiteral("the frequency sweep is empty"));
        return result;
    }
    for (size_t i = 1; i < frequencies.size(); ++i) {
        if (!(frequencies[i] > frequencies[i - 1])) {
            result.errors.append(QStringLiteral("the sweep frequencies must be strictly increasing"));
            return result;
        }
    }
    if (frequencies.front() < 0) {
        result.errors.append(QStringLiteral("frequencies must not be negative"));
        return result;
    }
    for (const QString &message : netlist.errors)
        result.errors.append(message);
    if (!result.errors.isEmpty())
        return result;

    TouchstoneCache privateCache;
    ElementContext context{options.projectDirectory, options.touchstones ? options.touchstones : &privateCache,
                           options.noise};

    // Build every element; collect all errors rather than stopping at the first.
    struct Placed {
        Element element;
        int firstPort = 0; // index of its first port in the global wave vectors
        const FlatComponent *component = nullptr;
    };
    std::vector<Placed> placed;
    for (const FlatComponent &component : netlist.components) {
        QString error;
        auto element = buildElement(component, frequencies, context, &result.warnings, &error);
        if (!element) {
            result.errors.append(error);
            continue;
        }
        placed.push_back(Placed{std::move(*element), 0, &component});
    }
    if (!result.errors.isEmpty())
        return result;

    // Global port numbering and the simulation ports.
    int total = 0;
    QMap<int, int> portOfNumber; // port number -> index into placed
    for (int i = 0; i < static_cast<int>(placed.size()); ++i) {
        Placed &item = placed[static_cast<size_t>(i)];
        item.firstPort = total;
        total += item.element.ports;
        if (item.element.isPort) {
            const int number = item.element.portNumber;
            if (portOfNumber.contains(number)) {
                result.errors.append(QStringLiteral("port number %1 is used by %2 and %3")
                                         .arg(number)
                                         .arg(placed[static_cast<size_t>(portOfNumber.value(number))].element.path,
                                              item.element.path));
            }
            portOfNumber.insert(number, i);
            const QString net = item.component->pinNets.value(item.component->definition->pins.first());
            if (net == GroundNet)
                result.errors.append(QStringLiteral("%1 is connected to ground").arg(item.element.path));
        }
    }
    if (portOfNumber.isEmpty())
        result.errors.append(QStringLiteral("the schematic has no simulation port (rf-system:port)"));
    if (!result.errors.isEmpty())
        return result;

    const int portCount = static_cast<int>(portOfNumber.size());
    std::vector<int> externalIndex; // global wave index of each simulation port, in port order
    for (auto it = portOfNumber.begin(); it != portOfNumber.end(); ++it) {
        const Placed &item = placed[static_cast<size_t>(it.value())];
        externalIndex.push_back(item.firstPort);
        result.portNumbers.push_back(it.key());
        result.portComponents.append(item.element.path);
        result.network.z0.push_back(item.element.portZ0);
    }

    // Junctions: the global port indices on each net.
    QHash<QString, std::vector<int>> nets;
    for (const Placed &item : placed) {
        const QStringList &pins = item.component->definition->pins;
        for (int k = 0; k < pins.size(); ++k)
            nets[item.component->pinNets.value(pins.at(k))].push_back(item.firstPort + k);
    }
    std::vector<Triplet> junctionEntries;
    for (auto it = nets.begin(); it != nets.end(); ++it) {
        const std::vector<int> &ports = it.value();
        if (it.key() == GroundNet || it.key().isEmpty()) {
            for (int port : ports)
                junctionEntries.emplace_back(port, port, Complex(-1, 0));
            continue;
        }
        const double m = static_cast<double>(ports.size());
        for (int row : ports) {
            for (int col : ports)
                junctionEntries.emplace_back(row, col, Complex(2.0 / m - (row == col ? 1.0 : 0.0), 0));
        }
    }
    SparseMatrix junctions(total, total);
    junctions.setFromTriplets(junctionEntries.begin(), junctionEntries.end());

    // Sparsity of A = I - C S is the same at every frequency.
    Eigen::SparseLU<SparseMatrix, Eigen::COLAMDOrdering<int>> lu;
    Eigen::SparseLU<SparseMatrix, Eigen::COLAMDOrdering<int>> luTransposed;
    bool analysed = false;

    const std::vector<double> solverZ0(static_cast<size_t>(portCount), SolverZ0);
    result.network.frequencies = frequencies;
    result.network.s.reserve(frequencies.size());
    if (options.noise)
        result.network.noiseCorrelation.reserve(frequencies.size());

    for (size_t fi = 0; fi < frequencies.size(); ++fi) {
        if (options.cancel && options.cancel->load()) {
            result.cancelled = true;
            return result;
        }

        std::vector<Triplet> sEntries;
        for (const Placed &item : placed) {
            if (item.element.isPort)
                continue; // simulation ports scatter nothing back
            const CMatrix &s = item.element.s[fi];
            // Every entry of the block, zeros included: the sparsity pattern
            // must be identical at all frequencies for the reused LU analysis.
            for (int r = 0; r < item.element.ports; ++r) {
                for (int c = 0; c < item.element.ports; ++c)
                    sEntries.emplace_back(item.firstPort + r, item.firstPort + c, s(r, c));
            }
        }
        SparseMatrix scattering(total, total);
        scattering.setFromTriplets(sEntries.begin(), sEntries.end());

        SparseMatrix identity(total, total);
        identity.setIdentity();
        // The product keeps structural zeros, so the pattern is the same each time.
        SparseMatrix a = identity - junctions * scattering;
        a.makeCompressed();
        if (!analysed) {
            lu.analyzePattern(a);
            analysed = true;
        }
        lu.factorize(a);
        if (lu.info() != Eigen::Success) {
            result.errors.append(QStringLiteral("the circuit equations are singular at %1 Hz").arg(frequencies[fi]));
            return result;
        }

        // Right-hand side C E: column k injects at the k-th simulation port.
        CMatrix injected = CMatrix::Zero(total, portCount);
        for (int k = 0; k < portCount; ++k) {
            const int port = externalIndex[static_cast<size_t>(k)];
            for (SparseMatrix::InnerIterator it(junctions, port); it; ++it)
                injected(it.row(), k) = it.value();
        }
        const CMatrix waves = lu.solve(injected);
        CMatrix s(portCount, portCount);
        for (int r = 0; r < portCount; ++r)
            s.row(r) = waves.row(externalIndex[static_cast<size_t>(r)]);

        if (options.noise) {
            // Outgoing noise waves b_ext = W c with W = E^T A^-1 C, so
            // W^T = C^T A^-T E: solve with the transposed matrix.
            SparseMatrix transposed = SparseMatrix(a.transpose());
            transposed.makeCompressed();
            if (fi == 0)
                luTransposed.analyzePattern(transposed);
            luTransposed.factorize(transposed);
            if (luTransposed.info() != Eigen::Success) {
                result.errors.append(QStringLiteral("the noise equations are singular at %1 Hz").arg(frequencies[fi]));
                return result;
            }
            CMatrix selector = CMatrix::Zero(total, portCount);
            for (int k = 0; k < portCount; ++k)
                selector(externalIndex[static_cast<size_t>(k)], k) = 1.0;
            const CMatrix wt = SparseMatrix(junctions.transpose()) * CMatrix(luTransposed.solve(selector));
            const CMatrix w = wt.transpose(); // portCount x total

            CMatrix correlation = CMatrix::Zero(portCount, portCount);
            for (const Placed &item : placed) {
                if (item.element.isPort || item.element.noise.empty())
                    continue;
                const CMatrix &cs = item.element.noise[fi];
                const CMatrix block = w.middleCols(item.firstPort, item.element.ports);
                correlation += block * cs * block.adjoint();
            }
            const CMatrix t = noiseRenormalization(s, solverZ0, result.network.z0);
            result.network.noiseCorrelation.push_back(t * correlation * t.adjoint());
        }
        result.network.s.push_back(renormalize(s, solverZ0, result.network.z0));
    }
    return result;
}

} // namespace lvd::rf
