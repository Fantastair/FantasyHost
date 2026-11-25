#include "window.h"

#include "canvas.h"
#include "scope.h"
#include "waveform.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDockWidget>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSerialPortInfo>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QStyleHints>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include <QToolBar>
#include <QToolButton>
#include <QThread>
#include <QVBoxLayout>
#include <QWindow>
#include <memory>
#ifdef Q_OS_WIN
#include <QScopeGuard>
#include <qt_windows.h>
#include <shobjidl.h>
#include <windowsx.h>
#endif

namespace {
const QString projectFilter = "FantasyHost 工程 (*.fhgraph)";
const QString appearanceSetting = "view/appearance";
constexpr qsizetype serialPreviewLimit = 16 * 1024;
const QMap<QString, WorkspaceIcon> moduleIcons{
    {"serial", WorkspaceIcon::Serial}, {"scope", WorkspaceIcon::Waveform},
    {"recorder", WorkspaceIcon::Recorder}, {"text", WorkspaceIcon::Text},
    {"spectrum", WorkspaceIcon::Spectrum},
};

#ifdef Q_OS_WIN
struct ReleaseComObject {
    void operator()(IUnknown *object) const { object->Release(); }
};

QString chooseWindowsExportFile(QWidget *parent, const QString &directory, const QString &caption, bool image, QString &error)
{
    const auto check = [&error](HRESULT result) {
        if (SUCCEEDED(result))
            return true;
        if (result != HRESULT_FROM_WIN32(ERROR_CANCELLED))
            error = QString("Windows 保存对话框出错（0x%1）。").arg(quint32(result), 8, 16, QLatin1Char('0'));
        return false;
    };
    if (!check(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        return {};
    const auto uninitialize = qScopeGuard([] { CoUninitialize(); });

    IFileSaveDialog *nativeDialog = nullptr;
    if (!check(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&nativeDialog))))
        return {};
    const std::unique_ptr<IFileSaveDialog, ReleaseComObject> dialog(nativeDialog);
    // The atomic writer checks access when it creates the actual export.
    constexpr DWORD options = FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT | FOS_NOCHANGEDIR | FOS_NOTESTFILECREATE;
    const COMDLG_FILTERSPEC filter = image ? COMDLG_FILTERSPEC{L"PNG 图像 (*.png)", L"*.png"}
                                          : COMDLG_FILTERSPEC{L"CSV 文件 (*.csv)", L"*.csv"};
    if (!check(dialog->SetOptions(options)) || !check(dialog->SetTitle(caption.toStdWString().c_str()))
        || !check(dialog->SetFileTypes(1, &filter)) || !check(dialog->SetFileTypeIndex(1))
        || !check(dialog->SetDefaultExtension(image ? L"png" : L"csv"))
        || !check(dialog->SetFileName(image ? L"waveform.png" : L"waveform.csv"))) {
        return {};
    }

    IShellItem *folderItem = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(QDir::toNativeSeparators(directory).toStdWString().c_str(), nullptr,
                                             IID_PPV_ARGS(&folderItem)))) {
        const std::unique_ptr<IShellItem, ReleaseComObject> folder(folderItem);
        dialog->SetFolder(folder.get());
    }
    if (!check(dialog->Show(reinterpret_cast<HWND>(parent->winId()))))
        return {};

    IShellItem *selectedItem = nullptr;
    if (!check(dialog->GetResult(&selectedItem)))
        return {};
    const std::unique_ptr<IShellItem, ReleaseComObject> selected(selectedItem);
    PWSTR path = nullptr;
    if (!check(selected->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return {};
    const auto freePath = qScopeGuard([path] { CoTaskMemFree(path); });
    return QDir::fromNativeSeparators(QString::fromWCharArray(path));
}
#endif

void showNodeStatus(QLabel &label, const QString &status)
{
    label.setText(status);
    if (label.objectName() == "recorderStatus")
        label.setVisible(!status.isEmpty() && status != "等待输入" && status != "正在采集");
}

QLabel *wrappedLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setObjectName("secondaryLabel");
    return label;
}

class NumberEditor final : public QWidget {
public:
    NumberEditor(const ParameterDefinition &parameter, double maximum, double current, QWidget *parent)
        : QWidget(parent), spin(new QDoubleSpinBox(this)), minus(new QToolButton(this)), plus(new QToolButton(this))
    {
        setObjectName("numberEditor");
        setAttribute(Qt::WA_StyledBackground);
        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(4, 2, 3, 2);
        row->setSpacing(2);
        spin->setObjectName(parameter.id);
        spin->setAccessibleName(parameter.label);
        spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        spin->setDecimals(parameter.decimals);
        spin->setRange(parameter.minimum, maximum);
        spin->setStepType(QAbstractSpinBox::AdaptiveDecimalStepType);
        spin->setValue(current);
        spin->setKeyboardTracking(false);
        spin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        spin->setMinimumWidth(67);
        row->addWidget(spin, 1);

        minus->setObjectName("numberMinus");
        plus->setObjectName("numberPlus");
        minus->setText("−");
        plus->setText("+");
        minus->setToolTip(QString("减小%1").arg(parameter.label));
        plus->setToolTip(QString("增大%1").arg(parameter.label));
        for (QToolButton *button : {minus, plus}) {
            button->setAccessibleName(button->toolTip());
            button->setFixedSize(26, 24);
            button->setFocusPolicy(Qt::TabFocus);
            button->setAutoRepeat(true);
            row->addWidget(button);
        }
        connect(minus, &QToolButton::clicked, spin, &QAbstractSpinBox::stepDown);
        connect(plus, &QToolButton::clicked, spin, &QAbstractSpinBox::stepUp);
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { updateButtons(); });
        updateButtons();
        setFocusProxy(spin);
    }

    void updateButtons()
    {
        minus->setEnabled(spin->value() > spin->minimum());
        plus->setEnabled(spin->value() < spin->maximum());
    }

    QDoubleSpinBox *const spin;

