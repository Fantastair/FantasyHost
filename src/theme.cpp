#include "theme.h"

#include <QPainter>
#include <QPixmap>
#include <initializer_list>
#include <utility>

const ThemeColors &themeColors(bool dark)
{
    static const ThemeColors light{
        QColor("#fafbfd"), QColor("#edf1f7"), QColor("#ffffff"), QColor("#d6dfea"),
        QColor("#afbdd0"), QColor("#1d293b"), QColor("#4f6076"), QColor("#6d7d91"),
        QColor("#2855b5"), QColor("#ffffff"), QColor("#21489d"), QColor("#eaf0f8"),
        QColor("#e4edff"), QColor("#177971"), QColor("#92530d"), QColor("#b23541"),
        QColor("#f1f6fa"), QColor(28, 49, 78, 18),
    };
    static const ThemeColors darkColors{
        QColor("#1c2532"), QColor("#131c28"), QColor("#232f40"), QColor("#38485d"),
        QColor("#42536b"), QColor("#e8eff9"), QColor("#afbed2"), QColor("#91a2b9"),
        QColor("#a0bfff"), QColor("#142749"), QColor("#b7ccff"), QColor("#2a384b"),
        QColor("#2c4061"), QColor("#75d6cb"), QColor("#eec188"), QColor("#ffabb1"),
        QColor("#172333"), QColor(0, 0, 0, 54),
    };
    return dark ? darkColors : light;
}

QPalette themePalette(const ThemeColors &colors)
{
    QPalette palette;
    palette.setColor(QPalette::Window, colors.surface);
    palette.setColor(QPalette::WindowText, colors.text);
    palette.setColor(QPalette::Base, colors.card);
    palette.setColor(QPalette::AlternateBase, colors.hover);
    palette.setColor(QPalette::Text, colors.text);
    palette.setColor(QPalette::PlaceholderText, colors.muted);
    palette.setColor(QPalette::Button, colors.surface);
    palette.setColor(QPalette::ButtonText, colors.text);
    palette.setColor(QPalette::Light, colors.hover);
    palette.setColor(QPalette::Midlight, colors.hover);
    palette.setColor(QPalette::Mid, colors.border);
    palette.setColor(QPalette::Dark, colors.muted);
    palette.setColor(QPalette::Shadow, colors.shadow);
    palette.setColor(QPalette::Highlight, colors.accent);
    palette.setColor(QPalette::HighlightedText, colors.onAccent);
    palette.setColor(QPalette::Link, colors.accent);
    palette.setColor(QPalette::ToolTipBase, colors.card);
    palette.setColor(QPalette::ToolTipText, colors.text);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    palette.setColor(QPalette::Accent, colors.accent);
#endif
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, colors.disabled);
    return palette;
}

