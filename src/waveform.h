#pragma once

#include "scope.h"

#include <QRectF>
#include <QTemporaryFile>
#include <functional>
#include <memory>

class QColor;
class QPainter;
struct ThemeColors;

struct WaveformRange {
    double low;
    double high;
    double span() const { return high - low; }
    bool operator==(const WaveformRange &other) const { return low == other.low && high == other.high; }
};

struct WaveformView {
    WaveformRange time{0, 1};
    WaveformRange value{-1, 1};
    bool operator==(const WaveformView &other) const { return time == other.time && value == other.value; }
};

enum class WaveformGesture { ZoomValue, PanValue, ZoomTime, PanTime };
enum class WaveformLayout { Overlay, Separate };
enum class WaveformRegion { None, Plot, TimeAxis, ValueAxis };

struct WaveformHit {
    WaveformRegion region = WaveformRegion::None;
    int lane = 0;
    bool isAxis() const { return region == WaveformRegion::TimeAxis || region == WaveformRegion::ValueAxis; }
};

struct WaveformHover {
    WaveformHit hit;
    QPointF position;
    qreal radius = 20;
};

struct WaveformFrame {
    WaveformView view;
    QList<QList<QPointF>> traces;
    bool hasSamples = false;
};

class WaveformCsvSnapshot;
class WaveformViewport;

class WaveformRecording {
public:
    bool append(int channel, const QList<double> &values, int sampleRate, QString &error);
    void clear();
    WaveformView bounds() const;
    qint64 sampleCount() const;
    int channelCount() const;
    QList<QPointF> trace(int channel, WaveformRange time, int columns) const;
    WaveformCsvSnapshot csvSnapshot() const;
    std::shared_ptr<const WaveformViewport> viewport(WaveformView view, int channels) const;

private:
    friend class WaveformCsvSnapshot;
    friend class WaveformViewport;
    static constexpr int blockSize = 256;
    struct Sample { double time; double value; };
    struct Block {
        qint64 offset;
        int count;
        double firstTime;
        double lastTime;
        Sample minimum;
        Sample maximum;
    };
    struct Series {
        std::shared_ptr<QTemporaryFile> file;
        QList<Block> blocks;
        qint64 samples = 0;
        double nextTime = 0;
        double minimum = 0;
        double maximum = 0;
    };

    QByteArray readBlock(const Series &series, const Block &block) const;
    qint64 sampleBoundary(const Series &series, double time, bool afterEqual) const;
    std::array<Series, maxScopeChannels> tracks;
};

// A stable time slice of the original samples. Consumers can read each channel in chunks.
class WaveformViewport {
public:
    using ReadSamples = std::function<QList<QPointF>(qint64, qsizetype)>;
    using Reader = std::shared_ptr<const ReadSamples>;

    WaveformView view() const { return range; }
    int channelCount() const { return channels; }
    int firstSourceChannel() const { return sourceOffset; }
    std::shared_ptr<const WaveformViewport> singleChannel(int channel) const;
    std::shared_ptr<const WaveformViewport> withChannelReaders(const std::array<Reader, maxScopeChannels> &readers) const;
    qint64 sampleCount(int channel) const;
    bool sameSamples(const WaveformViewport &other, int channel) const;
    QList<QPointF> readChannel(int channel, qint64 offset, qsizetype count) const;

private:
    friend class WaveformRecording;
    struct Channel {
        std::shared_ptr<QTemporaryFile> storage;
        QString path;
        qint64 first = 0;
        qint64 count = 0;
        Reader reader;
    };
    std::array<Channel, maxScopeChannels> tracks;
    WaveformView range;
    int channels = 0;
    int sourceOffset = 0;
};

class WaveformCsvSnapshot {
public:
    bool save(const QString &path, QString &error) const;

private:
    friend class WaveformRecording;
    struct Channel {
        std::shared_ptr<QTemporaryFile> storage;
        QString path;
        qint64 samples = 0;
    };
    std::array<Channel, maxScopeChannels> channels;
};

void adjustWaveformView(WaveformView &view, const WaveformView &bounds, WaveformGesture gesture, double steps, QPointF anchor);
void panWaveformAxis(WaveformView &view, const WaveformView &bounds, WaveformRegion axis, double fraction);
QRectF waveformPlotRect(const QRectF &chart, int lanes, int lane = 0);
WaveformHit waveformHit(const QRectF &chart, int lanes, QPointF position);
QColor waveformChannelColor(int channel, const ThemeColors &colors);
WaveformFrame waveformFrame(const WaveformRecording &recording, const WaveformView &view, int channels, int columns);
void paintWaveform(QPainter &painter, const QRectF &chart, const WaveformFrame &frame, WaveformLayout layout,
                   const ThemeColors &colors, const WaveformHover &hover = {});
bool saveWaveformImage(const QString &path, QSizeF size, const WaveformRecording &recording,
                       const WaveformView &view, int channels, WaveformLayout layout, const ThemeColors &colors, QString &error);
