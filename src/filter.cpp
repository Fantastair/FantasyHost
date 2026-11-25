#include "filter.h"
#include "waveform.h"

#include <algorithm>
#include <array>
#include <cmath>
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

class WaveformFilter final : public Module {
public:
    explicit WaveformFilter(ModuleCallbacks callbacks) : Module(std::move(callbacks)) {}

    void receive(const QString &port, const Packet &packet) override
    {
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

    void reportStatus() const
    {
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
};

QList<PortDefinition> filterPorts(const QJsonObject &parameters, bool output)
{
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
    return {"filter", "滤波器", "接在波形记录器（视口输出）和频谱分析仪之间，对当前视口的波形逐通道滤波并输出处理后的采样。", {}, {}, {
        {"inputLayout", "接线布局", ParameterKind::Choice, "frame", 0, 0, 0, true, true,
            {{"frame", "合并多通道"}, {"ports", "逐通道接线"}}},
        {"channels", "通道数", ParameterKind::Number, 2, 1, maxScopeChannels, 0, true, true, {}},
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
