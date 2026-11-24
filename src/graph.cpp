#include "graph.h"
#include "scope.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMultiHash>
#include <QSaveFile>
#include <QSet>
#include <cmath>

namespace {
QString uuidText(const QUuid &id) { return id.toString(QUuid::WithoutBraces); }

QJsonObject endpointJson(const Endpoint &endpoint)
{
    return {{"node", uuidText(endpoint.node)}, {"port", endpoint.port}};
}

Endpoint readEndpoint(const QJsonValue &value)
{
    const QJsonObject object = value.toObject();
    return {QUuid(object.value("node").toString()), object.value("port").toString()};
}

QJsonObject defaultParameters(const ModuleDefinition &definition)
{
    QJsonObject parameters;
    for (const ParameterDefinition &parameter : definition.parameters)
        parameters.insert(parameter.id, parameter.initial);
    return parameters;
}

void normalizeParameters(const ModuleDefinition &definition, QJsonObject &parameters)
{
    if (definition.scope && parameters.value("triggerChannel").toInt() > scopeChannels(parameters))
        parameters.insert("triggerChannel", scopeChannels(parameters));
}

bool validScopeSize(QSizeF size)
{
    return size.width() >= Graph::minScopeWidth && size.width() <= Graph::maxScopeSize
        && size.height() >= Graph::minScopeHeight && size.height() <= Graph::maxScopeSize;
}
}

const Node *Graph::node(const QUuid &id) const
{
    const auto found = nodes_.constFind(id);
    return found == nodes_.cend() ? nullptr : &found.value();
}

QUuid Graph::addNode(const QString &type, QPointF position)
{
    const ModuleDefinition *definition = moduleDefinition(type);
    if (!definition)
        return {};
    const QUuid id = QUuid::createUuid();
    nodes_.insert(id, Node{id, type, position, defaultParameters(*definition), {}});
    return id;
}

void Graph::moveNode(const QUuid &id, QPointF position)
{
    const auto found = nodes_.find(id);
    if (found != nodes_.end())
        found->position = position;
}

bool Graph::resizeNode(const QUuid &id, QSizeF size)
{
    const auto found = nodes_.find(id);
    if (found == nodes_.end() || !validScopeSize(size) || found->customSize == size)
        return false;
    const auto *definition = moduleDefinition(found->type);
    if (!definition->scope && !definition->recordsWaveform)
        return false;
    found->customSize = size;
    return true;
}

bool Graph::setParameter(const QUuid &id, const QString &name, const QJsonValue &value, QList<QUuid> *removed)
{
    const auto found = nodes_.find(id);
    if (found == nodes_.end())
        return false;
    const ModuleDefinition &definition = *moduleDefinition(found->type);
    const auto *parameter = parameterDefinition(definition, name);
    if (!parameter || !acceptsParameter(*parameter, value) || found->parameters.value(name) == value)
        return false;
    found->parameters.insert(name, value);
    normalizeParameters(definition, found->parameters);
    if (parameter->affectsPorts) {
        QSet<QString> inputs;
        QSet<QString> outputs;
        for (const auto &port : modulePorts(definition, found->parameters, false))
            inputs.insert(port.id);
        for (const auto &port : modulePorts(definition, found->parameters, true))
            outputs.insert(port.id);
        for (auto edge = connections_.begin(); edge != connections_.end();) {
            const bool missingInput = edge->to.node == id && !inputs.contains(edge->to.port);
            const bool missingOutput = edge->from.node == id && !outputs.contains(edge->from.port);
            if (missingInput || missingOutput) {
                if (removed)
                    removed->append(edge.key());
                edge = connections_.erase(edge);
            } else {
                ++edge;
            }
        }
    }
    return true;
}

void Graph::removeNode(const QUuid &id)
{
    for (auto it = connections_.begin(); it != connections_.end();) {
        if (it->from.node == id || it->to.node == id)
            it = connections_.erase(it);
        else
            ++it;
    }
    nodes_.remove(id);
}

void Graph::removeConnection(const QUuid &id) { connections_.remove(id); }

QString Graph::connectionError(const Endpoint &from, const Endpoint &to, bool replaceInput) const
{
    const Node *source = node(from.node);
    const Node *target = node(to.node);
    const auto output = source ? portDefinition(*moduleDefinition(source->type), source->parameters, true, from.port) : std::nullopt;
    const auto input = target ? portDefinition(*moduleDefinition(target->type), target->parameters, false, to.port) : std::nullopt;
    if (!output || !input)
        return "请连接一个输出端口和一个输入端口。";
    if (output->type != input->type && input->type != DataType::Any)
        return "端口的数据类型不匹配。";
    if (from.node == to.node)
        return "端口必须属于不同的模块。";

    QMultiHash<QUuid, QUuid> downstream;
    for (const Connection &connection : connections_) {
        if (connection.to == to) {
            if (connection.from == from)
                return "这两个端口已经相连。";
            if (!replaceInput)
                return "每个输入端口只能连接一个输出。";
            continue;
        }
        downstream.insert(connection.from.node, connection.to.node);
    }

    QList<QUuid> pending{to.node};
    QSet<QUuid> visited{to.node};
    while (!pending.isEmpty()) {
        const QUuid current = pending.takeLast();
        if (current == from.node)
            return "这条连接会形成反馈环路。";
        for (const QUuid &next : downstream.values(current)) {
            if (!visited.contains(next)) {
                visited.insert(next);
                pending.append(next);
            }
        }
    }
    return {};
}

