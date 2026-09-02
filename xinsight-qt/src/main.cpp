#include <QApplication>

#include "MainWindow.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    // Fixes QStandardPaths::AppDataLocation to "~/Library/Application
    // Support/XInsight" (PRD 5.7) rather than falling back to the raw
    // executable name. Deliberately no setOrganizationName(): Qt nests
    // AppDataLocation under <org>/<app> when both are set, which would
    // produce ".../XInsight/XInsight" here.
    QCoreApplication::setApplicationName(QStringLiteral("XInsight"));

    MainWindow window;
    window.show();

    return app.exec();
}
