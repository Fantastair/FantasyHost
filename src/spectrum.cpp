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

SpectrumFrame analyse(const WaveformViewport &viewport, const QJsonObject &parameters)
{
    SpectrumFrame frame;
    const int channel = parameters.value("spectrumChannel").toInt(1) - 1;
    const int fftSize = parameters.value("fftSize").toString().toInt();
    const qint64 available = viewport.sampleCount(channel);
    const qint64 count = std::min<qint64>(available, fftSize > 0 ? fftSize : maximumFftSize);
    if (count < minimumSamples) {
        frame.status = count == 0 ? QString("CH%1 在当前时窗没有样本").arg(channel + 1)
                                  : QString("当前时窗至少需要 %1 个样本").arg(minimumSamples);
        return frame;
    }
    const auto samples = viewport.readChannel(channel, available - count, count);
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

    frame.decibels = parameters.value("amplitudeScale").toString() == "db";
    frame.samples = length;
    frame.binWidth = rate / size;
    const double maximum = parameters.value("maxFrequency").toDouble();
    frame.frequencyLimit = maximum > 0 ? std::min(maximum, rate / 2) : rate / 2;
    const int lastBin = std::min(size / 2, qFloor(frame.frequencyLimit / frame.binWidth + 1e-10));
    frame.bins.reserve(lastBin + 1);
    for (int bin = 0; bin <= lastBin; ++bin) {
        const double factor = bin == 0 || bin == size / 2 ? 1 : 2;
        const double linear = std::abs(data[bin]) * factor / windowSum;
        const double amplitude = frame.decibels ? 20 * std::log10(std::max(1e-15, linear)) : linear;
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
        if (port == "waveform" && view) {
            const int channel = parameters.value("spectrumChannel").toInt(1) - 1;
            const bool changed = !input || !*view || !input->sameSamples(**view, channel);
            input = *view;
            if (changed)
                refresh();
        }
    }

    void resetInput(const QString &port) override
    {
        if (port == "waveform") {
            input.reset();
            refresh();
        }
    }

private:
    void parametersChanged(const QJsonObject &) override { refresh(); }

    void refresh()
    {
        SpectrumFrame frame;
        if (input)
            frame = analyse(*input, parameters);
        else
            frame.status = "等待视口波形输入";
        io.displaySpectrum(frame);
        io.report(frame.status);
    }

    WaveformPacket input;
};

ParameterDefinition choice(const QString &id, const QString &name, const QString &initial, QList<ParameterChoice> choices)
{
    return {id, name, ParameterKind::Choice, initial, 0, 0, 0, true, false, std::move(choices)};
}

QString frequencyText(double hz)
{
    return QString::number(hz, 'g', 5);
}
}

ModuleDefinition spectrumDefinition()
{
    return {"spectrum", "频谱分析仪", "对波形记录器当前时间窗中的采样进行频谱分析。",
        {{"waveform", "波形", DataType::Waveform}}, {}, {
            {"spectrumChannel", "分析通道", ParameterKind::Number, 1, 1, maxScopeChannels, 0, true, false, {}},
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
        }, nullptr, false, false, false, false, nullptr, true};
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
    const QRectF plot = chart.adjusted(46, 12, -17, -44);
    if (frame.bins.isEmpty()) {
        painter.setPen(colors.muted);
        painter.drawText(chart, Qt::AlignCenter, frame.status.isEmpty() ? "等待视口波形输入" : frame.status);
        painter.restore();
        return;
    }

    const double high = frame.decibels ? std::ceil(frame.peak.y() / 10) * 10 + 5
                                       : (frame.peak.y() > 0 ? frame.peak.y() * 1.08 : 1.0);
    const double low = frame.decibels ? high - 100 : 0;
    const int divisions = std::max(2, qFloor(plot.width() / 72));
    for (int i = 0; i <= divisions; ++i) {
        const double ratio = double(i) / divisions;
        const double x = plot.left() + ratio * plot.width();
        painter.setPen(QPen(colors.border, 0.8));
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        const QString label = frequencyText(frame.frequencyLimit * ratio) + (i == divisions ? " Hz" : "");
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
        return QPointF(plot.left() + bin.x() / frame.frequencyLimit * plot.width(),
                       plot.bottom() - (bin.y() - low) / (high - low) * plot.height());
    };
    QPainterPath trace(position(frame.bins.first()));
    const int columns = std::max(1, qRound(plot.width()));
    const auto column = [&](qsizetype index) { return qFloor(frame.bins.at(index).x() / frame.frequencyLimit * columns); };
    for (qsizetype begin = 0; begin < frame.bins.size();) {
        qsizetype minimum = begin;
        qsizetype maximum = begin;
        qsizetype end = begin + 1;
        const int current = column(begin);
        while (end < frame.bins.size() && column(end) == current) {
            if (frame.bins.at(end).y() < frame.bins.at(minimum).y())
                minimum = end;
            if (frame.bins.at(end).y() > frame.bins.at(maximum).y())
                maximum = end;
            ++end;
        }
        trace.lineTo(position(frame.bins.at(std::min(minimum, maximum))));
        trace.lineTo(position(frame.bins.at(std::max(minimum, maximum))));
        begin = end;
    }
    painter.save();
    painter.setClipRect(plot);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(colors.accent, 1.35));
    painter.drawPath(trace);
    painter.setBrush(colors.accent);
    painter.drawEllipse(position(frame.peak), 2.4, 2.4);
    painter.restore();

    painter.setPen(colors.text);
    const QString peak = QString("峰值 %1 Hz · %2%3").arg(frequencyText(frame.peak.x()),
        QString::number(frame.peak.y(), 'g', 5), frame.decibels ? " dB" : "");
    const QRectF footer = chart.adjusted(10, chart.height() - 21, -10, -4);
    painter.drawText(footer, Qt::AlignLeft | Qt::AlignVCenter, painter.fontMetrics().elidedText(peak, Qt::ElideRight, footer.width()));
    painter.restore();
}
