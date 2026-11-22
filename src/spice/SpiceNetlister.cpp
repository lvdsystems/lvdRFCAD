#include "spice/SpiceNetlister.h"

#include "core/Document.h"
#include "core/Project.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <cmath>

namespace lvd::spice {

namespace {

constexpr double Pi = 3.14159265358979323846;

// Letters, digits and underscores only, lower case unless keepCase; never empty.
QString sanitize(const QString &text, bool keepCase = false)
{
    QString out;
    out.reserve(text.size());
    bool lastUnderscore = false;
    for (const QChar c : text) {
        if (c.isLetterOrNumber() && c.unicode() < 128) {
            out += keepCase ? c : c.toLower();
            lastUnderscore = false;
        } else if (!lastUnderscore && !out.isEmpty()) {
            out += QLatin1Char('_');
            lastUnderscore = true;
        }
    }
    while (out.endsWith(QLatin1Char('_')))
        out.chop(1);
    return out.isEmpty() ? QStringLiteral("x") : out;
}

// SPICE names are case-insensitive: taken holds lower-case names.
QString uniqueName(const QString &base, QSet<QString> &taken)
{
    QString name = base;
    for (int i = 2; taken.contains(name.toLower()); ++i)
        name = QStringLiteral("%1_%2").arg(base).arg(i);
    taken.insert(name.toLower());
    return name;
}

ProbeKind probeKind(const QString &text, bool *known)
{
    *known = true;
    if (text.isEmpty())
        return ProbeKind::None;
    if (text == QLatin1String("voltage"))
        return ProbeKind::Voltage;
    if (text == QLatin1String("differential"))
        return ProbeKind::Differential;
    if (text == QLatin1String("current"))
        return ProbeKind::Current;
    *known = false;
    return ProbeKind::None;
}

// Element and its own current/power vectors, for the annotated schematic.
// Only primitives SPICE knows instance parameters for; subcircuit instances
// have no single current.
QStringList deviceVectors(const QString &element)
{
    const QString name = element.toLower();
    const auto at = [&](const char *parameter) { return QStringLiteral("@%1[%2]").arg(name, QLatin1String(parameter)); };
    switch (name.at(0).unicode()) {
    case 'r':
        return {at("i"), at("p")};
    case 'c':
    case 'l':
        return {at("i")};
    case 'd':
        return {at("id"), at("p")};
    case 'q':
        return {at("ic"), at("ib"), at("ie"), at("p")};
    case 'm':
        return {at("id"), at("p")};
    case 'j':
        return {at("id"), at("p")};
    case 'b':
        return {at("i")};
    case 'v':
    case 'i':
        return {at("p")};
    default:
        return {};
    }
}

PythonModelHandler &pythonHandler()
{
    static PythonModelHandler handler;
    return handler;
}

} // namespace

void setPythonModelHandler(PythonModelHandler handler)
{
    pythonHandler() = std::move(handler);
}

bool hasPythonModelHandler()
{
    return bool(pythonHandler());
}

QString formatSpiceNumber(double value)
{
    if (value == 0.0)
        return QStringLiteral("0");
    if (!std::isfinite(value))
        return QStringLiteral("0");
    QString text = QString::number(value, 'g', 12);
    // QString::number gives "1e-12"; SPICE accepts that form.
    return text;
}

const SpiceElement *SpiceDeck::element(const QString &path) const
{
    for (const SpiceElement &e : elements)
        if (e.path.compare(path, Qt::CaseInsensitive) == 0)
            return &e;
    return nullptr;
}

const SpiceElement *SpiceDeck::elementByName(const QString &spiceName) const
{
    const QString lower = spiceName.toLower();
    for (const SpiceElement &e : elements)
        if (e.elementNames.contains(lower))
            return &e;
    return nullptr;
}

QString SpiceDeck::node(const QString &net) const
{
    const QString trimmed = net.trimmed();
    if (trimmed == QLatin1String("0"))
        return trimmed;
    for (auto it = nodeOfNet.begin(); it != nodeOfNet.end(); ++it)
        if (it.key().compare(trimmed, Qt::CaseInsensitive) == 0)
            return it.value();
    // Already a SPICE node name?
    const QString lower = trimmed.toLower();
    if (netOfNode.contains(lower))
        return lower;
    return {};
}

SpiceDeck buildSpiceDeck(const Project &project, const Document &top, const SpiceNetlistOptions &options)
{
    const Netlist netlist = buildNetlist(project, top);
    SpiceNetlistOptions withTitle = options;
    if (withTitle.title.isEmpty())
        withTitle.title = top.name();
    return buildSpiceDeck(netlist, project.directory(), withTitle);
}

SpiceDeck buildSpiceDeck(const Netlist &netlist, const QString &projectDirectory, const SpiceNetlistOptions &options)
{
    SpiceDeck deck;
    deck.errors = netlist.errors;
    deck.warnings = netlist.warnings;

    // Nodes. "gnd" is ground to ngspice, so a net of that name that is not
    // connected to a ground symbol gets another name.
    QSet<QString> takenNodes{QStringLiteral("0")};
    for (const QString &net : netlist.nets) {
        if (net == QLatin1String("0")) {
            deck.nodeOfNet.insert(net, net);
            continue;
        }
        QString base = sanitize(net);
        if (base == QLatin1String("gnd") || base == QLatin1String("0")) {
            deck.warnings << QStringLiteral("Net \"%1\" is not connected to a ground symbol; it is simulated as node \"%2\".")
                                 .arg(net, base + QStringLiteral("_net"));
            base += QStringLiteral("_net");
        }
        deck.nodeOfNet.insert(net, uniqueName(base, takenNodes));
    }
    for (auto it = deck.nodeOfNet.begin(); it != deck.nodeOfNet.end(); ++it)
        deck.netOfNode.insert(it.value(), it.key());

    QStringList includes;
    QStringList sharedTexts;
    QStringList elementLines;
    QStringList saves;
    QSet<QString> takenRefs;
    QSet<QString> takenElements;
    static const QRegularExpression placeholder(QStringLiteral("\\{([^{}]+)\\}"));

    for (const FlatComponent &component : netlist.components) {
        const ComponentDefinition *definition = component.definition.get();
        if (!definition)
            continue;
        const ModelDefinition *model = definition->model(QStringLiteral("spice"));
        const ModelDefinition *pythonModel = model ? nullptr : definition->model(QStringLiteral("python"));
        if (!model && !pythonModel) {
            deck.errors << QStringLiteral("%1 (%2) has no SPICE model.").arg(component.path, definition->name);
            continue;
        }
        if (pythonModel && !hasPythonModelHandler()) {
            deck.errors << QStringLiteral("%1 (%2) has a Python model, but Python is not available here.").arg(component.path, definition->name);
            continue;
        }
        // A Python model renders like a template: its text is the template.
        ModelDefinition pythonRendered;
        if (pythonModel) {
            PythonModelRequest request;
            request.model = pythonModel->data;
            QSet<QString> preview = takenRefs; // the name the element gets below
            request.reference = uniqueName(sanitize(component.path, true), preview);
            request.projectDirectory = projectDirectory;
            for (const QString &pin : definition->pins) {
                const QString node = deck.nodeOfNet.value(component.pinNets.value(pin));
                request.nodes.insert(pin, node.isEmpty() ? QStringLiteral("nc_%1_%2").arg(request.reference.toLower(), sanitize(pin)) : node);
            }
            for (const ParameterDefinition &parameter : definition->parameters) {
                if (parameter.isExpression() && component.parameterValues.contains(parameter.name))
                    request.parameters.insert(parameter.name, component.parameterValues.value(parameter.name));
                else
                    request.parameters.insert(parameter.name, component.parameterText.value(parameter.name));
            }
            QString error;
            const std::optional<QString> text = pythonHandler()(request, &error);
            if (!text) {
                deck.errors << QStringLiteral("%1: Python model: %2").arg(component.path, error);
                continue;
            }
            pythonRendered.kind = QStringLiteral("spice");
            pythonRendered.data = QJsonObject{{QStringLiteral("kind"), QStringLiteral("spice")},
                                              {QStringLiteral("template"), *text},
                                              {QStringLiteral("verbatim"), true}};
            model = &pythonRendered;
        }

        SpiceElement element;
        element.path = component.path;
        element.document = component.document;
        element.componentId = component.componentId;
        bool knownProbe = true;
        element.probe = probeKind(model->data.value(QStringLiteral("probe")).toString(), &knownProbe);
        if (!knownProbe)
            deck.warnings << QStringLiteral("%1: unknown probe kind \"%2\"").arg(component.path, model->data.value(QStringLiteral("probe")).toString());

        const QString templateText = model->data.value(QStringLiteral("template")).toString();
        const QString ref = uniqueName(sanitize(component.path, true), takenRefs);
        for (const QString &pin : definition->pins) {
            const QString net = component.pinNets.value(pin);
            QString node = deck.nodeOfNet.value(net);
            if (node.isEmpty()) {
                node = uniqueName(QStringLiteral("nc_%1_%2").arg(ref.toLower(), sanitize(pin)), takenNodes);
                // Pins of probes and instruments without elements may stay open.
                if (!templateText.trimmed().isEmpty())
                    deck.warnings << QStringLiteral("%1: pin %2 is not connected").arg(component.path, pin);
                deck.netOfNode.insert(node, QString());
            }
            element.pinNodes.insert(pin, node);
        }

        // Template rendering.
        bool failed = false;
        const auto render = [&](const QString &text) {
            QString out;
            qsizetype last = 0;
            auto it = placeholder.globalMatch(text);
            while (it.hasNext()) {
                const QRegularExpressionMatch match = it.next();
                out += text.mid(last, match.capturedStart() - last);
                last = match.capturedEnd();
                const QString key = match.captured(1).trimmed();
                if (key == QLatin1String("ref")) {
                    out += ref;
                } else if (key.startsWith(QLatin1String("pin:"))) {
                    const QString pin = key.mid(4).trimmed();
                    if (!element.pinNodes.contains(pin)) {
                        deck.errors << QStringLiteral("%1: SPICE template names unknown pin \"%2\"").arg(component.path, pin);
                        failed = true;
                    }
                    out += element.pinNodes.value(pin);
                } else if (const ParameterDefinition *parameter = definition->parameter(key)) {
                    if (parameter->isExpression()) {
                        const auto value = component.parameterValues.constFind(key);
                        if (value == component.parameterValues.cend()) {
                            deck.errors << QStringLiteral("%1: parameter %2 = \"%3\" has no value")
                                               .arg(component.path, key, component.parameterText.value(key));
                            failed = true;
                        } else {
                            out += formatSpiceNumber(*value);
                        }
                    } else {
                        QString value = component.parameterText.value(key);
                        if (parameter->isFile && !value.isEmpty() && QFileInfo(value).isRelative())
                            value = QDir(projectDirectory).absoluteFilePath(value);
                        out += value;
                    }
                } else {
                    deck.errors << QStringLiteral("%1: SPICE template names unknown parameter \"%2\"").arg(component.path, key);
                    failed = true;
                }
            }
            out += text.mid(last);
            return out;
        };

        // Parasitics of the basic R, L and C (model field "parasitics",
        // docs/file-format.md section 4.4): when one is non-zero the part
        // becomes a small network with internal nodes, the same circuit as
        // its linear RF model. Without parasitics the template is used.
        const auto parasiticLines = [&]() -> std::optional<QStringList> {
            const QString kind = model->data.value(QStringLiteral("parasitics")).toString();
            if (kind.isEmpty())
                return std::nullopt;
            const auto value = [&](const char *name) {
                return component.parameterValues.value(QString::fromLatin1(name), 0.0);
            };
            const QString p = element.pinNodes.value(QStringLiteral("p")), n = element.pinNodes.value(QStringLiteral("n"));
            int internal = 0;
            const auto node = [&]() {
                const QString name = uniqueName(QStringLiteral("%1_int%2").arg(ref.toLower()).arg(++internal), takenNodes);
                deck.netOfNode.insert(name, QString());
                return name;
            };
            const auto number = [](double v) { return formatSpiceNumber(v); };
            QStringList lines;
            if (kind == QLatin1String("resistor")) {
                const double r = value("r"), esl = value("esl"), cp = value("cp");
                if (esl <= 0 && cp <= 0)
                    return std::nullopt;
                const QString mid = esl > 0 ? node() : n;
                lines << QStringLiteral("R%1 %2 %3 %4").arg(ref, p, mid, number(r));
                if (esl > 0)
                    lines << QStringLiteral("L%1_esl %2 %3 %4").arg(ref, mid, n, number(esl));
                if (cp > 0)
                    lines << QStringLiteral("C%1_cp %2 %3 %4").arg(ref, p, n, number(cp));
                return lines;
            }
            if (kind == QLatin1String("capacitor")) {
                const double c = value("c"), q = value("q"), fq = value("freq");
                const double rs = (q > 0 && fq > 0 && c > 0 ? 1.0 / (2 * Pi * fq * c * q) : 0.0) + value("esr");
                const double esl = value("esl");
                if (rs <= 0 && esl <= 0)
                    return std::nullopt;
                const QString a = rs > 0 ? node() : p;
                const QString b = esl > 0 ? node() : a;
                lines << QStringLiteral("C%1 %2 %3 %4").arg(ref, b, n, number(c));
                if (rs > 0)
                    lines << QStringLiteral("R%1_esr %2 %3 %4").arg(ref, p, a, number(rs));
                if (esl > 0)
                    lines << QStringLiteral("L%1_esl %2 %3 %4").arg(ref, a, b, number(esl));
                return lines;
            }
            if (kind == QLatin1String("inductor")) {
                const double l = value("l"), q = value("q"), fq = value("freq"), cp = value("cp");
                const double rs = q > 0 && fq > 0 ? 2 * Pi * fq * l / q : 0.0;
                if (rs <= 0 && cp <= 0)
                    return std::nullopt;
                const QString a = rs > 0 ? node() : n;
                lines << QStringLiteral("L%1 %2 %3 %4").arg(ref, p, a, number(l));
                if (rs > 0)
                    lines << QStringLiteral("R%1_q %2 %3 %4").arg(ref, a, n, number(rs));
                if (cp > 0)
                    lines << QStringLiteral("C%1_cp %2 %3 %4").arg(ref, p, n, number(cp));
                return lines;
            }
            deck.errors << QStringLiteral("%1: unknown parasitics expansion \"%2\"").arg(component.path, kind);
            failed = true;
            return QStringList();
        };

        // Text from a Python model is final: its braces are SPICE expressions.
        const bool verbatim = model->data.value(QStringLiteral("verbatim")).toBool();
        const std::optional<QStringList> parasitic = parasiticLines();
        const QStringList rendered = parasitic ? *parasitic
                                               : (verbatim ? templateText : render(templateText))
                                                     .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QStringList lines;
        for (const QString &raw : rendered) {
            const QString line = raw.trimmed();
            if (line.isEmpty())
                continue;
            if (!line.startsWith(QLatin1Char('.')) && !line.startsWith(QLatin1Char('*')) && !line.startsWith(QLatin1Char('+'))) {
                const QString name = line.section(QRegularExpression(QStringLiteral("\\s+")), 0, 0).toLower();
                if (takenElements.contains(name)) {
                    deck.errors << QStringLiteral("%1: element name %2 is used twice").arg(component.path, name);
                    failed = true;
                }
                takenElements.insert(name);
                element.elementNames << name;
                if (options.saveDeviceCurrents)
                    saves << deviceVectors(name);
            }
            lines << line;
        }
        if (!element.elementNames.isEmpty())
            element.name = element.elementNames.first();
        element.instrument = model->data.value(QStringLiteral("instrument")).toString();
        if (templateText.trimmed().isEmpty() && element.probe == ProbeKind::None && element.instrument.isEmpty())
            deck.warnings << QStringLiteral("%1 has an empty SPICE model and is left out.").arg(component.path);

        // Shared text is emitted verbatim: its braces are ngspice parameter
        // expressions ({aol/gbw}), not placeholders.
        const QString shared = model->data.value(QStringLiteral("subcircuit")).toString().trimmed();
        if (!shared.isEmpty() && !sharedTexts.contains(shared))
            sharedTexts << shared;
        const QString include = render(model->data.value(QStringLiteral("include")).toString()).trimmed();
        if (!include.isEmpty()) {
            const QString path = QFileInfo(include).isRelative() ? QDir(projectDirectory).absoluteFilePath(include) : include;
            if (!QFileInfo::exists(path)) {
                deck.errors << QStringLiteral("%1: model file %2 not found").arg(component.path, QDir::toNativeSeparators(path));
                failed = true;
            }
            // A .lib file with sections is read through its section.
            const QString section = model->data.value(QStringLiteral("section")).toString().trimmed();
            const QString card = section.isEmpty()
                                     ? QStringLiteral(".include \"%1\"").arg(QDir::fromNativeSeparators(path))
                                     : QStringLiteral(".lib \"%1\" %2").arg(QDir::fromNativeSeparators(path), section);
            if (!includes.contains(card))
                includes << card;
        }
        if (!failed && !lines.isEmpty())
            elementLines << QStringLiteral("* %1").arg(component.path) << lines;
        deck.elements << element;
    }

    QStringList text;
    text << QStringLiteral("* %1").arg(options.title.isEmpty() ? netlist.topDocument : options.title);
    text << includes;
    for (const QString &shared : std::as_const(sharedTexts))
        text << shared;
    text << elementLines;
    if (options.saveDeviceCurrents && !saves.isEmpty())
        text << QStringLiteral(".save all ") + saves.join(QLatin1Char(' '));
    text << options.cards;
    text << QStringLiteral(".end");
    deck.text = text.join(QLatin1Char('\n')) + QLatin1Char('\n');
    return deck;
}

} // namespace lvd::spice
