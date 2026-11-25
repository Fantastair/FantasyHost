#include "spectrum.h"
#include "scope.h"
#include "theme.h"
#include "waveform.h"

#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numeric>
#include <vector>

namespace {
constexpr double pi = 3.14159265358979323846;
constexpr int minimumSamples = 8;
constexpr int maximumFftSize = 65536;

void fft(std::vector<std::complex<double>> &data)
{
    const int count = int(data.size());
    for (int i = 1, j = 0; i < count; ++i) {
        int bit = count >> 1;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
        if (i < j)
            std::swap(data[i], data[j]);
    }
    for (int length = 2; length <= count; length *= 2) {
        const auto step = std::polar(1.0, -2 * pi / length);
        for (int start = 0; start < count; start += length) {
            std::complex<double> phase(1);
            for (int i = 0; i < length / 2; ++i) {
                const int left = start + i;
                const int right = left + length / 2;
                const auto even = data[left];
                const auto odd = data[right] * phase;
                data[left] = even + odd;
                data[right] = even - odd;
                phase *= step;
            }
        }
    }
}

double windowWeight(const QString &window, double cosine)
{
    if (window == "rectangular")
        return 1;
    if (window == "hamming")
        return 0.54 - 0.46 * cosine;
    if (window == "blackman")
        return 0.42 - 0.5 * cosine + 0.08 * (2 * cosine * cosine - 1);
    return 0.5 - 0.5 * cosine;
}

SpectrumTrace analyse(const WaveformViewport &viewport, int sourceChannel, int channel, const QJsonObject &parameters)
{
    SpectrumTrace frame;
    frame.channel = channel;
    const int fftSize = parameters.value("fftSize").toString().toInt();
    const qint64 available = viewport.sampleCount(sourceChannel);
    const qint64 count = std::min<qint64>(available, fftSize > 0 ? fftSize : maximumFftSize);
    if (count < minimumSamples) {
        frame.status = count == 0 ? QString("CH%1 在当前时窗没有样本").arg(channel + 1)
                                  : QString("当前时窗至少需要 %1 个样本").arg(minimumSamples);
        return frame;
    }
    const auto samples = viewport.readChannel(sourceChannel, available - count, count);
    if (samples.size() != count) {
        frame.status = "无法读取波形样本";
        return frame;
    }

    // Use the latest contiguous sampling interval if the recorder's rate changed.
    const double lastTime = samples.last().x();
    const double interval = lastTime - samples.at(count - 2).x();
    const double tolerance = std::max(interval * 1e-5, std::abs(lastTime) * 8 * std::numeric_limits<double>::epsilon());
    qsizetype first = count - 2;
    while (first > 0 && std::abs(samples.at(first).x() - samples.at(first - 1).x() - interval) <= tolerance)
        --first;
    const qsizetype length = count - first;
    if (!(interval > 0) || length < minimumSamples) {
        frame.status = "当前时窗内等间隔样本不足";
        return frame;
    }
    const double rate = (length - 1) / (lastTime - samples.at(first).x());
    int size = fftSize > 0 ? fftSize : 1;
    while (size < length)
        size *= 2;

    double mean = 0;
    if (parameters.value("removeDc").toString() != "off") {
        mean = std::accumulate(samples.cbegin() + first, samples.cend(), 0.0,
                               [](double sum, QPointF sample) { return sum + sample.y(); }) / length;
    }
    const QString window = parameters.value("fftWindow").toString();
    std::vector<std::complex<double>> data(size);
    double windowSum = 0;
    for (qsizetype i = 0; i < length; ++i) {
        const double weight = windowWeight(window, std::cos(2 * pi * i / length));
        data[i] = (samples.at(first + i).y() - mean) * weight;
        windowSum += weight;
    }
    fft(data);

    const bool decibels = parameters.value("amplitudeScale").toString() == "db";
    frame.samples = length;
    frame.binWidth = rate / size;
    const double maximum = parameters.value("maxFrequency").toDouble();
    frame.frequencyLimit = maximum > 0 ? std::min(maximum, rate / 2) : rate / 2;
    const int lastBin = std::min(size / 2, qFloor(frame.frequencyLimit / frame.binWidth + 1e-10));
    frame.bins.reserve(lastBin + 1);
    for (int bin = 0; bin <= lastBin; ++bin) {
        const double factor = bin == 0 || bin == size / 2 ? 1 : 2;
        const double linear = std::abs(data[bin]) * factor / windowSum;
        const double amplitude = decibels ? 20 * std::log10(std::max(1e-15, linear)) : linear;
        const QPointF point(bin * frame.binWidth, amplitude);
        if (bin == 0 || point.y() > frame.peak.y())
            frame.peak = point;
        frame.bins.append(point);
    }
    frame.status = QString("CH%1 · %2 样本 · %3 点 FFT · %4 Hz/格")
        .arg(channel + 1).arg(length).arg(size).arg(QString::number(frame.binWidth, 'g', 5));
    return frame;
}

class SpectrumAnalyser final : public Module {
public:
    explicit SpectrumAnalyser(ModuleCallbacks callbacks) : Module(std::move(callbacks)) {}

