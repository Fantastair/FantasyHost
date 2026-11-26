#include "filter.h"
#include "waveform.h"

#include <QByteArray>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace {
constexpr double pi = 3.14159265358979323846;
const QString bundledPort = QStringLiteral("waveform");

enum class Mode { LowPass, HighPass, BandPass, BandStop, Bypass };

struct Settings {
    Mode mode;
    double low;
    double high;
    int taps;

    bool band() const { return mode == Mode::BandPass || mode == Mode::BandStop; }
};

Settings readSettings(const QJsonObject &parameters)
{
    const QString type = parameters.value("filterMode").toString();
    const auto mode = type == "highpass" ? Mode::HighPass : type == "bandpass" ? Mode::BandPass
        : type == "bandstop" ? Mode::BandStop : type == "bypass" ? Mode::Bypass : Mode::LowPass;
    const double cutoff = parameters.value("cutoff").toDouble(100);
    const int taps = parameters.value("tapCount").toString().toInt();
    Settings settings{mode, cutoff, cutoff, taps >= 31 && taps <= 511 && taps % 2 == 1 ? taps : 127};
    if (settings.band()) {
        settings.low = parameters.value("bandLow").toDouble(45);
        settings.high = parameters.value("bandHigh").toDouble(55);
    }
    return settings;
}

double lowPass(double cyclesPerSample, int offset)
{
    if (cyclesPerSample >= 0.5)
        return offset == 0 ? 1 : 0;
    if (!offset)
        return 2 * cyclesPerSample;
    return std::sin(2 * pi * cyclesPerSample * offset) / (pi * offset);
}

std::vector<double> kernel(const Settings &settings, double interval)
{
    const double low = std::min(0.5, settings.low * interval);
    const double high = std::min(0.5, settings.high * interval);
    const int half = settings.taps / 2;
    const double reference = settings.mode == Mode::HighPass ? 0.5
        : settings.mode == Mode::BandPass ? (low + high) / 2 : 0;
    std::vector<double> weights(half + 1);
    double gain = 0;
    for (int offset = 0; offset <= half; ++offset) {
        const double impulse = offset == 0 ? 1 : 0;
        const double lower = lowPass(low, offset);
        double weight = lower;
        switch (settings.mode) {
        case Mode::HighPass: weight = impulse - lower; break;
        case Mode::BandPass: weight = lowPass(high, offset) - lower; break;
        case Mode::BandStop: weight = impulse - (lowPass(high, offset) - lower); break;
        case Mode::Bypass:
        case Mode::LowPass: break;
        }
        weight *= 0.54 + 0.46 * std::cos(pi * offset / half);
        weights[offset] = weight;
        gain += weight * (offset == 0 ? 1 : 2 * std::cos(2 * pi * reference * offset));
    }
    if (std::abs(gain) > 1e-12) {
        for (double &weight : weights)
            weight /= gain;
    }
    return weights;
}

double intervalBetween(QPointF left, QPointF right)
{
    const double interval = right.x() - left.x();
    if (!(interval > 0) || !std::isfinite(interval))
        return 0;
    const double rate = 1 / interval;
    const double integerRate = std::round(rate);
    return integerRate >= 1 && std::abs(rate - integerRate) <= rate * 1e-7 ? 1 / integerRate : interval;
}

void filterSegment(const QList<QPointF> &samples, qsizetype start, qsizetype end, qsizetype skip,
                   const Settings &settings, double interval, QList<QPointF> &result)
{
    if (end <= skip)
        return;
    const auto weights = kernel(settings, interval);
    const int half = settings.taps / 2;
    const int preRoll = settings.taps - 1;
    std::vector<double> padded(end - start + preRoll, samples.at(start).y());
    for (qsizetype index = start; index < end; ++index)
        padded[index - start + preRoll] = samples.at(index).y();
    for (qsizetype index = std::max(start, skip); index < end; ++index) {
        const double *center = padded.data() + (index - start + half);
        double value = weights[0] * *center;
        for (int tap = 1; tap <= half; ++tap)
            value += weights[tap] * (center[-tap] + center[tap]);
        result.append({samples.at(index).x(), value});
    }
}

