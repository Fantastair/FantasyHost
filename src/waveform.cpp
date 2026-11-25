#include "waveform.h"
#include "theme.h"

#include <QDir>
#include <QImage>
#include <QImageWriter>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>

bool WaveformRecording::append(int channel, const QList<double> &values, int sampleRate, QString &error)
{
    if (channel < 0 || channel >= int(tracks.size()) || sampleRate <= 0 || values.isEmpty())
        return true;
    Series &series = tracks[channel];
    QList<Sample> points;
    points.reserve(values.size());
    const double step = 1.0 / sampleRate;
    for (double value : values) {
        if (std::isfinite(value))
            points.append({series.nextTime + points.size() * step, value});
    }
    if (points.isEmpty())
        return true;
    if (!series.file) {
        auto file = std::shared_ptr<QTemporaryFile>(new QTemporaryFile(QDir::temp().filePath("FantasyHost-waveform-XXXXXX")),
            [](QTemporaryFile *storage) {
                if (storage->thread() == QThread::currentThread())
                    delete storage;
                else
                    storage->deleteLater();
            });
        if (!file->open()) {
            error = file->errorString();
            return false;
        }
        series.file = std::move(file);
    }
    const qint64 offset = series.samples * sizeof(Sample);
    const qint64 size = points.size() * sizeof(Sample);
    if (!series.file->seek(offset) || series.file->write(reinterpret_cast<const char *>(points.constData()), size) != size
        || !series.file->flush()) {
        error = series.file->errorString();
        return false;
    }

    for (const Sample &sample : points) {
        if (series.blocks.isEmpty() || series.blocks.last().count == blockSize) {
            series.blocks.append({qint64(series.samples * sizeof(Sample)), 0, sample.time, sample.time, sample, sample});
        }
        Block &block = series.blocks.last();
        ++block.count;
        block.lastTime = sample.time;
        if (sample.value < block.minimum.value) block.minimum = sample;
        if (sample.value > block.maximum.value) block.maximum = sample;
        if (!series.samples)
            series.minimum = series.maximum = sample.value;
        else {
            series.minimum = std::min(series.minimum, sample.value);
            series.maximum = std::max(series.maximum, sample.value);
        }
        ++series.samples;
    }
    series.nextTime += points.size() * step;
    return true;
}

void WaveformRecording::clear()
{
    // A CSV export already in progress retains its own reference to these files.
    tracks = {};
}

WaveformView WaveformRecording::bounds() const
{
    double time = 0;
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -minimum;
    for (const Series &series : tracks) {
        if (series.samples) {
            time = std::max(time, series.samples > 1 ? series.blocks.last().lastTime : series.nextTime);
            minimum = std::min(minimum, series.minimum);
            maximum = std::max(maximum, series.maximum);
        }
    }
    if (!std::isfinite(minimum))
        return {};
    const double pad = minimum == maximum ? std::max(1e-9, std::abs(minimum) * 0.05)
                                         : std::max((maximum - minimum) * 0.04, 1e-12);
    return {{0, time}, {minimum - pad, maximum + pad}};
}

qint64 WaveformRecording::sampleCount() const
{
    qint64 total = 0;
    for (const Series &series : tracks)
        total += series.samples;
    return total;
}

int WaveformRecording::channelCount() const
{
    for (int k = int(tracks.size()); k > 0; --k) {
        if (tracks[k - 1].samples)
            return k;
    }
    return 0;
}

QByteArray WaveformRecording::readBlock(const Series &series, const Block &block) const
{
    if (!series.file->seek(block.offset))
        return {};
    return series.file->read(block.count * sizeof(Sample));
}

