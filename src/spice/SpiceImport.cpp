#include "spice/SpiceImport.h"

#include "core/Document.h"
#include "core/Project.h"
#include "core/Schematic.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

namespace lvd::spice {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("lvd::spice::import", text);
}

// Splits a card into tokens; "a = b" and "a=b" both become "a=b", and the
// parentheses of a .model's parameter list are separated from the type.
QStringList tokens(const QString &line)
{
    QString text = line;
    text.replace(QRegularExpression(QStringLiteral("\\s*=\\s*")), QStringLiteral("="));
    return text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

QString identifierFrom(const QString &text)
{
    QString id;
    for (const QChar c : text.toLower()) {
        if ((c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('0') && c <= QLatin1Char('9')))
            id += c;
        else if (!id.endsWith(QLatin1Char('_')))
            id += QLatin1Char('_');
    }
    while (id.endsWith(QLatin1Char('_')))
        id.chop(1);
    while (id.startsWith(QLatin1Char('_')))
        id.remove(0, 1);
    return id.isEmpty() ? QStringLiteral("model") : id;
}

bool isIdentifier(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    return pattern.match(text).hasMatch();
}

// Pins of a subcircuit that read as inputs go on the left of the box.
bool looksLikeInput(const QString &pin)
{
    const QString p = pin.toLower();
    static const QStringList inputs{QStringLiteral("in"),  QStringLiteral("inp"),  QStringLiteral("inn"),
                                    QStringLiteral("+in"), QStringLiteral("-in"),  QStringLiteral("in+"),
                                    QStringLiteral("in-"), QStringLiteral("clk"),  QStringLiteral("en"),
                                    QStringLiteral("adj"), QStringLiteral("fb"),   QStringLiteral("ctrl"),
                                    QStringLiteral("a"),   QStringLiteral("b"),    QStringLiteral("d"),
                                    QStringLiteral("vin"), QStringLiteral("inv"),  QStringLiteral("noninv"),
                                    QStringLiteral("set"), QStringLiteral("reset"), QStringLiteral("shdn")};
    return inputs.contains(p) || p.startsWith(QLatin1String("in")) || p.endsWith(QLatin1String("in"));
}

} // namespace

QList<QPair<int, QString>> logicalSpiceLines(const QString &text)
{
    QList<QPair<int, QString>> lines;
    const QStringList raw = text.split(QLatin1Char('\n'));
    for (int i = 0; i < raw.size(); ++i) {
        QString line = raw.at(i);
        line.remove(QLatin1Char('\r'));
        // Inline comments: "; ..." anywhere, " $ ..." after white space.
        if (const qsizetype semicolon = line.indexOf(QLatin1Char(';')); semicolon >= 0)
            line.truncate(semicolon);
        static const QRegularExpression dollar(QStringLiteral("\\s\\$.*$"));
        line.remove(dollar);
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('*')))
            continue;
        if (trimmed.startsWith(QLatin1Char('+'))) {
            if (!lines.isEmpty())
                lines.last().second += QLatin1Char(' ') + trimmed.mid(1).trimmed();
            continue;
        }
        lines.append({i + 1, trimmed});
    }
    return lines;
}