private:
    QToolButton *const minus;
    QToolButton *const plus;
};

class WorkspaceComboBox final : public QComboBox {
public:
    explicit WorkspaceComboBox(QWidget *parent) : QComboBox(parent)
    {
        setProperty("workspaceChevron", true);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStyleOptionComboBox option;
        initStyleOption(&option);
        const QRect arrow = style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, this);
        QStylePainter painter(this);
        // Keep the native hit target and label layout while drawing our own indicator.
        option.subControls = QStyle::SC_ComboBoxFrame;
        painter.drawComplexControl(QStyle::CC_ComboBox, option);
        painter.drawControl(QStyle::CE_ComboBoxLabel, option);

        const QPointF center = QRectF(arrow).center();
        const qreal direction = (option.state & QStyle::State_On) ? -1 : 1;
        const QColor color = option.palette.color(isEnabled() ? QPalette::PlaceholderText : QPalette::ButtonText);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPolyline(QPolygonF{center + QPointF(-4, -2 * direction), center + QPointF(0, 2 * direction),
                                      center + QPointF(4, -2 * direction)});
    }
};

class WindowHeader final : public QWidget {
public:
    explicit WindowHeader(QWidget *parent) : QWidget(parent) {}

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && window()->windowHandle() && window()->windowHandle()->startSystemMove()) {
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            if (window()->isMaximized()) window()->showNormal();
            else window()->showMaximized();
            event->accept();
        } else {
            QWidget::mouseDoubleClickEvent(event);
        }
    }
};
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), canvas(new Canvas(graph, this))
{
    setObjectName("workspaceWindow");
    const QString storedAppearance = QSettings().value(appearanceSetting).toString();
    if (storedAppearance == "light")
        appearance = Appearance::Light;
    else if (storedAppearance == "dark")
        appearance = Appearance::Dark;
    resize(1260, 740);
    setMinimumSize(780, 480);
    setDockNestingEnabled(true);
    setCentralWidget(canvas);
    createDockPanels();
    createActions();

    statusBar()->addWidget(new QLabel("滚轮缩放  ·  中键平移  ·  拖动端口接线", this), 1);
    zoomStatus = new QLabel("100%", this);
    zoomStatus->setMinimumWidth(56);
    zoomStatus->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    statusBar()->addPermanentWidget(zoomStatus);

    connect(canvas, &Canvas::graphChanged, this, &MainWindow::markModified);
    connect(canvas, &Canvas::topologyChanged, this, [this] { runner.sync(graph); });
    connect(canvas, &Canvas::selectionChanged, this, &MainWindow::refreshInspector);
    connect(canvas, &Canvas::message, this, [this](const QString &text) { statusBar()->showMessage(text, 4500); });
    connect(canvas, &Canvas::zoomChanged, this, [this](double percent) {
        zoomStatus->setText(QString::number(percent, 'f', percent < 1 ? 3 : 0) + '%');
    });
    connect(&runner, &GraphRunner::samplesReceived, canvas, &Canvas::sample);
    connect(&runner, &GraphRunner::textReceived, canvas, &Canvas::setNodeDisplayText);
    connect(&runner, &GraphRunner::spectrumReceived, canvas, &Canvas::setNodeSpectrum);
    const auto publishViewport = [this](const QUuid &id) {
        if (runner.hasWaveformReceivers(id))
            runner.publishWaveform(id, canvas->recordingViewport(id));
    };
    connect(canvas, &Canvas::recordingViewportChanged, this, publishViewport);
    connect(&runner, &GraphRunner::waveformRequested, this, publishViewport);
    connect(canvas, &Canvas::displayTextPresented, this, [this](const QUuid &id, const QString &text) {
        if (textDisplayPreview && id == inspectedNode)
            textDisplayPreview->setPlainText(text);
    });
    serialPreviewTimer.setSingleShot(true);
    serialPreviewTimer.setInterval(80);
    connect(&serialPreviewTimer, &QTimer::timeout, this, &MainWindow::refreshSerialPreview);
    connect(&runner, &GraphRunner::serialReceived, this, [this](const QUuid &id, const QByteArray &bytes) {
        auto &history = serialHistory[id];
        if (bytes.size() >= serialPreviewLimit) {
            history = bytes.right(serialPreviewLimit);
        } else {
            history.append(bytes);
            if (history.size() > serialPreviewLimit)
                history.remove(0, history.size() - serialPreviewLimit);
        }
        if (serialPreview && id == inspectedNode && !serialPreviewTimer.isActive())
            serialPreviewTimer.start();
    });
    connect(&runner, &GraphRunner::statusChanged, canvas, &Canvas::setNodeStatus);
    connect(&runner, &GraphRunner::statusChanged, this, [this](const QUuid &id, const QString &status) {
        if (nodeStatus && id == inspectedNode)
            showNodeStatus(*nodeStatus, status);
    });
    connect(&runner, &GraphRunner::failed, this, &MainWindow::showFailure);
    connect(QApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (appearance == Appearance::System)
            refreshAppearance();
    }, Qt::QueuedConnection);

    canvas->rebuild();
    refreshInspector();
    applyAppearancePreference();
    updateTitle();
    canvas->setFocus();
    QTimer::singleShot(0, canvas, &Canvas::fitGraph);
}

MainWindow::~MainWindow()
{
    runner.reset();
    disconnect(canvas, nullptr, this, nullptr);
}

