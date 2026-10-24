// Quota shows limits, usage, and cost from local history.

#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QSize>
#include <QUrl>

#include "backend.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[])
{
    // quota --preview [out.png] [WIDTHxHEIGHT] renders sample data to an image and quits.
    bool preview = false;
    QString shot;
    QSize shotSize(960, 1240);
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        const QStringList size = arg.split('x');
        if (arg == "--preview")
            preview = true;
        else if (preview && size.size() == 2 && size.at(0).toInt() > 0 && size.at(1).toInt() > 0)
            shotSize = QSize(size.at(0).toInt(), size.at(1).toInt());
        else if (preview && shot.isEmpty() && !arg.startsWith('-'))
            shot = arg;
    }

    QGuiApplication app(argc, argv);
    QFont font("Noto Sans");
    font.setPixelSize(14);
    app.setFont(font);
    app.setApplicationName("quota");
    app.setApplicationVersion("0.1.0");
    app.setDesktopFileName("quota");
    app.setWindowIcon(QIcon::fromTheme("quota"));
    QQuickStyle::setStyle("Material");

    Translator translator;
    app.installTranslator(&translator);

    // The sample image uses the default accent, not the current desktop theme.
    Theme theme(preview ? QStringLiteral("/tmp/quota-no-theme") : QString());
    Backend backend(preview);

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    if (preview) {
        if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)))
            window->resize(shotSize);
        QTimer::singleShot(500, &app, [&engine, &app, shot] {
            auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
            if (window)
                window->grabWindow().save(shot.isEmpty() ? QStringLiteral("screenshot.png") : shot);
            app.quit();
        });
    }

    return app.exec();
}