QList<QPointF> filterSamples(const WaveformViewport &input, int channel, qint64 offset, qsizetype count,
                           const Settings &settings, double referenceInterval)
{
    // A finite history makes pagination and downstream tail reads produce the same samples.
    const qsizetype prefix = std::min<qint64>(offset, settings.taps);
    const qsizetype length = count + prefix;
    const auto samples = input.readChannel(channel, offset - prefix, std::max<qsizetype>(2, length));
    if (samples.size() < length)
        return {};
    QList<QPointF> result;
    result.reserve(count);

    qsizetype start = 0;
    double interval = samples.size() > 1 ? intervalBetween(samples.at(0), samples.at(1)) : referenceInterval;
    for (qsizetype index = 2; index < length; ++index) {
        const double next = intervalBetween(samples.at(index - 1), samples.at(index));
        const double tolerance = std::max(interval * 1e-5, std::abs(samples.at(index).x()) * 8 * std::numeric_limits<double>::epsilon());
        if (std::abs(next - interval) > tolerance) {
            filterSegment(samples, start, index, prefix, settings, interval > 0 ? interval : referenceInterval, result);
            start = index;
            interval = next;
        }
    }
    filterSegment(samples, start, length, prefix, settings, interval > 0 ? interval : referenceInterval, result);
    return result;
}

struct Track {
    WaveformViewport::Reader reader;
    QString issue;
};

Track createTrack(const WaveformPacket &input, int source, int displayChannel, const Settings &settings)
{
    const qint64 size = input->sampleCount(source);
    if (!size)
        return {};
    const QString label = QString("CH%1").arg(displayChannel + 1);
    if (size < 2)
        return {{}, label + " 至少需要两个采样"};
    const auto tail = input->readChannel(source, size - 2, 2);
    const double interval = tail.size() == 2 ? intervalBetween(tail.first(), tail.last()) : 0;
    if (!(interval > 0))
        return {{}, label + " 无法确定采样率"};
    if (settings.high * interval >= 0.5)
        return {{}, QString("%1 的截止频率须低于 %2 Hz").arg(label, QString::number(0.5 / interval, 'g', 8))};
    auto reader = std::make_shared<const WaveformViewport::ReadSamples>(
        [input, source, settings, interval](qint64 offset, qsizetype count) {
            return filterSamples(*input, source, offset, count, settings, interval);
        });
    return {std::move(reader), {}};
}

bool separateChannels(const QJsonObject &parameters) { return parameters.value("inputLayout") == "ports"; }

bool streamMode(const QJsonObject &parameters) { return parameters.value("processType").toString() == "stream"; }

int streamChannels(const QJsonObject &parameters)
{
    return std::clamp(parameters.value("channels").toInt(1), 1, maxScopeChannels);
}

double streamRate(const QJsonObject &parameters)
{
    const double rate = parameters.value("sampleRate").toDouble(1000);
    return rate >= 1 ? rate : 1000;
}

bool usableSample(double value) { return std::isfinite(value) && std::abs(value) <= 1e100; }

void appendFloat(QByteArray &buffer, double value)
{
    const float sample = static_cast<float>(value);
    quint32 bits = 0;
    std::memcpy(&bits, &sample, sizeof(bits));
    char bytes[sizeof(bits)];
    qToLittleEndian(bits, bytes);
    buffer.append(bytes, sizeof(bytes));
}

// Causal FIR over a live sample stream. The symmetric kernel used for viewports gives the
// same magnitude response here, delayed by (taps - 1) / 2 samples.
class StreamFilter {
public:
    void configure(const Settings &settings, double rate)
    {
        taps.assign(settings.taps, 0.0);
        if (settings.mode == Mode::Bypass) {
            taps.assign(1, 1.0);
        } else {
            const int half = settings.taps / 2;
            const auto weights = kernel(settings, 1.0 / rate);
            for (int offset = 0; offset <= half; ++offset) {
                taps[half + offset] = weights[offset];
                taps[half - offset] = weights[offset];
            }
        }
        reset();
    }

    void reset()
    {
        for (auto &window : windows)
            window.assign(taps.size(), 0.0);
    }

    bool empty() const { return taps.empty(); }

