#include "window.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("FantasyHost"));
    app.setOrganizationName(QStringLiteral("FantasyHost"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));

    MainWindow window;
    if (app.arguments().size() == 2)
        window.openProject(app.arguments().at(1));
    window.show();
    return app.exec();
}