void MainWindow::createActions()
{
    const bool integrated = QGuiApplication::platformName() == "windows";
    QMenuBar *menu = integrated ? new QMenuBar(this) : menuBar();
    menu->setObjectName("workspaceMenu");
    auto *file = menu->addMenu("工程(&F)");
    QAction *newAction = file->addAction("新建工程", this, [this] {
        if (confirmReplace())
            setProject({});
    });
    newAction->setShortcut(QKeySequence::New);
    QAction *openAction = file->addAction("打开工程…", this, &MainWindow::chooseProject);
    openAction->setShortcut(QKeySequence::Open);
    QAction *saveAction = file->addAction("保存工程", this, [this] { saveProject(); });
    saveAction->setShortcut(QKeySequence::Save);
    file->addAction("另存为…", this, [this] { saveProject(true); })->setShortcut(QKeySequence::SaveAs);
    file->addSeparator();
    file->addAction("退出", this, &QWidget::close)->setShortcut(QKeySequence::Quit);

    auto *view = menu->addMenu("视图(&V)");
    QAction *fit = view->addAction("适配画布", canvas, &Canvas::fitGraph);
    fit->setShortcut(Qt::Key_Home);
    auto *appearanceMenu = view->addMenu("外观");
    auto *appearanceGroup = new QActionGroup(appearanceMenu);
    const struct { Appearance mode; QString title; QString setting; } choices[]{
        {Appearance::System, "跟随系统", "system"}, {Appearance::Light, "浅色", "light"},
        {Appearance::Dark, "深色", "dark"},
    };
    for (const auto &choice : choices) {
        QAction *action = appearanceMenu->addAction(choice.title);
        action->setObjectName("appearance-" + choice.setting);
        action->setCheckable(true);
        appearanceGroup->addAction(action);
        action->setChecked(appearance == choice.mode);
        connect(action, &QAction::triggered, this, [this, mode = choice.mode, value = choice.setting] {
            appearance = mode;
            QSettings().setValue(appearanceSetting, value);
            applyAppearancePreference();
        });
    }
#ifdef FANTASYHOST_OPENGL
    auto *openGL = view->addAction("画布使用 OpenGL");
    openGL->setObjectName("openGLViewport");
    openGL->setCheckable(true);
    openGL->setToolTip("切换画布绘制后端");
    connect(openGL, &QAction::triggered, canvas, [this](bool enabled) {
        QTimer::singleShot(0, canvas, [this, enabled] { canvas->setOpenGLViewport(enabled); });
    });
    connect(canvas, &Canvas::openGLViewportChanged, openGL, &QAction::setChecked);
#endif
    view->addSeparator();
    for (QDockWidget *dock : findChildren<QDockWidget *>())
        view->addAction(dock->toggleViewAction());

    QToolBar *bar = integrated ? new QToolBar("工程工具栏", this) : addToolBar("工程工具栏");
    bar->setObjectName("projectToolbar");
    bar->setMovable(false);
    bar->setToolButtonStyle(integrated ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
    bar->setIconSize(QSize(18, 18));
    actionIcons = {{newAction, WorkspaceIcon::New}, {openAction, WorkspaceIcon::Open},
                   {saveAction, WorkspaceIcon::Save}, {fit, WorkspaceIcon::Fit}};
    newAction->setIconText("新建");
    openAction->setIconText("打开");
    saveAction->setIconText("保存");
    bar->addActions({newAction, openAction, saveAction});
    bar->addSeparator();
    bar->addAction(fit);
    if (integrated)
        createWindowHeader(menu, bar);
}

void MainWindow::createWindowHeader(QMenuBar *menu, QToolBar *toolbar)
{
    setWindowFlag(Qt::FramelessWindowHint);
    auto *header = new WindowHeader(this);
    header->setObjectName("workspaceHeader");
    header->setAttribute(Qt::WA_StyledBackground);
    header->setFixedHeight(44);
    auto *layout = new QHBoxLayout(header);
    layout->setContentsMargins(7, 4, 5, 3);
    layout->setSpacing(4);
    menu->setNativeMenuBar(false);
    layout->addWidget(menu);
    layout->addWidget(toolbar);

    workspaceTitle = new QLabel(header);
    workspaceTitle->setTextFormat(Qt::PlainText);
    workspaceTitle->setObjectName("workspaceTitle");
    workspaceTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    workspaceTitle->setAlignment(Qt::AlignCenter);
    workspaceTitle->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(workspaceTitle, 1);

    const auto button = [header, layout](const QString &name, const QString &label) {
        auto *control = new QToolButton(header);
        control->setObjectName(name);
        control->setToolTip(label);
        control->setAccessibleName(label);
        control->setIconSize(QSize(16, 16));
        control->setFixedSize(38, 32);
        control->setFocusPolicy(Qt::TabFocus);
        layout->addWidget(control);
        return control;
    };
    auto *minimize = button("windowMinimize", "最小化");
    maximizeButton = button("windowMaximize", "最大化");
    auto *close = button("windowClose", "关闭窗口");
    dockIcons.append({minimize, WorkspaceIcon::Minimize});
    dockIcons.append({close, WorkspaceIcon::Close});
    connect(minimize, &QToolButton::clicked, this, &QWidget::showMinimized);
    connect(maximizeButton, &QToolButton::clicked, this, [this] { isMaximized() ? showNormal() : showMaximized(); });
    connect(close, &QToolButton::clicked, this, &QWidget::close);
    setMenuWidget(header);
}

void MainWindow::updateWindowControl()
{
    if (maximizeButton) {
        const bool maximized = isMaximized();
        const QColor color = themeColors(darkAppearance.value_or(false)).muted;
        maximizeButton->setIcon(workspaceIcon(maximized ? WorkspaceIcon::Restore : WorkspaceIcon::Maximize, color));
        maximizeButton->setToolTip(maximized ? "还原窗口" : "最大化");
        maximizeButton->setAccessibleName(maximizeButton->toolTip());
    }
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange)
        updateWindowControl();
}

