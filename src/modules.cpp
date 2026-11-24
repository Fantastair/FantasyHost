#include "modules.h"

#include "scope.h"
#include "textdisplay.h"

#include <QSerialPort>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace {
constexpr qsizetype maxSerialQueue = 1024 * 1024;
constexpr qsizetype maxTextFrame = 4096;

class SerialTransceiver final : public Module {
public:
    explicit SerialTransceiver(ModuleCallbacks callbacks) : Module(std::move(callbacks))
    {
        QObject::connect(&serial, &QSerialPort::readyRead, &serial, [this] {
            const QByteArray bytes = serial.readAll();
            if (bytes.isEmpty())
                return;
            rx += bytes.size();
            io.received(bytes);
            io.publish("out", bytes);
            reportCounts();
        });
        QObject::connect(&serial, &QSerialPort::bytesWritten, &serial, [this](qint64 count) {
            tx += count;
            reportCounts();
        });
        QObject::connect(&serial, &QSerialPort::errorOccurred, &serial, [this](QSerialPort::SerialPortError code) {
            if (code != QSerialPort::NoError)
                io.fail(serial.errorString());
        });
    }

    bool open(QString &error) override
    {
        const QString name = parameters.value("port").toString().trimmed();
        if (name.isEmpty()) {
            error = "请先在参数面板中选择串口。";
            return false;
        }
        serial.setPortName(name);
        serial.setReadBufferSize(65536);
        rx = tx = 0;
        const auto option = [this](const char *key) { return parameters.value(QLatin1String(key)).toString().toInt(); };
        if (!serial.setBaudRate(parameters.value("baud").toInt())
            || !serial.setDataBits(QSerialPort::DataBits(option("dataBits")))
            || !serial.setParity(QSerialPort::Parity(option("parity")))
            || !serial.setStopBits(QSerialPort::StopBits(option("stopBits")))
            || !serial.setFlowControl(QSerialPort::FlowControl(option("flow")))
            || !serial.open(QIODevice::ReadWrite)) {
            error = QString("无法打开 %1：%2").arg(name, serial.errorString());
            return false;
        }
        reportCounts();
        return true;
    }

    void close() override
    {
        if (serial.isOpen())
            serial.close();
    }

    void receive(const QString &, const Packet &packet) override
    {
        if (const auto *bytes = std::get_if<QByteArray>(&packet)) {
            QString error;
            if (!bytes->isEmpty() && !write(*bytes, error))
                io.fail(error);
        }
    }

    bool sendManual(const QString &text, QString &error) override
    {
        const auto bytes = encodeSerialInput(text, parameters, error);
        return bytes && write(*bytes, error);
    }

private:
    bool write(const QByteArray &bytes, QString &error)
    {
        if (bytes.size() > maxSerialQueue - serial.bytesToWrite()) {
            error = "串口发送队列已满，请降低发送速率。";
            return false;
        }
        const qint64 queued = serial.write(bytes);
        if (queued == bytes.size())
            return true;
        error = queued < 0 ? serial.errorString()
                           : QString("仅将 %1 / %2 字节加入发送队列。").arg(queued).arg(bytes.size());
        return false;
    }

    void reportCounts()
    {
        io.report(QString("%1  ·  收 %2 B  /  发 %3 B").arg(serial.portName()).arg(rx).arg(tx));
    }

    QSerialPort serial;
    qint64 rx = 0;
    qint64 tx = 0;
};

class ScopeInput final : public Module {
public:
    explicit ScopeInput(ModuleCallbacks callbacks) : Module(std::move(callbacks)) {}

    void resetInput(const QString &port) override
    {
        const int channel = port.mid(2).toInt() - 1;
        if (channel >= 0 && channel < maxScopeChannels)
            streams[channel] = {};
    }

