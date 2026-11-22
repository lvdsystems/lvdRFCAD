#include "rfsim/Budget.h"

#include "core/Document.h"
#include "core/Project.h"
#include "core/Schematic.h"
#include "rfdata/NetworkMath.h"
#include "rfdata/NetworkOps.h"
#include "rfsim/Analysis.h"
#include "rfsim/BlockModels.h"
#include "rfsim/PhaseNoise.h"

#include <QFileInfo>
#include <QJsonArray>

#include <cmath>

namespace lvd::rf {

namespace {

double toMilliwatt(double dbm)
{
    return std::pow(10.0, dbm / 10.0);
}

double toDbm(double milliwatt)
{
    return milliwatt > 0 ? 10.0 * std::log10(milliwatt) : -INFINITY;
}

double noiseDbm(double temperature, double bandwidth)
{
    return toDbm(Boltzmann * temperature * bandwidth * 1000.0);
}

// One-frequency network for the cascade. All stages share a dummy frequency,
// because each is evaluated at its own (possibly translated) frequency.
NetworkData single(const CMatrix &s, const CMatrix &noise)
{
    NetworkData n;
    n.frequencies = {1.0};
    n.s = {s};
    n.z0 = {SolverZ0, SolverZ0};
    n.noiseCorrelation = {noise};
    return n;
}

double stageParameter(const ChainStage &stage, const char *name)
{
    return stage.component->parameterValues.value(QString::fromLatin1(name), NotSet);
}

const QString AnalysesKey = QStringLiteral("analyses");

} // namespace

BudgetResult computeBudget(ChainEvaluator &evaluator, const BudgetOptions &options, double inputFrequency)
{
    const SignalChain &chain = evaluator.chain();
    BudgetResult result;
    result.start = chain.start;
    result.end = chain.end;
    result.inputFrequency = std::isfinite(inputFrequency) ? inputFrequency : chain.inputFrequency;
    result.inputPowerDbm = chain.inputPowerDbm;
    result.bandwidth = options.bandwidth;
    result.sourceTemperature = chain.sourceTemperature;
    result.inputNoiseDbm = noiseDbm(chain.sourceTemperature, options.bandwidth);
    result.mismatch = options.mismatch;
    if (!chain.ok()) {
        result.errors = chain.errors;
        return result;
    }
    if (!(options.bandwidth > 0)) {
        result.errors.append(QStringLiteral("the bandwidth must be positive"));
        return result;
    }

    // A DAC at the start: signal and noise density at this frequency.
    if (chain.dac) {
        const DacStart &dac = *chain.dac;
        const DacFigures figures = dacFigures(result.inputFrequency, dac.sampleRate, dac.bits, dac.fullScaleDbm,
                                              dac.backoffDb, dac.nsdDbmHz);
        if (!chain.powerOverridden)
            result.inputPowerDbm = figures.signalDbm;
        result.hasDac = true;
        result.dacSincDb = figures.sincDb;
        result.dacNsdDbmHz = figures.nsdDbmHz;
        result.dacNsdFromQuantisation = figures.nsdFromQuantisation;
        result.dacImageFrequency = figures.imageFrequency;
        result.dacImageDbm = figures.imageDbm + (result.inputPowerDbm - figures.signalDbm); // follows a power override
    }

    // Phase-noise integration range and blocker.
    result.phaseNoiseStart = std::isfinite(options.phaseNoiseStart) ? options.phaseNoiseStart : options.bandwidth / 1000;
    result.phaseNoiseStop = std::isfinite(options.phaseNoiseStop) ? options.phaseNoiseStop : options.bandwidth / 2;
    if (!(result.phaseNoiseStart > 0) || !(result.phaseNoiseStop > result.phaseNoiseStart)) {
        result.errors.append(QStringLiteral("the phase-noise integration range needs 0 < start < stop"));
        return result;
    }
    if (std::isfinite(options.blockerPowerDbm)) {
        if (!(options.blockerOffset > 0)) {
            result.errors.append(QStringLiteral("the blocker offset must be positive"));
            return result;
        }
        result.hasBlocker = true;
        result.blockerOffset = options.blockerOffset;
        result.blockerPowerDbm = options.blockerPowerDbm;
    }

    std::optional<NetworkData> cumulative;
    double gainBefore = 1.0;          // linear cumulative gain at the input of the current stage
    double inverseIip3 = 0, inverseIp1 = 0, inverseRootIip2 = 0; // 1/mW and 1/sqrt(mW)
    double f = result.inputFrequency;
    double previousGainDb = 0;
    double phaseNoiseRatio = 0;       // phase-noise power over signal power, linear
    double reciprocalMixingInput = 0; // mW, referred to the chain input with the wanted signal's gain

    const auto addStage = [&](BudgetStage &row, const CMatrix &s, const CMatrix &noise) -> bool {
        CMatrix used = s, usedNoise = noise;
        // Stage noise figure with matched terminations.
        const double transmission = std::norm(s(1, 0));
        const double stageF = transmission > 0 ? 1.0 + noise(1, 1).real() / (T0 * transmission) : INFINITY;
        row.nfDb = 10.0 * std::log10(stageF);
        row.inputReturnLossDb = -db20(s(0, 0));
        if (!options.mismatch) {
            // Ideal matched cascade: keep the transmission and the stage NF only.
            used = CMatrix::Zero(2, 2);
            used(1, 0) = s(1, 0);
            usedNoise = CMatrix::Zero(2, 2);
            usedNoise(1, 1) = std::isfinite(stageF) ? T0 * (stageF - 1.0) * transmission : 0.0;
        }
        const NetworkData stageNetwork = single(used, usedNoise);
        if (!cumulative) {
            cumulative = stageNetwork;
        } else {
            QString error;
            auto next = cascade(*cumulative, stageNetwork, &error);
            if (!next) {
                result.errors.append(QStringLiteral("%1: %2").arg(row.path, error));
                return false;
            }
            cumulative = std::move(next);
        }
        const CMatrix &cs = cumulative->s.front();
        const double gain = std::norm(cs(1, 0));
        row.cumulativeGainDb = 10.0 * std::log10(gain);
        row.gainDb = row.cumulativeGainDb - previousGainDb;
        previousGainDb = row.cumulativeGainDb;
        const double added = cumulative->noiseCorrelation.front()(1, 1).real();
        const double cumulativeF = gain > 0 ? 1.0 + added / (T0 * gain) : INFINITY;
        row.cumulativeNfDb = 10.0 * std::log10(cumulativeF);

        // Nonlinearity, input-referred cascade formulas.
        const double stageGain = std::pow(10.0, row.gainDb / 10.0);
        if (std::isfinite(row.oip3Dbm))
            inverseIip3 += gainBefore / (toMilliwatt(row.oip3Dbm) / stageGain);
        if (std::isfinite(row.op1dbDbm))
            inverseIp1 += gainBefore / (toMilliwatt(row.op1dbDbm) / stageGain);
        if (std::isfinite(row.oip2Dbm))
            inverseRootIip2 += std::sqrt(gainBefore / (toMilliwatt(row.oip2Dbm) / stageGain));
        gainBefore = gain;
        row.iip3Dbm = inverseIip3 > 0 ? toDbm(1.0 / inverseIip3) : INFINITY;
        row.ip1dbDbm = inverseIp1 > 0 ? toDbm(1.0 / inverseIp1) : INFINITY;
        row.iip2Dbm = inverseRootIip2 > 0 ? toDbm(1.0 / (inverseRootIip2 * inverseRootIip2)) : INFINITY;
        row.oip3CumulativeDbm = row.iip3Dbm + row.cumulativeGainDb;
        row.op1dbCumulativeDbm = row.ip1dbDbm + row.cumulativeGainDb;

        // Levels
        const double effectiveTemperature = result.sourceTemperature + T0 * (cumulativeF - 1.0);
        row.signalDbm = result.inputPowerDbm + row.cumulativeGainDb;
        row.noiseDbm = noiseDbm(effectiveTemperature, options.bandwidth) + row.cumulativeGainDb;
        row.thermalSnrDb = row.signalDbm - row.noiseDbm;
        row.phaseNoiseDbm = phaseNoiseRatio > 0 ? row.signalDbm + 10.0 * std::log10(phaseNoiseRatio) : -INFINITY;
        row.reciprocalMixingDbm = reciprocalMixingInput > 0 ? toDbm(reciprocalMixingInput) + row.cumulativeGainDb : -INFINITY;
        row.phaseNoiseSnrDb = row.signalDbm - row.phaseNoiseDbm;
        row.reciprocalMixingSnrDb = row.signalDbm - row.reciprocalMixingDbm;
        // Without phase noise and blocker the total is the thermal noise, bit for bit.
        row.totalNoiseDbm = phaseNoiseRatio > 0 || reciprocalMixingInput > 0
                                ? toDbm(toMilliwatt(row.noiseDbm) + toMilliwatt(row.phaseNoiseDbm)
                                        + toMilliwatt(row.reciprocalMixingDbm))
                                : row.noiseDbm;
        row.snrDb = row.signalDbm - row.totalNoiseDbm;
        const double inputFloor = noiseDbm(effectiveTemperature, options.bandwidth);
        row.sfdrDb = std::isfinite(row.iip3Dbm) ? 2.0 / 3.0 * (row.iip3Dbm - inputFloor) : INFINITY;
        row.headroomDb = row.op1dbCumulativeDbm - row.signalDbm;
        row.compressed = row.headroomDb < 0;
        if (row.compressed)
            result.warnings.append(QStringLiteral("%1: the signal is %2 dB above the cumulative 1 dB compression point")
                                       .arg(row.path)
                                       .arg(-row.headroomDb, 0, 'f', 1));
        result.stages.push_back(row);
        return true;
    };

    for (int i = 0; i < static_cast<int>(chain.stages.size()); ++i) {
        const ChainStage &stage = chain.stages[static_cast<size_t>(i)];
        BudgetStage row;
        row.path = stage.path;
        row.name = stage.name;
        row.kind = stage.kind;
        row.inputFrequency = f;
        row.outputFrequency = evaluator.outputFrequency(i, f);

        QString error;
        const auto s = evaluator.stageS(i, f, &error);
        const auto noise = evaluator.stageNoise(i, f, &error);
        if (!s || !noise) {
            result.errors.append(error);
            return result;
        }
        if (stage.kind == QLatin1String("amplifier")) {
            row.oip3Dbm = stageParameter(stage, "oip3");
            row.op1dbDbm = stageParameter(stage, "op1db");
            row.oip2Dbm = stageParameter(stage, "oip2");
        } else if (stage.kind == QLatin1String("limiter")) {
            // The output clamp acts as a hard compression point.
            row.op1dbDbm = stageParameter(stage, "limit");
            row.notes.append(QStringLiteral("output limited to %1 dBm (used as OP1dB)").arg(row.op1dbDbm, 0, 'g', 4));
        } else if (stage.kind == QLatin1String("mixer")) {
            const double iip3 = stageParameter(stage, "iip3");
            const double gain = stageParameter(stage, "gain");
            row.oip3Dbm = iip3 + gain;
            // Typical for diode mixers: input P1dB about 10 dB below IIP3.
            row.op1dbDbm = iip3 - 10.0 + gain;
            row.notes.append(QStringLiteral("P1dB estimated as IIP3 - 10 dB"));
            if (std::isfinite(stage.loPowerDbm)) {
                const double nominal = stageParameter(stage, "lo_power");
                if (std::isfinite(nominal) && std::abs(stage.loPowerDbm - nominal) > 3.0)
                    result.warnings.append(QStringLiteral("%1: the LO drive is %2 dBm, the nominal level is %3 dBm")
                                               .arg(stage.path)
                                               .arg(stage.loPowerDbm, 0, 'f', 1)
                                               .arg(nominal, 0, 'f', 1));
            }

            // LO phase noise on the signal, and reciprocal mixing of the blocker.
            QString noiseError;
            const PhaseNoiseProfile *profile = evaluator.loPhaseNoise(i, &noiseError);
            if (!noiseError.isEmpty())
                result.warnings.append(QStringLiteral("%1: the LO phase noise is not used (%2)").arg(stage.path, noiseError));
            if (profile) {
                result.hasPhaseNoise = true;
                const IntegratedPhaseNoise integrated =
                    integratePhaseNoise(*profile, result.phaseNoiseStart, result.phaseNoiseStop, stage.loFrequency);
                row.loPhaseNoiseDbc = integrated.dbc;
                row.loRmsPhaseDeg = integrated.rmsPhaseDegrees;
                row.loJitterS = integrated.rmsJitterSeconds;
                phaseNoiseRatio += toMilliwatt(integrated.dbc);
                row.notes.append(QStringLiteral("LO phase noise %1 dBc from %2 to %3 (%4 deg rms, %5 rms jitter)%6")
                                     .arg(integrated.dbc, 0, 'f', 1)
                                     .arg(formatEngineering(result.phaseNoiseStart, QStringLiteral("Hz")),
                                          formatEngineering(result.phaseNoiseStop, QStringLiteral("Hz")))
                                     .arg(integrated.rmsPhaseDegrees, 0, 'g', 3)
                                     .arg(formatEngineering(integrated.rmsJitterSeconds, QStringLiteral("s")))
                                     .arg(stage.loMultiplication > 1
                                              ? QStringLiteral(", LO multiplied by %1").arg(stage.loMultiplication)
                                              : QString()));
            }
            if (result.hasBlocker) {
                QString blockerError;
                double blockerFrequency = NotSet;
                const double gainToMixer = evaluator.forwardGainDb(0, i, result.inputFrequency + result.blockerOffset,
                                                                   &blockerFrequency, &blockerError);
                if (!std::isfinite(gainToMixer)) {
                    result.warnings.append(QStringLiteral("%1: the blocker level is not known (%2)").arg(stage.path, blockerError));
                } else {
                    row.blockerDbm = result.blockerPowerDbm + gainToMixer;
                    const double offset = std::abs(blockerFrequency - f);
                    if (profile && offset > 0) {
                        row.loPhaseNoiseAtOffsetDbcHz = profile->level(offset);
                        const double inChannel = row.blockerDbm + row.loPhaseNoiseAtOffsetDbcHz + 10.0 * std::log10(options.bandwidth);
                        reciprocalMixingInput += toMilliwatt(inChannel - previousGainDb);
                        row.notes.append(QStringLiteral("blocker %1 dBm at the mixer, L(%2) = %3 dBc/Hz: reciprocal mixing "
                                                        "%4 dBm in the channel")
                                             .arg(row.blockerDbm, 0, 'f', 1)
                                             .arg(formatEngineering(offset, QStringLiteral("Hz")))
                                             .arg(row.loPhaseNoiseAtOffsetDbcHz, 0, 'f', 1)
                                             .arg(inChannel, 0, 'f', 1));
                    }
                }
            }
        } else if (stage.translation == ChainStage::Translation::Multiplier) {
            // Phase deviations are multiplied by N: the phase-noise ratio rises by N^2.
            phaseNoiseRatio *= static_cast<double>(stage.factor) * stage.factor;
        }
        if (result.hasDac) {
            QString imageError;
            const double imageGain = evaluator.forwardGainDb(0, i + 1, result.dacImageFrequency, nullptr, &imageError);
            row.dacImageDbm = std::isfinite(imageGain) ? result.dacImageDbm + imageGain : NotSet;
        }
        if (!addStage(row, *s, *noise))
            return result;
        f = row.outputFrequency;
    }

    // The component at the end of the chain.
    const FlatComponent *endComponent = chain.end.isEmpty() ? nullptr : chain.netlist.component(chain.end);
    const auto endValue = [&](const char *name) {
        return endComponent ? endComponent->parameterValues.value(QString::fromLatin1(name), NotSet) : NotSet;
    };

    // An ADC at the end adds its noise figure and a full-scale clip.
    if (chain.endKind == QLatin1String("adc") && endComponent) {
        BudgetStage row;
        row.path = endComponent->path;
        row.name = endComponent->definition->name;
        row.kind = QStringLiteral("adc");
        row.inputFrequency = row.outputFrequency = f;
        row.op1dbDbm = endValue("full_scale");
        if (!result.stages.empty())
            row.dacImageDbm = result.stages.back().dacImageDbm;
        CMatrix s = CMatrix::Zero(2, 2);
        s(1, 0) = 1.0;
        CMatrix noise = CMatrix::Zero(2, 2);
        noise(1, 1) = T0 * (std::pow(10.0, std::max(0.0, endValue("nf")) / 10.0) - 1.0);
        if (!addStage(row, s, noise))
            return result;
        result.hasAdc = true;
        result.adcFullScaleDbm = endValue("full_scale");
        result.adcHeadroomDb = result.adcFullScaleDbm - row.signalDbm;
        const double bits = endValue("bits");
        result.adcQuantisationSnrDb = 6.02 * bits + 1.76;
        result.adcSampleRate = endValue("fs");
        if (result.adcSampleRate > 0) {
            double alias = std::fmod(f, result.adcSampleRate);
            if (alias > result.adcSampleRate / 2)
                alias = result.adcSampleRate - alias;
            result.adcAliasFrequency = alias;
        }
        if (result.adcHeadroomDb < 0)
            result.warnings.append(QStringLiteral("%1: the signal exceeds the ADC full scale by %2 dB")
                                       .arg(row.path)
                                       .arg(-result.adcHeadroomDb, 0, 'f', 1));
    }

    // A detector at the end: margin over its sensitivity and room to the top of its range.
    if (chain.endKind == QLatin1String("detector") && endComponent && !result.stages.empty()) {
        const double signal = result.stages.back().signalDbm;
        result.hasDetector = true;
        result.detectorSensitivityDbm = endValue("sensitivity");
        result.detectorMaxDbm = result.detectorSensitivityDbm + endValue("dynamic_range");
        result.detectorMarginDb = signal - result.detectorSensitivityDbm;
        result.detectorHeadroomDb = result.detectorMaxDbm - signal;
        if (result.detectorMarginDb < 0)
            result.warnings.append(QStringLiteral("%1: the signal is %2 dB below the detector's sensitivity")
                                       .arg(endComponent->path)
                                       .arg(-result.detectorMarginDb, 0, 'f', 1));
        if (result.detectorHeadroomDb < 0)
            result.warnings.append(QStringLiteral("%1: the signal is %2 dB above the detector's dynamic range")
                                       .arg(endComponent->path)
                                       .arg(-result.detectorHeadroomDb, 0, 'f', 1));
    }
    return result;
}

// ---------------------------------------------------------------- setup

BudgetAnalysisSetup BudgetAnalysisSetup::fromJson(const QJsonObject &object)
{
    BudgetAnalysisSetup setup;
    setup.id = object.value(QStringLiteral("id")).toString();
    setup.name = object.value(QStringLiteral("name")).toString();
    for (const QJsonValue &value : object.value(QStringLiteral("path")).toArray())
        setup.path.append(value.toString());
    setup.start = object.value(QStringLiteral("start")).toString();
    setup.frequency = object.value(QStringLiteral("frequency")).toString();
    setup.power = object.value(QStringLiteral("power")).toString();
    setup.bandwidth = object.value(QStringLiteral("bandwidth")).toString(setup.bandwidth);
    setup.mismatch = object.value(QStringLiteral("mismatch")).toString(QStringLiteral("actual")) != QLatin1String("ideal");
    const QJsonObject sweep = object.value(QStringLiteral("sweep")).toObject();
    setup.sweepStart = sweep.value(QStringLiteral("start")).toString();
    setup.sweepStop = sweep.value(QStringLiteral("stop")).toString();
    setup.sweepPoints = sweep.value(QStringLiteral("points")).toString(setup.sweepPoints);
    const QJsonObject spurs = object.value(QStringLiteral("spurs")).toObject();
    setup.spurs = spurs.value(QStringLiteral("enabled")).toBool(true);
    setup.spurOrder = spurs.value(QStringLiteral("maxOrder")).toString(setup.spurOrder);
    const QJsonObject phaseNoise = object.value(QStringLiteral("phaseNoise")).toObject();
    setup.phaseNoiseStart = phaseNoise.value(QStringLiteral("start")).toString();
    setup.phaseNoiseStop = phaseNoise.value(QStringLiteral("stop")).toString();
    const QJsonObject blocker = object.value(QStringLiteral("blocker")).toObject();
    setup.blockerOffset = blocker.value(QStringLiteral("offset")).toString();
    setup.blockerPower = blocker.value(QStringLiteral("power")).toString();
    return setup;
}

QJsonObject BudgetAnalysisSetup::toJson() const
{
    QJsonObject object{{QStringLiteral("id"), id},
                       {QStringLiteral("type"), type()},
                       {QStringLiteral("name"), name},
                       {QStringLiteral("bandwidth"), bandwidth},
                       {QStringLiteral("mismatch"), mismatch ? QStringLiteral("actual") : QStringLiteral("ideal")}};
    if (!path.isEmpty())
        object.insert(QStringLiteral("path"), QJsonArray::fromStringList(path));
    if (!start.isEmpty())
        object.insert(QStringLiteral("start"), start);
    if (!frequency.isEmpty())
        object.insert(QStringLiteral("frequency"), frequency);
    if (!power.isEmpty())
        object.insert(QStringLiteral("power"), power);
    if (!sweepStart.isEmpty() || !sweepStop.isEmpty())
        object.insert(QStringLiteral("sweep"), QJsonObject{{QStringLiteral("start"), sweepStart},
                                                           {QStringLiteral("stop"), sweepStop},
                                                           {QStringLiteral("points"), sweepPoints}});
    object.insert(QStringLiteral("spurs"),
                  QJsonObject{{QStringLiteral("enabled"), spurs}, {QStringLiteral("maxOrder"), spurOrder}});
    if (!phaseNoiseStart.isEmpty() || !phaseNoiseStop.isEmpty())
        object.insert(QStringLiteral("phaseNoise"),
                      QJsonObject{{QStringLiteral("start"), phaseNoiseStart}, {QStringLiteral("stop"), phaseNoiseStop}});
    if (!blockerOffset.isEmpty() || !blockerPower.isEmpty())
        object.insert(QStringLiteral("blocker"),
                      QJsonObject{{QStringLiteral("offset"), blockerOffset}, {QStringLiteral("power"), blockerPower}});
    return object;
}

QList<BudgetAnalysisSetup> budgetAnalyses(const QJsonObject &simulation)
{
    QList<BudgetAnalysisSetup> result;
    for (const QJsonValue &value : simulation.value(AnalysesKey).toArray()) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("type")).toString() == BudgetAnalysisSetup::type())
            result.append(BudgetAnalysisSetup::fromJson(object));
    }
    return result;
}

