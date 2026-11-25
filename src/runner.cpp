#include "runner.h"
#include "spectrum.h"

#include <QSet>
#include <QTimer>

GraphRunner::GraphRunner(QObject *parent) : QObject(parent) {}

GraphRunner::~GraphRunner() { reset(); }

void GraphRunner::sync(const Graph &graph)
{
    for (auto entry = modules.begin(); entry != modules.end();) {
        if (graph.nodes().contains(entry->first)) {
            ++entry;
        } else {
            entry->second.active = false;
            entry->second.module->close();
            statuses.remove(entry->first);
            entry = modules.erase(entry);
        }
    }
    for (const Node &node : graph.nodes()) {
        if (modules.find(node.id) == modules.end())
            add(node);
    }

    // A fragment from the previous source must not become part of a new stream.
    for (const Connection &edge : connections) {
        if (!graph.connections().contains(edge.id))
            resetInput(edge.to);
    }
    routes.clear();
    QSet<QUuid> waveformSources;
    for (const Connection &edge : graph.connections()) {
        if (!connections.contains(edge.id)) {
            resetInput(edge.to);
            if (edge.from.port == waveformViewportPort)
                waveformSources.insert(edge.from.node);
        }
        routes.insert(edge.from.node, edge);
    }
    connections = graph.connections();
    for (const QUuid &source : waveformSources)
        Q_EMIT waveformRequested(source);
}

void GraphRunner::add(const Node &node)
{
    const QUuid id = node.id;
    const ModuleDefinition &definition = *moduleDefinition(node.type);
    auto module = definition.create({
        [this, id](const QString &output, const Packet &packet) { publish(id, output, packet); },
        [this, id](const QString &status) { setStatus(id, status); },
        [this, id](int channel, const QList<double> &values) { Q_EMIT samplesReceived(id, channel, values); },
        [this, id](const QByteArray &bytes) { if (isOpen(id)) Q_EMIT serialReceived(id, bytes); },
        [this, id](const QString &message) { fail(id, message); },
        [this, id](const QString &text) { Q_EMIT textReceived(id, text); },
        [this, id](const SpectrumFrame &frame) { Q_EMIT spectrumReceived(id, frame); },
    });
    module->setParameters(node.parameters);
    modules.emplace(id, Entry{std::move(module), definition.manualOpen, !definition.manualOpen});
    setStatus(id, definition.manualOpen ? "串口已关闭" : "等待输入");
}

void GraphRunner::reset()
{
    for (auto &entry : modules) {
        entry.second.active = false;
        entry.second.module->close();
    }
    modules.clear();
    connections.clear();
    routes.clear();
    pending.clear();
    statuses.clear();
}

bool GraphRunner::open(const QUuid &id, QString &error)
{
    const auto found = modules.find(id);
    if (found == modules.end() || !found->second.manualOpen)
        return false;
    Entry &entry = found->second;
    if (entry.closing) {
        error = "串口正在关闭，请稍后再试。";
        return false;
    }
    if (entry.active)
        return true;
    if (!entry.module->open(error)) {
        setStatus(id, error);
        return false;
    }
    entry.active = true;
    entry.session = ++nextSession;
    resetOutputs(id);
    return true;
}

void GraphRunner::close(const QUuid &id)
{
    const auto found = modules.find(id);
    if (found == modules.end() || !found->second.manualOpen)
        return;
    Entry &entry = found->second;
    entry.active = false;
    entry.closing = false;
    entry.module->close();
    resetOutputs(id);
    setStatus(id, "串口已关闭");
}

bool GraphRunner::isOpen(const QUuid &id) const
{
    const auto found = modules.find(id);
    return found != modules.end() && found->second.manualOpen && found->second.active && !found->second.closing;
}

void GraphRunner::updateParameters(const Node &node)
{
    const auto found = modules.find(node.id);
    if (found != modules.end())
        found->second.module->setParameters(node.parameters);
}

bool GraphRunner::hasWaveformReceivers(const QUuid &id) const
{
    const auto range = routes.equal_range(id);
    for (auto route = range.first; route != range.second; ++route) {
        if (route->from.port == waveformViewportPort)
            return true;
    }
    return false;
}

void GraphRunner::publishWaveform(const QUuid &id, const WaveformPacket &packet)
{
    publish(id, waveformViewportPort, packet);
}

bool GraphRunner::sendManual(const QUuid &id, const QString &text, QString &error)
{
    if (!isOpen(id)) {
        error = "打开此模块的串口后即可发送。";
        return false;
    }
    return modules.at(id).module->sendManual(text, error);
}

void GraphRunner::publish(const QUuid &id, const QString &output, const Packet &packet)
{
    const auto source = modules.find(id);
    if (source == modules.end() || !source->second.active || source->second.closing)
        return;
    const auto range = routes.equal_range(id);
    for (auto route = range.first; route != range.second; ++route) {
        if (route->from.port == output)
            pending.enqueue({route->to, packet});
    }
    if (delivering)
        return;

    delivering = true;
    while (!pending.isEmpty()) {
        const Delivery delivery = pending.dequeue();
        const auto target = modules.find(delivery.target.node);
        if (target != modules.end() && target->second.active && !target->second.closing)
            target->second.module->receive(delivery.target.port, delivery.packet);
    }
    delivering = false;
}

void GraphRunner::resetInput(const Endpoint &target)
{
    const auto found = modules.find(target.node);
    if (found != modules.end())
        found->second.module->resetInput(target.port);
}

void GraphRunner::resetOutputs(const QUuid &source)
{
    const auto range = routes.equal_range(source);
    for (auto route = range.first; route != range.second; ++route)
        resetInput(route->to);
}

void GraphRunner::setStatus(const QUuid &id, const QString &status)
{
    if (statuses.value(id) != status) {
        statuses.insert(id, status);
        Q_EMIT statusChanged(id, status);
    }
}

void GraphRunner::fail(const QUuid &id, const QString &message)
{
    if (!isOpen(id))
        return;
    Entry &entry = modules.at(id);
    entry.closing = true;
    // An error can arrive from inside QSerialPort's own signal handler.
    QTimer::singleShot(0, this, [this, id, message, session = entry.session] {
        const auto found = modules.find(id);
        if (found == modules.end() || !found->second.closing || found->second.session != session)
            return;
        found->second.active = false;
        found->second.closing = false;
        found->second.module->close();
        resetOutputs(id);
        setStatus(id, message);
        Q_EMIT failed(id, message);
    });
}