    void receive(const QString &port, const Packet &packet) override
    {
        if (!port.startsWith("ch"))
            return;
        const int channel = port.sliced(2).toInt() - 1;
        const int channelCount = scopeChannels(parameters);
        const bool interleaved = parameters.value("layout").toString() == "frame";
        if (channel < 0 || channel >= channelCount || (interleaved && channel != 0))
            return;
        if (const auto *number = std::get_if<double>(&packet)) {
            if (!interleaved && validSample(*number))
                io.samples(channel, {*number});
            return;
        }

        const int valuesPerFrame = interleaved ? channelCount : 1;
        Batches batches;
        Stream &stream = streams[channel];
        const QString format = parameters.value("encoding").toString();
        const qint64 before = invalid;
        const auto &bytes = std::get<QByteArray>(packet);
        if (format == "text")
            readText(stream, bytes, valuesPerFrame, batches);
        else
            readBinary(stream, bytes, format, valuesPerFrame, batches);

        bool receivedSamples = false;
        for (int i = 0; i < valuesPerFrame; ++i) {
            if (!batches[i].isEmpty()) {
                io.samples(channel + i, batches[i]);
                receivedSamples = true;
            }
        }
        if (invalid != before || (receivedSamples && !reportedData)) {
            io.report(invalid ? QString("已跳过 %1 帧无效数据").arg(invalid) : QString("正在采集"));
            reportedData = reportedData || receivedSamples;
        }
    }

private:
    void parametersChanged(const QJsonObject &previous) override
    {
        if (previous.value("layout") != parameters.value("layout")
            || previous.value("channels") != parameters.value("channels")
            || previous.value("encoding") != parameters.value("encoding")) {
            streams = {};
            invalid = 0;
            reportedData = false;
            io.report("等待输入");
        }
    }

    using Batches = std::array<QList<double>, maxScopeChannels>;
    using Frame = std::array<double, maxScopeChannels>;
    struct Stream {
        QByteArray tail;
        bool discardLine = false;
    };

    static bool validSample(double number) { return std::isfinite(number) && std::abs(number) <= 1e100; }

    void append(const Frame &frame, int width, Batches &batches)
    {
        for (int i = 0; i < width; ++i) {
            if (!validSample(frame[i])) {
                ++invalid;
                return;
            }
        }
        for (int i = 0; i < width; ++i)
            batches[i].append(frame[i]);
    }

    void readLine(QByteArray line, int width, Batches &batches)
    {
        line = line.trimmed();
        if (line.isEmpty())
            return;
        line.replace(';', ',');
        const QList<QByteArray> fields = line.contains(',') ? line.split(',') : line.simplified().split(' ');
        if (fields.size() != width) {
            ++invalid;
            return;
        }
        Frame frame{};
        for (int i = 0; i < width; ++i) {
            bool ok = false;
            frame[i] = fields[i].trimmed().toDouble(&ok);
            if (!ok) {
                ++invalid;
                return;
            }
        }
        append(frame, width, batches);
    }

    void readText(Stream &stream, const QByteArray &bytes, int width, Batches &batches)
    {
        for (const char byte : bytes) {
            if (byte == '\n') {
                if (!stream.discardLine)
                    readLine(std::move(stream.tail), width, batches);
                stream.tail.clear();
                stream.discardLine = false;
            } else if (!stream.discardLine) {
                if (stream.tail.size() < maxTextFrame) {
                    stream.tail.append(byte);
                } else {
                    ++invalid;
                    stream.tail.clear();
                    stream.discardLine = true;
                }
            }
        }
    }

    void readBinary(Stream &stream, const QByteArray &bytes, const QString &format, int width, Batches &batches)
    {
        const bool byteFormat = format == "u8";
        const bool floatFormat = format == "f32le";
        const bool unsignedFormat = format == "u16le";
        const int sampleBytes = byteFormat ? 1 : floatFormat ? 4 : 2;
        const int frameBytes = sampleBytes * width;
        const QByteArray block = stream.tail + bytes;
        qsizetype offset = 0;
        for (; offset + frameBytes <= block.size(); offset += frameBytes) {
            Frame frame{};
            for (int i = 0; i < width; ++i) {
                const char *sample = block.constData() + offset + sampleBytes * i;
                if (byteFormat) {
                    frame[i] = static_cast<quint8>(*sample);
                } else if (floatFormat) {
                    const quint32 bits = qFromLittleEndian<quint32>(sample);
                    float value;
                    static_assert(sizeof(value) == sizeof(bits));
                    std::memcpy(&value, &bits, sizeof(value));
                    frame[i] = value;
                } else if (unsignedFormat) {
                    frame[i] = qFromLittleEndian<quint16>(sample);
                } else {
                    frame[i] = qFromLittleEndian<qint16>(sample);
                }
            }
            append(frame, width, batches);
        }
        stream.tail = block.sliced(offset);
    }

