#include "canvas.h"
#include "scope.h"
#include "spectrum.h"
#include "waveform.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStyleOptionGraphicsItem>
#include <QTextLayout>
#include <QToolTip>
#include <QVarLengthArray>
#include <QWheelEvent>
#ifdef FANTASYHOST_OPENGL
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPointer>
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace {
constexpr qreal portTop = 58;
constexpr qreal portSpacing = 18;
constexpr qreal chartTop = 47;
constexpr qreal minimumZoom = 0.000015;
constexpr qreal cardRadius = 10;
constexpr qreal scopeFooter = 56;
constexpr qreal textPaddingX = 14;
constexpr qreal textPaddingY = 9;
constexpr qreal textPortTop = 21;
constexpr qreal maxTextWidth = 480;

QFont displayFont()
{
    QFont font = QApplication::font();
    font.setPixelSize(19);
    font.setWeight(QFont::DemiBold);
    return font;
}

qreal lowestPort(qsizetype inputs, qsizetype outputs)
{
    return portTop + portSpacing * (std::max<qsizetype>(1, std::max(inputs, outputs)) - 1);
}

QSizeF cardSize(const ModuleDefinition &definition, const QJsonObject &parameters, QSizeF customSize = {})
{
    if (definition.resizable()) {
        if (customSize.isValid())
            return customSize;
        if (definition.displaysSpectrum)
            return {436, 277};
        return definition.recordsWaveform ? QSizeF(488, 317) : QSizeF(352, 248);
    }
    if (definition.displaysText)
        return {92, 42};
    const qreal last = lowestPort(modulePorts(definition, parameters, false).size(), modulePorts(definition, parameters, true).size());
    return {234, last + 47};
}

QColor portColor(DataType type, const ThemeColors &colors)
{
    if (type == DataType::Any || type == DataType::Waveform)
        return colors.accent;
    return type == DataType::Number ? colors.number : colors.bytes;
}

QPainterPath wirePath(QPointF from, QPointF to)
{
    const qreal dx = to.x() - from.x();
    const qreal distance = std::clamp(std::abs(dx) * 0.5, dx >= 0 ? 12.0 : 42.0, 220.0);
    QPainterPath path(from);
    path.cubicTo(from + QPointF(distance, 0), to - QPointF(distance, 0), to);
    return path;
}
}

class NodeItem final : public QGraphicsItem {
public:
    static constexpr int Type = UserType + 1;

    NodeItem(const Node &node, const ThemeColors &colors)
        : id(node.id), definition(*moduleDefinition(node.type)), colors(colors)
    {
        if (definition.scope)
            capture = std::make_unique<ScopeCapture>();
        if (definition.recordsWaveform)
            recording = std::make_unique<WaveformRecording>();
        if (definition.displaysText) {
            displayLayout.setFont(displayFont());
            displayLayout.setCacheEnabled(true);
            QTextOption options;
            options.setAlignment(Qt::AlignLeft | Qt::AlignAbsolute);
            options.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            displayLayout.setTextOption(options);
            layoutDisplayText();
        }
        applySettings(node);
        setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
        setToolTip(resizable() ? definition.description + " 拖动右下角可调整大小。" : definition.description);
        setPos(node.position);
    }

    int type() const override { return Type; }
    QRectF card() const { return {{0, 0}, dimensions}; }
    QRectF boundingRect() const override { return card().adjusted(-12, -5, 12, 7); }

    bool onResizeGrip(QPointF position) const
    {
        return resizable() && QRectF(card().bottomRight() - QPointF(25, 25), QSizeF(24, 24)).contains(position);
    }

    QSizeF fittedResize(QSizeF requested) const
    {
        const int lanes = recording ? recordingLanes() : 1;
        const qreal minimumHeight = lanes > 1 ? chartTop + 18 + 28 + lanes * 68 : Graph::minScopeHeight;
        return {std::clamp(requested.width(), Graph::minScopeWidth, Graph::maxScopeSize),
                std::clamp(requested.height(), std::max(minimumHeight, Graph::minScopeHeight), Graph::maxScopeSize)};
    }

    void setCardSize(QSizeF size)
    {
        if (dimensions != size) {
            prepareGeometryChange();
            dimensions = size;
            update();
        }
    }

    void resizeCard(QSizeF size)
    {
        setCardSize(size);
        if (recording)
            refreshRecordingFrame();
    }

    QPainterPath shape() const override
    {
        QPainterPath path;
        path.addRoundedRect(card(), cardRadius, cardRadius);
        for (bool output : {false, true}) {
            for (const PortDefinition &port : ports(output))
                path.addEllipse(portPosition(output, port.id), 9, 9);
        }
        path.setFillRule(Qt::WindingFill);
        return path;
    }

    const QList<PortDefinition> &ports(bool output) const { return output ? outputPorts : inputPorts; }

    const PortDefinition *port(bool output, const QString &id) const
    {
        for (const auto &candidate : ports(output)) {
            if (candidate.id == id)
                return &candidate;
        }
        return nullptr;
    }

    QPointF portPosition(bool output, const QString &id) const
    {
        const auto &list = ports(output);
        for (qsizetype index = 0; index < list.size(); ++index) {
            if (list.at(index).id == id) {
                const qreal top = definition.displaysText ? textPortTop : output && recording ? 20 : portTop;
                return {output ? dimensions.width() : 0, top + portSpacing * index};
            }
        }
        return {};
    }