QList<QPointF> WaveformRecording::trace(int channel, WaveformRange time, int columns) const
{
    QList<QPointF> result;
    if (channel < 0 || channel >= int(tracks.size()))
        return result;
    const Series &series = tracks.at(channel);
    if (!series.samples || !(time.span() > 0) || columns < 1)
        return result;

    constexpr int noColumn = -2;
    int column = noColumn;
    Sample minimum{}, maximum{};
    const auto flush = [&] {
        if (minimum.time < maximum.time) {
            result.append({minimum.time, minimum.value});
            result.append({maximum.time, maximum.value});
        } else {
            result.append({maximum.time, maximum.value});
            if (maximum.time != minimum.time)
                result.append({minimum.time, minimum.value});
        }
    };
    const auto put = [&](const Sample &sample) {
        const int pixel = int(std::clamp((sample.time - time.low) / time.span() * columns, -1.0, double(columns)));
        if (pixel != column) {
            if (column != noColumn)
                flush();
            column = pixel;
            minimum = maximum = sample;
        } else {
            if (sample.value < minimum.value) minimum = sample;
            if (sample.value > maximum.value) maximum = sample;
        }
    };
    std::optional<Sample> before;
    const auto addVisible = [&](const Sample &sample) {
        if (before) {
            put(*before);
            before.reset();
        }
        put(sample);
    };

    const auto first = std::lower_bound(series.blocks.cbegin(), series.blocks.cend(), time.low,
                                        [](const Block &block, double start) { return block.lastTime < start; });
    for (auto it = first == series.blocks.cbegin() ? first : std::prev(first); it != series.blocks.cend(); ++it) {
        const Block &block = *it;
        if (block.firstTime >= time.low && block.lastTime <= time.high
            && (block.lastTime - block.firstTime) / time.span() * columns < 1 && block.count > 2) {
            const bool ordered = block.minimum.time <= block.maximum.time;
            addVisible(ordered ? block.minimum : block.maximum);
            addVisible(ordered ? block.maximum : block.minimum);
            continue;
        }
        const QByteArray bytes = readBlock(series, block);
        for (qsizetype offset = 0; offset + qsizetype(sizeof(Sample)) <= bytes.size(); offset += sizeof(Sample)) {
            Sample sample;
            std::memcpy(&sample, bytes.constData() + offset, sizeof(Sample));
            if (sample.time < time.low) {
                before = sample;
                continue;
            }
            addVisible(sample);
            if (sample.time > time.high)
                break;
        }
        if (block.lastTime > time.high)
            break;
    }
    if (column != noColumn)
        flush();
    return result;
}

WaveformCsvSnapshot WaveformRecording::csvSnapshot() const
{
    WaveformCsvSnapshot result;
    for (int channel = 0; channel < int(tracks.size()); ++channel) {
        const Series &series = tracks[channel];
        if (series.samples)
            result.channels[channel] = {series.file, series.file->fileName(), series.samples};
    }
    return result;
}

bool WaveformCsvSnapshot::save(const QString &path, QString &error) const
{
    using Sample = WaveformRecording::Sample;
    QSaveFile file(path);
    const QByteArray header("time_s,channel,value\n");
    if (!file.open(QIODevice::WriteOnly) || file.write(header) != header.size()) {
        error = file.errorString();
        return false;
    }
    for (int channel = 0; channel < int(channels.size()); ++channel) {
        const Channel &series = channels[channel];
        if (!series.samples)
            continue;
        QFile source(series.path);
        if (!source.open(QIODevice::ReadOnly)) {
            error = source.errorString();
            return false;
        }
        const QByteArray label = ",CH" + QByteArray::number(channel + 1) + ',';
        for (qint64 remaining = series.samples; remaining > 0;) {
            if (QThread::currentThread()->isInterruptionRequested()) {
                error = "CSV 导出已取消。";
                return false;
            }
            const qint64 count = std::min<qint64>(remaining, 4096);
            const auto byteCount = count * qint64(sizeof(Sample));
            const QByteArray bytes = source.read(byteCount);
            if (bytes.size() != byteCount) {
                error = "无法读取暂存的波形数据。";
                return false;
            }
            QByteArray rows;
            rows.reserve(count * 45);
            for (qsizetype offset = 0; offset < bytes.size(); offset += sizeof(Sample)) {
                Sample sample;
                std::memcpy(&sample, bytes.constData() + offset, sizeof(Sample));
                rows += QByteArray::number(sample.time, 'g', 17) + label + QByteArray::number(sample.value, 'g', 17) + '\n';
            }
            if (file.write(rows) != rows.size()) {
                error = file.errorString();
                return false;
            }
            remaining -= count;
        }
    }
    if (!file.commit()) {
        error = file.errorString();
        return false;
    }
    return true;
}