    void receive(const QString &port, const Packet &packet) override
    {
        const auto *view = std::get_if<WaveformPacket>(&packet);
        if (!view)
            return;
        if (usesIndividualPorts()) {
            const int channel = channelPortIndex(port);
            if (channel >= 0 && channel < scopeChannels(parameters) && (!*view || (*view)->channelCount() == 1)
                && setInput(channel, *view, 0)) {
                refresh();
            }
        } else if (port == "waveform") {
            const int first = *view ? (*view)->firstSourceChannel() : 0;
            const int count = *view ? (*view)->channelCount() : 0;
            bool changed = false;
            for (int channel = 0; channel < maxScopeChannels; ++channel) {
                const int source = channel - first;
                changed |= setInput(channel, source >= 0 && source < count ? *view : WaveformPacket{}, source);
            }
            if (changed)
                refresh();
        }
    }

    void resetInput(const QString &port) override
    {
        if (port == "waveform") {
            inputs = {};
            refresh();
        } else if (const int channel = channelPortIndex(port); channel >= 0 && setInput(channel, {}, 0)) {
            refresh();
        }
    }

private:
    struct Input {
        WaveformPacket viewport;
        int sourceChannel = 0;
        std::optional<SpectrumTrace> spectrum;
    };

    bool usesIndividualPorts() const { return parameters.value("inputLayout") == "ports"; }

    bool setInput(int channel, const WaveformPacket &viewport, int source)
    {
        Input &input = inputs[channel];
        const bool changed = bool(input.viewport) != bool(viewport)
            || (viewport && (input.sourceChannel != source || !input.viewport->sameSamples(*viewport, source)));
        input.viewport = viewport;
        input.sourceChannel = source;
        if (changed)
            input.spectrum.reset();
        return changed;
    }

    void parametersChanged(const QJsonObject &previous) override
    {
        if (previous.value("inputLayout") != parameters.value("inputLayout")) {
            inputs = {};
        } else {
            if (usesIndividualPorts()) {
                for (int channel = scopeChannels(parameters); channel < maxScopeChannels; ++channel)
                    inputs[channel] = {};
            }
            for (const QString key : {"fftSize", "fftWindow", "amplitudeScale", "removeDc", "maxFrequency"}) {
                if (previous.value(key) != parameters.value(key)) {
                    for (Input &input : inputs)
                        input.spectrum.reset();
                    break;
                }
            }
        }
        refresh();
    }

    void refresh()
    {
        SpectrumFrame frame;
        frame.decibels = parameters.value("amplitudeScale") == "db";
        const int selected = parameters.value("spectrumChannel").toInt() - 1;
        int connected = 0;
        QString firstIssue;
        for (int channel = 0; channel < maxScopeChannels; ++channel) {
            Input &input = inputs[channel];
            if (!input.viewport || (selected >= 0 && selected != channel))
                continue;
            ++connected;
            if (!input.spectrum)
                input.spectrum = analyse(*input.viewport, input.sourceChannel, channel, parameters);
            if (!input.spectrum->bins.isEmpty())
                frame.traces.append(*input.spectrum);
            else if (firstIssue.isEmpty())
                firstIssue = input.spectrum->status;
        }
        if (connected > 1)
            frame.status = QString("已连接 %1 路 · %2 路有频谱").arg(connected).arg(frame.traces.size());
        else if (!frame.traces.isEmpty())
            frame.status = frame.traces.first().status;
        else if (!firstIssue.isEmpty())
            frame.status = firstIssue;
        else if (selected >= 0)
            frame.status = QString("CH%1 等待输入").arg(selected + 1);
        else
            frame.status = "等待视口波形输入";
        io.displaySpectrum(frame);
        io.report(frame.status);
    }