    void paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) override
    {
        painter->setRenderHint(QPainter::Antialiasing);
        const QRectF frame = card();
        const qreal cardWidth = dimensions.width();
        const qreal zoom = QStyleOptionGraphicsItem::levelOfDetailFromTransform(painter->worldTransform());
        if (zoom < 0.12) {
            painter->fillRect(frame, failed ? colors.error : isSelected() ? colors.accent : colors.muted);
            return;
        }
        painter->setPen(Qt::NoPen);
        painter->setBrush(colors.shadow);
        painter->drawRoundedRect(frame.translated(0, 3), cardRadius, cardRadius);
        painter->setBrush(colors.card);
        painter->setPen(QPen(failed ? colors.error : isSelected() ? colors.accent : colors.border, isSelected() ? 2 : 1));
        painter->drawRoundedRect(frame, cardRadius, cardRadius);

        if (definition.displaysText) {
            painter->setPen(colors.text);
            displayLayout.draw(painter, QPointF(textPaddingX, textPaddingY));
        }

        QFont font = painter->font();
        if (!definition.displaysText) {
            font.setPixelSize(15);
            font.setWeight(QFont::DemiBold);
            painter->setFont(font);
            painter->setPen(colors.text);
            painter->drawText(QRectF(15, 0, cardWidth - 38, 39), Qt::AlignLeft | Qt::AlignVCenter, definition.name);
            if (failed) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(colors.error);
                painter->drawEllipse(QPointF(cardWidth - (recording && !outputPorts.isEmpty() ? 102 : 17), 20), 3.5, 3.5);
            }
            painter->setPen(colors.border);
            painter->drawLine(QPointF(14, 40), QPointF(cardWidth - 14, 40));
        }

        font.setPixelSize(12);
        font.setWeight(QFont::Normal);
        painter->setFont(font);
        for (bool output : {false, true}) {
            for (const PortDefinition &port : ports(output)) {
                const QPointF position = portPosition(output, port.id);
                painter->setPen(QPen(colors.canvas, 2));
                painter->setBrush(portColor(port.type, colors));
                const qreal radius = definition.displaysText ? 4.5 : 5.5;
                painter->drawEllipse(position, radius, radius);
                if (definition.displaysText)
                    continue;
                painter->setPen(colors.muted);
                const qreal labelWidth = resizable() && !output ? chartLeft() - 16 : cardWidth / 2 - 13;
                const QRectF label(output ? cardWidth / 2 : 12, position.y() - 9, labelWidth, 18);
                painter->drawText(label, Qt::AlignVCenter | (output ? Qt::AlignRight : Qt::AlignLeft),
                                  painter->fontMetrics().elidedText(port.label, Qt::ElideRight, label.width()));
            }
        }

        if (capture)
            paintPlot(*painter);
        else if (recording)
            paintWaveform(*painter, recordingChart(), recordingFrame, recordingLayout, colors, recordingHover);
        else if (definition.displaysSpectrum)
            paintSpectrum(*painter, recordingChart(), spectrum, colors);
        else if (!definition.displaysText) {
            painter->setPen(failed ? colors.error : colors.muted);
            const QString label = shownStatus.isEmpty() ? QString("串口已关闭") : shownStatus;
            painter->drawText(QRectF(14, lowestPort(inputPorts.size(), outputPorts.size()) + 16, cardWidth - 28, 22),
                              Qt::AlignLeft | Qt::AlignVCenter, painter->fontMetrics().elidedText(label, Qt::ElideRight, cardWidth - 28));
        }
        if (resizable()) {
            const QPointF corner = frame.bottomRight();
            painter->setPen(QPen(isSelected() ? colors.accent : colors.muted, 1.35, Qt::SolidLine, Qt::RoundCap));
            painter->drawLine(corner - QPointF(7, 15), corner - QPointF(15, 7));
            painter->drawLine(corner - QPointF(7, 10), corner - QPointF(10, 7));
        }
    }

    bool applySettings(const Node &node)
    {
        const auto nextPorts = modulePorts(definition, node.parameters, false);
        const auto nextOutputs = modulePorts(definition, node.parameters, true);
        const auto nextSize = cardSize(definition, node.parameters, node.customSize);
        const bool changed = nextPorts != inputPorts || nextOutputs != outputPorts;
        if (changed) {
            prepareGeometryChange();
            inputPorts = nextPorts;
            outputPorts = nextOutputs;
        }
        if (definition.displaysText)
            fitDisplayText();
        else if (!recording)
            setCardSize(nextSize);
        if (capture) {
            capture->configure(node.parameters);
            if (!paused)
                shownScope = capture->frame();
        }
        if (recording) {
            recordingRate = node.parameters.value("sampleRate").toInt(1000);
            configuredChannels = scopeChannels(node.parameters);
            recordingLayout = node.parameters.value("viewLayout") == "separate" ? WaveformLayout::Separate : WaveformLayout::Overlay;
            resizeCard(fittedResize(nextSize));
        }
        update();
        return changed;
    }

    void push(int channel, const QList<double> &values, QString &error)
    {
        if (capture && !paused)
            capture->append(channel, values);
        if (recording && !failed && !recording->append(channel, values, recordingRate, error))
            failed = true;
    }

    bool isPaused() const { return paused; }

    void setPaused(bool value)
    {
        if (capture) {
            paused = value;
            if (!paused)
                capture->clear();
            presentValues();
        }
    }

    void rearm()
    {
        if (capture) {
            paused = false;
            capture->rearm();
            presentValues();
        }
    }

    void setStatus(const QString &status) { pendingStatus = status; }
    const QString &displayText() const { return pendingText; }
    void setDisplayText(const QString &text) { pendingText = text; }
    void setSpectrum(const SpectrumFrame &frame) { spectrum = frame; update(); }

    void presentValues()
    {
        shownStatus = pendingStatus;
        if (shownText != pendingText) {
            shownText = pendingText;
            layoutDisplayText();
            fitDisplayText();
        }
        if (capture && !paused)
            shownScope = capture->frame();
        if (recording)
            refreshRecordingFrame();
        update();
    }

    QUuid id;
    const ModuleDefinition &definition;
    bool failed = false;
    std::function<void(QPointF)> moved;

    qint64 recordingSamples() const { return recording ? recording->sampleCount() : 0; }

    WaveformHit recordingHit(QPointF position) const
    {
        return recording ? waveformHit(recordingChart(), recordingLanes(), position) : WaveformHit{};
    }

    void setRecordingHover(WaveformHover hover)
    {
        if (recordingHover.hit.region != hover.hit.region || recordingHover.hit.lane != hover.hit.lane
            || (hover.hit.region == WaveformRegion::Plot && (recordingHover.position != hover.position || recordingHover.radius != hover.radius))) {
            recordingHover = hover;
            update();
        }
    }

    bool adjustRecording(WaveformHit hit, QPointF position, double steps, bool pan)
    {
        refreshRecordingBounds();
        const WaveformView previous = recordingView;
        const QRectF plot = waveformPlotRect(recordingChart(), recordingLanes(), hit.lane);
        const QPointF anchor((position.x() - plot.left()) / plot.width(), (plot.bottom() - position.y()) / plot.height());
        const bool time = hit.region == WaveformRegion::TimeAxis;
        adjustWaveformView(recordingView, recordingLimits, time ? (pan ? WaveformGesture::PanTime : WaveformGesture::ZoomTime)
                                                                : (pan ? WaveformGesture::PanValue : WaveformGesture::ZoomValue),
                           steps, anchor);
        return finishRecordingMove(previous);
    }

    bool dragRecording(WaveformHit hit, QPointF delta)
    {
        refreshRecordingBounds();
        const WaveformView previous = recordingView;
        const QRectF plot = waveformPlotRect(recordingChart(), recordingLanes(), std::min(hit.lane, recordingLanes() - 1));
        const double fraction = hit.region == WaveformRegion::TimeAxis ? -delta.x() / plot.width() : delta.y() / plot.height();
        panWaveformAxis(recordingView, recordingLimits, hit.region, fraction);
        return finishRecordingMove(previous);
    }

    void fitRecording()
    {
        if (recording) {
            recordingView = recordingLimits = recording->bounds();
            refreshRecordingFrame();
            update();
        }
    }

    void clearRecording(const Node &node)
    {
        recording->clear();
        recordingView = recordingLimits = {};
        failed = false;
        applySettings(node);
    }

    WaveformCsvSnapshot recordingCsv() const { return recording ? recording->csvSnapshot() : WaveformCsvSnapshot(); }
    WaveformPacket recordingViewport() const
    {
        return recording && !outputPorts.isEmpty() ? recording->viewport(recordingView, recordingChannels()) : WaveformPacket{};
    }

    bool exportRecordingImage(const QString &path, QString &error) const
    {
        if (!recording) {
            error = "未找到波形记录器。";
            return false;
        }
        return saveWaveformImage(path, recordingChart().size(), *recording, recordingView, recordingChannels(), recordingLayout, colors, error);
    }

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override
    {
        if (change == ItemPositionChange) {
            const QPointF point = value.toPointF();
            constexpr double limit = Graph::nodePositionLimit;
            return QPointF(std::clamp(point.x(), -limit, limit), std::clamp(point.y(), -limit, limit));
        }
        if (change == ItemPositionHasChanged && moved)
            moved(value.toPointF());
        if (change == ItemSelectedHasChanged)
            setZValue(value.toBool() ? 1 : 0);
        return QGraphicsItem::itemChange(change, value);
    }

