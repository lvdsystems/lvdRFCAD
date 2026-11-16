#include "antenna/ArrayDesign.h"

namespace lvd::ant {

QJsonObject TrSideSpec::toJson() const
{
    QJsonObject o{{QStringLiteral("source"), source},
                  {QStringLiteral("power"), powerDbm},
                  {QStringLiteral("gain"), gainDb},
                  {QStringLiteral("nf"), nfDb}};
    if (!preset.isEmpty())
        o.insert(QStringLiteral("preset"), preset);
    if (!document.isEmpty())
        o.insert(QStringLiteral("document"), document);
    if (!analysis.isEmpty())
        o.insert(QStringLiteral("analysis"), analysis);
    return o;
}

TrSideSpec TrSideSpec::fromJson(const QJsonObject &o, bool transmit)
{
    TrSideSpec s;
    if (!transmit) {
        s.gainDb = 25;
        s.nfDb = 3;
    }
    const QString source = o.value(QStringLiteral("source")).toString(s.source);
    s.source = (source == QLatin1String("preset") || source == QLatin1String("linked")) ? source : QStringLiteral("typed");
    s.preset = o.value(QStringLiteral("preset")).toString();
    s.document = o.value(QStringLiteral("document")).toString();
    s.analysis = o.value(QStringLiteral("analysis")).toString();
    s.powerDbm = o.value(QStringLiteral("power")).toDouble(s.powerDbm);
    s.gainDb = o.value(QStringLiteral("gain")).toDouble(s.gainDb);
    s.nfDb = std::max(0.0, o.value(QStringLiteral("nf")).toDouble(s.nfDb));
    return s;
}

QJsonObject TrModuleSpec::toJson() const
{
    return QJsonObject{{QStringLiteral("tx"), tx.toJson()},
                       {QStringLiteral("rx"), rx.toJson()},
                       {QStringLiteral("frontLoss"), frontLossDb},
                       {QStringLiteral("beamformerLoss"), beamformerLossDb},
                       {QStringLiteral("antennaTemperature"), antennaTemperatureK},
                       {QStringLiteral("ambientTemperature"), ambientTemperatureK},
                       {QStringLiteral("txTaper"), txTaper}};
}

TrModuleSpec TrModuleSpec::fromJson(const QJsonObject &o)
{
    TrModuleSpec s;
    s.tx = TrSideSpec::fromJson(o.value(QStringLiteral("tx")).toObject(), true);
    s.rx = TrSideSpec::fromJson(o.value(QStringLiteral("rx")).toObject(), false);
    s.frontLossDb = std::max(0.0, o.value(QStringLiteral("frontLoss")).toDouble(s.frontLossDb));
    s.beamformerLossDb = std::max(0.0, o.value(QStringLiteral("beamformerLoss")).toDouble(s.beamformerLossDb));
    s.antennaTemperatureK = std::max(0.0, o.value(QStringLiteral("antennaTemperature")).toDouble(s.antennaTemperatureK));
    s.ambientTemperatureK = std::max(0.0, o.value(QStringLiteral("ambientTemperature")).toDouble(s.ambientTemperatureK));
    s.txTaper = o.value(QStringLiteral("txTaper")).toBool(false);
    return s;
}

QJsonObject SweepSpec::toJson() const
{
    return QJsonObject{{QStringLiteral("scanPlane"), scanPlanePhiDeg},
                       {QStringLiteral("scanMax"), scanMaxDeg},
                       {QStringLiteral("scanStep"), scanStepDeg},
                       {QStringLiteral("bandStart"), bandStart},
                       {QStringLiteral("bandStop"), bandStop},
                       {QStringLiteral("bandPoints"), bandPoints}};
}

SweepSpec SweepSpec::fromJson(const QJsonObject &o)
{
    SweepSpec s;
    s.scanPlanePhiDeg = o.value(QStringLiteral("scanPlane")).toDouble(0);
    s.scanMaxDeg = std::clamp(o.value(QStringLiteral("scanMax")).toDouble(60), 1.0, 90.0);
    s.scanStepDeg = std::clamp(o.value(QStringLiteral("scanStep")).toDouble(2), 0.1, 30.0);
    s.bandStart = std::max(0.0, o.value(QStringLiteral("bandStart")).toDouble(0));
    s.bandStop = std::max(0.0, o.value(QStringLiteral("bandStop")).toDouble(0));
    s.bandPoints = std::clamp(o.value(QStringLiteral("bandPoints")).toInt(21), 2, 1001);
    return s;
}

QJsonObject ArrayDesign::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("element"), elementRef);
    if (elementRef.isEmpty())
        o.insert(QStringLiteral("inlineElement"), inlineElement.toJson());
    o.insert(QStringLiteral("frequency"), frequency);
    o.insert(QStringLiteral("designFrequency"), designFrequency);
    o.insert(QStringLiteral("geometry"), geometry.toJson());
    o.insert(QStringLiteral("excitation"), excitation.toJson());
    o.insert(QStringLiteral("aesa"), aesa.toJson());
    o.insert(QStringLiteral("trModule"), trModule.toJson());
    o.insert(QStringLiteral("synthesis"), synthesis.toJson());
    o.insert(QStringLiteral("sweeps"), sweeps.toJson());
    if (!view.isEmpty())
        o.insert(QStringLiteral("view"), view);
    return o;
}

ArrayDesign ArrayDesign::fromJson(const QJsonObject &o)
{
    ArrayDesign d;
    d.elementRef = o.value(QStringLiteral("element")).toString();
    if (o.contains(QStringLiteral("inlineElement")))
        d.inlineElement = ElementSpec::fromJson(o.value(QStringLiteral("inlineElement")).toObject());
    d.frequency = o.value(QStringLiteral("frequency")).toDouble(d.frequency);
    if (d.frequency <= 0)
        d.frequency = 10e9;
    d.designFrequency = o.value(QStringLiteral("designFrequency")).toDouble(d.frequency);
    if (d.designFrequency <= 0)
        d.designFrequency = d.frequency;
    if (!o.contains(QStringLiteral("inlineElement")))
        d.inlineElement.frequency = d.designFrequency;
    if (o.contains(QStringLiteral("geometry")))
        d.geometry = GeometrySpec::fromJson(o.value(QStringLiteral("geometry")).toObject());
    d.excitation = ExcitationSpec::fromJson(o.value(QStringLiteral("excitation")).toObject());
    d.aesa = AesaSpec::fromJson(o.value(QStringLiteral("aesa")).toObject());
    d.trModule = TrModuleSpec::fromJson(o.value(QStringLiteral("trModule")).toObject());
    d.synthesis = SynthesisSpec::fromJson(o.value(QStringLiteral("synthesis")).toObject());
    d.sweeps = SweepSpec::fromJson(o.value(QStringLiteral("sweeps")).toObject());
    d.view = o.value(QStringLiteral("view")).toObject();
    return d;
}

QJsonObject ElementDesign::toJson() const
{
    QJsonObject o{{QStringLiteral("element"), element.toJson()}};
    if (!notes.isEmpty())
        o.insert(QStringLiteral("notes"), notes);
    if (!view.isEmpty())
        o.insert(QStringLiteral("view"), view);
    return o;
}

ElementDesign ElementDesign::fromJson(const QJsonObject &o)
{
    ElementDesign d;
    if (o.contains(QStringLiteral("element")))
        d.element = ElementSpec::fromJson(o.value(QStringLiteral("element")).toObject());
    d.notes = o.value(QStringLiteral("notes")).toString();
    d.view = o.value(QStringLiteral("view")).toObject();
    return d;
}

} // namespace lvd::ant
