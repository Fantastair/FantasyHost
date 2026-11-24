#pragma once

#include "graph.h"
#include "theme.h"

#include <QGraphicsView>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <optional>

class NodeItem;
class EdgeItem;

class Canvas : public QGraphicsView {
    Q_OBJECT

public:
    explicit Canvas(Graph &graph, QWidget *parent = nullptr);
    void rebuild();
    void addModule(const QString &type);
    void fitGraph();
    void removeSelection();
    QUuid selectedNode() const;
    int selectedConnectionCount() const;
    void selectNode(const QUuid &id);
    void setFailedNode(const QUuid &id, bool failed);
    void setNodeStatus(const QUuid &id, const QString &status);
    void setNodeDisplayText(const QUuid &id, const QString &text);
    QString nodeDisplayText(const QUuid &id) const;
    void sample(const QUuid &id, int channel, const QList<double> &values);
    void updateNodeSettings(const Node &node, const QList<QUuid> &removedEdges);
    bool scopePaused(const QUuid &id) const;
    void setScopePaused(const QUuid &id, bool paused);
    void rearmScope(const QUuid &id);
    void setTheme(const ThemeColors &theme);
#ifdef FANTASYHOST_OPENGL
    void setOpenGLViewport(bool enabled);
#endif

Q_SIGNALS:
    void graphChanged();
    void topologyChanged();
    void selectionChanged();
    void message(const QString &text);
    void zoomChanged(double percent);
    void displayTextPresented(const QUuid &id, const QString &text);
#ifdef FANTASYHOST_OPENGL
    void openGLViewportChanged(bool enabled);
#endif

protected:
    void drawBackground(QPainter *painter, const QRectF &bounds) override;
    void drawForeground(QPainter *painter, const QRectF &) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    struct PortHit {
        Endpoint endpoint;
        bool output;
    };

    void addModuleAt(const QString &type, QPointF position);
    void addNodeItem(const Node &node);
    void addEdgeItem(const Connection &edge);
    void removeEdgeItem(const QUuid &id);
    void updateEdges(const QUuid &node);
    std::optional<PortHit> portAt(const QPoint &position) const;
    QString wireError(const PortHit &target) const;
    void updateWire(const QPoint &position);
    void cancelWire();
    NodeItem *resizeGripAt(const QPoint &position) const;
    void resizeCard(const QPoint &position);
    void scheduleRepaint(const QUuid &id);
    void flushSamples();

    Graph &graph;
    ThemeColors colors = themeColors(false);
    QGraphicsScene *workspace;
    QHash<QUuid, NodeItem *> nodes;
    QHash<QUuid, EdgeItem *> edges;
    QMultiHash<QUuid, QUuid> incidentEdges;
    QSet<QUuid> dirtyNodes;
    QTimer repaintTimer;
    std::optional<PortHit> wireStart;
    QGraphicsPathItem *draftWire = nullptr;
    QUuid resizingNode;
    QPointF resizeOrigin;
    QSizeF resizeInitialSize;
    QPoint panPosition;
    Qt::MouseButton panButton = Qt::NoButton;
    bool spaceHeld = false;
};