private:
    bool resizable() const { return definition.resizable(); }

    qreal chartLeft() const
    {
        return inputPorts.isEmpty() ? 13 : inputPorts.first().id == "ch1" && inputPorts.first().label != "CH1" ? 78 : 47;
    }

    QRectF recordingChart() const { return card().adjusted(chartLeft(), chartTop, -13, -18); }

    int recordingChannels() const { return std::max(configuredChannels, recording->channelCount()); }
    int recordingLanes() const { return recordingLayout == WaveformLayout::Separate ? recordingChannels() : 1; }

    bool finishRecordingMove(const WaveformView &previous)
    {
        if (recordingView == previous)
            return false;
        refreshRecordingFrame();
        update();
        return true;
    }

    void refreshRecordingBounds()
    {
        const WaveformView next = recording->bounds();
        const auto adjust = [](WaveformRange &view, WaveformRange previous, WaveformRange limit) {
            if (view == previous || view.span() >= limit.span()) {
                view = limit;
            } else if (view.low < limit.low || view.high > limit.high) {
                const double span = view.span();
                view.low = std::clamp(view.low, limit.low, std::max(limit.low, limit.high - span));
                view.high = std::min(limit.high, view.low + span);
            }
        };
        adjust(recordingView.time, recordingLimits.time, next.time);
        adjust(recordingView.value, recordingLimits.value, next.value);
        recordingLimits = next;
    }

    void refreshRecordingFrame()
    {
        refreshRecordingBounds();
        recordingFrame = waveformFrame(*recording, recordingView, recordingChannels(),
                                       std::max(1, qRound(waveformPlotRect(recordingChart(), recordingLanes()).width())));
    }

    void layoutDisplayText()
    {
        QString text = shownText;
        text.replace("\r\n", "\n");
        text.replace('\r', '\n');
        text.replace('\n', QChar::LineSeparator);
        displayLayout.setText(text);
        qreal width = 0;
        qreal height = 0;
        displayLayout.beginLayout();
        for (QTextLine line = displayLayout.createLine(); line.isValid(); line = displayLayout.createLine()) {
            line.setLineWidth(maxTextWidth);
            line.setPosition(QPointF(0, height));
            width = std::max(width, line.naturalTextWidth());
            height += line.height();
        }
        displayLayout.endLayout();
        displayContent = {std::ceil(width), std::max(std::ceil(height), QFontMetricsF(displayLayout.font()).height())};
    }

    void fitDisplayText()
    {
        const qreal contentHeight = displayContent.height() + 2 * textPaddingY;
        const qreal portHeight = inputPorts.isEmpty() ? 0 : textPortTop + portSpacing * (inputPorts.size() - 1) + textPaddingY;
        setCardSize({displayContent.width() + 2 * textPaddingX, std::max(contentHeight, portHeight)});
    }

    void paintPlot(QPainter &painter) const
    {
        const QRectF plot(chartLeft(), chartTop, dimensions.width() - chartLeft() - 13, dimensions.height() - chartTop - scopeFooter);
        const QRectF area = plot.adjusted(4, 4, -4, -4);
        painter.setPen(Qt::NoPen);
        painter.setBrush(colors.plot);
        painter.drawRoundedRect(plot, 6, 6);
        painter.setPen(QPen(colors.border, 0.8));
        for (int division = 0; division <= 10; ++division) {
            const qreal x = area.left() + area.width() * division / 10;
            painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
        }
        for (int division = 0; division <= 8; ++division) {
            const qreal y = area.top() + area.height() * division / 8;
            painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
        }
        if (shownScope.samples == 0) {
            painter.setPen(colors.muted);
            painter.drawText(plot, Qt::AlignCenter, shownScope.status);
        }

        const double span = shownScope.upper - shownScope.lower;
        painter.save();
        painter.setClipRect(area);
        painter.setBrush(Qt::NoBrush);
        if (shownScope.triggerPosition) {
            painter.setPen(QPen(waveformChannelColor(shownScope.triggerChannel, colors), 0.8, Qt::DashLine));
            const qreal x = area.left() + area.width() * *shownScope.triggerPosition;
            painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
            const qreal y = area.bottom() - area.height() * ((shownScope.triggerLevel - shownScope.lower) / span);
            if (y > area.top() && y < area.bottom())
                painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
        }
        for (qsizetype channel = 0; channel < shownScope.traces.size(); ++channel) {
            QPainterPath trace;
            const auto &points = shownScope.traces[channel];
            if (points.isEmpty())
                continue;
            const auto position = [&](const QPointF &point) {
                const double value = std::clamp((point.y() - shownScope.lower) / span, -64.0, 65.0);
                return QPointF(area.left() + area.width() * point.x(), area.bottom() - area.height() * value);
            };
            trace.moveTo(position(points.first()));
            for (qsizetype i = 1; i < points.size(); ++i)
                trace.lineTo(position(points.at(i)));
            painter.setPen(QPen(waveformChannelColor(channel, colors), 1.45));
            painter.drawPath(trace);
            painter.drawEllipse(trace.currentPosition(), 1.5, 1.5);
        }
        painter.restore();

        const qreal legendWidth = std::min<qreal>(40, plot.width() / std::max<qsizetype>(1, shownScope.traces.size()));
        for (qsizetype channel = 0; channel < shownScope.traces.size(); ++channel) {
            const qreal x = plot.left() + channel * legendWidth;
            painter.setPen(waveformChannelColor(channel, colors));
            painter.drawText(QRectF(x, plot.bottom() + 6, legendWidth, 16), Qt::AlignLeft | Qt::AlignVCenter,
                             QString("CH%1").arg(channel + 1));
        }
        painter.setPen(colors.muted);
        QString summary = QString("%1 ms/格   ·   %2 单位/格")
            .arg(QString::number(shownScope.millisecondsPerDivision, 'g', 4), QString::number(span / 8, 'g', 4));
        if (paused || shownScope.samples > 0)
            summary += "   ·   " + (paused ? QString("已暂停") : shownScope.status);
        painter.drawText(QRectF(plot.left(), plot.bottom() + 25, plot.width(), 17), Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(summary, Qt::ElideRight, plot.width()));
    }

    const ThemeColors &colors;
    QList<PortDefinition> inputPorts;
    QList<PortDefinition> outputPorts;
    QSizeF dimensions;
    std::unique_ptr<ScopeCapture> capture;
    std::unique_ptr<WaveformRecording> recording;
    WaveformView recordingView;
    WaveformView recordingLimits;
    WaveformFrame recordingFrame;
    WaveformLayout recordingLayout = WaveformLayout::Overlay;
    WaveformHover recordingHover;
    SpectrumFrame spectrum;
    int recordingRate = 1000;
    int configuredChannels = 1;
    QString pendingStatus;
    QString shownStatus;
    QString pendingText;
    QString shownText;
    QTextLayout displayLayout;
    QSizeF displayContent;
    ScopeFrame shownScope;
    bool paused = false;
};