SpiceModelFile parseSpiceModels(const QString &text)
{
    SpiceModelFile file;
    const QString upper = text.toUpper();
    if (upper.contains(QLatin1String("$CDNENCSTART")) || upper.contains(QLatin1String(".PROTECT"))
        || upper.contains(QLatin1String("ENCRYPTED"))) {
        file.errors << tr("The file holds encrypted models, which cannot be simulated.");
        return file;
    }
    QString section;
    int depth = 0;
    int subcircuitLine = 0;
    for (const auto &[lineNumber, line] : logicalSpiceLines(text)) {
        const QStringList t = tokens(line);
        if (t.isEmpty())
            continue;
        const QString card = t.first().toLower();
        if (card == QLatin1String(".lib") && t.size() == 2 && !t.at(1).contains(QLatin1Char('"'))
            && !t.at(1).contains(QLatin1Char('.')) && depth == 0) {
            section = t.at(1);
            file.sections << section;
        } else if (card == QLatin1String(".endl")) {
            section.clear();
        } else if (card == QLatin1String(".subckt")) {
            if (t.size() < 2) {
                file.warnings << tr("Line %1: .subckt without a name").arg(lineNumber);
                continue;
            }
            if (depth == 0) {
                SpiceModelEntry entry;
                entry.kind = SpiceModelEntry::Kind::Subcircuit;
                entry.name = t.at(1);
                entry.section = section;
                entry.line = lineNumber;
                bool inParams = false;
                for (qsizetype i = 2; i < t.size(); ++i) {
                    const QString token = t.at(i);
                    if (token.compare(QStringLiteral("params:"), Qt::CaseInsensitive) == 0) {
                        inParams = true;
                        continue;
                    }
                    if (token.startsWith(QLatin1String("params:"), Qt::CaseInsensitive)) {
                        inParams = true;
                        const QString rest = token.mid(7);
                        if (rest.contains(QLatin1Char('=')))
                            entry.parameters.insert(rest.section(QLatin1Char('='), 0, 0), rest.section(QLatin1Char('='), 1));
                        continue;
                    }
                    if (token.contains(QLatin1Char('='))) {
                        inParams = true;
                        entry.parameters.insert(token.section(QLatin1Char('='), 0, 0), token.section(QLatin1Char('='), 1));
                        continue;
                    }
                    if (!inParams)
                        entry.pins << token;
                }
                file.entries << entry;
                subcircuitLine = lineNumber;
            }
            ++depth;
        } else if (card == QLatin1String(".ends")) {
            if (depth == 0)
                file.warnings << tr("Line %1: .ends without .subckt").arg(lineNumber);
            depth = std::max(0, depth - 1);
        } else if (card == QLatin1String(".model") && depth == 0) {
            if (t.size() < 3) {
                file.warnings << tr("Line %1: incomplete .model").arg(lineNumber);
                continue;
            }
            SpiceModelEntry entry;
            entry.kind = SpiceModelEntry::Kind::Model;
            entry.name = t.at(1);
            entry.type = t.at(2).section(QLatin1Char('('), 0, 0).toUpper();
            entry.section = section;
            entry.line = lineNumber;
            // A p-channel VDMOS is a VDMOS model with the pchan flag.
            if (entry.type == QLatin1String("VDMOS") && line.contains(QLatin1String("pchan"), Qt::CaseInsensitive))
                entry.type = QStringLiteral("VDMOS-P");
            file.entries << entry;
        }
    }
    if (depth > 0)
        file.errors << tr("The subcircuit starting on line %1 has no .ends.").arg(subcircuitLine);
    return file;
}

std::optional<SpiceModelFile> readSpiceModelFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return std::nullopt;
    }
    // Vendor files are ASCII or Latin-1 more often than UTF-8.
    const QByteArray bytes = file.readAll();
    QString text = QString::fromUtf8(bytes);
    if (text.contains(QChar::ReplacementCharacter))
        text = QString::fromLatin1(bytes);
    return parseSpiceModels(text);
}