QJsonObject withBudgetAnalysis(const QJsonObject &simulation, BudgetAnalysisSetup analysis, QString *assignedId)
{
    QJsonArray analyses = simulation.value(AnalysesKey).toArray();
    if (analysis.id.isEmpty()) {
        int highest = 0;
        for (const QJsonValue &value : analyses) {
            const QString id = value.toObject().value(QStringLiteral("id")).toString();
            if (id.startsWith(QLatin1Char('a')))
                highest = std::max(highest, id.mid(1).toInt());
        }
        analysis.id = QStringLiteral("a%1").arg(highest + 1);
    }
    if (analysis.name.isEmpty())
        analysis.name = QStringLiteral("Budget");
    // Keys this version does not know are kept; known optional keys that the
    // new setup leaves out are removed.
    static const QStringList known = {QStringLiteral("id"),        QStringLiteral("type"),  QStringLiteral("name"),
                                      QStringLiteral("path"),      QStringLiteral("start"), QStringLiteral("frequency"),
                                      QStringLiteral("power"),     QStringLiteral("bandwidth"),
                                      QStringLiteral("mismatch"),  QStringLiteral("sweep"), QStringLiteral("spurs"),
                                      QStringLiteral("phaseNoise"), QStringLiteral("blocker")};
    bool replaced = false;
    for (qsizetype i = 0; i < analyses.size(); ++i) {
        QJsonObject existing = analyses.at(i).toObject();
        if (existing.value(QStringLiteral("id")).toString() == analysis.id) {
            for (const QString &key : known)
                existing.remove(key);
            const QJsonObject fresh = analysis.toJson();
            for (auto it = fresh.begin(); it != fresh.end(); ++it)
                existing.insert(it.key(), it.value());
            analyses.replace(i, existing);
            replaced = true;
            break;
        }
    }
    if (!replaced)
        analyses.append(analysis.toJson());
    QJsonObject result = simulation;
    result.insert(AnalysesKey, analyses);
    if (assignedId)
        *assignedId = analysis.id;
    return result;
}