class EdgeItem final : public QGraphicsPathItem {
public:
    static constexpr int Type = UserType + 2;

    EdgeItem(const Connection &edge, NodeItem &source, NodeItem &target, const ThemeColors &colors)
        : id(edge.id), link(edge), from(source), to(target),
          dataType(source.port(true, link.from.port)->type), colors(colors)
    {
        setFlag(ItemIsSelectable);
        setZValue(-1);
        setPen(QPen(Qt::transparent, 12));
        updatePath();
    }

    int type() const override { return Type; }

    void updatePath()
    {
        setPath(wirePath(from.mapToScene(from.portPosition(true, link.from.port)),
                         to.mapToScene(to.portPosition(false, link.to.port))));
    }

    void paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) override
    {
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(isSelected() ? colors.accent : portColor(dataType, colors),
                             isSelected() ? 3.5 : 2.1, Qt::SolidLine, Qt::RoundCap));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(path());
    }

    QUuid id;
    Connection link;
    NodeItem &from;
    NodeItem &to;
    DataType dataType;
    const ThemeColors &colors;
};

Canvas::Canvas(Graph &graph, QWidget *parent)
    : QGraphicsView(parent), graph(graph), workspace(new QGraphicsScene(this))
{
    setObjectName("canvas");
    setAccessibleName("数据流画布");
    constexpr qreal limit = Graph::canvasLimit;
    workspace->setSceneRect(-limit, -limit, limit * 2, limit * 2);
    workspace->setItemIndexMethod(QGraphicsScene::NoIndex);
    setScene(workspace);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setTransformationAnchor(NoAnchor);
    setResizeAnchor(AnchorViewCenter);
    setDragMode(RubberBandDrag);
    setViewportUpdateMode(BoundingRectViewportUpdate);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setRenderHint(QPainter::Antialiasing);
    repaintTimer.setInterval(32);
    repaintTimer.setSingleShot(true);
    QObject::connect(&repaintTimer, &QTimer::timeout, this, &Canvas::flushSamples);
    waveformPublishTimer.setInterval(100);
    waveformPublishTimer.setSingleShot(true);
    QObject::connect(&waveformPublishTimer, &QTimer::timeout, this, [this] {
        const auto pending = std::exchange(changedViewports, {});
        for (const QUuid &id : pending)
            Q_EMIT recordingViewportChanged(id);
    });
    QObject::connect(workspace, &QGraphicsScene::selectionChanged, this, &Canvas::selectionChanged);
}

void Canvas::rebuild()
{
    cancelWire();
    resizingNode = QUuid{};
    waveformDrag.reset();
    hoveredWaveform = QUuid{};
    changedViewports.clear();
    waveformPublishTimer.stop();
    nodes.clear();
    edges.clear();
    incidentEdges.clear();
    dirtyNodes.clear();
    workspace->clear();
    for (const Node &node : graph.nodes())
        addNodeItem(node);
    for (const Connection &edge : graph.connections())
        addEdgeItem(edge);
    viewport()->update();
}

void Canvas::setTheme(const ThemeColors &theme)
{
    colors = theme;
    if (wireStart)
        updateWire(viewport()->mapFromGlobal(QCursor::pos()));
    workspace->invalidate();
    viewport()->update();
}