    std::array<Input, maxScopeChannels> inputs;
};

QList<PortDefinition> spectrumInputs(const QJsonObject &parameters)
{
    if (parameters.value("inputLayout") != "ports")
        return {{"waveform", "波形", DataType::Waveform}};
    QList<PortDefinition> inputs;
    for (int channel = 0; channel < scopeChannels(parameters); ++channel)
        inputs.append({channelPortId(channel), QString("CH%1").arg(channel + 1), DataType::WaveformChannel});
    return inputs;
}

ParameterDefinition choice(const QString &id, const QString &name, const QString &initial, QList<ParameterChoice> choices,
                           bool affectsPorts = false)
{
    return {id, name, ParameterKind::Choice, initial, 0, 0, 0, true, affectsPorts, std::move(choices)};
}

QString frequencyText(double hz)
{
    return QString::number(hz, 'g', 5);
}
}

ModuleDefinition spectrumDefinition()
{
    return {"spectrum", "频谱分析仪", "对波形记录器当前时间窗中的采样进行频谱分析。",
        {}, {}, {
            choice("inputLayout", "输入布局", "frame", {{"frame", "合并多通道"}, {"ports", "逐通道接线"}}, true),
            {"channels", "独立输入数", ParameterKind::Number, 2, 1, maxScopeChannels, 0, true, true, {}},
            {"spectrumChannel", "显示通道 (0 = 全部)", ParameterKind::Number, 0, 0, maxScopeChannels, 0, true, false, {}},
            choice("fftSize", "FFT 点数", "auto", {{"auto", "自动"}, {"256", "256"}, {"512", "512"},
                {"1024", "1024"}, {"2048", "2048"}, {"4096", "4096"}, {"8192", "8192"}, {"16384", "16384"},
                {"32768", "32768"}, {"65536", "65536"}}),
            choice("fftWindow", "窗函数", "hann", {{"hann", "Hann"}, {"hamming", "Hamming"},
                {"blackman", "Blackman"}, {"rectangular", "矩形"}}),
            choice("amplitudeScale", "幅值刻度", "linear", {{"linear", "线性"}, {"db", "dB（参考幅值 1）"}}),
            choice("removeDc", "去除直流", "on", {{"on", "开启"}, {"off", "关闭"}}),
            {"maxFrequency", "频率上限 (Hz)", ParameterKind::Number, 0, 0, 500000, 2, true, false, {}},
        }, [](ModuleCallbacks callbacks) -> std::unique_ptr<Module> {
            return std::make_unique<SpectrumAnalyser>(std::move(callbacks));
        }, &spectrumInputs, false, false, false, false, nullptr, true};
}