    std::array<Stream, maxScopeChannels> streams;
    qint64 invalid = 0;
    bool reportedData = false;
};

QList<PortDefinition> scopeInputs(const QJsonObject &parameters)
{
    if (parameters.value("layout").toString() == "frame")
        return {{"ch1", "多通道帧", DataType::Bytes}};
    QList<PortDefinition> ports;
    for (int i = 1; i <= scopeChannels(parameters); ++i)
        ports.append({QString("ch%1").arg(i), QString("CH%1").arg(i), DataType::Bytes});
    return ports;
}

ParameterDefinition choice(const QString &id, const QString &label, const QString &initial,
                           const QList<ParameterChoice> &choices, bool live = false, bool affectsPorts = false)
{
    return {id, label, ParameterKind::Choice, initial, 0, 0, 0, live, affectsPorts, choices};
}

ParameterDefinition number(const QString &id, const QString &label, double initial, double min, double max,
                           int places = 0, bool live = false, bool affectsPorts = false)
{
    return {id, label, ParameterKind::Number, initial, min, max, places, live, affectsPorts, {}};
}

QList<ParameterDefinition> waveformInputParameters()
{
    return {
        number("channels", "通道数", 1, 1, maxScopeChannels, 0, false, true),
        choice("layout", "输入布局", "ports", {{"ports", "逐通道接线"}, {"frame", "单线多通道帧"}}, false, true),
        choice("encoding", "样本格式", "text", {{"text", "按行文本数值"}, {"u8", "UInt8"}, {"i16le", "Int16 LE"},
                                              {"u16le", "UInt16 LE"}, {"f32le", "Float32 LE"}}),
        number("sampleRate", "采样率 (Hz)", 1000, 1, 1000000, 0, true),
    };
}

template<typename T>
std::unique_ptr<Module> create(ModuleCallbacks io)
{
    return std::make_unique<T>(std::move(io));
}
}

void Module::setParameters(const QJsonObject &values)
{
    if (parameters != values) {
        const QJsonObject previous = parameters;
        parameters = values;
        parametersChanged(previous);
    }
}

const QList<ModuleDefinition> &moduleDefinitions()
{
    static const QList<ParameterChoice> serialFormats{{"text", "文本 (UTF-8)"}, {"hex", "HEX（二进制）"}};
    static const QList<ModuleDefinition> definitions{
        {"serial", "串口收发", "向串口发送字节，并将收到的字节输出到画布。",
         {{"in", "发送", DataType::Bytes}}, {{"out", "接收", DataType::Bytes}}, {
            {"port", "串口", ParameterKind::SerialPort, QString(), 0, 0, 0, false, false, {}},
            number("baud", "波特率", 115200, 50, 4000000),
            choice("dataBits", "数据位", "8", {{"5", "5"}, {"6", "6"}, {"7", "7"}, {"8", "8"}}),
            choice("parity", "校验", "0", {{"0", "无"}, {"2", "偶校验"}, {"3", "奇校验"}, {"5", "标记位"}, {"4", "空格位"}}),
            choice("stopBits", "停止位", "1", {{"1", "1"}, {"3", "1.5"}, {"2", "2"}}),
            choice("flow", "流控", "0", {{"0", "无"}, {"1", "RTS/CTS"}, {"2", "XON/XOFF"}}),
            choice("txFormat", "手动发送格式", "text", serialFormats, true),
            choice("rxFormat", "接收预览格式", "text", serialFormats, true),
            choice("lineEnding", "发送追加", "none", {{"none", "无"}, {"lf", "LF"}, {"crlf", "CRLF"}, {"cr", "CR"}}, true),
        }, &create<SerialTransceiver>, nullptr, false, true},
        {"scope", "示波器", "从串口字节流解码样本，并实时显示多通道波形。", {}, {}, waveformInputParameters() + QList<ParameterDefinition>{
            number("timeBase", "时基 (ms/格)", 10, 0.001, 10000, 3, true),
            choice("verticalMode", "垂直范围", "auto", {{"auto", "自动缩放"}, {"manual", "手动"}}, true),
            number("voltsPerDivision", "垂直刻度 (单位/格)", 1, 0.000001, 1000000000000.0, 6, true),
            number("verticalOffset", "垂直偏移", 0, -1000000000000.0, 1000000000000.0, 6, true),
            choice("triggerMode", "触发模式", "auto", {{"auto", "自动"}, {"normal", "常态"}, {"single", "单次"}}, true),
            number("triggerChannel", "触发通道", 1, 1, maxScopeChannels, 0, true),
            choice("triggerEdge", "触发边沿", "rising", {{"rising", "上升沿"}, {"falling", "下降沿"}}, true),
            number("triggerLevel", "触发电平", 0, -1000000000000.0, 1000000000000.0, 6, true),
        }, &create<ScopeInput>, &scopeInputs, true},
        {"recorder", "波形记录器", "持续记录各通道的波形，可回看和导出。", {}, {}, waveformInputParameters(),
         &create<ScopeInput>, &scopeInputs, false, false, false, true},
        textDisplayDefinition(),
    };
    return definitions;
}