#ifdef FANTASYHOST_OPENGL
void Canvas::setOpenGLViewport(bool enabled)
{
    if (enabled == (qobject_cast<QOpenGLWidget *>(viewport()) != nullptr))
        return;
    if (enabled) {
        QOpenGLContext probe;
        if (!probe.create()) {
            Q_EMIT openGLViewportChanged(false);
            Q_EMIT message("当前环境无法创建 OpenGL 上下文，画布继续使用软件绘制。");
            return;
        }
    }
    const QPointF center = mapToScene(viewport()->rect().center());
    cancelWire();
    setViewportUpdateMode(enabled ? FullViewportUpdate : BoundingRectViewportUpdate);
    if (enabled) {
        auto *gl = new QOpenGLWidget;
        QSurfaceFormat format = gl->format();
        format.setSamples(4);
        gl->setFormat(format);
        setViewport(gl);
        QTimer::singleShot(180, this, [this, gl = QPointer<QOpenGLWidget>(gl)] {
            if (!gl || viewport() != gl.data())
                return;
            if (!gl->isValid()) {
                setOpenGLViewport(false);
                Q_EMIT message("OpenGL 初始化失败，已切回软件绘制。");
                return;
            }
            gl->makeCurrent();
            const auto *name = gl->context()->functions()->glGetString(GL_RENDERER);
            const QString renderer = name ? QString::fromLatin1(reinterpret_cast<const char *>(name)) : QString();
            gl->doneCurrent();
            Q_EMIT message(renderer.isEmpty() ? "OpenGL 绘制已启用" : QString("OpenGL 绘制 · %1").arg(renderer));
        });
    } else {
        setViewport(new QWidget);
    }
    viewport()->setMouseTracking(true);
    centerOn(center);
    Q_EMIT openGLViewportChanged(enabled);
}
#endif

void Canvas::addModule(const QString &type)
{
    const auto *definition = moduleDefinition(type);
    if (!definition)
        return;
    const QSizeF size = cardSize(*definition, {});
    QPointF position = mapToScene(viewport()->rect().center()) - QPointF(size.width() / 2, size.height() / 2);
    const int direction = position.y() >= 0 ? -1 : 1;
    while (std::any_of(nodes.cbegin(), nodes.cend(), [position, size](const NodeItem *node) {
        return QRectF(position, size).adjusted(-20, -20, 20, 20).intersects(node->sceneBoundingRect());
    })) {
        position.ry() += direction * (size.height() + 34);
    }
    addModuleAt(type, position);
}

void Canvas::addModuleAt(const QString &type, QPointF position)
{
    constexpr double limit = Graph::nodePositionLimit;
    position = {std::clamp(position.x(), -limit, limit), std::clamp(position.y(), -limit, limit)};
    const QUuid id = graph.addNode(type, position);
    if (id.isNull())
        return;
    addNodeItem(*graph.node(id));
    Q_EMIT topologyChanged();
    selectNode(id);
    ensureVisible(nodes.value(id), 28, 28);
    setFocus();
    Q_EMIT graphChanged();
    viewport()->update();
}

void Canvas::addNodeItem(const Node &node)
{
    auto *item = new NodeItem(node, colors);
    item->moved = [this, id = node.id](QPointF position) {
        graph.moveNode(id, position);
        updateEdges(id);
        Q_EMIT graphChanged();
    };
    nodes.insert(node.id, item);
    workspace->addItem(item);
}

void Canvas::addEdgeItem(const Connection &edge)
{
    auto *item = new EdgeItem(edge, *nodes.value(edge.from.node), *nodes.value(edge.to.node), colors);
    edges.insert(edge.id, item);
    incidentEdges.insert(edge.from.node, edge.id);
    incidentEdges.insert(edge.to.node, edge.id);
    workspace->addItem(item);
}

void Canvas::removeEdgeItem(const QUuid &id)
{
    if (EdgeItem *edge = edges.take(id)) {
        incidentEdges.remove(edge->link.from.node, id);
        incidentEdges.remove(edge->link.to.node, id);
        delete edge;
    }
}

void Canvas::updateEdges(const QUuid &node)
{
    const auto range = incidentEdges.equal_range(node);
    for (auto edge = range.first; edge != range.second; ++edge)
        edges.value(edge.value())->updatePath();
}

void Canvas::removeSelection()
{
    QSet<QUuid> selectedNodes;
    QSet<QUuid> selectedEdges;
    for (QGraphicsItem *item : workspace->selectedItems()) {
        if (auto *node = qgraphicsitem_cast<NodeItem *>(item)) {
            selectedNodes.insert(node->id);
            const auto linked = incidentEdges.values(node->id);
            selectedEdges.unite(QSet<QUuid>(linked.cbegin(), linked.cend()));
        } else if (auto *edge = qgraphicsitem_cast<EdgeItem *>(item)) {
            selectedEdges.insert(edge->id);
        }
    }
    if (selectedNodes.isEmpty() && selectedEdges.isEmpty())
        return;
    cancelWire();
    if (selectedNodes.contains(resizingNode)) {
        resizingNode = QUuid{};
        viewport()->unsetCursor();
    }
    for (const QUuid &id : selectedEdges) {
        graph.removeConnection(id);
        removeEdgeItem(id);
    }
    for (const QUuid &id : selectedNodes) {
        graph.removeNode(id);
        dirtyNodes.remove(id);
        changedViewports.remove(id);
        if (hoveredWaveform == id)
            hoveredWaveform = QUuid{};
        if (waveformDrag && waveformDrag->node == id) {
            waveformDrag.reset();
            viewport()->unsetCursor();
        }
        delete nodes.take(id);
    }
    Q_EMIT topologyChanged();
    Q_EMIT graphChanged();
    viewport()->update();
}

QUuid Canvas::selectedNode() const
{
    const auto selected = workspace->selectedItems();
    if (selected.size() == 1) {
        if (auto *node = qgraphicsitem_cast<NodeItem *>(selected.first()))
            return node->id;
    }
    return {};
}

int Canvas::selectedConnectionCount() const
{
    const auto selected = workspace->selectedItems();
    return std::count_if(selected.cbegin(), selected.cend(), [](QGraphicsItem *item) { return item->type() == EdgeItem::Type; });
}

void Canvas::selectNode(const QUuid &id)
{
    workspace->clearSelection();
    if (NodeItem *item = nodes.value(id))
        item->setSelected(true);
}

void Canvas::fitGraph()
{
    resetTransform();
    if (nodes.isEmpty()) {
        centerOn(0, 0);
    } else if (viewport()->width() > 80 && viewport()->height() > 80) {
        const QRectF bounds = workspace->itemsBoundingRect();
        const qreal factor = std::clamp(std::min((viewport()->width() - 64) / bounds.width(),
                                                (viewport()->height() - 64) / bounds.height()), minimumZoom, 1.25);
        scale(factor, factor);
        centerOn(bounds.center());
    }
    Q_EMIT zoomChanged(transform().m11() * 100);
}

