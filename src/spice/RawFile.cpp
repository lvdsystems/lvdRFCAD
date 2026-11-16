#include "spice/RawFile.h"

#include <QFile>

#include <cstring>

namespace lvd::spice {

namespace {

VectorKind kindFromRawType(const QString &type)
{
    const QString t = type.toLower();
    if (t == QLatin1String("time"))
        return VectorKind::Time;
    if (t == QLatin1String("frequency"))
        return VectorKind::Frequency;
    if (t == QLatin1String("voltage"))
        return VectorKind::Voltage;
    if (t == QLatin1String("current"))
        return VectorKind::Current;
    if (t == QLatin1String("voltage-density") || t == QLatin1String("voltage_density"))
        return VectorKind::VoltageDensity;
    if (t == QLatin1String("current-density") || t == QLatin1String("current_density"))
        return VectorKind::CurrentDensity;
    if (t == QLatin1String("pole"))
        return VectorKind::Pole;
    if (t == QLatin1String("zero"))
        return VectorKind::Zero;
    if (t == QLatin1String("temp-sweep") || t == QLatin1String("temperature"))
        return VectorKind::Temperature;
    if (t == QLatin1String("impedance"))
        return VectorKind::Impedance;
    if (t == QLatin1String("admittance"))
        return VectorKind::Admittance;
    if (t == QLatin1String("power"))
        return VectorKind::Power;
    return VectorKind::Other;
}

void finishPlot(SpicePlot &plot)
{
    for (SpiceVector &v : plot.vectors) {
        if (v.isComplex && (v.kind == VectorKind::Frequency || v.kind == VectorKind::Time)) {
            v.real.resize(v.complex.size());
            for (std::size_t i = 0; i < v.complex.size(); ++i)
                v.real[i] = v.complex[i].real();
            v.complex.clear();
            v.isComplex = false;
        }
    }
    if (!plot.vectors.empty()) {
        const QString &first = plot.vectors.front().name;
        if (first == QLatin1String("time") || first == QLatin1String("frequency") || first.endsWith(QLatin1String("sweep")))
            plot.scaleName = first;
    }
}

} // namespace

QString plotTypeFromTitle(const QString &plotName)
{
    const QString t = plotName.toLower();
    if (t.contains(QLatin1String("transient")))
        return QStringLiteral("tran");
    if (t.startsWith(QLatin1String("ac")))
        return QStringLiteral("ac");
    if (t.contains(QLatin1String("dc transfer")))
        return QStringLiteral("dc");
    if (t.contains(QLatin1String("operating point")))
        return QStringLiteral("op");
    if (t.contains(QLatin1String("noise")))
        return QStringLiteral("noise");
    if (t.contains(QLatin1String("transfer function")))
        return QStringLiteral("tf");
    if (t.contains(QLatin1String("pole")))
        return QStringLiteral("pz");
    if (t.contains(QLatin1String("sensitivity")))
        return QStringLiteral("sens");
    return t.section(QLatin1Char(' '), 0, 0);
}

std::optional<QList<SpicePlot>> readRawFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return std::nullopt;
    }
    return parseRawData(file.readAll(), error);
}

