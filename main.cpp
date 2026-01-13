#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "Database.h"
#include "AppController.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    if (!Database::init()) {
        return -1; // fail fast if DB cannot open
    }

    AppController controller;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("App", &controller);

    const QUrl url(QStringLiteral("qrc:/CodeLeveling/Main.qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url](QObject *obj, const QUrl &objUrl) {
                         if (!obj && url == objUrl) QCoreApplication::exit(-1);
                     }, Qt::QueuedConnection);

    engine.load(url);

    return app.exec();
}
