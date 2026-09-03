#include <QApplication>
#include <QIcon>

#include "MainWindow.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    // Fixes QStandardPaths::AppDataLocation to "~/Library/Application
    // Support/XInsight" (PRD 5.7) rather than falling back to the raw
    // executable name. Deliberately no setOrganizationName(): Qt nests
    // AppDataLocation under <org>/<app> when both are set, which would
    // produce ".../XInsight/XInsight" here.
    QCoreApplication::setApplicationName(QStringLiteral("XInsight"));
    // Application-wide, not per-window: covers the Dock icon while running
    // and every MainWindow's title-bar icon with one call. Packaging this
    // as a signed/notarized .app with a proper .icns is deferred (PRD 9),
    // so this is the icon until then.
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/code_view.svg")));

    MainWindow window;
    window.show();

    return app.exec();
}
