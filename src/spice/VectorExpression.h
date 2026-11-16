#pragma once

#include "spice/WaveformMath.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

namespace lvd::spice {

// Expressions on traces, as typed in the waveform viewer:
//
//   numbers      1.5  2k  4n7  1e-3  10meg  (SI prefixes as in the schematic
//                values: a f p n u µ m k M G T and meg in any case; m is milli,
//                M is mega; a unit after the prefix is ignored: 2.4GHz, 10mV)
//   operators    + - * / ^ and parentheses; ^ is right-associative and binds
//                tighter than unary minus
//   vectors      v(out)  v(a,b) = v(a) - v(b)  i(vin)  and bare names such
//                as time, frequency, onoise_spectrum, vout#branch
//   constants    pi  e  j (imaginary unit)
//   functions    db db10 mag abs ph phase cph real re imag im conj
//                sqrt exp ln log10 sin cos tan atan sinh cosh tanh
//                deriv integ gd groupdelay mean avg rms max min pkpk
//
// Names (vectors, functions, constants) ignore case, and vector names are
// lower-cased as SPICE does. db is 20 log10|x|, db10 is 10 log10|x| (for
// powers such as noise spectra); ph is in degrees wrapped to (-180, 180] and
// cph unwrapped along the scale. deriv is d/dx along the scale, integ the
// cumulative trapezoidal integral from the first point, gd the group delay
// -d(unwrapped phase)/d(2 pi x) in seconds. mean, rms, max, min and pkpk give
// a constant trace equal to the scale-weighted value over the whole scale
// (max, min and pkpk use the magnitude of complex traces).
//
// Operations between traces are pointwise and need the same number of points;
// scalars broadcast. A constant expression evaluates to one value with an
// empty scale.
class VectorExpression
{
public:
    // Returns the vector for a canonical name, or nullopt if unknown.
    // Canonical names: "v(out)", "i(vin)" and bare identifiers that are not
    // functions or constants ("time", "vout#branch"), all lower case.
    using Resolver = std::function<std::optional<Waveform>(const QString &name)>;

    static std::optional<VectorExpression> parse(const QString &text, QString *error = nullptr);

    std::optional<Waveform> evaluate(const Resolver &resolve, QString *error = nullptr) const;

    // Canonical names of the vectors used, without duplicates, in order of
    // first appearance (for "which plot provides this").
    QStringList referencedVectors() const;
    QString text() const { return m_text; }

    struct Node; // implementation detail

private:
    QString m_text;
    std::shared_ptr<const Node> m_root;
};

} // namespace lvd::spice