    double push(int channel, double value)
    {
        std::vector<double> &window = windows[channel];
        if (window.size() > 1)
            std::memmove(window.data() + 1, window.data(), (window.size() - 1) * sizeof(double));
        window[0] = value;
        double sum = 0.0;
        for (size_t tap = 0; tap < taps.size(); ++tap)
            sum += taps[tap] * window[tap];
        return sum;
    }

private:
    std::array<std::vector<double>, maxScopeChannels> windows;
    std::vector<double> taps;
};

class WaveformFilter final : public Module {
public:
    explicit WaveformFilter(ModuleCallbacks callbacks) : Module(std::move(callbacks)) {}

    void receive(const QString &port, const Packet &packet) override
    {
        if (streamMode(parameters)) {
            if (const auto *bytes = std::get_if<QByteArray>(&packet))
                receiveStream(port, *bytes);
            return;
        }
        Slot *slot = inputSlot(port);
        const auto *waveform = std::get_if<WaveformPacket>(&packet);
        if (!slot || !waveform || (separateChannels(parameters) && *waveform && (*waveform)->channelCount() != 1))
            return;
        const auto previous = std::exchange(slot->input, *waveform);
        updateSlot(*slot, port, previous);
        reportStatus();
    }

    void resetInput(const QString &port) override
    {
        if (streamMode(parameters)) {
            stream.reset();
            tail.clear();
            discardLine = false;
            return;
        }
        if (Slot *slot = inputSlot(port)) {
            *slot = {};
            io.publish(port, WaveformPacket{});
            reportStatus();
        }
    }

    std::optional<Packet> currentOutput(const QString &port) const override
    {
        const int index = slotIndex(port);
        if (index >= 0 && slotsByChannel[index].output)
            return Packet{slotsByChannel[index].output};
        return std::nullopt;
    }

private:
    using Frame = std::array<double, maxScopeChannels>;

    struct Slot {
        WaveformPacket input;
        WaveformPacket output;
        std::array<Track, maxScopeChannels> tracks;
    };

    int slotIndex(const QString &port) const
    {
        if (!separateChannels(parameters))
            return port == bundledPort ? 0 : -1;
        const int index = channelPortIndex(port);
        return index < scopeChannels(parameters) ? index : -1;
    }

    Slot *inputSlot(const QString &port)
    {
        const int index = slotIndex(port);
        return index < 0 ? nullptr : &slotsByChannel[index];
    }

    void parametersChanged(const QJsonObject &previous) override
    {
        settings = readSettings(parameters);
        if (streamMode(parameters)) {
            stream.configure(settings, streamRate(parameters));
            tail.clear();
            discardLine = false;
            invalid = 0;
            slotsByChannel = {};
            reportStatus();
            return;
        }
        if (separateChannels(previous) != separateChannels(parameters)) {
            slotsByChannel = {};
        } else {
            const bool separate = separateChannels(parameters);
            const int count = separate ? scopeChannels(parameters) : 1;
            for (int index = count; index < maxScopeChannels; ++index)
                slotsByChannel[index] = {};
            for (int index = 0; index < count; ++index) {
                Slot &slot = slotsByChannel[index];
                if (slot.input)
                    updateSlot(slot, separate ? channelPortId(index) : bundledPort, {});
            }
        }
        reportStatus();
    }

    void updateSlot(Slot &slot, const QString &port, const WaveformPacket &previous)
    {
        slot.output.reset();
        if (!slot.input || settings.mode == Mode::Bypass || (settings.band() && settings.low >= settings.high)) {
            slot.tracks = {};
            if (settings.mode == Mode::Bypass)
                slot.output = slot.input;
        } else {
            std::array<WaveformViewport::Reader, maxScopeChannels> readers;
            const int offset = separateChannels(parameters) ? channelPortIndex(port) : slot.input->firstSourceChannel();
            for (int channel = 0; channel < slot.input->channelCount(); ++channel) {
                if (!previous || !previous->sameSamples(*slot.input, channel))
                    slot.tracks[channel] = createTrack(slot.input, channel, offset + channel, settings);
                readers[channel] = slot.tracks[channel].reader;
            }
            for (int channel = slot.input->channelCount(); channel < maxScopeChannels; ++channel)
                slot.tracks[channel] = {};
            slot.output = slot.input->withChannelReaders(readers);
        }
        io.publish(port, slot.output);
    }

