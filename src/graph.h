#pragma once

#include "modules.h"

#include <QMap>
#include <QPointF>
#include <QSizeF>
#include <QUuid>

struct Node {
    QUuid id;
    QString type;
    QPointF position;
    QJsonObject parameters;
    QSizeF customSize;
};

struct Endpoint {
    QUuid node;
    QString port;
    bool operator==(const Endpoint &other) const { return node == other.node && port == other.port; }
};

struct Connection {
    QUuid id;
    Endpoint from;
    Endpoint to;
};

class Graph {
public:
    static constexpr double canvasLimit = 10000000;
    static constexpr double nodePositionLimit = canvasLimit - 300;
    static constexpr double minScopeWidth = 268;
    static constexpr double minScopeHeight = 226;
    static constexpr double maxScopeSize = 4096;

    const QMap<QUuid, Node> &nodes() const { return nodes_; }
    const QMap<QUuid, Connection> &connections() const { return connections_; }
    const Node *node(const QUuid &id) const;

    QUuid addNode(const QString &type, QPointF position);
    void moveNode(const QUuid &id, QPointF position);
    bool resizeNode(const QUuid &id, QSizeF size);
    bool setParameter(const QUuid &id, const QString &name, const QJsonValue &value, QList<QUuid> *removed = nullptr);
    void removeNode(const QUuid &id);
    void removeConnection(const QUuid &id);

    QString connectionError(const Endpoint &from, const Endpoint &to, bool replaceInput) const;
    QUuid connect(const Endpoint &from, const Endpoint &to, QUuid *replaced, QString &error);

    bool save(const QString &path, QString &error) const;
    static bool load(const QString &path, Graph &graph, QString &error);

private:
    QMap<QUuid, Node> nodes_;
    QMap<QUuid, Connection> connections_;
};