void Canvas::setFailedNode(const QUuid &id, bool failed)
{
    if (NodeItem *item = nodes.value(id)) {
        item->failed = failed;
        item->presentValues();
    }
}

void Canvas::setNodeStatus(const QUuid &id, const QString &status)
{
    if (NodeItem *item = nodes.value(id)) {
        item->setStatus(status);
        scheduleRepaint(id);
    }
}

void Canvas::setNodeDisplayText(const QUuid &id, const QString &text)
{
    if (NodeItem *item = nodes.value(id); item && item->displayText() != text) {
        item->setDisplayText(text);
        scheduleRepaint(id);
    }
}

void Canvas::setNodeSpectrum(const QUuid &id, const SpectrumFrame &frame)
{
    if (NodeItem *item = nodes.value(id); item && item->definition.displaysSpectrum)
        item->setSpectrum(frame);
}

QString Canvas::nodeDisplayText(const QUuid &id) const
{
    const NodeItem *item = nodes.value(id);
    return item ? item->displayText() : QString();
}

void Canvas::sample(const QUuid &id, int channel, const QList<double> &values)
{
    if (NodeItem *item = nodes.value(id); item && !item->isPaused()) {
        QString error;
        item->push(channel, values, error);
        scheduleRepaint(id);
        scheduleRecordingViewport(id);
        if (!error.isEmpty())
            Q_EMIT message(QString("波形暂存失败：%1").arg(error));
    }
}

void Canvas::updateNodeSettings(const Node &node, const QList<QUuid> &removedEdges)
{
    for (const QUuid &edge : removedEdges)
        removeEdgeItem(edge);
    if (NodeItem *item = nodes.value(node.id); item && item->applySettings(node)) {
        cancelWire();
        updateEdges(node.id);
    }
    scheduleRecordingViewport(node.id);
}

bool Canvas::scopePaused(const QUuid &id) const
{
    const NodeItem *item = nodes.value(id);
    return item && item->isPaused();
}

void Canvas::setScopePaused(const QUuid &id, bool paused)
{
    if (NodeItem *item = nodes.value(id))
        item->setPaused(paused);
}

void Canvas::rearmScope(const QUuid &id)
{
    if (NodeItem *item = nodes.value(id))
        item->rearm();
}

qint64 Canvas::recordingSampleCount(const QUuid &id) const
{
    const NodeItem *item = nodes.value(id);
    return item ? item->recordingSamples() : 0;
}

void Canvas::fitRecording(const QUuid &id)
{
    if (NodeItem *item = nodes.value(id)) {
        item->fitRecording();
        scheduleRecordingViewport(id);
    }
}

void Canvas::clearRecording(const QUuid &id)
{
    if (NodeItem *item = nodes.value(id); item && item->definition.recordsWaveform) {
        item->clearRecording(*graph.node(id));
        Q_EMIT recordingUpdated(id, 0);
        changedViewports.remove(id);
        Q_EMIT recordingViewportChanged(id);
    }
}

WaveformCsvSnapshot Canvas::recordingCsv(const QUuid &id) const
{
    const NodeItem *item = nodes.value(id);
    return item ? item->recordingCsv() : WaveformCsvSnapshot();
}

WaveformPacket Canvas::recordingViewport(const QUuid &id) const
{
    const NodeItem *item = nodes.value(id);
    return item ? item->recordingViewport() : WaveformPacket{};
}

bool Canvas::exportRecordingImage(const QUuid &id, const QString &path, QString &error) const
{
    if (const NodeItem *item = nodes.value(id))
        return item->exportRecordingImage(path, error);
    error = "未找到波形记录器。";
    return false;
}

void Canvas::scheduleRepaint(const QUuid &id)
{
    dirtyNodes.insert(id);
    if (!repaintTimer.isActive())
        repaintTimer.start();
}

void Canvas::scheduleRecordingViewport(const QUuid &id)
{
    if (NodeItem *item = nodes.value(id); item && item->port(true, waveformViewportPort)) {
        changedViewports.insert(id);
        if (!waveformPublishTimer.isActive())
            waveformPublishTimer.start();
    }
}

void Canvas::flushSamples()
{
    for (const QUuid &id : std::as_const(dirtyNodes)) {
        if (NodeItem *item = nodes.value(id)) {
            item->presentValues();
            if (item->definition.displaysText)
                Q_EMIT displayTextPresented(id, item->displayText());
            if (item->definition.recordsWaveform)
                Q_EMIT recordingUpdated(id, item->recordingSamples());
        }
    }
    dirtyNodes.clear();
}

void Canvas::drawBackground(QPainter *painter, const QRectF &bounds)
{
    const qreal margin = 2 / transform().m11();
    const QRectF exposed = bounds.adjusted(-margin, -margin, margin, margin);
    painter->fillRect(exposed, colors.canvas);
    qreal spacing = 24;
    while (spacing * transform().m11() < 9.6)
        spacing *= 5;
    const qreal xFirst = std::floor(exposed.left() / spacing) * spacing;
    const qreal yFirst = std::floor(exposed.top() / spacing) * spacing;
    QVarLengthArray<QPointF, 1024> points;
    for (qreal x = xFirst; x <= exposed.right(); x += spacing) {
        for (qreal y = yFirst; y <= exposed.bottom(); y += spacing)
            points.append({x, y});
    }
    QPen pen(colors.grid);
    pen.setCosmetic(true);
    pen.setWidthF(1.5);
    pen.setCapStyle(Qt::RoundCap);
    painter->setPen(pen);
    if (!points.isEmpty())
        painter->drawPoints(points.constData(), points.size());
}

void Canvas::drawForeground(QPainter *painter, const QRectF &)
{
    if (!nodes.isEmpty())
        return;
    painter->save();
    painter->resetTransform();
    painter->setPen(colors.muted);
    painter->drawText(viewport()->rect(), Qt::AlignCenter, "双击左侧模块，或在画布上右键添加");
    painter->restore();
}

bool Canvas::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::Leave && !waveformDrag)
        clearWaveformHover();
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        if (const auto hit = portAt(help->pos())) {
            if (const NodeItem *item = nodes.value(hit->endpoint.node)) {
                if (const auto *port = item->port(hit->output, hit->endpoint.port)) {
                    QToolTip::showText(help->globalPos(), port->label, viewport());
                    return true;
                }
            }
        }
    }
    return QGraphicsView::viewportEvent(event);
}