void adjustWaveformView(WaveformView &view, const WaveformView &bounds, WaveformGesture gesture, double steps, QPointF anchor)
{
    if (steps == 0 || !std::isfinite(steps))
        return;
    const bool horizontal = gesture == WaveformGesture::ZoomTime || gesture == WaveformGesture::PanTime;
    const bool zoom = gesture == WaveformGesture::ZoomTime || gesture == WaveformGesture::ZoomValue;
    WaveformRange &axis = horizontal ? view.time : view.value;
    const WaveformRange limit = horizontal ? bounds.time : bounds.value;
    const double ratio = std::clamp(horizontal ? anchor.x() : anchor.y(), 0.0, 1.0);
    const double span = zoom ? axis.span() * std::exp2(-steps * 0.28) : axis.span();
    if (span >= limit.span()) {
        axis = limit;
    } else if (span > 0) {
        const double wanted = zoom ? axis.low + ratio * (axis.span() - span) : axis.low + span * steps * 0.12;
        const double low = std::clamp(wanted, limit.low, std::max(limit.low, limit.high - span));
        const double high = std::min(limit.high, low + span);
        if (high > low)
            axis = {low, high};
    }
}

QRectF waveformPlotRect(const QRectF &chart, int lanes, int lane)
{
    if (lanes == 1)
        return chart.adjusted(61, 12, -16, -48);

    constexpr qreal gap = 12;
    const QRectF plots = chart.adjusted(96, 12, -16, -28);
    const qreal height = (plots.height() - (lanes - 1) * gap) / lanes;
    return {plots.left(), plots.top() + lane * (height + gap), plots.width(), height};
}

QPointF waveformAnchor(const QRectF &chart, int lanes, QPointF position)
{
    QRectF plot = waveformPlotRect(chart, lanes);
    for (int lane = 1; lane < lanes; ++lane) {
        const QRectF next = waveformPlotRect(chart, lanes, lane);
        if (position.y() <= (plot.bottom() + next.top()) / 2)
            break;
        plot = next;
    }
    return {(position.x() - plot.left()) / plot.width(), (plot.bottom() - position.y()) / plot.height()};
}

QColor waveformChannelColor(int channel, const ThemeColors &colors)
{
    static const std::array<QColor, maxScopeChannels> light{
        QColor("#177971"), QColor("#2855b5"), QColor("#a45012"), QColor("#a53771"),
        QColor("#6c4aa6"), QColor("#587318"), QColor("#ad3d3d"), QColor("#266a85"),
    };
    static const std::array<QColor, maxScopeChannels> dark{
        QColor("#75d6cb"), QColor("#a0bfff"), QColor("#f4b778"), QColor("#eea1ce"),
        QColor("#c6a8f2"), QColor("#b4d77a"), QColor("#ffa2a2"), QColor("#89cde8"),
    };
    return (colors.canvas.lightness() < 128 ? dark : light).at(channel);
}

WaveformFrame waveformFrame(const WaveformRecording &recording, const WaveformView &view, int channels, int columns)
{
    WaveformFrame frame{view, {}, recording.sampleCount() > 0};
    for (int channel = 0; channel < channels; ++channel)
        frame.traces.append(recording.trace(channel, view.time, columns));
    return frame;
}

namespace {
void paintTrace(QPainter &painter, const QRectF &plot, const WaveformView &view, const QList<QPointF> &points, const QColor &color)
{
    if (points.isEmpty())
        return;
    const auto position = [&](QPointF point) {
        const double x = std::clamp((point.x() - view.time.low) / view.time.span(), -64.0, 65.0);
        const double y = std::clamp((point.y() - view.value.low) / view.value.span(), -64.0, 65.0);
        return QPointF(plot.left() + x * plot.width(), plot.bottom() - y * plot.height());
    };
    QPainterPath trace(position(points.first()));
    for (qsizetype i = 1; i < points.size(); ++i)
        trace.lineTo(position(points.at(i)));
    painter.setPen(QPen(color, 1.35));
    if (points.size() == 1)
        painter.drawEllipse(trace.currentPosition(), 1.7, 1.7);
    else
        painter.drawPath(trace);
}
}

