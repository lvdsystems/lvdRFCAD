#pragma once

#include <QString>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace lvd::fw {

// Read-only access to the parts of HDF5 files that openEMS writes: numeric
// datasets of any rank (read as double) and numeric attributes. Thread-safe
// only in the sense that separate files may be used on separate threads
// one at a time (the HDF5 library is built without thread safety).
class Hdf5File
{
public:
    explicit Hdf5File(const QString &path);
    ~Hdf5File();
    Hdf5File(const Hdf5File &) = delete;
    Hdf5File &operator=(const Hdf5File &) = delete;

    bool isOpen() const { return m_file >= 0; }
    QString error() const { return m_error; }

    bool exists(const QString &path) const; // a group or dataset, e.g. "/nf2ff/E_theta/FD/f0_real"
    // Dataset values in row-major order (the last dimension varies fastest).
    std::optional<std::vector<double>> readDataset(const QString &path, std::vector<std::uint64_t> *dims = nullptr) const;
    std::optional<std::vector<double>> readAttribute(const QString &objectPath, const QString &name) const;
    // Names of the members of a group (datasets and subgroups), sorted.
    std::vector<QString> members(const QString &groupPath) const;

private:
    std::int64_t m_file = -1;
    mutable QString m_error;
};

} // namespace lvd::fw