QString workspaceStyle(const ThemeColors &colors)
{
    QString style = QStringLiteral(R"(
        QMainWindow#workspaceWindow, QWidget#libraryPane, QWidget#inspectorPane,
        QScrollArea#inspectorScroll, QWidget#dockHeader, QWidget#workspaceHeader, QToolBar#projectToolbar, QStatusBar {
            background: @surface;
        }
        QMainWindow::separator { background: @border; width: 1px; height: 1px; }
        QMainWindow::separator:hover { background: @accent; width: 1px; height: 1px; }
        QMenuBar#workspaceMenu { background: @surface; padding: 3px 9px 1px; color: @text; }
        QMenuBar#workspaceMenu::item { background: transparent; border-radius: 5px; padding: 5px 9px; }
        QMenuBar#workspaceMenu::item:selected { background: @hover; }
        QMenuBar#workspaceMenu::item:pressed { background: @selection; }
        QWidget#workspaceHeader { border-bottom: 1px solid @border; }
        QWidget#workspaceHeader QMenuBar#workspaceMenu { padding: 0 2px; }
        QWidget#workspaceHeader QToolBar#projectToolbar { border: 0; padding: 0 3px; spacing: 1px; }
        QWidget#workspaceHeader QToolBar#projectToolbar QToolButton { padding: 4px; }
        QLabel#workspaceTitle { color: @muted; }
        QMenu { background: @card; color: @text; border: 1px solid @border; padding: 4px; }
        QMenu::item { padding: 5px 23px 5px 24px; border-radius: 4px; }
        QMenu::item:selected { background: @hover; }
        QMenu::item:disabled { color: @disabled; }
        QMenu::separator { background: @border; height: 1px; margin: 4px 8px; }
        QToolBar#projectToolbar { border: 0; border-bottom: 1px solid @border; spacing: 5px; padding: 7px 11px; }
        QToolBar#projectToolbar::separator { background: @border; width: 1px; margin: 6px 5px; }
        QToolBar#projectToolbar QToolButton, QWidget#dockHeader QToolButton, QPushButton {
            background: transparent; color: @text; border: 1px solid transparent; border-radius: 6px;
        }
        QToolButton#windowMinimize, QToolButton#windowMaximize, QToolButton#windowClose {
            background: transparent; border: 1px solid transparent; border-radius: 5px;
        }
        QToolButton#windowMinimize:hover, QToolButton#windowMaximize:hover { background: @hover; }
        QToolButton#windowClose:hover { background: @selection; border-color: @error; }
        QToolButton#windowMinimize:focus, QToolButton#windowMaximize:focus, QToolButton#windowClose:focus { border-color: @accent; }
        QToolBar#projectToolbar QToolButton { padding: 5px 8px; min-height: 20px; }
        QWidget#dockHeader QToolButton { padding: 2px; }
        QToolBar#projectToolbar QToolButton:hover, QWidget#dockHeader QToolButton:hover, QPushButton:hover { background: @hover; }
        QToolBar#projectToolbar QToolButton:pressed, QWidget#dockHeader QToolButton:pressed, QPushButton:pressed { background: @selection; }
        QToolBar#projectToolbar QToolButton:focus, QWidget#dockHeader QToolButton:focus,
        QPushButton:focus, QLineEdit:focus, QDoubleSpinBox:focus, QComboBox:focus { border: 1px solid @accent; }
        QPushButton { background: @card; border-color: @border; padding: 5px 9px; min-height: 19px; }
        QPushButton#serialToggle[portOpen="false"], QPushButton#applyTextTemplate:enabled { background: @accent; color: @onAccent; border-color: @accent; }
        QPushButton#serialToggle[portOpen="false"]:hover, QPushButton#applyTextTemplate:enabled:hover { background: @accentHover; }
        QPushButton#serialToggle[portOpen="false"]:focus, QPushButton#applyTextTemplate:enabled:focus { border-color: @onAccent; }
        QPushButton:disabled, QLineEdit:disabled, QDoubleSpinBox:disabled, QComboBox:disabled {
            color: @disabled; background: @surface;
        }
        QLabel#dockTitle { color: @text; font-weight: 600; }
        QLabel#secondaryLabel, QLabel#recorderStatus { color: @muted; }
        QListWidget#moduleLibrary { background: transparent; border: 0; padding: 0; outline: 0; }
        QListWidget#moduleLibrary::item { color: @text; padding-left: 8px; border: 1px solid transparent; border-radius: 6px; }
        QListWidget#moduleLibrary::item:hover { background: @hover; }
        QListWidget#moduleLibrary::item:selected { background: @selection; }
        QListWidget#moduleLibrary::item:focus { border: 1px solid @accent; }
        QLineEdit, QDoubleSpinBox, QComboBox {
            background: @card; color: @text; border: 1px solid @border; border-radius: 5px; padding: 4px 6px;
        }
        QComboBox { border-radius: 7px; padding: 4px 8px; min-height: 18px; combobox-popup: 0; }
        QComboBox:enabled:focus, QComboBox:enabled:on { border-color: @accent; }
        QComboBox::drop-down {
            subcontrol-origin: padding; subcontrol-position: top right; width: 28px;
            background: transparent; border: 0;
        }
        QComboBox[workspaceChevron="true"]::down-arrow { image: none; width: 0; height: 0; }
        QComboBox QAbstractItemView {
            background: @card; color: @text; selection-background-color: @selection; selection-color: @text;
            border: 1px solid @border; border-radius: 6px; outline: 0; padding: 3px;
        }
        QComboBox QAbstractItemView::item { min-height: 23px; padding: 1px 6px; border-radius: 4px; }
        QComboBox QAbstractItemView::item:hover { background: @hover; }
        QComboBox QAbstractItemView::item:selected { background: @selection; color: @text; }
        QComboBox QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
        QComboBox QScrollBar::handle:vertical { background: @border; border-radius: 2px; min-height: 20px; }
        QComboBox QScrollBar::handle:vertical:hover { background: @muted; }
        QComboBox QScrollBar::add-line:vertical, QComboBox QScrollBar::sub-line:vertical { height: 0; }
        QComboBox QScrollBar::add-page:vertical, QComboBox QScrollBar::sub-page:vertical { background: transparent; }
        QWidget#numberEditor {
            background: @card; border: 1px solid @border; border-radius: 7px;
        }
        QWidget#numberEditor > QDoubleSpinBox {
            background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 2px 4px;
        }
        QWidget#numberEditor > QDoubleSpinBox:focus { border-color: @accent; }
        QWidget#numberEditor > QToolButton {
            background: transparent; color: @text; border: 1px solid transparent; border-radius: 5px;
            padding: 0; font-size: 17px;
        }
        QWidget#numberEditor > QToolButton:hover { background: @hover; }
        QWidget#numberEditor > QToolButton:pressed { background: @selection; }
        QWidget#numberEditor > QToolButton:focus { border-color: @accent; }
        QWidget#numberEditor > QToolButton:disabled { color: @disabled; }
        QPlainTextEdit#serialPreview, QPlainTextEdit#serialInput, QPlainTextEdit#textTemplateInput, QPlainTextEdit#textDisplayPreview {
            background: @card; color: @text; border: 1px solid @border; border-radius: 5px; padding: 4px;
        }
        QPlainTextEdit#serialPreview:focus, QPlainTextEdit#serialInput:focus,
        QPlainTextEdit#textTemplateInput:focus, QPlainTextEdit#textDisplayPreview:focus { border-color: @accent; }
        QComboBox QLineEdit { border: 0; padding: 0; }
        QStatusBar { border-top: 1px solid @border; }
        QStatusBar::item { border: 0; }
        QStatusBar QLabel { color: @muted; padding: 4px 8px; }
    )");
    const std::pair<const char *, QColor> replacements[]{
        {"@surface", colors.surface}, {"@border", colors.border}, {"@text", colors.text},
        {"@accentHover", colors.accentHover}, {"@accent", colors.accent}, {"@onAccent", colors.onAccent},
        {"@hover", colors.hover}, {"@selection", colors.selection}, {"@card", colors.card},
        {"@disabled", colors.disabled}, {"@muted", colors.muted}, {"@number", colors.number}, {"@error", colors.error},
    };
    for (const auto &[token, color] : replacements)
        style.replace(QLatin1String(token), color.name());
    return style;
}

