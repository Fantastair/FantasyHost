#include "scope.h"

#include <algorithm>
#include <cmath>
#include <limits>

int scopeChannels(const QJsonObject &parameters)
{
    return std::clamp(parameters.value("channels").toInt(1), 1, maxScopeChannels);
}

void ScopeCapture::configure(const QJsonObject &parameters)
{
    if (settings == parameters)
        return;
    if (scopeChannels(parameters) != count || parameters.value("layout") != settings.value("layout")
        || parameters.value("encoding") != settings.value("encoding"))
        clear();
    for (const QString &key : {QString("sampleRate"), QString("timeBase"), QString("triggerMode"),
                               QString("triggerChannel"), QString("triggerEdge"), QString("triggerLevel")}) {
        if (parameters.value(key) != settings.value(key)) {
            rearm();
            break;
        }
    }
    settings = parameters;
    count = scopeChannels(parameters);
    rate = parameters.value("sampleRate").toInt(1000);
    length = std::clamp(qRound(rate * parameters.value("timeBase").toDouble(10) / 100.0), 2, capacity);
    pretrigger = (length - 1) / 4;
    triggerChannel = std::clamp(parameters.value("triggerChannel").toInt(1), 1, count) - 1;
    triggerMode = parameters.value("triggerMode").toString("auto");
    level = parameters.value("triggerLevel").toDouble();
    fallingEdge = parameters.value("triggerEdge").toString() == "falling";
}

void ScopeCapture::clear()
{
    rings = {};
    rearm();
}

void ScopeCapture::rearm()
{
    trigger.reset();
    held.reset();
    lastTrigger = 0;
}

void ScopeCapture::append(int channel, const QList<double> &samples)
{
    if (channel < 0 || channel >= count || samples.isEmpty())
        return;
    Ring &ring = rings[channel];
    if (ring.values.isEmpty())
        ring.values.resize(capacity);
    const bool listenForTrigger = channel == triggerChannel && !(triggerMode == "single" && held);
    for (double value : samples) {
        if (!std::isfinite(value))
            continue;
        if (listenForTrigger) {
            if (trigger && *trigger - pretrigger < ring.begin())
                trigger.reset();
            if (!trigger && ring.next > 0 && ring.next - pretrigger >= ring.begin()) {
                const double previous = ring.at(ring.next - 1);
                if (fallingEdge ? previous > level && value <= level : previous < level && value >= level)
                    trigger = ring.next;
            }
        }
        ring.values[ring.next % capacity] = value;
        ++ring.next;
    }
}

ScopeFrame ScopeCapture::frame()
{
    const Ring &source = rings[triggerChannel];
    if (trigger && *trigger - pretrigger >= source.begin()
        && source.next >= *trigger - pretrigger + length) {
        lastTrigger = *trigger;
        held = makeFrame(lastTrigger - pretrigger, triggerMode == "single" ? "单次已捕获" : "已触发");
        held->triggerPosition = double(pretrigger) / (length - 1);
        held->triggerLevel = level;
        held->triggerChannel = triggerChannel;
        trigger.reset();
    }
    if (held && (triggerMode != "auto" || source.next - lastTrigger <= length * 2)) {
        ScopeFrame result = *held;
        updateScale(result);
        return result;
    }
    if (triggerMode != "auto")
        return makeFrame(-length, "等待触发");

    qint64 newest = 0;
    for (int channel = 0; channel < count; ++channel)
        newest = std::max(newest, rings[channel].next);
    return makeFrame(newest - length, newest ? "自动" : "等待样本");
}

ScopeFrame ScopeCapture::makeFrame(qint64 from, const QString &status) const
{
    ScopeFrame result;
    result.traces.resize(count);
    result.status = status;
    result.millisecondsPerDivision = length * 100.0 / rate;
    for (int channel = 0; channel < count; ++channel) {
        const Ring &ring = rings[channel];
        const qint64 first = std::max(from, ring.begin());
        const qint64 end = std::min(from + length, ring.next);
        if (first >= end)
            continue;
        result.samples = std::max(result.samples, int(end - first));
        auto &trace = result.traces[channel];
        const qint64 stride = (end - first + 223) / 224;
        for (qint64 offset = first; offset < end; offset += stride) {
            qint64 low = offset;
            qint64 high = offset;
            for (qint64 i = offset + 1; i < std::min(end, offset + stride); ++i) {
                if (ring.at(i) < ring.at(low)) low = i;
                if (ring.at(i) > ring.at(high)) high = i;
            }
            const auto emitPoint = [&](qint64 i) { trace.append(QPointF(double(i - from) / (length - 1), ring.at(i))); };
            emitPoint(std::min(low, high));
            if (low != high)
                emitPoint(std::max(low, high));
        }
    }
    updateScale(result);
    return result;
}

void ScopeCapture::updateScale(ScopeFrame &result) const
{
    const double center = settings.value("verticalOffset").toDouble();
    const double halfRange = std::max(settings.value("voltsPerDivision").toDouble(1) * 4, std::abs(center) * 1e-14);
    result.lower = center - halfRange;
    result.upper = center + halfRange;
    if (settings.value("verticalMode").toString("auto") == "manual" || result.samples == 0)
        return;

    double low = std::numeric_limits<double>::infinity();
    double high = -low;
    for (const auto &trace : result.traces) {
        for (const QPointF &point : trace) {
            low = std::min(low, point.y());
            high = std::max(high, point.y());
        }
    }
    const double margin = std::max({(high - low) * 0.08, std::max(std::abs(low), std::abs(high)) * 1e-6, 1e-9});
    result.lower = low - margin;
    result.upper = high + margin;
}
