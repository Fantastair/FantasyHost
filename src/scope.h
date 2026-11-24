#pragma once

#include <QJsonObject>
#include <QList>
#include <QPointF>
#include <QString>
#include <array>
#include <optional>

constexpr int maxScopeChannels = 8;

int scopeChannels(const QJsonObject &parameters);

struct ScopeFrame {
    QList<QList<QPointF>> traces;
    QString status;
    double millisecondsPerDivision = 10;
    double lower = -4;
    double upper = 4;
    int samples = 0;
    std::optional<double> triggerPosition;
    double triggerLevel = 0;
    int triggerChannel = 0;
};

class ScopeCapture {
public:
    void configure(const QJsonObject &parameters);
    void clear();
    void append(int channel, const QList<double> &samples);
    void rearm();
    ScopeFrame frame();

private:
    static constexpr int capacity = 16384;
    struct Ring {
        QList<double> values;
        qint64 next = 0;
        qint64 begin() const { return qMax<qint64>(0, next - capacity); }
        double at(qint64 index) const { return values.at(index % capacity); }
    };

    ScopeFrame makeFrame(qint64 from, const QString &status) const;
    void updateScale(ScopeFrame &result) const;

    std::array<Ring, maxScopeChannels> rings;
    QJsonObject settings;
    std::optional<qint64> trigger;
    std::optional<ScopeFrame> held;
    qint64 lastTrigger = 0;
    int count = 1;
    int triggerChannel = 0;
    int length = 100;
    int pretrigger = 25;
    int rate = 1000;
    double level = 0;
    bool fallingEdge = false;
    QString triggerMode = "auto";
};