    void receiveStream(const QString &port, const QByteArray &bytes)
    {
        const int count = streamChannels(parameters);
        const bool interleaved = !separateChannels(parameters);
        const int channel = interleaved ? 0 : channelPortIndex(port);
        if (channel < 0 || channel >= count || port != channelPortId(channel))
            return;
        const int width = interleaved ? count : 1;
        QList<Frame> frames;
        decode(bytes, width, frames);
        std::array<QByteArray, maxScopeChannels> output;
        for (const Frame &frame : frames) {
            if (interleaved) {
                for (int index = 0; index < count; ++index)
                    appendFloat(output[0], stream.push(index, frame[index]));
            } else {
                appendFloat(output[channel], stream.push(channel, frame[0]));
            }
        }
        for (int index = 0; index < count; ++index) {
            if (!output[index].isEmpty())
                io.publish(channelPortId(index), output[index]);
        }
        if (invalid != reportedInvalid) {
            reportedInvalid = invalid;
            io.report(QString("已跳过 %1 帧无效数据").arg(invalid));
        }
    }

    void decode(const QByteArray &bytes, int width, QList<Frame> &frames)
    {
        const QString format = parameters.value("encoding").toString();
        if (format == "text") {
            for (const char byte : bytes) {
                if (byte == '\n') {
                    if (!discardLine)
                        parseLine(tail, width, frames);
                    tail.clear();
                    discardLine = false;
                } else if (!discardLine) {
                    if (tail.size() < 256) {
                        tail.append(byte);
                    } else {
                        ++invalid;
                        tail.clear();
                        discardLine = true;
                    }
                }
            }
            return;
        }
        const bool byteFormat = format == "u8";
        const bool floatFormat = format == "f32le";
        const bool unsignedFormat = format == "u16le";
        const int sampleBytes = byteFormat ? 1 : floatFormat ? 4 : 2;
        const int frameBytes = sampleBytes * width;
        const QByteArray block = tail + bytes;
        qsizetype offset = 0;
        for (; offset + frameBytes <= block.size(); offset += frameBytes) {
            Frame frame{};
            for (int index = 0; index < width; ++index) {
                const char *sample = block.constData() + offset + sampleBytes * index;
                if (byteFormat) {
                    frame[index] = static_cast<quint8>(*sample);
                } else if (floatFormat) {
                    const quint32 bits = qFromLittleEndian<quint32>(sample);
                    float value = 0;
                    std::memcpy(&value, &bits, sizeof(value));
                    frame[index] = value;
                } else if (unsignedFormat) {
                    frame[index] = qFromLittleEndian<quint16>(sample);
                } else {
                    frame[index] = qFromLittleEndian<qint16>(sample);
                }
            }
            frames.append(frame);
        }
        tail = block.mid(offset);
    }

    void parseLine(QByteArray line, int width, QList<Frame> &frames)
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
        for (int index = 0; index < width; ++index) {
            bool ok = false;
            frame[index] = fields[index].trimmed().toDouble(&ok);
            if (!ok || !usableSample(frame[index])) {
                ++invalid;
                return;
            }
        }
        frames.append(frame);
    }

    void reportStatus() const
    {
        if (streamMode(parameters)) {
            if (settings.mode == Mode::Bypass) {
                io.report("流式 · 旁路");
                return;
            }
            const double rate = streamRate(parameters);
            if (settings.band() && settings.low >= settings.high) {
                io.report("下限频率必须小于上限频率");
                return;
            }
            if (settings.high * 2 >= rate)
                io.report(QString("截止频率须低于 %1 Hz").arg(QString::number(rate / 2, 'g', 8)));
            else
                io.report(QString("流式 · FIR %1 抽头 · %2 Hz").arg(settings.taps).arg(QString::number(rate, 'g', 8)));
            return;
        }
        if (settings.mode == Mode::Bypass) {
            io.report("旁路 · 原样转发");
            return;
        }
        if (settings.band() && settings.low >= settings.high) {
            io.report("下限频率必须小于上限频率");
            return;
        }
        int active = 0;
        QString issue;
        for (const Slot &slot : slotsByChannel) {
            for (const Track &track : slot.tracks) {
                if (track.reader)
                    ++active;
                else if (issue.isEmpty() && !track.issue.isEmpty())
                    issue = track.issue;
            }
        }
        QString status = active ? QString("%1 路 · FIR %2 抽头").arg(active).arg(settings.taps) : QString("等待波形样本");
        if (!issue.isEmpty())
            status = active ? status + " · " + issue : issue;
        io.report(status);
    }

    Settings settings{Mode::LowPass, 100, 100, 127};
    std::array<Slot, maxScopeChannels> slotsByChannel;
    StreamFilter stream;
    QByteArray tail;
    bool discardLine = false;
    qint64 invalid = 0, reportedInvalid = 0;
};