std::optional<QList<SpicePlot>> parseRawData(const QByteArray &data, QString *error)
{
    const auto fail = [&](const QString &message) -> std::optional<QList<SpicePlot>> {
        if (error)
            *error = message;
        return std::nullopt;
    };

    QList<SpicePlot> plots;
    qsizetype pos = 0;
    const auto readLine = [&]() -> std::optional<QString> {
        if (pos >= data.size())
            return std::nullopt;
        qsizetype end = data.indexOf('\n', pos);
        if (end < 0)
            end = data.size();
        QString line = QString::fromLatin1(data.constData() + pos, end - pos);
        pos = end + 1;
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        return line;
    };

    while (pos < data.size()) {
        SpicePlot plot;
        bool complex = false;
        int variableCount = -1;
        int pointCount = -1;
        QList<QPair<QString, QString>> variables; // name, type
        bool binary = false;
        bool values = false;
        bool sawHeader = false;

        // Header.
        while (auto line = readLine()) {
            const QString text = line->trimmed();
            if (text.isEmpty())
                continue;
            const qsizetype colon = text.indexOf(QLatin1Char(':'));
            const QString key = colon >= 0 ? text.left(colon).trimmed().toLower() : QString();
            const QString value = colon >= 0 ? text.mid(colon + 1).trimmed() : QString();
            sawHeader = true;
            if (key == QLatin1String("title")) {
                plot.title = value;
            } else if (key == QLatin1String("plotname")) {
                if (plot.name.isEmpty())
                    plot.name = value;
                plot.type = plotTypeFromTitle(value);
            } else if (key == QLatin1String("plotid")) {
                plot.name = value;
            } else if (key == QLatin1String("flags")) {
                complex = value.contains(QLatin1String("complex"), Qt::CaseInsensitive);
            } else if (key == QLatin1String("no. variables")) {
                variableCount = value.toInt();
            } else if (key == QLatin1String("no. points")) {
                pointCount = value.toInt();
            } else if (key == QLatin1String("variables")) {
                if (variableCount < 0)
                    return fail(QStringLiteral("raw file: \"No. Variables\" missing"));
                for (int i = 0; i < variableCount; ++i) {
                    const auto variableLine = readLine();
                    if (!variableLine)
                        return fail(QStringLiteral("raw file: variable list ends early"));
                    const QStringList parts = variableLine->simplified().split(QLatin1Char(' '));
                    if (parts.size() < 3)
                        return fail(QStringLiteral("raw file: bad variable line \"%1\"").arg(*variableLine));
                    variables.append({parts.at(1), parts.at(2)});
                }
            } else if (key == QLatin1String("values")) {
                values = true;
                break;
            } else if (key == QLatin1String("binary")) {
                binary = true;
                break;
            }
        }
        if (!sawHeader)
            break;
        if (!values && !binary)
            return fail(QStringLiteral("raw file: no data section"));
        if (pointCount < 0 || variables.size() != variableCount)
            return fail(QStringLiteral("raw file: incomplete header"));

        for (const auto &[name, type] : std::as_const(variables)) {
            SpiceVector v;
            v.kind = kindFromRawType(type);
            v.name = normalisedVectorName(name, v.kind, plot.type);
            v.isComplex = complex;
            if (complex)
                v.complex.reserve(std::size_t(pointCount));
            else
                v.real.reserve(std::size_t(pointCount));
            plot.vectors.push_back(std::move(v));
        }

        if (binary) {
            const qsizetype valueSize = complex ? 16 : 8;
            const qsizetype needed = qsizetype(pointCount) * variableCount * valueSize;
            if (pos + needed > data.size())
                return fail(QStringLiteral("raw file: binary data ends early"));
            const char *p = data.constData() + pos;
            for (int point = 0; point < pointCount; ++point) {
                for (int i = 0; i < variableCount; ++i) {
                    double re = 0, im = 0;
                    std::memcpy(&re, p, 8);
                    p += 8;
                    if (complex) {
                        std::memcpy(&im, p, 8);
                        p += 8;
                        plot.vectors[std::size_t(i)].complex.emplace_back(re, im);
                    } else {
                        plot.vectors[std::size_t(i)].real.push_back(re);
                    }
                }
            }
            pos += needed;
            // Binary data may be followed by a newline before the next plot.
            while (pos < data.size() && (data.at(pos) == '\n' || data.at(pos) == '\r'))
                ++pos;
        } else {
            // ASCII: "<index> <value of var 0>", then one value per line.
            int read = 0;
            const int total = pointCount * variableCount;
            while (read < total) {
                const auto line = readLine();
                if (!line)
                    return fail(QStringLiteral("raw file: ASCII data ends early"));
                QStringList parts = line->simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
                if (parts.isEmpty())
                    continue;
                if (read % variableCount == 0) {
                    if (parts.size() < 2)
                        return fail(QStringLiteral("raw file: bad data line \"%1\"").arg(*line));
                    parts.removeFirst(); // point index
                }
                const QString token = parts.first();
                SpiceVector &v = plot.vectors[std::size_t(read % variableCount)];
                bool ok = true;
                if (complex) {
                    const QStringList pair = token.split(QLatin1Char(','));
                    bool okIm = true;
                    const double re = pair.value(0).toDouble(&ok);
                    const double im = pair.size() > 1 ? pair.at(1).toDouble(&okIm) : 0.0;
                    ok = ok && okIm;
                    v.complex.emplace_back(re, im);
                } else {
                    v.real.push_back(token.toDouble(&ok));
                }
                if (!ok)
                    return fail(QStringLiteral("raw file: bad number \"%1\"").arg(token));
                ++read;
            }
        }
        finishPlot(plot);
        plots << plot;
    }
    if (plots.isEmpty())
        return fail(QStringLiteral("raw file: no plots"));
    return plots;
}

} // namespace lvd::spice