const ModuleDefinition *moduleDefinition(const QString &id)
{
    for (const ModuleDefinition &definition : moduleDefinitions()) {
        if (definition.id == id)
            return &definition;
    }
    return nullptr;
}

QList<PortDefinition> modulePorts(const ModuleDefinition &module, const QJsonObject &parameters, bool output)
{
    if (!output && module.dynamicInputs)
        return module.dynamicInputs(parameters);
    return output ? module.outputs : module.inputs;
}

std::optional<PortDefinition> portDefinition(const ModuleDefinition &module, const QJsonObject &parameters,
                                             bool output, const QString &id)
{
    const auto ports = modulePorts(module, parameters, output);
    for (const PortDefinition &port : ports) {
        if (port.id == id)
            return port;
    }
    return std::nullopt;
}

const ParameterDefinition *parameterDefinition(const ModuleDefinition &module, const QString &id)
{
    for (const ParameterDefinition &parameter : module.parameters) {
        if (parameter.id == id)
            return &parameter;
    }
    return nullptr;
}

bool acceptsParameter(const ParameterDefinition &parameter, const QJsonValue &value)
{
    if (parameter.kind == ParameterKind::Choice) {
        return value.isString() && std::any_of(parameter.choices.cbegin(), parameter.choices.cend(),
                                               [&value](const auto &item) { return item.value == value.toString(); });
    }
    if (parameter.kind == ParameterKind::SerialPort)
        return value.isString();
    if (parameter.kind == ParameterKind::TextTemplate)
        return value.isString() && value.toString().size() <= parameter.maximum;
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    return std::isfinite(number) && number >= parameter.minimum && number <= parameter.maximum
        && (parameter.decimals != 0 || std::trunc(number) == number);
}

std::optional<QByteArray> encodeSerialInput(const QString &text, const QJsonObject &parameters, QString &error)
{
    QByteArray bytes;
    if (parameters.value("txFormat").toString() == "hex") {
        QByteArray digits;
        for (const QChar character : text) {
            if (character.isSpace())
                continue;
            const auto ascii = character.unicode();
            if (!((ascii >= '0' && ascii <= '9') || (ascii >= 'a' && ascii <= 'f') || (ascii >= 'A' && ascii <= 'F'))) {
                error = "HEX 只接受十六进制数字和空白，例如 01 A0 FF。";
                return std::nullopt;
            }
            digits.append(static_cast<char>(ascii));
        }
        if (digits.size() % 2 != 0) {
            error = "HEX 必须每两个数字组成一个字节。";
            return std::nullopt;
        }
        bytes = QByteArray::fromHex(digits);
    } else {
        bytes = text.toUtf8();
    }
    const QString ending = parameters.value("lineEnding").toString();
    if (ending == "cr" || ending == "crlf")
        bytes.append('\r');
    if (ending == "lf" || ending == "crlf")
        bytes.append('\n');
    if (bytes.isEmpty() || bytes.size() > maxSerialQueue) {
        error = bytes.isEmpty() ? "请输入要发送的内容。" : "单次手动发送至多 1 MiB。";
        return std::nullopt;
    }
    return bytes;
}

QString formatSerialOutput(const QByteArray &bytes, bool hex)
{
    if (!hex)
        return QString::fromUtf8(bytes);
    QByteArray lines;
    for (qsizetype offset = 0; offset < bytes.size(); offset += 16) {
        if (offset)
            lines.append('\n');
        lines += bytes.mid(offset, 16).toHex(' ').toUpper();
    }
    return QString::fromLatin1(lines);
}