void Canvas::wheelEvent(QWheelEvent *event)
{
    if (!resizingNode.isNull() || waveformDrag) {
        event->accept();
        return;
    }
    const QPoint cursor = event->position().toPoint();
    const QPoint angle = event->angleDelta();
    const QPoint pixel = event->pixelDelta();
    const qreal vertical = angle.y() ? angle.y() / 120.0 : pixel.y() / 90.0;
    const qreal horizontal = angle.x() ? angle.x() / 120.0 : pixel.x() / 90.0;
    if (!spaceHeld && !wireStart && panButton == Qt::NoButton) {
        if (auto *node = qgraphicsitem_cast<NodeItem *>(itemAt(cursor))) {
            const QPointF position = node->mapFromScene(mapToScene(cursor));
            const WaveformHit hit = node->recordingHit(position);
            if (hit.isAxis() && !node->onResizeGrip(position)) {
                const double steps = hit.region == WaveformRegion::TimeAxis && std::abs(horizontal) > std::abs(vertical)
                    ? horizontal : vertical;
                if (steps != 0 && node->adjustRecording(hit, position, steps, event->modifiers().testFlag(Qt::ShiftModifier)))
                    scheduleRecordingViewport(node->id);
                updateWaveformHover(cursor);
                event->accept();
                return;
            }
        }
    }
    const qreal steps = vertical;
    if (steps == 0) {
        event->ignore();
        return;
    }
    const QPointF anchor = mapToScene(cursor);
    const qreal current = transform().m11();
    const qreal next = std::clamp(current * std::pow(1.16, steps), minimumZoom, 2.6);
    scale(next / current, next / current);
    centerOn(mapToScene(viewport()->rect().center()) + anchor - mapToScene(cursor));
    if (wireStart)
        updateWire(cursor);
    else if (!spaceHeld && panButton == Qt::NoButton)
        updateWaveformHover(cursor);
    Q_EMIT zoomChanged(next * 100);
    event->accept();
}

std::optional<Canvas::PortHit> Canvas::portAt(const QPoint &position) const
{
    const QPointF scenePosition = mapToScene(position);
    for (QGraphicsItem *item : items(position)) {
        auto *node = qgraphicsitem_cast<NodeItem *>(item);
        if (!node)
            continue;
        for (bool output : {false, true}) {
            for (const PortDefinition &port : node->ports(output)) {
                if (QLineF(node->mapFromScene(scenePosition), node->portPosition(output, port.id)).length() <= 9)
                    return PortHit{{node->id, port.id}, output};
            }
        }
        break;
    }
    return std::nullopt;
}

QString Canvas::wireError(const PortHit &target) const
{
    if (wireStart->output == target.output)
        return "请连接一个输出端口和一个输入端口。";
    const Endpoint &from = wireStart->output ? wireStart->endpoint : target.endpoint;
    const Endpoint &to = wireStart->output ? target.endpoint : wireStart->endpoint;
    return graph.connectionError(from, to, true);
}

void Canvas::updateWire(const QPoint &position)
{
    const auto target = portAt(position);
    const NodeItem *item = nodes.value(wireStart->endpoint.node);
    const bool output = wireStart->output;
    const QPointF start = item->mapToScene(item->portPosition(output, wireStart->endpoint.port));
    const QPointF end = mapToScene(position);
    const auto *port = item->port(output, wireStart->endpoint.port);
    const QColor color = target && !wireError(*target).isEmpty() ? colors.error : portColor(port->type, colors);
    draftWire->setPen(QPen(color, 2, Qt::DashLine, Qt::RoundCap));
    draftWire->setPath(output ? wirePath(start, end) : wirePath(end, start));
}

void Canvas::cancelWire()
{
    if (!wireStart)
        return;
    wireStart.reset();
    delete draftWire;
    draftWire = nullptr;
    viewport()->setCursor(spaceHeld ? Qt::OpenHandCursor : Qt::ArrowCursor);
}

NodeItem *Canvas::resizeGripAt(const QPoint &position) const
{
    auto *node = qgraphicsitem_cast<NodeItem *>(itemAt(position));
    return node && node->onResizeGrip(node->mapFromScene(mapToScene(position))) ? node : nullptr;
}

void Canvas::resizeCard(const QPoint &position)
{
    if (NodeItem *node = nodes.value(resizingNode)) {
        const QPointF delta = mapToScene(position) - resizeOrigin;
        const QSizeF size = node->fittedResize(resizeInitialSize + QSizeF(delta.x(), delta.y()));
        if (size != node->card().size() && graph.resizeNode(node->id, size)) {
            node->resizeCard(size);
            updateEdges(node->id);
            Q_EMIT graphChanged();
        }
    }
}

void Canvas::clearWaveformHover()
{
    if (NodeItem *item = nodes.value(hoveredWaveform))
        item->setRecordingHover({});
    hoveredWaveform = QUuid{};
}

void Canvas::updateWaveformHover(const QPoint &position)
{
    auto *node = qgraphicsitem_cast<NodeItem *>(itemAt(position));
    if (node && !node->onResizeGrip(node->mapFromScene(mapToScene(position)))) {
        const QPointF local = node->mapFromScene(mapToScene(position));
        const WaveformHit hit = node->recordingHit(local);
        if (hit.region != WaveformRegion::None) {
            if (hoveredWaveform != node->id)
                clearWaveformHover();
            hoveredWaveform = node->id;
            const qreal radius = std::clamp(18 / transform().m11(), 7.0, 36.0);
            node->setRecordingHover({hit, local, radius});
            viewport()->setCursor(hit.region == WaveformRegion::TimeAxis ? Qt::SizeHorCursor
                : hit.region == WaveformRegion::ValueAxis ? Qt::SizeVerCursor : Qt::CrossCursor);
            return;
        }
    }
    clearWaveformHover();
    viewport()->setCursor(resizeGripAt(position) ? Qt::SizeFDiagCursor : Qt::ArrowCursor);
}