namespace {
void drawIcon(QPainter &p, WorkspaceIcon symbol)
{
    const auto polyline = [&p](std::initializer_list<QPointF> points) {
        p.drawPolyline(QPolygonF(points));
    };
    switch (symbol) {
    case WorkspaceIcon::New:
        polyline({{10, 2}, {4, 2}, {4, 15}, {14, 15}, {14, 6}, {10, 2}, {10, 6}, {14, 6}});
        break;
    case WorkspaceIcon::Open:
        polyline({{2, 13}, {2, 4}, {7, 4}, {8, 6}, {14, 6}, {14, 7}});
        polyline({{2, 13}, {5, 8}, {16, 8}, {13, 13}, {2, 13}});
        break;
    case WorkspaceIcon::Save:
        p.drawLine(QPointF(9, 2), QPointF(9, 11));
        polyline({{6, 8}, {9, 11}, {12, 8}});
        polyline({{3, 11}, {3, 15}, {15, 15}, {15, 11}});
        break;
    case WorkspaceIcon::Fit:
        polyline({{3, 7}, {3, 3}, {7, 3}});
        polyline({{11, 3}, {15, 3}, {15, 7}});
        polyline({{15, 11}, {15, 15}, {11, 15}});
        polyline({{7, 15}, {3, 15}, {3, 11}});
        p.drawRect(QRectF(7, 7, 4, 4));
        break;
    case WorkspaceIcon::Float:
        polyline({{11, 8}, {11, 14}, {4, 14}, {4, 7}, {10, 7}});
        polyline({{9, 4}, {14, 4}, {14, 9}});
        p.drawLine(QPointF(14, 4), QPointF(9, 9));
        break;
    case WorkspaceIcon::Close:
        p.drawLine(QPointF(5, 5), QPointF(13, 13));
        p.drawLine(QPointF(13, 5), QPointF(5, 13));
        break;
    case WorkspaceIcon::Minimize:
        p.drawLine(QPointF(4, 11), QPointF(14, 11));
        break;
    case WorkspaceIcon::Maximize:
        p.drawRect(QRectF(4, 4, 10, 10));
        break;
    case WorkspaceIcon::Restore:
        polyline({{7, 4}, {14, 4}, {14, 11}});
        p.drawRect(QRectF(4, 7, 7, 7));
        break;
    case WorkspaceIcon::Serial:
        p.drawRoundedRect(QRectF(3, 5, 12, 8), 2, 2);
        for (int x : {6, 9, 12})
            p.drawPoint(QPointF(x, 9));
        break;
    case WorkspaceIcon::Waveform:
        p.drawRoundedRect(QRectF(2, 3, 14, 12), 2, 2);
        polyline({{4, 9}, {6, 9}, {8, 6}, {10, 12}, {12, 9}, {14, 9}});
        break;
    case WorkspaceIcon::Recorder:
        p.drawRoundedRect(QRectF(2, 3, 14, 12), 2, 2);
        polyline({{4, 10}, {6, 10}, {8, 7}, {10, 11}, {12, 9}, {14, 9}});
        p.drawPoint(QPointF(13, 6));
        break;
    case WorkspaceIcon::Spectrum:
        polyline({{3, 4}, {3, 14}, {15, 14}});
        p.drawLine(QPointF(6, 11), QPointF(6, 9));
        p.drawLine(QPointF(9, 11), QPointF(9, 5));
        p.drawLine(QPointF(12, 11), QPointF(12, 7));
        p.drawLine(QPointF(15, 11), QPointF(15, 9));
        break;
    case WorkspaceIcon::Filter:
        polyline({{3, 4}, {15, 4}, {11, 9}, {11, 13}, {7, 15}, {7, 9}, {3, 4}});
        break;
    case WorkspaceIcon::Text:
        p.drawRoundedRect(QRectF(2, 3, 14, 12), 2, 2);
        p.drawLine(QPointF(5, 7), QPointF(13, 7));
        p.drawLine(QPointF(5, 11), QPointF(10, 11));
        break;
    }
}
}

QIcon workspaceIcon(WorkspaceIcon symbol, const QColor &color)
{
    QIcon icon;
    for (const int scale : {1, 2, 3}) {
        QPixmap pixmap(18 * scale, 18 * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(color, 1.45, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        drawIcon(painter, symbol);
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}
