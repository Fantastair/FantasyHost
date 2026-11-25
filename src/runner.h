#pragma once

#include "graph.h"
#include "spectrum.h"

#include <QMultiHash>
#include <QObject>
#include <QQueue>
#include <map>

class GraphRunner : public QObject {
    Q_OBJECT

public:
    explicit GraphRunner(QObject *parent = nullptr);
    ~GraphRunner() override;

    void sync(const Graph &graph);
    void reset();
    bool open(const QUuid &id, QString &error);
    void close(const QUuid &id);
    bool isOpen(const QUuid &id) const;
    void updateParameters(const Node &node);
    bool hasWaveformReceivers(const QUuid &id) const;
    void publishWaveform(const QUuid &id, const WaveformPacket &packet);
    bool sendManual(const QUuid &id, const QString &text, QString &error);
    QString status(const QUuid &id) const { return statuses.value(id); }

Q_SIGNALS:
    void samplesReceived(const QUuid &id, int channel, const QList<double> &values);
    void serialReceived(const QUuid &id, const QByteArray &bytes);
    void textReceived(const QUuid &id, const QString &text);
    void spectrumReceived(const QUuid &id, const SpectrumFrame &frame);
    void waveformRequested(const QUuid &id);
    void statusChanged(const QUuid &id, const QString &status);
    void failed(const QUuid &id, const QString &message);

private:
    struct Delivery {
        Endpoint target;
        Packet packet;
    };
    struct Entry {
        std::unique_ptr<Module> module;
        bool manualOpen;
        bool active;
        bool closing = false;
        quint64 session = 0;
    };

    void add(const Node &node);
    void publish(const QUuid &id, const QString &output, const Packet &packet);
    void resetInput(const Endpoint &target);
    void resetOutputs(const QUuid &source);
    void setStatus(const QUuid &id, const QString &status);
    void fail(const QUuid &id, const QString &message);

    std::map<QUuid, Entry> modules;
    QMap<QUuid, Connection> connections;
    QMultiHash<QUuid, Connection> routes;
    QMap<QUuid, QString> statuses;
    QQueue<Delivery> pending;
    quint64 nextSession = 0;
    bool delivering = false;
};
