#pragma once

#include "graph.h"
#include "runner.h"
#include "theme.h"

#include <QMainWindow>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <optional>
#include <utility>

class Canvas;
class QAction;
class QDockWidget;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QToolButton;
class QVBoxLayout;
class QMenuBar;
class QToolBar;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    void openProject(const QString &path);

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
#ifdef Q_OS_WIN
    bool event(QEvent *event) override;
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#endif

private:
    enum class Appearance { System, Light, Dark };

    void createActions();
    void createWindowHeader(QMenuBar *menu, QToolBar *toolbar);
    void updateWindowControl();
    void createDockPanels();
    void createDockHeader(QDockWidget *dock);
    void applyAppearancePreference();
    void refreshAppearance();
    void refreshInspector();
    void createSerialControls(QWidget *pane, const Node &node);
    void createScopeControls(QWidget *pane, const QUuid &id);
    void createRecorderControls(QWidget *pane, const QUuid &id);
    void exportRecording(const QUuid &id, bool image);
    QWidget *createTextTemplateEditor(QWidget *parent, const Node &node, const ParameterDefinition &parameter);
    void refreshSerialPreview();
    void commitInspectorEdits();
    void setParameter(const QUuid &id, const QString &parameter, const QJsonValue &value);
    void toggleSerial(const QUuid &id);
    void showFailure(const QUuid &id, const QString &message);
    void setProject(Graph next, const QString &path = {});
    void chooseProject();
    bool saveProject(bool saveAs = false);
    bool confirmReplace();
    void markModified();
    void updateTitle();

    Graph graph;
    GraphRunner runner;
    Canvas *canvas;
    QListWidget *library = nullptr;
    QDockWidget *inspectorDock = nullptr;
    QVBoxLayout *inspectorLayout = nullptr;
    QLabel *nodeStatus = nullptr;
    QLabel *zoomStatus = nullptr;
    QLabel *workspaceTitle = nullptr;
    QToolButton *maximizeButton = nullptr;
    QPlainTextEdit *serialPreview = nullptr;
    QPlainTextEdit *serialInput = nullptr;
    QPlainTextEdit *textDisplayPreview = nullptr;
    QHash<QUuid, QByteArray> serialHistory;
    QHash<QUuid, QString> serialDrafts;
    QHash<QUuid, QString> textTemplateDrafts;
    QSet<QString> activeCsvDestinations;
    QTimer serialPreviewTimer;
    QUuid inspectedNode;
    QString projectPath;
    QString recordingExportDirectory;
    QList<std::pair<QAction *, WorkspaceIcon>> actionIcons;
    QList<std::pair<QToolButton *, WorkspaceIcon>> dockIcons;
    Appearance appearance = Appearance::System;
    std::optional<bool> darkAppearance;
    bool modified = false;
};