void paintSpectrum(QPainter &painter, const QRectF &chart, const SpectrumFrame &frame, const ThemeColors &colors)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(colors.plot);
    painter.drawRoundedRect(chart, 7, 7);
    QFont font = painter.font();
    font.setPixelSize(10);
    font.setWeight(QFont::Normal);
    painter.setFont(font);
    if (frame.traces.isEmpty()) {
        painter.setPen(colors.muted);
        painter.drawText(chart, Qt::AlignCenter, frame.status.isEmpty() ? "等待视口波形输入" : frame.status);
        painter.restore();
        return;
    }

    double frequencyLimit = 0;
    double peak = -std::numeric_limits<double>::infinity();
    for (const SpectrumTrace &trace : frame.traces) {
        frequencyLimit = std::max(frequencyLimit, trace.frequencyLimit);
        peak = std::max(peak, trace.peak.y());
    }
    const int channels = int(frame.traces.size());
    const int legendColumns = std::min(channels, std::max(2, qFloor((chart.width() - 20) / 108)));
    const int legendRows = (channels + legendColumns - 1) / legendColumns;
    constexpr qreal legendRowHeight = 15;
    const QRectF plot = chart.adjusted(46, 12, -17, -44 - (legendRows - 1) * legendRowHeight);
    const double high = frame.decibels ? std::ceil(peak / 10) * 10 + 5 : (peak > 0 ? peak * 1.08 : 1.0);
    const double low = frame.decibels ? high - 100 : 0;
    const int divisions = std::max(2, qFloor(plot.width() / 72));
    for (int i = 0; i <= divisions; ++i) {
        const double ratio = double(i) / divisions;
        const double x = plot.left() + ratio * plot.width();
        painter.setPen(QPen(colors.border, 0.8));
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        const QString label = frequencyText(frequencyLimit * ratio) + (i == divisions ? " Hz" : "");
        const qreal half = painter.fontMetrics().horizontalAdvance(label) / 2.0;
        const qreal center = std::min(x, chart.right() - half - 4);
        painter.setPen(colors.muted);
        painter.drawText(QRectF(center - 36, plot.bottom() + 4, 72, 15), Qt::AlignCenter, label);
    }
    for (int i = 0; i <= 4; ++i) {
        const double ratio = i / 4.0;
        const double y = plot.bottom() - ratio * plot.height();
        painter.setPen(QPen(colors.border, 0.8));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(colors.muted);
        painter.drawText(QRectF(chart.left(), y - 8, 41, 16), Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(low + ratio * (high - low), 'g', 4));
    }
    const auto position = [&](QPointF bin) {
        return QPointF(plot.left() + bin.x() / frequencyLimit * plot.width(),
                       plot.bottom() - (bin.y() - low) / (high - low) * plot.height());
    };
    painter.save();
    painter.setClipRect(plot);
    for (const SpectrumTrace &spectrum : frame.traces) {
        const auto &bins = spectrum.bins;
        QPainterPath trace(position(bins.first()));
        const int columns = std::max(1, qRound(plot.width()));
        const auto column = [&](qsizetype index) { return qFloor(bins.at(index).x() / frequencyLimit * columns); };
        for (qsizetype begin = 0; begin < bins.size();) {
            qsizetype minimum = begin;
            qsizetype maximum = begin;
            qsizetype end = begin + 1;
            const int current = column(begin);
            while (end < bins.size() && column(end) == current) {
                if (bins.at(end).y() < bins.at(minimum).y())
                    minimum = end;
                if (bins.at(end).y() > bins.at(maximum).y())
                    maximum = end;
                ++end;
            }
            trace.lineTo(position(bins.at(std::min(minimum, maximum))));
            if (minimum != maximum)
                trace.lineTo(position(bins.at(std::max(minimum, maximum))));
            begin = end;
        }
        const QColor color = waveformChannelColor(spectrum.channel, colors);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(color, 1.35));
        painter.drawPath(trace);
        painter.setBrush(color);
        painter.drawEllipse(position(spectrum.peak), 2.4, 2.4);
    }
    painter.restore();

    const qreal cellWidth = (chart.width() - 20) / legendColumns;
    for (int i = 0; i < channels; ++i) {
        const auto &spectrum = frame.traces.at(i);
        const QRectF cell(chart.left() + 10 + (i % legendColumns) * cellWidth,
                          plot.bottom() + 23 + (i / legendColumns) * legendRowHeight, cellWidth - 4, legendRowHeight);
        QString text = QString("CH%1  %2 Hz").arg(spectrum.channel + 1).arg(frequencyText(spectrum.peak.x()));
        if (channels == 1)
            text += QString(" · %1%2").arg(QString::number(spectrum.peak.y(), 'g', 5), frame.decibels ? " dB" : "");
        painter.setPen(waveformChannelColor(spectrum.channel, colors));
        painter.drawText(cell, Qt::AlignLeft | Qt::AlignVCenter, painter.fontMetrics().elidedText(text, Qt::ElideRight, cell.width()));
    }
    painter.restore();
}