ComponentDefinition proposeComponent(const SpiceModelEntry &entry, const QString &includePath,
                                     const QMap<QString, ComponentDefinition> &templates)
{
    ComponentDefinition definition;
    definition.id = identifierFrom(entry.name);
    definition.name = entry.name;
    definition.description = tr("Imported from %1").arg(QFileInfo(includePath).fileName());

    QJsonObject model{{QStringLiteral("kind"), QStringLiteral("spice")}, {QStringLiteral("include"), includePath}};
    if (!entry.section.isEmpty())
        model.insert(QStringLiteral("section"), entry.section);

    // Device models reuse the symbol of the matching generic part; the pins
    // keep that part's names and order.
    struct Device {
        const char *letter;
        const char *category;
        QStringList pins;
        const char *extra; // appended nodes (MOSFET body tied to the source)
    };
    static const QMap<QString, Device> devices{
        {QStringLiteral("D"), {"D", "Diodes", {QStringLiteral("a"), QStringLiteral("k")}, ""}},
        {QStringLiteral("NPN"), {"Q", "Transistors/BJT", {QStringLiteral("c"), QStringLiteral("b"), QStringLiteral("e")}, ""}},
        {QStringLiteral("PNP"), {"Q", "Transistors/BJT", {QStringLiteral("c"), QStringLiteral("b"), QStringLiteral("e")}, ""}},
        {QStringLiteral("NMOS"), {"M", "Transistors/MOSFET", {QStringLiteral("d"), QStringLiteral("g"), QStringLiteral("s")}, " {pin:s}"}},
        {QStringLiteral("PMOS"), {"M", "Transistors/MOSFET", {QStringLiteral("d"), QStringLiteral("g"), QStringLiteral("s")}, " {pin:s}"}},
        {QStringLiteral("VDMOS"), {"M", "Transistors/MOSFET", {QStringLiteral("d"), QStringLiteral("g"), QStringLiteral("s")}, ""}},
        {QStringLiteral("VDMOS-P"), {"M", "Transistors/MOSFET", {QStringLiteral("d"), QStringLiteral("g"), QStringLiteral("s")}, ""}},
        {QStringLiteral("NJF"), {"J", "Transistors/JFET", {QStringLiteral("d"), QStringLiteral("g"), QStringLiteral("s")}, ""}},
        {QStringLiteral("PJF"), {"J", "Transistors/JFET", {QStringLiteral("d"), QStringLiteral("g"), QStringLiteral("s")}, ""}},
    };

    if (entry.kind == SpiceModelEntry::Kind::Model && devices.contains(entry.type)) {
        const Device &device = devices.value(entry.type);
        const QString letter = QString::fromLatin1(device.letter);
        QString templateKey = entry.type;
        if (templateKey == QLatin1String("VDMOS"))
            templateKey = QStringLiteral("NMOS");
        else if (templateKey == QLatin1String("VDMOS-P"))
            templateKey = QStringLiteral("PMOS");
        if (const auto found = templates.constFind(templateKey); found != templates.cend()) {
            definition.symbol = found->symbol;
            definition.pins = found->pins;
            definition.referencePrefix = found->referencePrefix;
        } else {
            definition.pins = device.pins;
            definition.referencePrefix = letter;
        }
        definition.category = QStringLiteral("Imported/") + QString::fromLatin1(device.category);
        QString line = letter + QStringLiteral("{ref}");
        for (const QString &pin : device.pins)
            line += QStringLiteral(" {pin:%1}").arg(pin);
        line += QString::fromLatin1(device.extra) + QLatin1Char(' ') + entry.name;
        model.insert(QStringLiteral("template"), line);
        definition.models << ModelDefinition{QStringLiteral("spice"), model};
        // Without a template symbol the pins have no drawing; a box follows.
        if (!definition.symbol.pins.isEmpty())
            return definition;
    }

    // Subcircuits (and models of other types): a box with one pin per node.
    QStringList pins = entry.kind == SpiceModelEntry::Kind::Subcircuit ? entry.pins : definition.pins;
    if (pins.isEmpty())
        pins = {QStringLiteral("p"), QStringLiteral("n")};
    definition.pins = pins;
    if (definition.referencePrefix.isEmpty())
        definition.referencePrefix = QStringLiteral("U");
    if (definition.category.isEmpty())
        definition.category = entry.kind == SpiceModelEntry::Kind::Subcircuit ? QStringLiteral("Imported/Subcircuits")
                                                                              : QStringLiteral("Imported/Models");
    QStringList left, right;
    for (const QString &pin : pins)
        (looksLikeInput(pin) ? left : right) << pin;
    if (left.isEmpty() && right.size() > 1) {
        // No recognisable inputs: split evenly in pin order.
        const qsizetype half = (right.size() + 1) / 2;
        left = right.mid(0, half);
        right = right.mid(half);
    }
    const int rows = int(std::max(left.size(), right.size()));
    const int halfHeight = 10 * rows + 10;
    const int top = -((halfHeight + 9) / 10) * 10;
    const int bottom = top + 20 * rows + 20;
    Symbol symbol;
    SymbolGraphic box;
    box.type = SymbolGraphic::Type::Rect;
    box.from = QPoint(-40, top);
    box.to = QPoint(40, bottom);
    box.fill = Fill::Background;
    symbol.graphics << box;
    SymbolGraphic title;
    title.type = SymbolGraphic::Type::Text;
    title.at = QPoint(0, top - 4);
    title.text = entry.name;
    title.size = 9;
    title.anchor = TextAnchor::Center;
    symbol.graphics << title;
    const auto placePins = [&](const QStringList &names, bool onLeft) {
        for (int i = 0; i < names.size(); ++i) {
            const int y = top + 20 + 20 * i;
            SymbolPin pin;
            pin.pin = names.at(i);
            pin.at = QPoint(onLeft ? -60 : 60, y);
            pin.length = 20;
            pin.direction = onLeft ? PinDirection::Right : PinDirection::Left;
            symbol.pins << pin;
            SymbolGraphic label;
            label.type = SymbolGraphic::Type::Text;
            label.at = QPoint(onLeft ? -36 : 36, y + 3);
            label.text = names.at(i);
            label.size = 7;
            label.anchor = onLeft ? TextAnchor::Left : TextAnchor::Right;
            symbol.graphics << label;
        }
    };
    placePins(left, true);
    placePins(right, false);
    symbol.referenceAt = QPoint(0, bottom + 12);
    symbol.valueAt = QPoint(0, bottom + 24);
    definition.symbol = symbol;

    if (entry.kind == SpiceModelEntry::Kind::Subcircuit) {
        QString line = QStringLiteral("X{ref}");
        for (const QString &pin : pins)
            line += QStringLiteral(" {pin:%1}").arg(pin);
        line += QLatin1Char(' ') + entry.name;
        QStringList assignments;
        for (auto it = entry.parameters.begin(); it != entry.parameters.end(); ++it) {
            if (!isIdentifier(it.key()))
                continue;
            ParameterDefinition parameter;
            parameter.name = it.key().toLower();
            parameter.label = it.key();
            QString value = it.value();
            if (value.startsWith(QLatin1Char('{')) && value.endsWith(QLatin1Char('}')))
                value = value.mid(1, value.size() - 2);
            parameter.defaultValue = value;
            parameter.isText = true; // SPICE syntax, passed through
            definition.parameters << parameter;
            assignments << QStringLiteral("%1={%2}").arg(it.key(), parameter.name);
        }
        if (!assignments.isEmpty())
            line += QStringLiteral(" params: ") + assignments.join(QLatin1Char(' '));
        model.insert(QStringLiteral("template"), line);
        definition.models = {ModelDefinition{QStringLiteral("spice"), model}};
    } else if (definition.models.isEmpty()) {
        // A model of a type without a generic symbol: the user writes the
        // element line; a two-node placeholder is proposed.
        model.insert(QStringLiteral("template"), QStringLiteral("X{ref} {pin:p} {pin:n} %1").arg(entry.name));
        definition.models << ModelDefinition{QStringLiteral("spice"), model};
    }
    return definition;
}