void paintWaveform(QPainter &painter, const QRectF &chart, const WaveformFrame &frame, WaveformLayout layout, const ThemeColors &colors)
{
    const WaveformView &view = frame.view;
    const int channels = int(frame.traces.size());
    const bool separate = layout == WaveformLayout::Separate && channels > 1;
    const int lanes = separate ? channels : 1;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(colors.plot);
    painter.drawRoundedRect(chart, 7, 7);
    QFont font = painter.font();
    font.setPixelSize(10);
    font.setWeight(QFont::Normal);
    painter.setFont(font);
    const QPen grid(colors.border, 0.8);
    for (int lane = 0; lane < lanes; ++lane) {
        const QRectF plot = waveformPlotRect(chart, lanes, lane);
        const int horizontalTicks = std::max(2, qFloor(plot.width() / 78));
        for (int tick = 0; tick <= horizontalTicks; ++tick) {
            const double ratio = double(tick) / horizontalTicks;
            const double x = plot.left() + ratio * plot.width();
            painter.setPen(grid);
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
            if (lane == lanes - 1) {
                QString label = QString::number(view.time.low + ratio * view.time.span(), 'g', 7);
                if (separate && tick == horizontalTicks)
                    label += " 秒";
                painter.setPen(colors.muted);
                const qreal halfLabel = painter.fontMetrics().horizontalAdvance(label) / 2.0;
                const qreal center = std::min(x, chart.right() - halfLabel - 4);
                painter.drawText(QRectF(center - 38, plot.bottom() + 4, 76, 15), Qt::AlignCenter, label);
            }
        }
        const int verticalTicks = separate ? std::clamp(qFloor(plot.height() / 25), 1, 4) : 4;
        const qreal valueLabelLeft = chart.left() + (separate ? 39 : 2);
        for (int tick = 0; tick <= verticalTicks; ++tick) {
            const double ratio = double(tick) / verticalTicks;
            const double y = plot.bottom() - ratio * plot.height();
            painter.setPen(grid);
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            painter.setPen(colors.muted);
            painter.drawText(QRectF(valueLabelLeft, y - 8, plot.left() - valueLabelLeft - 5, 16), Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(view.value.low + ratio * view.value.span(), 'g', 5));
        }
        if (separate) {
            painter.setPen(waveformChannelColor(lane, colors));
            painter.drawText(QRectF(chart.left() + 8, plot.top(), 28, plot.height()), Qt::AlignVCenter | Qt::AlignLeft,
                             QString("CH%1").arg(lane + 1));
        }

        painter.save();
        painter.setClipRect(plot);
        painter.setBrush(Qt::NoBrush);
        const int first = separate ? lane : 0;
        const int end = separate ? lane + 1 : channels;
        for (int channel = first; channel < end; ++channel)
            paintTrace(painter, plot, view, frame.traces.at(channel), waveformChannelColor(channel, colors));
        if (!frame.hasSamples) {
            painter.setPen(colors.muted);
            painter.drawText(plot, Qt::AlignCenter, "等待样本");
        }
        painter.restore();
    }

    if (!separate) {
        const QRectF plot = waveformPlotRect(chart, lanes);
        const qreal legendWidth = std::min<qreal>(36, (plot.width() - 22) / std::max(1, channels));
        for (int channel = 0; channel < channels; ++channel) {
            const double x = plot.left() + channel * legendWidth;
            painter.setPen(waveformChannelColor(channel, colors));
            painter.drawText(QRectF(x, chart.bottom() - 22, legendWidth, 16), Qt::AlignLeft | Qt::AlignVCenter, QString("CH%1").arg(channel + 1));
        }
        painter.setPen(colors.muted);
        painter.drawText(QRectF(plot.right() - 22, chart.bottom() - 22, 22, 16), Qt::AlignRight | Qt::AlignVCenter, "秒");
    }
    painter.restore();
}

bool saveWaveformImage(const QString &path, QSizeF size, const WaveformRecording &recording,
                       const WaveformView &view, int channels, WaveformLayout layout, const ThemeColors &colors, QString &error)
{
    constexpr double maxExportPixels = 64000000;
    const double scale = std::min(300.0 / 96.0, std::sqrt(maxExportPixels / (size.width() * size.height())));
    const QSize pixels = (size * scale).toSize();
    QImage image(pixels, QImage::Format_RGB32);
    if (image.isNull()) {
        error = "无法分配高分辨率图像。";
        return false;
    }
    image.setDotsPerMeterX(11811);
    image.setDotsPerMeterY(11811);
    image.fill(colors.plot);
    QPainter painter(&image);
    painter.scale(scale, scale);
    const QRectF chart(QPointF(), size);
    const int lanes = layout == WaveformLayout::Separate ? channels : 1;
    const auto frame = waveformFrame(recording, view, channels, std::max(1, qRound(waveformPlotRect(chart, lanes).width() * scale)));
    paintWaveform(painter, chart, frame, layout, colors);
    painter.end();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        error = file.errorString();
        return false;
    }
    QImageWriter writer(&file, "png");
    if (!writer.write(image)) {
        error = writer.errorString();
        return false;
    }
    if (!file.commit()) {
        error = file.errorString();
        return false;
    }
    return true;
}
