#include "fullwave/StlFile.h"

#include "core/JsonFile.h"

#include <QFile>
#include <QRegularExpression>
#include <QtEndian>

#include <cstring>

namespace lvd::fw {

std::optional<std::vector<ant::Vec3>> readStl(const QString &path, double scale, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("cannot open %1: %2").arg(path, file.errorString()));
        return std::nullopt;
    }
    const QByteArray data = file.readAll();
    std::vector<ant::Vec3> v;
    // Binary: 80-byte header, uint32 count, 50 bytes per triangle. Some ASCII
    // exporters start with "solid" too, so the size decides.
    if (data.size() >= 84) {
        quint32 count;
        std::memcpy(&count, data.constData() + 80, 4);
        count = qFromLittleEndian(count);
        if (qint64(84) + qint64(count) * 50 == data.size() && count > 0) {
            v.reserve(std::size_t(count) * 3);
            for (quint32 t = 0; t < count; ++t) {
                const char *rec = data.constData() + 84 + qint64(t) * 50 + 12; // skip the normal
                for (int k = 0; k < 3; ++k) {
                    float xyz[3];
                    std::memcpy(xyz, rec + k * 12, 12);
                    v.emplace_back(double(qFromLittleEndian(xyz[0])) * scale, double(qFromLittleEndian(xyz[1])) * scale,
                                   double(qFromLittleEndian(xyz[2])) * scale);
                }
            }
            return v;
        }
    }
    // ASCII: "vertex x y z" lines.
    const QRegularExpression vertex(QStringLiteral("vertex\\s+(\\S+)\\s+(\\S+)\\s+(\\S+)"), QRegularExpression::CaseInsensitiveOption);
    auto it = vertex.globalMatch(QString::fromLatin1(data));
    while (it.hasNext()) {
        const auto m = it.next();
        v.emplace_back(m.captured(1).toDouble() * scale, m.captured(2).toDouble() * scale, m.captured(3).toDouble() * scale);
    }
    if (v.empty() || v.size() % 3 != 0) {
        setError(error, QStringLiteral("%1 is not a readable STL file").arg(path));
        return std::nullopt;
    }
    return v;
}

} // namespace lvd::fw