#ifdef Q_OS_WIN
bool MainWindow::event(QEvent *event)
{
    const bool handled = QMainWindow::event(event);
    if (event->type() == QEvent::WinIdChange && maximizeButton && internalWinId()) {
        QTimer::singleShot(0, this, [this] {
            const auto hwnd = reinterpret_cast<HWND>(internalWinId());
            if (!hwnd)
                return;
            const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
            if (!(style & WS_THICKFRAME)) {
                SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_THICKFRAME);
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        });
    }
    return handled;
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    const auto *event = static_cast<MSG *>(message);
    if (maximizeButton && event->message == WM_NCCALCSIZE) {
        *result = 0;
        return true;
    }
    if (maximizeButton && event->message == WM_NCHITTEST && !isFullScreen()) {
        POINT point{GET_X_LPARAM(event->lParam), GET_Y_LPARAM(event->lParam)};
        RECT client{};
        if (ScreenToClient(event->hwnd, &point) && GetClientRect(event->hwnd, &client)) {
            const qreal scale = devicePixelRatioF();
            if (!isMaximized()) {
                const int grip = qRound(6 * scale);
                const bool left = point.x < grip;
                const bool right = point.x >= client.right - grip;
                if (point.y < grip) {
                    *result = left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
                    return true;
                }
                if (point.y >= client.bottom - grip) {
                    *result = left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
                    return true;
                }
                if (left || right) {
                    *result = left ? HTLEFT : HTRIGHT;
                    return true;
                }
            }
            const QPoint position(qRound(point.x / scale), qRound(point.y / scale));
            if (workspaceTitle && workspaceTitle->rect().contains(workspaceTitle->mapFrom(this, position))) {
                *result = HTCAPTION;
                return true;
            }
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}
#endif

void MainWindow::applyAppearancePreference()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    const auto scheme = appearance == Appearance::System ? Qt::ColorScheme::Unknown
        : appearance == Appearance::Dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light;
    QApplication::styleHints()->setColorScheme(scheme);
#endif
    refreshAppearance();
}

void MainWindow::refreshAppearance()
{
    const auto systemScheme = QApplication::styleHints()->colorScheme();
    const bool dark = appearance == Appearance::Dark || (appearance == Appearance::System
        && (systemScheme == Qt::ColorScheme::Dark || (systemScheme == Qt::ColorScheme::Unknown
            && QApplication::style()->standardPalette().color(QPalette::Window).lightness() < 128)));
    if (darkAppearance == dark)
        return;
    darkAppearance = dark;
    const ThemeColors &colors = themeColors(dark);
    QApplication::setPalette(themePalette(colors));
    setStyleSheet(workspaceStyle(colors));
    canvas->setTheme(colors);
    for (const auto &[action, symbol] : actionIcons)
        action->setIcon(workspaceIcon(symbol, colors.text));
    for (const auto &[button, symbol] : dockIcons)
        button->setIcon(workspaceIcon(symbol, colors.muted));
    updateWindowControl();
    for (int row = 0; row < library->count(); ++row) {
        QListWidgetItem *item = library->item(row);
        const QString id = item->data(Qt::UserRole).toString();
        const auto found = moduleIcons.constFind(id);
        if (found != moduleIcons.cend())
            item->setIcon(workspaceIcon(found.value(), colors.muted));
    }
}

void MainWindow::createDockHeader(QDockWidget *dock)
{
    auto *header = new QWidget(dock);
    header->setObjectName("dockHeader");
    header->setAttribute(Qt::WA_StyledBackground);
    auto *layout = new QHBoxLayout(header);
    layout->setContentsMargins(14, 7, 8, 5);
    layout->setSpacing(2);
    auto *title = new QLabel(dock->windowTitle(), header);
    title->setObjectName("dockTitle");
    title->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(title, 1);
    auto *floating = new QToolButton(header);
    floating->setToolTip("停靠或浮动面板");
    floating->setAccessibleName(floating->toolTip());
    auto *close = new QToolButton(header);
    close->setToolTip("隐藏面板");
    close->setAccessibleName(close->toolTip());
    dockIcons.append({floating, WorkspaceIcon::Float});
    dockIcons.append({close, WorkspaceIcon::Close});
    for (QToolButton *button : {floating, close}) {
        button->setFixedSize(25, 25);
        button->setIconSize(QSize(16, 16));
        button->setFocusPolicy(Qt::StrongFocus);
        layout->addWidget(button);
    }
    connect(floating, &QToolButton::clicked, dock, [dock] { dock->setFloating(!dock->isFloating()); });
    connect(close, &QToolButton::clicked, dock, &QDockWidget::close);
    dock->setTitleBarWidget(header);
}

void MainWindow::createDockPanels()
{
    auto *libraryDock = new QDockWidget("模块库", this);
    libraryDock->setObjectName("libraryDock");
    libraryDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    auto *libraryPane = new QWidget(libraryDock);
    libraryPane->setObjectName("libraryPane");
    libraryPane->setAttribute(Qt::WA_StyledBackground);
    auto *libraryLayout = new QVBoxLayout(libraryPane);
    libraryLayout->setContentsMargins(9, 3, 9, 12);
    library = new QListWidget(libraryPane);
    library->setObjectName("moduleLibrary");
    library->setAccessibleName("模块库");
    library->setIconSize(QSize(18, 18));
    library->setSpacing(1);
    for (const ModuleDefinition &definition : moduleDefinitions()) {
        auto *item = new QListWidgetItem(definition.name, library);
        item->setData(Qt::UserRole, definition.id);
        item->setToolTip(definition.description);
        item->setSizeHint(QSize(150, 34));
    }
    library->setCurrentRow(0);
    libraryLayout->addWidget(library, 1);
    auto *addModuleButton = new QPushButton("添加到画布", libraryPane);
    libraryLayout->addWidget(addModuleButton);
    connect(library, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        canvas->addModule(item->data(Qt::UserRole).toString());
    });
    connect(addModuleButton, &QPushButton::clicked, this, [this] {
        if (const auto *item = library->currentItem())
            canvas->addModule(item->data(Qt::UserRole).toString());
    });
    libraryPane->setMinimumWidth(158);
    libraryDock->setWidget(libraryPane);
    createDockHeader(libraryDock);
    addDockWidget(Qt::LeftDockWidgetArea, libraryDock);

    inspectorDock = new QDockWidget("参数", this);
    inspectorDock->setObjectName("inspectorDock");
    inspectorDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    auto *scroll = new QScrollArea(inspectorDock);
    scroll->setObjectName("inspectorScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumWidth(196);
    auto *inspector = new QWidget(scroll);
    inspector->setObjectName("inspectorPane");
    inspector->setAttribute(Qt::WA_StyledBackground);
    inspectorLayout = new QVBoxLayout(inspector);
    inspectorLayout->setContentsMargins(14, 7, 14, 14);
    inspectorLayout->setSpacing(12);
    scroll->setWidget(inspector);
    inspectorDock->setWidget(scroll);
    createDockHeader(inspectorDock);
    addDockWidget(Qt::RightDockWidgetArea, inspectorDock);
    resizeDocks({libraryDock, inspectorDock}, {182, 284}, Qt::Horizontal);
}

void MainWindow::refreshInspector()
{
    serialPreview = nullptr;
    serialInput = nullptr;
    textDisplayPreview = nullptr;
    while (QLayoutItem *item = inspectorLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    inspectedNode = canvas->selectedNode();
    nodeStatus = nullptr;
    const Node *node = graph.node(inspectedNode);
    QWidget *pane = inspectorLayout->parentWidget();
    if (!node) {
        const QString prompt = canvas->selectedConnectionCount() > 0
            ? "按 Delete 删除选中的连接。"
            : "选择一个模块以查看参数和运行状态。";
        inspectorLayout->addWidget(wrappedLabel(prompt, pane));
        inspectorLayout->addStretch();
        return;
    }

    const ModuleDefinition &definition = *moduleDefinition(node->type);
    const QUuid id = node->id;
    auto *name = wrappedLabel(definition.name, pane);
    name->setObjectName("inspectorName");
    QFont font = name->font();
    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() + 3);
    name->setFont(font);
    inspectorLayout->addWidget(name);
    inspectorLayout->addWidget(wrappedLabel(definition.description, pane));
    if (!definition.displaysText) {
        const QString status = runner.status(id);
        nodeStatus = wrappedLabel(status.isEmpty() ? QString("等待输入") : status, pane);
        nodeStatus->setAccessibleName("模块状态");
        if (definition.recordsWaveform) {
            nodeStatus->setObjectName("recorderStatus");
            showNodeStatus(*nodeStatus, status);
        }
        inspectorLayout->addWidget(nodeStatus);
    }
    if (definition.scope)
        createScopeControls(pane, id);
    else if (definition.recordsWaveform)
        createRecorderControls(pane, id);
    else if (definition.id == "serial")
        createSerialControls(pane, *node);

    auto *formPane = new QWidget(pane);
    auto *form = new QFormLayout(formPane);
    form->setContentsMargins(0, 4, 0, 4);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    for (const ParameterDefinition &parameter : definition.parameters) {
        if (definition.displaysSpectrum && parameter.id == "channels" && node->parameters.value("inputLayout") != "ports")
            continue;
        const QJsonValue current = node->parameters.value(parameter.id);
        QWidget *editor = nullptr;
        if (parameter.kind == ParameterKind::TextTemplate) {
            editor = createTextTemplateEditor(formPane, *node, parameter);
            form->addRow(new QLabel(parameter.label, formPane));
            form->addRow(editor);
            continue;
        } else if (parameter.id == "baud") {
            auto *baud = new WorkspaceComboBox(formPane);
            baud->setEditable(true);
            baud->setInsertPolicy(QComboBox::NoInsert);
            auto *validator = new QIntValidator(parameter.minimum, parameter.maximum, baud);
            QLocale baudLocale = QLocale::c();
            baudLocale.setNumberOptions(QLocale::RejectGroupSeparator);
            validator->setLocale(baudLocale);
            baud->setValidator(validator);
            for (int rate : {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1000000, 2000000})
                baud->addItem(QString::number(rate), rate);
            baud->setCurrentIndex(baud->findData(current.toInt()));
            baud->setEditText(QString::number(current.toInt()));
            connect(baud, &QComboBox::activated, this, [this, id, baud](int row) { setParameter(id, "baud", baud->itemData(row).toInt()); });
            connect(baud->lineEdit(), &QLineEdit::editingFinished, this, [this, id, baud] {
                if (baud->lineEdit()->hasAcceptableInput())
                    setParameter(id, "baud", baud->currentText().toInt());
                else if (const Node *node = graph.node(id))
                    baud->setEditText(QString::number(node->parameters.value("baud").toInt()));
            });
            editor = baud;
        } else if (parameter.kind == ParameterKind::Number) {
            const double maximum = parameter.id == "triggerChannel" ? scopeChannels(node->parameters) : parameter.maximum;
            auto *number = new NumberEditor(parameter, maximum, current.toDouble(), formPane);
            connect(number->spin, &QDoubleSpinBox::valueChanged, this, [this, id, key = parameter.id](double value) { setParameter(id, key, value); });
            editor = number;
        } else if (parameter.kind == ParameterKind::SerialPort) {
            auto *row = new QWidget(formPane);
            auto *layout = new QHBoxLayout(row);
            layout->setContentsMargins(0, 0, 0, 0);
            auto *combo = new WorkspaceComboBox(row);
            combo->setObjectName("serialPortSelector");
            combo->setAccessibleName(parameter.label);
            combo->setEditable(true);
            combo->setInsertPolicy(QComboBox::NoInsert);
            combo->setEditText(current.toString());
            combo->lineEdit()->setPlaceholderText("选择或输入端口名");
            const auto scan = [combo] {
                const QString text = combo->currentText();
                const QSignalBlocker hold(combo);
                combo->clear();
                for (const QSerialPortInfo &port : QSerialPortInfo::availablePorts()) {
                    combo->addItem(port.portName());
                    combo->setItemData(combo->count() - 1, port.description(), Qt::ToolTipRole);
                }
                combo->setCurrentIndex(combo->findText(text));
                combo->setEditText(text);
            };
            scan();
            connect(combo, &QComboBox::currentTextChanged, this, [this, id, key = parameter.id](const QString &value) { setParameter(id, key, value); });
            auto *refresh = new QPushButton("刷新", row);
            connect(refresh, &QPushButton::clicked, this, scan);
            layout->addWidget(combo, 1);
            layout->addWidget(refresh);
            editor = row;
        } else {
            auto *combo = new WorkspaceComboBox(formPane);
            for (const ParameterChoice &choice : parameter.choices)
                combo->addItem(choice.label, choice.value);
            combo->setCurrentIndex(combo->findData(current.toString()));
            connect(combo, &QComboBox::currentIndexChanged, this, [this, id, key = parameter.id, combo](int row) {
                if (row >= 0)
                    setParameter(id, key, combo->itemData(row).toString());
            });
            editor = combo;
        }
        if (editor->objectName().isEmpty())
            editor->setObjectName(parameter.id);
        editor->setAccessibleName(parameter.label);
        editor->setEnabled(!runner.isOpen(id) || parameter.live);
        form->addRow(parameter.label, editor);
    }
    if (!definition.parameters.isEmpty())
        inspectorLayout->addWidget(formPane);
    else
        delete formPane;

    inspectorLayout->addStretch();
}

QWidget *MainWindow::createTextTemplateEditor(QWidget *parent, const Node &node, const ParameterDefinition &parameter)
{
    const QUuid id = node.id;
    const QString key = parameter.id;
    const auto limit = static_cast<qsizetype>(parameter.maximum);
    auto *section = new QWidget(parent);
    auto *layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(7);

    auto *editor = new QPlainTextEdit(section);
    editor->setObjectName("textTemplateInput");
    editor->setAccessibleName(parameter.label);
    editor->setPlaceholderText("例如 温度 {temperature} °C");
    editor->setFixedHeight(112);
    editor->setPlainText(textTemplateDrafts.value(id, node.parameters.value(key).toString()));
    layout->addWidget(editor);

    auto *limitMessage = wrappedLabel(QString("模板长度最多 %1 个字符。").arg(limit), section);
    layout->addWidget(limitMessage);
    auto *apply = new QPushButton("应用模板", section);
    apply->setObjectName("applyTextTemplate");
    layout->addWidget(apply);

    const auto refresh = [this, id, key, limit, editor, limitMessage, apply] {
        const Node *current = graph.node(id);
        if (!current)
            return;
        const QString text = editor->toPlainText();
        const bool modified = text != current->parameters.value(key).toString();
        if (modified)
            textTemplateDrafts.insert(id, text);
        else
            textTemplateDrafts.remove(id);
        limitMessage->setVisible(text.size() > limit);
        apply->setEnabled(modified && text.size() <= limit);
    };
    connect(editor, &QPlainTextEdit::textChanged, this, refresh);
    connect(apply, &QPushButton::clicked, this, [this, id, key, editor, refresh] {
        setParameter(id, key, editor->toPlainText());
        refresh();
    });
    refresh();

    layout->addWidget(wrappedLabel("显示预览", section));
    textDisplayPreview = new QPlainTextEdit(canvas->nodeDisplayText(id), section);
    textDisplayPreview->setObjectName("textDisplayPreview");
    textDisplayPreview->setAccessibleName("显示预览");
    textDisplayPreview->setReadOnly(true);
    textDisplayPreview->setFixedHeight(110);
    layout->addWidget(textDisplayPreview);
    return section;
}

void MainWindow::createSerialControls(QWidget *pane, const Node &node)
{
    const QUuid id = node.id;
    auto *section = new QWidget(pane);
    auto *layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    const bool portOpen = runner.isOpen(id);
    auto *toggle = new QPushButton(portOpen ? "关闭串口" : "打开串口", section);
    toggle->setObjectName("serialToggle");
    toggle->setProperty("portOpen", portOpen);
    connect(toggle, &QPushButton::clicked, this, [this, id] { toggleSerial(id); });
    layout->addWidget(toggle);
    layout->addWidget(wrappedLabel("接收预览", section));
    const QFont terminalFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    serialPreview = new QPlainTextEdit(section);
    serialPreview->setObjectName("serialPreview");
    serialPreview->setAccessibleName("接收预览");
    serialPreview->setFont(terminalFont);
    serialPreview->setReadOnly(true);
    serialPreview->setPlaceholderText("等待串口数据");
    serialPreview->setFixedHeight(102);
    layout->addWidget(serialPreview);
    serialInput = new QPlainTextEdit(section);
    serialInput->setObjectName("serialInput");
    serialInput->setAccessibleName("手动发送内容");
    serialInput->setFont(terminalFont);
    serialInput->setFixedHeight(60);
    serialInput->setPlainText(serialDrafts.value(id));
    connect(serialInput, &QPlainTextEdit::textChanged, this, [this, id, input = serialInput] {
        serialDrafts.insert(id, input->toPlainText());
    });
    layout->addWidget(serialInput);

    auto *actions = new QHBoxLayout;
    auto *send = new QPushButton("手动发送", section);
    send->setObjectName("serialSendButton");
    send->setEnabled(portOpen);
    connect(send, &QPushButton::clicked, this, [this, id, input = serialInput] {
        QString message;
        if (runner.sendManual(id, input->toPlainText(), message))
            message = "数据已加入串口发送队列。";
        statusBar()->showMessage(message, 6000);
    });
    auto *clear = new QPushButton("清空预览", section);
    connect(clear, &QPushButton::clicked, this, [this, id] {
        serialHistory.remove(id);
        refreshSerialPreview();
    });
    actions->addWidget(send);
    actions->addWidget(clear);
    layout->addLayout(actions);
    inspectorLayout->addWidget(section);
    refreshSerialPreview();
}

void MainWindow::createScopeControls(QWidget *pane, const QUuid &id)
{
    auto *section = new QWidget(pane);
    auto *layout = new QHBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *pause = new QPushButton(canvas->scopePaused(id) ? "继续" : "暂停", section);
    pause->setObjectName("scopePause");
    auto *rearm = new QPushButton("重新触发", section);
    rearm->setObjectName("scopeRearm");
    connect(pause, &QPushButton::clicked, this, [this, id, pause] {
        const bool paused = !canvas->scopePaused(id);
        canvas->setScopePaused(id, paused);
        pause->setText(paused ? "继续" : "暂停");
    });
    connect(rearm, &QPushButton::clicked, this, [this, id, pause] {
        canvas->rearmScope(id);
        pause->setText("暂停");
    });
    layout->addWidget(pause);
    layout->addWidget(rearm);
    inspectorLayout->addWidget(section);
}

void MainWindow::createRecorderControls(QWidget *pane, const QUuid &id)
{
    auto *section = new QWidget(pane);
    auto *layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(7);
    auto *samples = wrappedLabel({}, section);
    samples->setObjectName("recordedSamples");
    layout->addWidget(samples);
    auto *fit = new QPushButton("显示全部数据", section);
    fit->setObjectName("fitRecording");
    connect(fit, &QPushButton::clicked, canvas, [this, id] { canvas->fitRecording(id); });
    auto *clear = new QPushButton("清除记录", section);
    clear->setObjectName("clearRecording");
    connect(clear, &QPushButton::clicked, this, [this, id] {
        QMessageBox confirmation(QMessageBox::Question, "清除波形记录", "清除所有通道已记录的样本？新样本将从零开始记录。",
                                 QMessageBox::NoButton, this);
        auto *confirm = confirmation.addButton("清除记录", QMessageBox::DestructiveRole);
        auto *cancel = confirmation.addButton("取消", QMessageBox::RejectRole);
        confirmation.setDefaultButton(cancel);
        confirmation.setEscapeButton(cancel);
        confirmation.exec();
        if (confirmation.clickedButton() == confirm)
            canvas->clearRecording(id);
    });
    auto *viewActions = new QHBoxLayout;
    viewActions->addWidget(fit);
    viewActions->addWidget(clear);
    layout->addLayout(viewActions);

    auto *actions = new QHBoxLayout;
    auto *csv = new QPushButton("导出 CSV", section);
    auto *png = new QPushButton("导出当前视图", section);
    csv->setObjectName("exportRecordingCsv");
    png->setObjectName("exportRecordingPng");
    png->setToolTip("将当前视图保存为高分辨率 PNG");
    connect(csv, &QPushButton::clicked, this, [this, id] { exportRecording(id, false); });
    connect(png, &QPushButton::clicked, this, [this, id] { exportRecording(id, true); });
    actions->addWidget(csv);
    actions->addWidget(png);
    layout->addLayout(actions);
    layout->addWidget(wrappedLabel("轴区滚轮缩放，Shift + 滚轮或左键拖动平移。图内悬停可查看附近样本。", section));

    const auto update = [samples, fit, clear, csv, png](qint64 count) {
        samples->setText(QString("已记录 %1 个样本").arg(QLocale().toString(count)));
        fit->setEnabled(count > 0);
        clear->setEnabled(count > 0);
        csv->setEnabled(count > 0);
        png->setEnabled(count > 0);
    };
    update(canvas->recordingSampleCount(id));
    connect(canvas, &Canvas::recordingUpdated, section, [id, update](const QUuid &node, qint64 count) {
        if (id == node)
            update(count);
    });
    inspectorLayout->addWidget(section);
}

void MainWindow::exportRecording(const QUuid &id, bool image)
{
    QString directory = recordingExportDirectory;
    if (!QFileInfo(directory).isDir())
        directory = QStandardPaths::writableLocation(image ? QStandardPaths::PicturesLocation : QStandardPaths::DocumentsLocation);
    if (!QFileInfo(directory).isDir())
        directory = QDir::homePath();

    const QString caption = image ? "导出当前波形视图" : "导出全部波形数据";
    QString path;
#ifdef Q_OS_WIN
    QString dialogError;
    path = chooseWindowsExportFile(this, directory, caption, image, dialogError);
    if (!dialogError.isEmpty())
        QMessageBox::warning(this, "无法导出波形", dialogError);
#else
    const QString suffix = image ? "png" : "csv";
    QFileDialog dialog(this, caption, directory, image ? "PNG 图像 (*.png)" : "CSV 文件 (*.csv)");
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix(suffix);
    dialog.selectFile("waveform." + suffix);
    if (dialog.exec())
        path = dialog.selectedFiles().value(0);
#endif
    if (path.isEmpty())
        return;
    const QFileInfo target(path);
    recordingExportDirectory = target.absolutePath();
    QString destination = QDir::cleanPath(target.absoluteFilePath());
#ifdef Q_OS_WIN
    destination = destination.toCaseFolded();
#endif
    if (activeCsvDestinations.contains(destination)) {
        statusBar()->showMessage("此文件正在导出，请选择其他文件。", 4500);
        return;
    }
    const auto reportError = [this, path](const QString &error) {
        QMessageBox message(QMessageBox::Warning, "无法导出波形", {}, QMessageBox::Ok, this);
        message.setTextFormat(Qt::PlainText);
        message.setText(QString("无法保存文件 %1\n\n%2").arg(QDir::toNativeSeparators(path), error));
        message.exec();
    };
    if (!image) {
        struct ExportResult { bool success = false; QString error; };
        const auto result = std::make_shared<ExportResult>();
        auto *worker = QThread::create([snapshot = canvas->recordingCsv(id), path, result] {
            result->success = snapshot.save(path, result->error);
        });
        activeCsvDestinations.insert(destination);
        worker->setParent(this);
        connect(worker, &QThread::finished, this, [this, result, destination, reportError] {
            activeCsvDestinations.remove(destination);
            if (result->success)
                statusBar()->showMessage("全部采样数据已导出", 4500);
            else
                reportError(result->error);
        });
        connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start();
        statusBar()->showMessage("正在导出波形数据…");
        return;
    }
    QString error;
    if (!canvas->exportRecordingImage(id, path, error)) {
        reportError(error);
        return;
    }
    statusBar()->showMessage("当前波形视图已导出", 4500);
}

void MainWindow::refreshSerialPreview()
{
    const Node *node = graph.node(inspectedNode);
    if (!node || !serialPreview || !serialInput)
        return;
    const bool hex = node->parameters.value("rxFormat").toString() == "hex";
    const QString preview = formatSerialOutput(serialHistory.value(inspectedNode), hex);
    QScrollBar *scroll = serialPreview->verticalScrollBar();
    const int oldPosition = scroll->value();
    const bool follow = oldPosition == scroll->maximum();
    serialPreview->setPlainText(preview);
    scroll->setValue(follow ? scroll->maximum() : oldPosition);
    serialInput->setPlaceholderText(node->parameters.value("txFormat").toString() == "hex"
        ? "输入 HEX，例如 01 A0 FF" : "输入要发送的文本");
}

void MainWindow::setParameter(const QUuid &id, const QString &parameter, const QJsonValue &value)
{
    QList<QUuid> removed;
    if (!graph.setParameter(id, parameter, value, &removed))
        return;
    const Node &node = *graph.node(id);
    runner.updateParameters(node);
    canvas->updateNodeSettings(node, removed);
    if (parameter == "channels" && inspectedNode == id) {
        if (auto *trigger = inspectorLayout->parentWidget()->findChild<QDoubleSpinBox *>("triggerChannel")) {
            const QSignalBlocker hold(trigger);
            trigger->setMaximum(scopeChannels(node.parameters));
            trigger->setValue(node.parameters.value("triggerChannel").toInt());
            static_cast<NumberEditor *>(trigger->parentWidget())->updateButtons();
        }
    }
    if (parameter == "rxFormat" || parameter == "txFormat")
        refreshSerialPreview();
    if (inspectedNode == id && node.type == "spectrum" && parameter == "inputLayout")
        QTimer::singleShot(0, this, [this, id] { if (id == inspectedNode) refreshInspector(); });
    if (!removed.isEmpty()) {
        runner.sync(graph);
        statusBar()->showMessage(QString("已移除 %1 条不再对应输入端口的连线。").arg(removed.size()), 6500);
    }
    markModified();
}

void MainWindow::toggleSerial(const QUuid &id)
{
    commitInspectorEdits();
    if (runner.isOpen(id)) {
        runner.close(id);
    } else {
        QString error;
        if (!runner.open(id, error)) {
            showFailure(id, error);
            return;
        }
    }
    canvas->setFailedNode(id, false);
    QTimer::singleShot(0, this, [this, id] { if (id == inspectedNode) refreshInspector(); });
    statusBar()->clearMessage();
}

void MainWindow::commitInspectorEdits()
{
    QWidget *focused = QApplication::focusWidget();
    if (focused && inspectorDock->isAncestorOf(focused))
        canvas->setFocus(Qt::OtherFocusReason);
}

void MainWindow::showFailure(const QUuid &id, const QString &message)
{
    canvas->setFailedNode(id, true);
    QTimer::singleShot(0, this, [this, id] { if (id == inspectedNode) refreshInspector(); });
    statusBar()->showMessage(message, 8500);
}

void MainWindow::setProject(Graph next, const QString &path)
{
    runner.reset();
    serialHistory.clear();
    serialDrafts.clear();
    textTemplateDrafts.clear();
    graph = std::move(next);
    projectPath = path;
    modified = false;
    updateTitle();
    canvas->rebuild();
    runner.sync(graph);
    canvas->fitGraph();
    refreshInspector();
    canvas->setFocus();
}

void MainWindow::chooseProject()
{
    const QString path = QFileDialog::getOpenFileName(this, "打开工程", projectPath, projectFilter);
    if (!path.isEmpty())
        openProject(path);
}

void MainWindow::openProject(const QString &path)
{
    Graph opened;
    QString error;
    if (!Graph::load(path, opened, error)) {
        QMessageBox::warning(this, "无法打开工程", error);
        return;
    }
    if (confirmReplace()) {
        setProject(std::move(opened), path);
        statusBar()->showMessage("工程已打开", 3200);
    }
}

bool MainWindow::saveProject(bool saveAs)
{
    commitInspectorEdits();
    QString path = projectPath;
    if (saveAs || path.isEmpty()) {
        QFileDialog dialog(this, "保存工程", path, projectFilter);
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setDefaultSuffix("fhgraph");
        if (!dialog.exec())
            return false;
        path = dialog.selectedFiles().first();
    }
    QString error;
    if (!graph.save(path, error)) {
        QMessageBox::warning(this, "无法保存工程", error);
        return false;
    }
    projectPath = path;
    modified = false;
    updateTitle();
    statusBar()->showMessage("工程已保存", 3200);
    return true;
}

bool MainWindow::confirmReplace()
{
    if (!modified)
        return true;
    QMessageBox dialog(QMessageBox::Question, "保存工程", "当前工程有未保存的改动。", QMessageBox::NoButton, this);
    QPushButton *save = dialog.addButton("保存", QMessageBox::AcceptRole);
    QPushButton *discard = dialog.addButton("放弃改动", QMessageBox::DestructiveRole);
    dialog.addButton("取消", QMessageBox::RejectRole);
    dialog.setDefaultButton(save);
    dialog.exec();
    return dialog.clickedButton() == discard || (dialog.clickedButton() == save && saveProject());
}

void MainWindow::markModified()
{
    if (!modified) {
        modified = true;
        updateTitle();
    }
}

void MainWindow::updateTitle()
{
    const QString name = projectPath.isEmpty() ? QString("未命名工程") : QFileInfo(projectPath).completeBaseName();
    setWindowTitle(QString("%1[*]  |  FantasyHost").arg(name));
    setWindowModified(modified);
    if (workspaceTitle)
        workspaceTitle->setText(name + (modified ? " *" : "") + "  ·  FantasyHost");
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (confirmReplace()) {
        runner.reset();
        event->accept();
    } else {
        event->ignore();
    }
}
