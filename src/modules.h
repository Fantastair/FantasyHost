#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <variant>

enum class DataType { Bytes, Number, Any };
enum class ParameterKind { Number, SerialPort, Choice, TextTemplate };
using Packet = std::variant<QByteArray, double>;

struct PortDefinition {
    QString id;
    QString label;
    DataType type;
    bool operator==(const PortDefinition &other) const { return id == other.id && label == other.label && type == other.type; }
};

struct ParameterChoice {
    QString value;
    QString label;
};

struct ParameterDefinition {
    QString id;
    QString label;
    ParameterKind kind;
    QJsonValue initial;
    double minimum = 0;
    double maximum = 0;
    int decimals = 0;
    bool live = false;
    bool affectsPorts = false;
    QList<ParameterChoice> choices;
};

struct ModuleCallbacks {
    std::function<void(const QString &, const Packet &)> publish;
    std::function<void(const QString &)> report;
    std::function<void(int, const QList<double> &)> samples;
    std::function<void(const QByteArray &)> received;
    std::function<void(const QString &)> fail;
    std::function<void(const QString &)> displayText;
};

class Module {
public:
    virtual ~Module() = default;
    virtual bool open(QString &) { return true; }
    virtual void close() {}
    virtual void receive(const QString &, const Packet &) {}
    virtual void resetInput(const QString &) {}
    virtual bool sendManual(const QString &, QString &error) { error = "此模块不支持手动发送。"; return false; }
    void setParameters(const QJsonObject &values);

protected:
    explicit Module(ModuleCallbacks callbacks) : io(std::move(callbacks)) {}
    virtual void parametersChanged(const QJsonObject &) {}
    QJsonObject parameters;
    ModuleCallbacks io;
};

struct ModuleDefinition {
    QString id;
    QString name;
    QString description;
    QList<PortDefinition> inputs;
    QList<PortDefinition> outputs;
    QList<ParameterDefinition> parameters;
    std::unique_ptr<Module> (*create)(ModuleCallbacks);
    QList<PortDefinition> (*dynamicInputs)(const QJsonObject &) = nullptr;
    bool scope = false;
    bool manualOpen = false;
    bool displaysText = false;
    bool recordsWaveform = false;
};

const QList<ModuleDefinition> &moduleDefinitions();
const ModuleDefinition *moduleDefinition(const QString &id);
QList<PortDefinition> modulePorts(const ModuleDefinition &module, const QJsonObject &parameters, bool output);
std::optional<PortDefinition> portDefinition(const ModuleDefinition &module, const QJsonObject &parameters,
                                             bool output, const QString &id);
const ParameterDefinition *parameterDefinition(const ModuleDefinition &module, const QString &id);
bool acceptsParameter(const ParameterDefinition &parameter, const QJsonValue &value);
std::optional<QByteArray> encodeSerialInput(const QString &text, const QJsonObject &parameters, QString &error);
QString formatSerialOutput(const QByteArray &bytes, bool hex);