QUuid Graph::connect(const Endpoint &from, const Endpoint &to, QUuid *replaced, QString &error)
{
    error = connectionError(from, to, replaced != nullptr);
    if (!error.isEmpty())
        return {};

    if (replaced) {
        *replaced = QUuid{};
        for (auto it = connections_.begin(); it != connections_.end(); ++it) {
            if (it->to == to) {
                *replaced = it.key();
                connections_.erase(it);
                break;
            }
        }
    }
    const QUuid id = QUuid::createUuid();
    connections_.insert(id, Connection{id, from, to});
    return id;
}

bool Graph::save(const QString &path, QString &error) const
{
    QJsonArray nodes;
    for (const Node &node : nodes_) {
        QJsonObject entry{{"id", uuidText(node.id)}, {"type", node.type},
                          {"x", node.position.x()}, {"y", node.position.y()}, {"parameters", node.parameters}};
        if (node.customSize.isValid())
            entry.insert("size", QJsonObject{{"width", node.customSize.width()}, {"height", node.customSize.height()}});
        nodes.append(entry);
    }
    QJsonArray connections;
    for (const Connection &edge : connections_) {
        connections.append(QJsonObject{{"id", uuidText(edge.id)}, {"from", endpointJson(edge.from)},
                                       {"to", endpointJson(edge.to)}});
    }
    const QJsonDocument document(QJsonObject{{"format", "FantasyHost"}, {"version", 1},
                                             {"nodes", nodes}, {"connections", connections}});
    const QByteArray data = document.toJson();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        error = file.errorString();
        return false;
    }
    return true;
}

bool Graph::load(const QString &path, Graph &graph, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (file.error() != QFile::NoError) {
        error = file.errorString();
        return false;
    }
    if (parseError.error != QJsonParseError::NoError) {
        error = QString("工程文件 JSON 无法解析：%1").arg(parseError.errorString());
        return false;
    }
    const QJsonObject root = document.object();
    if (root.value("format") != QJsonValue("FantasyHost") || root.value("version") != QJsonValue(1)
        || !root.value("nodes").isArray() || !root.value("connections").isArray()) {
        error = "无法识别工程格式或版本。";
        return false;
    }

    Graph candidate;
    for (const QJsonValue &entry : root.value("nodes").toArray()) {
        const QJsonObject object = entry.toObject();
        const QUuid id(object.value("id").toString());
        const QString type = object.value("type").toString();
        const ModuleDefinition *definition = moduleDefinition(type);
        if (!definition) {
            error = QString("工程包含未知模块类型：%1").arg(type);
            return false;
        }
        const QJsonValue x = object.value("x");
        const QJsonValue y = object.value("y");
        if (id.isNull() || candidate.nodes_.contains(id) || !x.isDouble() || !y.isDouble()
            || std::abs(x.toDouble()) > nodePositionLimit || std::abs(y.toDouble()) > nodePositionLimit
            || !object.value("parameters").isObject()) {
            error = QString("模块 %1 的标识、坐标或参数格式无效。").arg(definition->name);
            return false;
        }
        QJsonObject parameters = defaultParameters(*definition);
        const QJsonObject values = object.value("parameters").toObject();
        for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
            const auto *parameter = parameterDefinition(*definition, it.key());
            if (!parameter || !acceptsParameter(*parameter, it.value())) {
                error = QString("模块 %1 的参数 %2 无效。").arg(definition->name, it.key());
                return false;
            }
            parameters.insert(it.key(), it.value());
        }
        normalizeParameters(*definition, parameters);
        QSizeF size;
        const QJsonValue layoutSize = object.value("size");
        if (!layoutSize.isUndefined()) {
            const QJsonObject dimensions = layoutSize.toObject();
            size = {dimensions.value("width").toDouble(), dimensions.value("height").toDouble()};
            if ((!definition->scope && !definition->recordsWaveform) || !validScopeSize(size)) {
                error = QString("模块 %1 的画布尺寸无效。").arg(definition->name);
                return false;
            }
        }
        candidate.nodes_.insert(id, Node{id, type, {x.toDouble(), y.toDouble()}, parameters, size});
    }

    for (const QJsonValue &entry : root.value("connections").toArray()) {
        const QJsonObject object = entry.toObject();
        const QUuid id(object.value("id").toString());
        if (id.isNull() || candidate.connections_.contains(id)) {
            error = "工程包含无效或重复的连线标识。";
            return false;
        }
        const Endpoint from = readEndpoint(object.value("from"));
        const Endpoint to = readEndpoint(object.value("to"));
        error = candidate.connectionError(from, to, false);
        if (!error.isEmpty())
            return false;
        candidate.connections_.insert(id, Connection{id, from, to});
    }
    graph = std::move(candidate);
    return true;
}