std::optional<double> parseSpiceNumber(const QString &text)
{
    static const QRegularExpression pattern(
        QStringLiteral("^([+-]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][+-]?\\d+)?)(meg|mil|[tgkmunpf])?[a-z]*$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(text.trimmed());
    if (!match.hasMatch())
        return std::nullopt;
    double value = match.captured(1).toDouble();
    const QString suffix = match.captured(2).toLower();
    if (suffix == QLatin1String("t"))
        value *= 1e12;
    else if (suffix == QLatin1String("g"))
        value *= 1e9;
    else if (suffix == QLatin1String("meg"))
        value *= 1e6;
    else if (suffix == QLatin1String("k"))
        value *= 1e3;
    else if (suffix == QLatin1String("mil"))
        value *= 25.4e-6;
    else if (suffix == QLatin1String("m"))
        value *= 1e-3;
    else if (suffix == QLatin1String("u"))
        value *= 1e-6;
    else if (suffix == QLatin1String("n"))
        value *= 1e-9;
    else if (suffix == QLatin1String("p"))
        value *= 1e-12;
    else if (suffix == QLatin1String("f"))
        value *= 1e-15;
    return value;
}

ImportedNetlist importSpiceNetlist(const QString &text, const QString &baseDirectory)
{
    ImportedNetlist imported;
    QStringList rawLines = text.split(QLatin1Char('\n'));
    if (!rawLines.isEmpty()) {
        imported.title = rawLines.first().trimmed();
        if (imported.title.startsWith(QLatin1Char('*')))
            imported.title = imported.title.mid(1).trimmed();
        rawLines.removeFirst(); // the first line of a SPICE deck is always the title
    }
    // A SPICE value as expression text for an analysis field.
    const auto value = [](const QString &token) {
        const auto number = parseSpiceNumber(token);
        return number ? formatSpiceNumber(*number) : token;
    };
    QSet<QString> nodes;
    QStringList circuit;
    qsizetype transientIndex = -1; // the list may reallocate: an index, not a pointer
    QList<QPair<QString, QStringList>> fourierCards; // fundamental, signals
    bool inControl = false;
    // Raw lines are kept for the circuit (continuations and comments as
    // written); logical lines are parsed.
    const QString body = rawLines.join(QLatin1Char('\n'));
    for (const auto &[lineNumber, line] : logicalSpiceLines(body)) {
        Q_UNUSED(lineNumber);
        const QStringList t = tokens(line);
        const QString card = t.first().toLower();
        if (inControl) {
            if (card == QLatin1String(".endc"))
                inControl = false;
            continue;
        }
        if (card == QLatin1String(".control")) {
            inControl = true;
            imported.warnings << tr("A .control block was left out; analyses run from the analysis list.");
            continue;
        }
        if (card == QLatin1String(".end"))
            break;
        const auto addAnalysis = [&](AnalysisKind kind) -> SpiceAnalysisSetup & {
            SpiceAnalysisSetup setup = SpiceAnalysisSetup::create(kind);
            setup.id = QStringLiteral("a%1").arg(imported.analyses.size() + 1);
            setup.data = QJsonObject{};
            imported.analyses << setup;
            return imported.analyses.last();
        };
        if (card == QLatin1String(".op")) {
            addAnalysis(AnalysisKind::OperatingPoint);
            continue;
        }
        if (card == QLatin1String(".tran") && t.size() >= 3) {
            SpiceAnalysisSetup &s = addAnalysis(AnalysisKind::Transient);
            s.set(QStringLiteral("step"), value(t.at(1)));
            s.set(QStringLiteral("stop"), value(t.at(2)));
            int next = 3;
            if (t.size() > next && t.at(next).compare(QStringLiteral("uic"), Qt::CaseInsensitive) != 0)
                s.set(QStringLiteral("start"), value(t.at(next++)));
            if (t.size() > next && t.at(next).compare(QStringLiteral("uic"), Qt::CaseInsensitive) != 0)
                s.set(QStringLiteral("maxStep"), value(t.at(next++)));
            s.set(QStringLiteral("uic"), line.contains(QRegularExpression(QStringLiteral("\\buic\\b"), QRegularExpression::CaseInsensitiveOption)));
            transientIndex = imported.analyses.size() - 1;
            continue;
        }
        if (card == QLatin1String(".ac") && t.size() >= 5) {
            SpiceAnalysisSetup &s = addAnalysis(AnalysisKind::Ac);
            s.set(QStringLiteral("sweep"), t.at(1).toLower());
            s.set(QStringLiteral("points"), value(t.at(2)));
            s.set(QStringLiteral("start"), value(t.at(3)));
            s.set(QStringLiteral("stop"), value(t.at(4)));
            continue;
        }
        if (card == QLatin1String(".dc") && t.size() >= 5) {
            SpiceAnalysisSetup &s = addAnalysis(AnalysisKind::DcSweep);
            s.set(QStringLiteral("source"), t.at(1));
            s.set(QStringLiteral("start"), value(t.at(2)));
            s.set(QStringLiteral("stop"), value(t.at(3)));
            s.set(QStringLiteral("step"), value(t.at(4)));
            if (t.size() >= 9) {
                s.set(QStringLiteral("source2"), t.at(5));
                s.set(QStringLiteral("start2"), value(t.at(6)));
                s.set(QStringLiteral("stop2"), value(t.at(7)));
                s.set(QStringLiteral("step2"), value(t.at(8)));
            }
            continue;
        }
        if (card == QLatin1String(".noise") && t.size() >= 7) {
            SpiceAnalysisSetup &s = addAnalysis(AnalysisKind::Noise);
            s.set(QStringLiteral("output"), t.at(1));
            s.set(QStringLiteral("source"), t.at(2));
            s.set(QStringLiteral("sweep"), t.at(3).toLower());
            s.set(QStringLiteral("points"), value(t.at(4)));
            s.set(QStringLiteral("start"), value(t.at(5)));
            s.set(QStringLiteral("stop"), value(t.at(6)));
            continue;
        }
        if (card == QLatin1String(".tf") && t.size() >= 3) {
            SpiceAnalysisSetup &s = addAnalysis(AnalysisKind::TransferFunction);
            s.set(QStringLiteral("output"), t.at(1));
            s.set(QStringLiteral("source"), t.at(2));
            continue;
        }
        if (card == QLatin1String(".pz") && t.size() >= 7) {
            SpiceAnalysisSetup &s = addAnalysis(AnalysisKind::PoleZero);
            s.set(QStringLiteral("inputPositive"), t.at(1));
            s.set(QStringLiteral("inputNegative"), t.at(2));
            s.set(QStringLiteral("outputPositive"), t.at(3));
            s.set(QStringLiteral("outputNegative"), t.at(4));
            s.set(QStringLiteral("transfer"), t.at(5).toLower() == QLatin1String("cur") ? QStringLiteral("current") : QStringLiteral("voltage"));
            const QString find = t.at(6).toLower();
            s.set(QStringLiteral("find"), find == QLatin1String("pol")   ? QStringLiteral("poles")
                                          : find == QLatin1String("zer") ? QStringLiteral("zeros")
                                                                         : QStringLiteral("both"));
            continue;
        }
        if (card == QLatin1String(".sens") && t.size() >= 2) {
            SpiceAnalysisSetup &s = addAnalysis(AnalysisKind::Sensitivity);
            s.set(QStringLiteral("output"), t.at(1));
            s.set(QStringLiteral("ac"), t.size() >= 7 && t.at(2).toLower() == QLatin1String("ac"));
            if (t.size() >= 7) {
                s.set(QStringLiteral("sweep"), t.at(3).toLower());
                s.set(QStringLiteral("points"), value(t.at(4)));
                s.set(QStringLiteral("start"), value(t.at(5)));
                s.set(QStringLiteral("stop"), value(t.at(6)));
            }
            continue;
        }
        if (card == QLatin1String(".four") && t.size() >= 3) {
            fourierCards.append({value(t.at(1)), t.mid(2)});
            continue;
        }
        if (card == QLatin1String(".print") || card == QLatin1String(".plot") || card == QLatin1String(".probe")
            || card == QLatin1String(".width") || card == QLatin1String(".meas") || card == QLatin1String(".measure")) {
            imported.warnings << tr("%1 was left out.").arg(t.first());
            continue;
        }
        if ((card == QLatin1String(".include") || card == QLatin1String(".inc") || card == QLatin1String(".lib"))
            && t.size() >= 2 && !baseDirectory.isEmpty()) {
            QString file = t.at(1);
            file.remove(QLatin1Char('"'));
            file.remove(QLatin1Char('\''));
            if (QFileInfo(file).isRelative() && (card != QLatin1String(".lib") || t.size() >= 3 || file.contains(QLatin1Char('.')))) {
                QStringList rewritten = t;
                rewritten[1] = QStringLiteral("\"%1\"").arg(QDir::fromNativeSeparators(QDir(baseDirectory).absoluteFilePath(file)));
                circuit << rewritten.join(QLatin1Char(' '));
                continue;
            }
        }

        // Element lines: collect nodes for the analysis fields.
        if (!card.startsWith(QLatin1Char('.'))) {
            imported.elements << t.first();
            const QChar letter = card.at(0);
            int count = 0;
            switch (letter.unicode()) {
            case 'r': case 'c': case 'l': case 'v': case 'i': case 'd': case 'f': case 'h': case 'b': case 'w':
                count = 2;
                break;
            case 'q': case 'j':
                count = 3;
                break;
            case 'm': case 'e': case 'g': case 's': case 't': case 'o':
                count = 4;
                break;
            case 'x': {
                // Nodes are the tokens before the subcircuit name, which is
                // the last token before parameters.
                qsizetype end = t.size();
                for (qsizetype i = 1; i < t.size(); ++i)
                    if (t.at(i).contains(QLatin1Char('=')) || t.at(i).compare(QStringLiteral("params:"), Qt::CaseInsensitive) == 0) {
                        end = i;
                        break;
                    }
                count = int(end) - 2;
                break;
            }
            default:
                break;
            }
            for (int i = 1; i <= count && i < t.size(); ++i) {
                const QString node = t.at(i).toLower();
                if (node != QLatin1String("0") && node != QLatin1String("gnd") && !node.contains(QLatin1Char('=')))
                    nodes.insert(node);
            }
        }
        circuit << line;
    }
    // .four turns the transient into a Fourier analysis.
    if (!fourierCards.isEmpty()) {
        if (transientIndex >= 0) {
            SpiceAnalysisSetup *transient = &imported.analyses[transientIndex];
            transient->kind = AnalysisKind::Fourier;
            transient->name = analysisDisplayName(AnalysisKind::Fourier);
            transient->set(QStringLiteral("fundamental"), fourierCards.first().first);
            QJsonArray outputsArray;
            for (const auto &[fundamental, outputs] : std::as_const(fourierCards))
                for (const QString &output : outputs)
                    outputsArray.append(output);
            transient->set(QStringLiteral("signals"), outputsArray);
        } else {
            imported.warnings << tr(".four needs a .tran analysis; it was left out.");
        }
    }
    for (SpiceAnalysisSetup &setup : imported.analyses)
        if (setup.name.isEmpty())
            setup.name = analysisDisplayName(setup.kind);
    imported.circuit = circuit.join(QLatin1Char('\n'));
    imported.nodes = QStringList(nodes.begin(), nodes.end());
    imported.nodes.sort();
    return imported;
}

SpiceDeck deckFromNetlist(const QString &circuit, const QString &title)
{
    SpiceDeck deck;
    const ImportedNetlist parsed = importSpiceNetlist(QStringLiteral("* title\n") + circuit);
    deck.nodeOfNet.insert(QStringLiteral("0"), QStringLiteral("0"));
    for (const QString &node : parsed.nodes) {
        deck.nodeOfNet.insert(node, node);
        deck.netOfNode.insert(node, node);
    }
    for (const QString &name : parsed.elements) {
        SpiceElement element;
        element.path = name;
        element.name = name.toLower();
        element.elementNames << element.name;
        deck.elements << element;
    }
    deck.text = QStringLiteral("* %1\n%2\n.end\n").arg(title, circuit);
    return deck;
}

} // namespace lvd::spice

namespace lvd::spice {

Document *importNetlistDocument(Project &project, const QString &path, const QString &name, QString *error, QStringList *warnings)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return nullptr;
    }
    const QByteArray bytes = file.readAll();
    QString text = QString::fromUtf8(bytes);
    if (text.contains(QChar::ReplacementCharacter))
        text = QString::fromLatin1(bytes);
    const ImportedNetlist imported = importSpiceNetlist(text, QFileInfo(path).absolutePath());
    if (imported.circuit.trimmed().isEmpty()) {
        if (error)
            *error = tr("%1 holds no circuit.").arg(QFileInfo(path).fileName());
        return nullptr;
    }
    const QString documentName = name.isEmpty() ? QFileInfo(path).completeBaseName() : name;
    Document *document = project.createDocument(DocumentType::SpiceSchematic, documentName, error);
    if (!document)
        return nullptr;
    QJsonObject simulation{{QStringLiteral("netlist"), imported.circuit},
                           {QStringLiteral("netlistSource"), QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath())}};
    for (const SpiceAnalysisSetup &setup : imported.analyses)
        simulation = withSpiceAnalysis(simulation, setup);
    document->schematic()->setSimulation(simulation);
    if (!imported.title.isEmpty())
        document->schematic()->addText(imported.title, QPoint(0, 0), 14);
    if (warnings)
        *warnings = imported.warnings;
    return document;
}

} // namespace lvd::spice