// ---------------------------------------------------------------- mixer products

QString mixerProductName(const ChainStage &stage)
{
    if (stage.translation != ChainStage::Translation::Mixer)
        return {};
    return stage.sumProduct ? QStringLiteral("sum") : QStringLiteral("difference");
}

QStringList checkMixerProducts(ChainEvaluator &evaluator)
{
    QStringList warnings;
    const SignalChain &chain = evaluator.chain();
    const int count = static_cast<int>(chain.stages.size());
    for (int i = 0; i < count; ++i) {
        const ChainStage &mixer = chain.stages[static_cast<size_t>(i)];
        if (mixer.translation != ChainStage::Translation::Mixer || !std::isfinite(mixer.loFrequency))
            continue;
        // The stages that see this mixer's output, up to the next frequency translation.
        int end = i + 1;
        while (end < count && chain.stages[static_cast<size_t>(end)].translation == ChainStage::Translation::None)
            ++end;
        if (end == i + 1)
            continue;
        const double wanted = mixer.outputFrequency;
        const double other = mixer.sumProduct ? std::abs(mixer.inputFrequency - mixer.loFrequency)
                                              : mixer.inputFrequency + mixer.loFrequency;
        if (!(other > 0))
            continue;
        const double gainWanted = evaluator.forwardGainDb(i + 1, end, wanted);
        const double gainOther = evaluator.forwardGainDb(i + 1, end, other);
        if (!std::isfinite(gainWanted) || !std::isfinite(gainOther) || gainOther - gainWanted < 20.0)
            continue;
        const QString otherName = mixer.sumProduct ? QStringLiteral("difference") : QStringLiteral("sum");
        warnings.append(QStringLiteral("%1: the following stages pass %2 (the %3 product) but attenuate the selected "
                                       "%4 product at %5 by %6 dB. Set the mixer's \"product\" to \"%3\"?")
                            .arg(mixer.path, formatEngineering(other, QStringLiteral("Hz")), otherName,
                                 mixerProductName(mixer), formatEngineering(wanted, QStringLiteral("Hz")))
                            .arg(gainOther - gainWanted, 0, 'f', 0));
    }
    return warnings;
}

