#include "fullwave/OpenEmsXml.h"

#include <QTextStream>

#include <numbers>

namespace lvd::fw {

namespace {

QString num(double v)
{
    return QString::number(v, 'g', 12);
}

QString colorAttributes(const QColor &c)
{
    const QColor color = c.isValid() ? c : QColor(150, 150, 150);
    return QStringLiteral("<FillColor R=\"%1\" G=\"%2\" B=\"%3\" a=\"%4\" />")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

QString point(const char *tag, const Vec3 &p)
{
    return QStringLiteral("<%1 X=\"%2\" Y=\"%3\" Z=\"%4\" />").arg(QLatin1String(tag), num(p.x()), num(p.y()), num(p.z()));
}

QString box(const Vec3 &a, const Vec3 &b, int priority)
{
    return QStringLiteral("<Box Priority=\"%1\">%2%3</Box>").arg(priority).arg(point("P1", a), point("P2", b));
}

QString transformXml(const FwPrimitive &p)
{
    if (p.transforms.empty())
        return {};
    QString s = QStringLiteral("<Transformation>");
    for (const FwTransform &t : p.transforms) {
        switch (t.kind) {
        case FwTransform::Kind::RotateX:
        case FwTransform::Kind::RotateY:
        case FwTransform::Kind::RotateZ: {
            const char axis = t.kind == FwTransform::Kind::RotateX ? 'X' : t.kind == FwTransform::Kind::RotateY ? 'Y' : 'Z';
            s += QStringLiteral("<Rotate_%1 Argument=\"%2\" />").arg(QLatin1Char(axis)).arg(num(ant::toRadians(t.value.x())));
            break;
        }
        case FwTransform::Kind::Translate:
            s += QStringLiteral("<Translate Argument=\"%1,%2,%3\" />").arg(num(t.value.x()), num(t.value.y()), num(t.value.z()));
            break;
        case FwTransform::Kind::Scale:
            s += QStringLiteral("<Scale Argument=\"%1\" />").arg(num(t.value.x()));
            break;
        }
    }
    return s + QStringLiteral("</Transformation>");
}

QString primitiveBody(const FwPrimitive &p)
{
    QString s;
    switch (p.kind) {
    case PrimitiveKind::Box:
        return box(p.p1, p.p2, p.priority);
    case PrimitiveKind::Cylinder:
        return QStringLiteral("<Cylinder Priority=\"%1\" Radius=\"%2\">%3%4</Cylinder>")
            .arg(p.priority)
            .arg(num(p.radius), point("P1", p.p1), point("P2", p.p2));
    case PrimitiveKind::Polygon:
    case PrimitiveKind::Extrusion: {
        const bool extrusion = p.kind == PrimitiveKind::Extrusion;
        s = QStringLiteral("<%1 Priority=\"%2\" NormDir=\"%3\" Elevation=\"%4\"%5>")
                .arg(extrusion ? QStringLiteral("LinPoly") : QStringLiteral("Polygon"))
                .arg(p.priority)
                .arg(p.normal)
                .arg(num(p.elevation), extrusion ? QStringLiteral(" Length=\"%1\"").arg(num(p.length)) : QString());
        for (const auto &v : p.points)
            s += QStringLiteral("<Vertex X1=\"%1\" X2=\"%2\" />").arg(num(v[0]), num(v[1]));
        s += extrusion ? QStringLiteral("</LinPoly>") : QStringLiteral("</Polygon>");
        return s;
    }
    case PrimitiveKind::Wire:
        s = p.radius > 0 ? QStringLiteral("<Wire Priority=\"%1\" WireRadius=\"%2\">").arg(p.priority).arg(num(p.radius))
                         : QStringLiteral("<Curve Priority=\"%1\">").arg(p.priority);
        for (const Vec3 &v : p.path)
            s += QStringLiteral("<Vertex X=\"%1\" Y=\"%2\" Z=\"%3\" />").arg(num(v.x()), num(v.y()), num(v.z()));
        s += p.radius > 0 ? QStringLiteral("</Wire>") : QStringLiteral("</Curve>");
        return s;
    case PrimitiveKind::StlFile:
        return QStringLiteral("<PolyhedronReader Priority=\"%1\" FileName=\"%2\" FileType=\"STL\"></PolyhedronReader>")
            .arg(p.priority)
            .arg(p.file.toHtmlEscaped());
    }
    return {};
}

// The transformation goes first inside the primitive element.
QString primitive(const FwPrimitive &p)
{
    FwPrimitive copy = p;
    if (p.kind == PrimitiveKind::StlFile && std::abs(p.stlScale - 1) > 1e-12)
        copy.transforms.insert(copy.transforms.begin(), FwTransform{FwTransform::Kind::Scale, Vec3(p.stlScale, 0, 0)});
    QString body = primitiveBody(copy);
    const QString t = transformXml(copy);
    if (t.isEmpty())
        return body;
    const qsizetype close = body.indexOf(QLatin1Char('>'));
    body.insert(close + 1, t);
    return body;
}

QString lines(const std::vector<double> &v)
{
    QStringList parts;
    for (double x : v)
        parts << num(x);
    return parts.join(QLatin1Char(','));
}

} // namespace

QString portVoltageFile(int port)
{
    return QStringLiteral("port_ut_%1").arg(port);
}

QString portCurrentFile(int port)
{
    return QStringLiteral("port_it_%1").arg(port);
}

QString openEmsXml(const FwModel &model, const FwMesh &mesh, const FwSettings &settings)
{
    QString xml;
    QTextStream out(&xml);
    const double f0 = settings.centre(), fc = settings.halfWidth();
    const double endCriteria = std::pow(10.0, settings.endCriteriaDb / 10.0);
    const QString bc = settings.boundary == Boundary::Pml8 ? QStringLiteral("PML_8") : QStringLiteral("MUR");
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>\n";
    out << "<openEMS>\n";
    out << "  <FDTD NumberOfTimesteps=\"" << settings.maxTimesteps << "\" endCriteria=\"" << num(endCriteria)
        << "\" OverSampling=\"4\" TimeStepMethod=\"3\">\n";
    out << "    <Excitation Type=\"0\" f0=\"" << num(f0) << "\" fc=\"" << num(fc) << "\" />\n";
    out << "    <BoundaryCond xmin=\"" << bc << "\" xmax=\"" << bc << "\" ymin=\"" << bc << "\" ymax=\"" << bc << "\" zmin=\"" << bc
        << "\" zmax=\"" << bc << "\" />\n";
    out << "  </FDTD>\n";
    out << "  <ContinuousStructure CoordSystem=\"0\">\n";
    out << "    <BackgroundMaterial Epsilon=\"1\" Mue=\"1\" Kappa=\"0\" Sigma=\"0\" />\n";
    out << "    <Properties>\n";

    int id = 0;
    // Materials and metals, each with its primitives.
    for (std::size_t m = 0; m < model.materials.size(); ++m) {
        const FwMaterial &mat = model.materials[m];
        QString body;
        for (const FwPrimitive &p : model.primitives)
            if (p.material == int(m))
                body += QStringLiteral("        ") + primitive(p) + QLatin1Char('\n');
        if (body.isEmpty())
            continue;
        const QString name = mat.name.isEmpty() ? QStringLiteral("material_%1").arg(m) : mat.name;
        if (mat.metal) {
            out << "      <Metal ID=\"" << id++ << "\" Name=\"" << name.toHtmlEscaped() << "\">" << colorAttributes(mat.color)
                << "\n      <Primitives>\n" << body << "      </Primitives></Metal>\n";
        } else {
            // Loss tangent as a conductivity at the centre frequency.
            const double kappa = mat.tanD * 2 * std::numbers::pi * f0 * 8.8541878128e-12 * mat.epsR;
            out << "      <Material ID=\"" << id++ << "\" Name=\"" << name.toHtmlEscaped() << "\" Isotropy=\"1\">"
                << colorAttributes(mat.color) << "\n      <Primitives>\n" << body << "      </Primitives>\n"
                << "        <Property Epsilon=\"" << num(mat.epsR) << "\" Mue=\"1\" Kappa=\"" << num(kappa) << "\" Sigma=\"0\" />\n"
                << "      </Material>\n";
        }
    }

    // Lumped ports: a resistor, an excitation and voltage and current probes
    // (as openEMS's AddLumpedPort).
    for (const FwPort &port : model.ports) {
        const int ax = port.axis;
        const double direction = port.stop[ax] >= port.start[ax] ? 1.0 : -1.0;
        const QString n = QString::number(port.number);
        out << "      <LumpedElement ID=\"" << id++ << "\" Name=\"port_resist_" << n << "\" Direction=\"" << ax
            << "\" Caps=\"1\" R=\"" << num(port.resistance) << "\">\n        <Primitives>"
            << box(port.start, port.stop, 5) << "</Primitives>\n      </LumpedElement>\n";
        if (port.excite) {
            Vec3 e = Vec3::Zero();
            e[ax] = -direction;
            out << "      <Excitation ID=\"" << id++ << "\" Name=\"port_excite_" << n
                << "\" Number=\"0\" Enabled=\"1\" Frequency=\"0\" Delay=\"0\" Type=\"0\" Excite=\"" << num(e.x()) << ','
                << num(e.y()) << ',' << num(e.z()) << "\" PropDir=\"0,0,0\">\n        <Primitives>" << box(port.start, port.stop, 5)
                << "</Primitives>\n        <Weight X=\"1\" Y=\"1\" Z=\"1\" />\n      </Excitation>\n";
        }
        Vec3 uStart = 0.5 * (port.start + port.stop), uStop = uStart;
        uStart[ax] = port.start[ax];
        uStop[ax] = port.stop[ax];
        out << "      <ProbeBox ID=\"" << id++ << "\" Name=\"" << portVoltageFile(port.number)
            << "\" Number=\"0\" Type=\"0\" Weight=\"-1\" NormDir=\"-1\">\n        <Primitives>" << box(uStart, uStop, 0)
            << "</Primitives>\n      </ProbeBox>\n";
        Vec3 iStart = port.start, iStop = port.stop;
        iStart[ax] = iStop[ax] = 0.5 * (port.start[ax] + port.stop[ax]);
        out << "      <ProbeBox ID=\"" << id++ << "\" Name=\"" << portCurrentFile(port.number) << "\" Number=\"0\" Type=\"1\" Weight=\""
            << num(direction) << "\" NormDir=\"" << ax << "\">\n        <Primitives>" << box(iStart, iStop, 0)
            << "</Primitives>\n      </ProbeBox>\n";
    }

    // Near-field box for the far field: a few cells inside the boundary
    // (inside the absorbing layer's reach of a PML).
    const std::size_t inset = settings.boundary == Boundary::Pml8 ? 10 : 2;
    Vec3 a, b;
    bool boxOk = true;
    for (int axis = 0; axis < 3; ++axis) {
        const auto &l = mesh.lines[std::size_t(axis)];
        if (l.size() < 2 * inset + 2) {
            boxOk = false;
            break;
        }
        a[axis] = l[inset];
        b[axis] = l[l.size() - 1 - inset];
    }
    if (boxOk) {
        for (int type = 0; type < 2; ++type) {
            out << "      <DumpBox ID=\"" << id++ << "\" Name=\"" << kNearFieldPrefix << (type == 0 ? "_E" : "_H")
                << "\" Number=\"0\" Type=\"0\" DumpType=\"" << type << "\" DumpMode=\"1\" FileType=\"1\">\n        <Primitives>";
            // Six faces: each pair at the low and high end of one axis.
            for (int axis = 0; axis < 3; ++axis)
                for (int side = 0; side < 2; ++side) {
                    Vec3 p1 = a, p2 = b;
                    p1[axis] = p2[axis] = side == 0 ? a[axis] : b[axis];
                    out << box(p1, p2, 0);
                }
            out << "</Primitives>\n      </DumpBox>\n";
        }
    }
    out << "    </Properties>\n";
    out << "    <RectilinearGrid DeltaUnit=\"0.001\" CoordSystem=\"0\">\n";
    out << "      <XLines>" << lines(mesh.lines[0]) << "</XLines>\n";
    out << "      <YLines>" << lines(mesh.lines[1]) << "</YLines>\n";
    out << "      <ZLines>" << lines(mesh.lines[2]) << "</ZLines>\n";
    out << "    </RectilinearGrid>\n";
    out << "  </ContinuousStructure>\n";
    out << "</openEMS>\n";
    return xml;
}

QString nf2ffXml(const std::vector<QString> &eFiles, const std::vector<QString> &hFiles, const std::vector<double> &frequencies,
                 const std::vector<double> &thetaDeg, const std::vector<double> &phiDeg, const Vec3 &centreMm, const QString &outfile)
{
    auto join = [](const std::vector<double> &v, double scale) {
        QStringList parts;
        for (double x : v)
            parts << num(x * scale);
        return parts.join(QLatin1Char(','));
    };
    const double rad = std::numbers::pi / 180.0;
    QString xml;
    QTextStream out(&xml);
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n";
    out << "<nf2ff Outfile=\"" << outfile.toHtmlEscaped() << "\" LegacyHDF5=\"1\" freq=\"" << join(frequencies, 1.0)
        << "\" Center=\"" << num(centreMm.x() * 1e-3) << ',' << num(centreMm.y() * 1e-3) << ',' << num(centreMm.z() * 1e-3) << "\">\n";
    out << "  <theta>" << join(thetaDeg, rad) << "</theta>\n";
    out << "  <phi>" << join(phiDeg, rad) << "</phi>\n";
    for (std::size_t i = 0; i < std::min(eFiles.size(), hFiles.size()); ++i)
        out << "  <Planes E_Field=\"" << eFiles[i].toHtmlEscaped() << "\" H_Field=\"" << hFiles[i].toHtmlEscaped() << "\" />\n";
    out << "</nf2ff>\n";
    return xml;
}

} // namespace lvd::fw