namespace lvd::spice {

namespace {

QString rawTypeName(VectorKind kind)
{
    switch (kind) {
    case VectorKind::Time: return QStringLiteral("time");
    case VectorKind::Frequency: return QStringLiteral("frequency");
    case VectorKind::Voltage: return QStringLiteral("voltage");
    case VectorKind::Current: return QStringLiteral("current");
    case VectorKind::VoltageDensity: return QStringLiteral("voltage-density");
    case VectorKind::CurrentDensity: return QStringLiteral("current-density");
    case VectorKind::Pole: return QStringLiteral("pole");
    case VectorKind::Zero: return QStringLiteral("zero");
    case VectorKind::Temperature: return QStringLiteral("temperature");
    case VectorKind::Impedance: return QStringLiteral("impedance");
    case VectorKind::Admittance: return QStringLiteral("admittance");
    case VectorKind::Power: return QStringLiteral("power");
    case VectorKind::Other: break;
    }
    return QStringLiteral("notype");
}

QString plotTitle(const QString &type)
{
    if (type == QLatin1String("tran")) return QStringLiteral("Transient Analysis");
    if (type == QLatin1String("ac")) return QStringLiteral("AC Analysis");
    if (type == QLatin1String("dc")) return QStringLiteral("DC transfer characteristic");
    if (type == QLatin1String("op")) return QStringLiteral("Operating Point");
    if (type == QLatin1String("noise")) return QStringLiteral("Noise Spectral Density Curves");
    if (type == QLatin1String("tf")) return QStringLiteral("Transfer Function");
    if (type == QLatin1String("pz")) return QStringLiteral("Pole-Zero Analysis");
    if (type == QLatin1String("sens")) return QStringLiteral("Sensitivity Analysis");
    return type;
}

} // namespace

QByteArray rawData(const QList<SpicePlot> &plots)
{
    QByteArray data;
    for (const SpicePlot &plot : plots) {
        // Scale first, as raw readers expect.
        std::vector<const SpiceVector *> order;
        if (const SpiceVector *scale = plot.scale())
            order.push_back(scale);
        for (const SpiceVector &v : plot.vectors)
            if (&v != plot.scale())
                order.push_back(&v);
        std::size_t points = 0;
        bool complex = false;
        for (const SpiceVector *v : order) {
            points = std::max(points, v->size());
            complex = complex || v->isComplex;
        }
        QString header;
        header += QStringLiteral("Title: %1\n").arg(plot.title.isEmpty() ? plot.name : plot.title);
        header += QStringLiteral("Date: \n");
        header += QStringLiteral("Plotname: %1\n").arg(plotTitle(plot.type));
        header += QStringLiteral("Plotid: %1\n").arg(plot.name);
        header += QStringLiteral("Flags: %1\n").arg(complex ? QStringLiteral("complex") : QStringLiteral("real"));
        header += QStringLiteral("No. Variables: %1\n").arg(order.size());
        header += QStringLiteral("No. Points: %1\n").arg(points);
        header += QStringLiteral("Variables:\n");
        for (std::size_t i = 0; i < order.size(); ++i)
            header += QStringLiteral("\t%1\t%2\t%3\n").arg(i).arg(order[i]->name, rawTypeName(order[i]->kind));
        header += QStringLiteral("Binary:\n");
        data += header.toUtf8();
        for (std::size_t point = 0; point < points; ++point) {
            for (const SpiceVector *v : order) {
                const std::complex<double> value = point < v->size() ? v->at(point) : std::complex<double>();
                const double re = value.real(), im = value.imag();
                data.append(reinterpret_cast<const char *>(&re), sizeof re);
                if (complex)
                    data.append(reinterpret_cast<const char *>(&im), sizeof im);
            }
        }
    }
    return data;
}

bool writeRawFile(const QList<SpicePlot> &plots, const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const QByteArray data = rawData(plots);
    if (file.write(data) != data.size()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

} // namespace lvd::spice