QList<PortDefinition> filterPorts(const QJsonObject &parameters, bool output)
{
    if (streamMode(parameters)) {
        if (!separateChannels(parameters))
            return {{channelPortId(0), output ? "滤波采样" : "采样", DataType::Bytes}};
        QList<PortDefinition> ports;
        for (int channel = 0; channel < streamChannels(parameters); ++channel)
            ports.append({channelPortId(channel), QString("CH%1").arg(channel + 1), DataType::Bytes});
        return ports;
    }
    if (!separateChannels(parameters))
        return {{bundledPort, output ? "滤波波形" : "波形", DataType::Waveform}};
    QList<PortDefinition> ports;
    for (int channel = 0; channel < scopeChannels(parameters); ++channel)
        ports.append({channelPortId(channel), QString("CH%1").arg(channel + 1), DataType::WaveformChannel});
    return ports;
}
}

ModuleDefinition filterDefinition()
{
    return {"filter", "滤波器", "对接入的采样逐通道滤波。视口模式接在波形记录器和频谱分析仪之间；流式模式接在串口或采集卡与波形记录器之间，输出为 Float32 LE。", {}, {}, {
        {"processType", "处理对象", ParameterKind::Choice, "waveform", 0, 0, 0, true, true,
            {{"waveform", "视口波形"}, {"stream", "流式采样"}}},
        {"inputLayout", "接线布局", ParameterKind::Choice, "frame", 0, 0, 0, true, true,
            {{"frame", "合并多通道"}, {"ports", "逐通道接线"}}},
        {"channels", "通道数", ParameterKind::Number, 2, 1, maxScopeChannels, 0, true, true, {}},
        {"encoding", "样本格式", ParameterKind::Choice, "f32le", 0, 0, 0, true, false,
            {{"text", "按行文本数值"}, {"u8", "UInt8"}, {"i16le", "Int16 LE"},
             {"u16le", "UInt16 LE"}, {"f32le", "Float32 LE"}}},
        {"sampleRate", "采样率 (Hz)", ParameterKind::Number, 1000, 1, 1000000, 0, true, false, {}},
        {"filterMode", "滤波类型", ParameterKind::Choice, "lowpass", 0, 0, 0, true, false,
            {{"lowpass", "低通"}, {"highpass", "高通"}, {"bandpass", "带通"}, {"bandstop", "带阻"}, {"bypass", "旁路"}}},
        {"cutoff", "截止频率 (Hz)", ParameterKind::Number, 100, 0.01, 500000, 2, true, false, {}},
        {"bandLow", "下限频率 (Hz)", ParameterKind::Number, 45, 0.01, 500000, 2, true, false, {}},
        {"bandHigh", "上限频率 (Hz)", ParameterKind::Number, 55, 0.01, 500000, 2, true, false, {}},
        {"tapCount", "FIR 抽头数", ParameterKind::Choice, "127", 0, 0, 0, true, false,
            {{"31", "31"}, {"63", "63"}, {"127", "127"}, {"255", "255"}, {"511", "511"}}},
    }, [](ModuleCallbacks callbacks) -> std::unique_ptr<Module> { return std::make_unique<WaveformFilter>(std::move(callbacks)); },
        [](const QJsonObject &parameters) { return filterPorts(parameters, false); },
        false, false, false, false, [](const QJsonObject &parameters) { return filterPorts(parameters, true); }};
}
