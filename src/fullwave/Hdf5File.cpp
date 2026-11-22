#include "fullwave/Hdf5File.h"

#include <hdf5.h>

#include <QStringList>

#include <algorithm>
#include <mutex>

namespace lvd::fw {

namespace {

// The library is not built thread-safe: serialise every call.
std::recursive_mutex &hdf5Lock()
{
    static std::recursive_mutex lock;
    return lock;
}

void quietErrors()
{
    static std::once_flag once;
    std::call_once(once, [] { H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr); });
}

} // namespace

Hdf5File::Hdf5File(const QString &path)
{
    std::lock_guard guard(hdf5Lock());
    quietErrors();
    const QByteArray name = path.toUtf8();
    m_file = H5Fopen(name.constData(), H5F_ACC_RDONLY, H5P_DEFAULT);
    if (m_file < 0)
        m_error = QStringLiteral("cannot open HDF5 file %1").arg(path);
}

Hdf5File::~Hdf5File()
{
    std::lock_guard guard(hdf5Lock());
    if (m_file >= 0)
        H5Fclose(m_file);
}

bool Hdf5File::exists(const QString &path) const
{
    std::lock_guard guard(hdf5Lock());
    if (m_file < 0)
        return false;
    // Check every level, as H5Lexists requires the parents to exist.
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString current;
    for (const QString &part : parts) {
        current += QLatin1Char('/') + part;
        const QByteArray name = current.toUtf8();
        if (H5Lexists(m_file, name.constData(), H5P_DEFAULT) <= 0)
            return false;
    }
    return true;
}

std::optional<std::vector<double>> Hdf5File::readDataset(const QString &path, std::vector<std::uint64_t> *dims) const
{
    std::lock_guard guard(hdf5Lock());
    if (!exists(path)) {
        m_error = QStringLiteral("no dataset %1").arg(path);
        return std::nullopt;
    }
    const QByteArray name = path.toUtf8();
    const hid_t set = H5Dopen2(m_file, name.constData(), H5P_DEFAULT);
    if (set < 0) {
        m_error = QStringLiteral("cannot open dataset %1").arg(path);
        return std::nullopt;
    }
    const hid_t space = H5Dget_space(set);
    const int rank = H5Sget_simple_extent_ndims(space);
    std::vector<hsize_t> extent(std::size_t(std::max(rank, 0)));
    if (rank > 0)
        H5Sget_simple_extent_dims(space, extent.data(), nullptr);
    std::size_t count = 1;
    for (hsize_t d : extent)
        count *= std::size_t(d);
    std::vector<double> values(count);
    const herr_t status = count ? H5Dread(set, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, values.data()) : 0;
    H5Sclose(space);
    H5Dclose(set);
    if (status < 0) {
        m_error = QStringLiteral("cannot read dataset %1 as numbers").arg(path);
        return std::nullopt;
    }
    if (dims)
        dims->assign(extent.begin(), extent.end());
    return values;
}

std::optional<std::vector<double>> Hdf5File::readAttribute(const QString &objectPath, const QString &name) const
{
    std::lock_guard guard(hdf5Lock());
    if (m_file < 0)
        return std::nullopt;
    const QByteArray object = objectPath.toUtf8();
    const QByteArray attribute = name.toUtf8();
    if (H5Aexists_by_name(m_file, object.constData(), attribute.constData(), H5P_DEFAULT) <= 0) {
        m_error = QStringLiteral("no attribute %1 on %2").arg(name, objectPath);
        return std::nullopt;
    }
    const hid_t attr = H5Aopen_by_name(m_file, object.constData(), attribute.constData(), H5P_DEFAULT, H5P_DEFAULT);
    if (attr < 0)
        return std::nullopt;
    const hid_t space = H5Aget_space(attr);
    const hssize_t count = H5Sget_simple_extent_npoints(space);
    std::vector<double> values(std::size_t(std::max<hssize_t>(count, 0)));
    const herr_t status = values.empty() ? 0 : H5Aread(attr, H5T_NATIVE_DOUBLE, values.data());
    H5Sclose(space);
    H5Aclose(attr);
    if (status < 0)
        return std::nullopt;
    return values;
}

std::vector<QString> Hdf5File::members(const QString &groupPath) const
{
    std::lock_guard guard(hdf5Lock());
    std::vector<QString> names;
    if (!exists(groupPath) && groupPath != QLatin1String("/"))
        return names;
    const QByteArray path = groupPath.toUtf8();
    const hid_t group = H5Gopen2(m_file, path.constData(), H5P_DEFAULT);
    if (group < 0)
        return names;
    H5G_info_t info;
    if (H5Gget_info(group, &info) >= 0) {
        for (hsize_t i = 0; i < info.nlinks; ++i) {
            char buffer[512] = {};
            if (H5Lget_name_by_idx(group, ".", H5_INDEX_NAME, H5_ITER_INC, i, buffer, sizeof(buffer), H5P_DEFAULT) >= 0)
                names.push_back(QString::fromUtf8(buffer));
        }
    }
    H5Gclose(group);
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace lvd::fw
