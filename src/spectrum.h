#pragma once

#include "modules.h"

#include <QPointF>
#include <QRectF>

class QPainter;
struct ThemeColors;

struct SpectrumTrace {
    int channel = 0;
    QList<QPointF> bins;
    QString status;
    double frequencyLimit = 0;
    double binWidth = 0;
    QPointF peak;
    qsizetype samples = 0;
};

struct SpectrumFrame {
    QList<SpectrumTrace> traces;
    QString status;
    bool decibels = false;
};

ModuleDefinition spectrumDefinition();
void paintSpectrum(QPainter &painter, const QRectF &chart, const SpectrumFrame &frame, const ThemeColors &colors);