// ---------------------------------------------------------------- runs

BudgetRun runBudget(const Netlist &netlist, const BudgetAnalysisSetup &setup, const Expression::Lookup &scope,
                    const QString &projectDirectory, TouchstoneCache *touchstones, const std::atomic_bool *cancel)
{
    BudgetRun run;
    const auto evaluate = [&](const QString &text, const QString &what) -> double {
        if (text.trimmed().isEmpty())
            return NotSet;
        QString error;
        const auto value = Expression::parse(text).evaluate(scope, &error);
        if (!value) {
            run.errors.append(QStringLiteral("%1: %2").arg(what, error));
            return NotSet;
        }
        return *value;
    };
    ChainOptions chainOptions;
    chainOptions.path = setup.path;
    chainOptions.start = setup.start;
    chainOptions.frequency = evaluate(setup.frequency, QStringLiteral("frequency"));
    chainOptions.powerDbm = evaluate(setup.power, QStringLiteral("power"));
    BudgetOptions options;
    options.bandwidth = evaluate(setup.bandwidth, QStringLiteral("bandwidth"));
    options.mismatch = setup.mismatch;
    options.phaseNoiseStart = evaluate(setup.phaseNoiseStart, QStringLiteral("phase-noise integration start"));
    options.phaseNoiseStop = evaluate(setup.phaseNoiseStop, QStringLiteral("phase-noise integration stop"));
    options.blockerPowerDbm = evaluate(setup.blockerPower, QStringLiteral("blocker power"));
    options.blockerOffset = evaluate(setup.blockerOffset, QStringLiteral("blocker offset"));
    if (std::isfinite(options.blockerPowerDbm) && !std::isfinite(options.blockerOffset))
        run.errors.append(QStringLiteral("give the blocker offset"));
    const double sweepStart = evaluate(setup.sweepStart, QStringLiteral("sweep start"));
    const double sweepStop = evaluate(setup.sweepStop, QStringLiteral("sweep stop"));
    const double sweepPoints = std::isfinite(sweepStart) ? evaluate(setup.sweepPoints, QStringLiteral("sweep points")) : NotSet;
    const double spurOrder = setup.spurs ? evaluate(setup.spurOrder, QStringLiteral("spur order")) : NotSet;
    if (!run.errors.isEmpty())
        return run;

    run.chain = traceChain(netlist, chainOptions);
    run.warnings.append(run.chain.warnings);
    if (!run.chain.ok())
        return run;

    ChainEvaluator evaluator(run.chain, projectDirectory, touchstones);
    run.budget = computeBudget(evaluator, options);
    run.warnings.append(run.budget.warnings);
    run.warnings.append(checkMixerProducts(evaluator));

    if (std::isfinite(sweepStart) && std::isfinite(sweepStop)) {
        const int points = static_cast<int>(std::lround(sweepPoints));
        if (points < 1 || points > 10000 || sweepStop < sweepStart) {
            run.errors.append(QStringLiteral("the budget sweep needs start <= stop and 1 to 10000 points"));
            return run;
        }
        for (int i = 0; i < points; ++i) {
            if (cancel && cancel->load()) {
                run.cancelled = true;
                return run;
            }
            const double f = points == 1 ? sweepStart : sweepStart + (sweepStop - sweepStart) * i / (points - 1);
            run.sweepFrequencies.push_back(f);
            run.sweep.push_back(computeBudget(evaluator, options, f));
        }
    }

    bool hasMixer = false;
    for (const ChainStage &stage : run.chain.stages)
        hasMixer = hasMixer || stage.translation == ChainStage::Translation::Mixer;
    if (setup.spurs && hasMixer) {
        SpurOptions spurOptions;
        spurOptions.maxOrder = std::max(2, static_cast<int>(std::lround(spurOrder)));
        spurOptions.bandwidth = options.bandwidth;
        spurOptions.projectDirectory = projectDirectory;
        run.spurs = analyseSpurs(evaluator, spurOptions);
        run.warnings.append(run.spurs.warnings);
        for (const QString &message : run.spurs.errors)
            run.warnings.append(QStringLiteral("spur analysis: %1").arg(message));
    }
    return run;
}

BudgetRun runBudgetAnalysis(const Project &project, const Document &document, const BudgetAnalysisSetup &setup,
                            TouchstoneCache *touchstones, const std::atomic_bool *cancel)
{
    if (!document.schematic()) {
        BudgetRun run;
        run.errors.append(QStringLiteral("%1 is not a schematic").arg(document.name()));
        return run;
    }
    const Netlist netlist = buildNetlist(project, document);
    return runBudget(netlist, setup, sheetScope(document), project.directory(), touchstones, cancel);
}

} // namespace lvd::rf
