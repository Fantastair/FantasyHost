#pragma once

#include "scope.h"

#include <QRectF>
#include <QTemporaryFile>
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
};

enum class WaveformGesture { ZoomValue, PanValue, ZoomTime, PanTime };
enum class WaveformLayout { Overlay, Separate };

struct WaveformFrame {
    WaveformView view;
    QList<QList<QPointF>> traces;
    bool hasSamples = false;
};

class WaveformCsvSnapshot;

class WaveformRecording {
public:
    bool append(int channel, const QList<double> &values, int sampleRate, QString &error);
    void clear();
    WaveformView bounds() const;
    qint64 sampleCount() const;
    int channelCount() const;
    QList<QPointF> trace(int channel, WaveformRange time, int columns) const;
    WaveformCsvSnapshot csvSnapshot() const;

private:
    friend class WaveformCsvSnapshot;
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
    std::array<Series, maxScopeChannels> tracks;
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
QRectF waveformPlotRect(const QRectF &chart, int lanes, int lane = 0);
QPointF waveformAnchor(const QRectF &chart, int lanes, QPointF position);
QColor waveformChannelColor(int channel, const ThemeColors &colors);
WaveformFrame waveformFrame(const WaveformRecording &recording, const WaveformView &view, int channels, int columns);
void paintWaveform(QPainter &painter, const QRectF &chart, const WaveformFrame &frame, WaveformLayout layout, const ThemeColors &colors);
bool saveWaveformImage(const QString &path, QSizeF size, const WaveformRecording &recording,
                       const WaveformView &view, int channels, WaveformLayout layout, const ThemeColors &colors, QString &error);
