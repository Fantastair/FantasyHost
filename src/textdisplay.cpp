#include "textdisplay.h"

#include "modules.h"

#include <QHash>
#include <QSet>
#include <QStringConverter>
#include <algorithm>
#include <iterator>
#include <map>

namespace {
constexpr qsizetype maxTemplateLength = 16 * 1024;
constexpr qsizetype maxDisplayLength = 32 * 1024;
const QString templateKey = QStringLiteral("template");

class TextTemplate {
public:
    explicit TextTemplate(QStringView source = {})
    {
        QSet<QString> seen;
        QString literal;
        for (qsizetype i = 0; i < source.size();) {
            const QChar ch = source[i];
            if ((ch == '{' || ch == '}') && i + 1 < source.size() && source[i + 1] == ch) {
                literal += ch;
                i += 2;
                continue;
            }
            if (ch == '{') {
                qsizetype end = i + 1;
                while (end < source.size() && source[end] != '{' && source[end] != '}' && source[end] != '\n' && source[end] != '\r')
                    ++end;
                if (end < source.size() && source[end] == '}') {
                    const QStringView candidate = source.sliced(i + 1, end - i - 1).trimmed();
                    if (isName(candidate)) {
                        if (!literal.isEmpty()) {
                            parts.append({std::move(literal), {}});
                            literal.clear();
                        }
                        const QString name = candidate.toString();
                        if (!seen.contains(name)) {
                            seen.insert(name);
                            names.append(name);
                        }
                        parts.append({source.sliced(i, end - i + 1).toString(), name});
                        i = end + 1;
                        continue;
                    }
                }
            }
            literal += ch;
            ++i;
        }
        if (!literal.isEmpty())
            parts.append({std::move(literal), {}});
    }

    const QList<QString> &placeholders() const { return names; }

    QString render(const QHash<QString, QString> &values) const
    {
        QString result;
        for (const Part &part : parts) {
            const auto value = part.name.isEmpty() ? values.cend() : values.constFind(part.name);
            const QString &segment = value == values.cend() ? part.text : value.value();
            const qsizetype room = maxDisplayLength - result.size();
            if (segment.size() <= room) {
                result += segment;
                continue;
            }
            result += QStringView(segment).first(room);
            result.chop(1);
            if (!result.isEmpty() && result.back().isHighSurrogate())
                result.chop(1);
            result += QChar(0x2026);
            break;
        }
        return result;
    }

private:
    static bool isName(QStringView name)
    {
        return !name.isEmpty() && std::all_of(name.cbegin(), name.cend(), [](QChar ch) {
            return ch.isLetterOrNumber() || ch == '_' || ch == '.' || ch == '-';
        });
    }

    struct Part { QString text; QString name; };
    QList<Part> parts;
    QList<QString> names;
};

class TextDisplay final : public Module {
public:
    explicit TextDisplay(ModuleCallbacks callbacks) : Module(std::move(callbacks)) {}

    void receive(const QString &port, const Packet &packet) override
    {
        if (!ports.contains(port))
            return;
        QString value;
        if (const auto *bytes = std::get_if<QByteArray>(&packet)) {
            QStringDecoder &decoder = decoders.try_emplace(port, QStringDecoder::Utf8).first->second;
            value = decoder(*bytes);
            if (value.isEmpty() && !bytes->isEmpty())
                return;
        } else {
            decoders.erase(port);
            value = QString::number(std::get<double>(packet), 'g', 15);
        }
        if (value.size() > maxDisplayLength + 1)
            value.truncate(maxDisplayLength + 1);
        const auto previous = values.constFind(port);
        if (previous == values.cend() || previous.value() != value) {
            values.insert(port, std::move(value));
            publishText();
        }
    }

    void resetInput(const QString &port) override
    {
        decoders.erase(port);
        if (values.remove(port))
            publishText();
    }

private:
    void parametersChanged(const QJsonObject &) override
    {
        const QString text = parameters.value(templateKey).toString();
        format = TextTemplate(text);
        const auto &names = format.placeholders();
        ports = QSet<QString>(names.cbegin(), names.cend());
        for (auto it = values.begin(); it != values.end();)
            it = ports.contains(it.key()) ? std::next(it) : values.erase(it);
        for (auto it = decoders.begin(); it != decoders.end();)
            it = ports.contains(it->first) ? std::next(it) : decoders.erase(it);
        publishText();
    }

    void publishText() const { io.displayText(format.render(values)); }

    TextTemplate format;
    QSet<QString> ports;
    QHash<QString, QString> values;
    std::map<QString, QStringDecoder> decoders;
};

QList<PortDefinition> textInputs(const QJsonObject &parameters)
{
    const QString source = parameters.value(templateKey).toString();
    const TextTemplate format(source);
    QList<PortDefinition> inputs;
    for (const QString &name : format.placeholders())
        inputs.append({name, name, DataType::Any});
    return inputs;
}
}

ModuleDefinition textDisplayDefinition()
{
    return {"text", "文本显示", "使用 {name} 创建输入端口并在卡片上显示收到的数据。", {}, {}, {
        {templateKey, "文本模板", ParameterKind::TextTemplate, "{value}", 0, maxTemplateLength, 0, true, true, {}},
    }, [](ModuleCallbacks callbacks) -> std::unique_ptr<Module> {
        return std::make_unique<TextDisplay>(std::move(callbacks));
    }, &textInputs, false, false, true};
}
