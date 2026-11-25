#pragma once

#include "modules.h"

#include <QPointF>
#include <QRectF>

class QPainter;
struct ThemeColors;

struct SpectrumFrame {
    QList<QPointF> bins;
    QString status;
    double frequencyLimit = 0;
    double binWidth = 0;
    QPointF peak;
    qsizetype samples = 0;
    bool decibels = false;
};

ModuleDefinition spectrumDefinition();
void paintSpectrum(QPainter &painter, const QRectF &chart, const SpectrumFrame &frame, const ThemeColors &colors);