void Canvas::mousePressEvent(QMouseEvent *event)
{
    const QPoint position = event->position().toPoint();
    if (panButton != Qt::NoButton || wireStart || !resizingNode.isNull() || waveformDrag) {
        event->accept();
        return;
    }
    if (event->button() == Qt::MiddleButton || (spaceHeld && event->button() == Qt::LeftButton)) {
        setFocus();
        clearWaveformHover();
        panButton = event->button();
        panPosition = position;
        viewport()->setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        if (NodeItem *node = resizeGripAt(position)) {
            setFocus();
            clearWaveformHover();
            resizingNode = node->id;
            if (!node->isSelected())
                selectNode(node->id);
            resizeOrigin = mapToScene(position);
            resizeInitialSize = node->card().size();
            viewport()->setCursor(Qt::SizeFDiagCursor);
            event->accept();
            return;
        }
        if (const auto port = portAt(position)) {
            setFocus();
            clearWaveformHover();
            wireStart = port;
            draftWire = workspace->addPath({});
            draftWire->setZValue(-0.5);
            draftWire->setAcceptedMouseButtons(Qt::NoButton);
            viewport()->setCursor(Qt::CrossCursor);
            updateWire(position);
            event->accept();
            return;
        }
        if (auto *node = qgraphicsitem_cast<NodeItem *>(itemAt(position))) {
            const QPointF local = node->mapFromScene(mapToScene(position));
            const WaveformHit hit = node->recordingHit(local);
            if (hit.isAxis()) {
                setFocus();
                if (!node->isSelected())
                    selectNode(node->id);
                updateWaveformHover(position);
                waveformDrag = WaveformDrag{node->id, hit, local};
                event->accept();
                return;
            }
        }
    }
    QGraphicsView::mousePressEvent(event);
}

void Canvas::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint position = event->position().toPoint();
    if (panButton != Qt::NoButton) {
        const QPoint delta = position - panPosition;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        panPosition = position;
        event->accept();
    } else if (wireStart) {
        updateWire(position);
        event->accept();
    } else if (!resizingNode.isNull()) {
        resizeCard(position);
        event->accept();
    } else if (waveformDrag) {
        if (NodeItem *node = nodes.value(waveformDrag->node)) {
            const QPointF local = node->mapFromScene(mapToScene(position));
            if (node->dragRecording(waveformDrag->hit, local - waveformDrag->previous))
                scheduleRecordingViewport(node->id);
            waveformDrag->previous = local;
        }
        event->accept();
    } else {
        if (!spaceHeld && !event->buttons())
            updateWaveformHover(position);
        else if (event->buttons())
            clearWaveformHover();
        QGraphicsView::mouseMoveEvent(event);
    }
}

void Canvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (waveformDrag && event->button() == Qt::LeftButton) {
        const QPoint position = event->position().toPoint();
        if (NodeItem *node = nodes.value(waveformDrag->node)) {
            const QPointF local = node->mapFromScene(mapToScene(position));
            if (node->dragRecording(waveformDrag->hit, local - waveformDrag->previous))
                scheduleRecordingViewport(node->id);
        }
        waveformDrag.reset();
        if (spaceHeld) {
            clearWaveformHover();
            viewport()->setCursor(Qt::OpenHandCursor);
        } else {
            updateWaveformHover(position);
        }
        event->accept();
        return;
    }
    if (!resizingNode.isNull() && event->button() == Qt::LeftButton) {
        const QPoint position = event->position().toPoint();
        resizeCard(position);
        resizingNode = QUuid{};
        viewport()->setCursor(spaceHeld ? Qt::OpenHandCursor : resizeGripAt(position) ? Qt::SizeFDiagCursor : Qt::ArrowCursor);
        event->accept();
        return;
    }
    if (event->button() == panButton) {
        panButton = Qt::NoButton;
        viewport()->setCursor(spaceHeld ? Qt::OpenHandCursor : Qt::ArrowCursor);
        event->accept();
        return;
    }
    if (wireStart && event->button() == Qt::LeftButton) {
        const auto target = portAt(event->position().toPoint());
        if (target && (!(target->endpoint == wireStart->endpoint) || target->output != wireStart->output)) {
            QString error = wireError(*target);
            if (error.isEmpty()) {
                const Endpoint &from = wireStart->output ? wireStart->endpoint : target->endpoint;
                const Endpoint &to = wireStart->output ? target->endpoint : wireStart->endpoint;
                QUuid replaced;
                const QUuid id = graph.connect(from, to, &replaced, error);
                if (!id.isNull()) {
                    removeEdgeItem(replaced);
                    addEdgeItem(graph.connections().value(id));
                    Q_EMIT topologyChanged();
                    Q_EMIT graphChanged();
                    Q_EMIT message(replaced.isNull() ? "已连接端口" : "已替换输入连接");
                }
            }
            if (!error.isEmpty())
                Q_EMIT message(error);
        }
        cancelWire();
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void Canvas::contextMenuEvent(QContextMenuEvent *event)
{
    if (wireStart || !resizingNode.isNull() || waveformDrag)
        return;
    QMenu menu(this);
    if (QGraphicsItem *item = itemAt(event->pos()); item && (item->flags() & QGraphicsItem::ItemIsSelectable)) {
        if (!item->isSelected()) {
            workspace->clearSelection();
            item->setSelected(true);
        }
        menu.addAction("删除选中项", this, &Canvas::removeSelection);
    } else {
        const QPointF position = mapToScene(event->pos());
        for (const ModuleDefinition &definition : moduleDefinitions()) {
            menu.addAction(definition.name, this, [this, id = definition.id, position] { addModuleAt(id, position); });
        }
    }
    menu.exec(event->globalPos());
}

void Canvas::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Delete) {
        removeSelection();
    } else if (event->key() == Qt::Key_Space) {
        if (!event->isAutoRepeat()) {
            spaceHeld = true;
            if (!wireStart && resizingNode.isNull() && panButton == Qt::NoButton && !waveformDrag) {
                clearWaveformHover();
                viewport()->setCursor(Qt::OpenHandCursor);
            }
        }
    } else if (event->key() == Qt::Key_Escape && wireStart) {
        cancelWire();
    } else {
        QGraphicsView::keyPressEvent(event);
        return;
    }
    event->accept();
}

void Canvas::keyReleaseEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space) {
        if (!event->isAutoRepeat()) {
            spaceHeld = false;
            if (!wireStart && resizingNode.isNull() && panButton == Qt::NoButton && !waveformDrag)
                updateWaveformHover(viewport()->mapFromGlobal(QCursor::pos()));
        }
        event->accept();
    } else {
        QGraphicsView::keyReleaseEvent(event);
    }
}

void Canvas::focusOutEvent(QFocusEvent *event)
{
    spaceHeld = false;
    panButton = Qt::NoButton;
    resizingNode = QUuid{};
    waveformDrag.reset();
    clearWaveformHover();
    cancelWire();
    viewport()->unsetCursor();
    QGraphicsView::focusOutEvent(event);
}
