#pragma once

#include <QColor>
#include <QIcon>
#include <QPalette>

class QPainter;
class QRectF;

struct ThemeColors {
    QColor surface;         // Toolbars and side panels
    QColor canvas;
    QColor card;
    QColor border;
    QColor grid;
    QColor text;
    QColor muted;
    QColor disabled;
    QColor accent;
    QColor onAccent;
    QColor accentHover;
    QColor hover;
    QColor selection;
    QColor number;
    QColor bytes;
    QColor error;
    QColor plot;
    QColor shadow;
    QColor control;
};

enum class WorkspaceIcon {
    New, Open, Save, Fit, Float, Close, Minimize, Maximize, Restore,
    Serial, Waveform, Recorder, Spectrum, Filter, Text, Control, Play, Pause, Stop,
};

const ThemeColors &themeColors(bool dark);
QPalette themePalette(const ThemeColors &colors);
QString workspaceStyle(const ThemeColors &colors);
QIcon workspaceIcon(WorkspaceIcon symbol, const QColor &color);
void paintWorkspaceIcon(QPainter &painter, WorkspaceIcon symbol, const QRectF &rect, const QColor &color);
