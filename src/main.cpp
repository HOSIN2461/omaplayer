#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QLocale>
#include <QTranslator>
#include <QDir>
#include <clocale>

#include "MpvCore.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    // libmpv refuses to create a handle while LC_NUMERIC is non-C (it would
    // misparse decimals). Qt resets the locale from the environment, so force
    // the C numeric locale back after QGuiApplication was constructed.
    std::setlocale(LC_NUMERIC, "C");
    QCoreApplication::setApplicationName(QStringLiteral("omaplayer"));
    QCoreApplication::setOrganizationName(QStringLiteral("omarchy"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    // Qt Quick must render through OpenGL for the libmpv OpenGL render API.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    QTranslator translator;
    const QStringList langs = QLocale::system().uiLanguages();
    for (const QString &lang : langs) {
        const QString base = lang == QLatin1String("hu")
                                 ? QStringLiteral("hu")
                                 : QLocale(lang).name().toLower();
        if (translator.load(QStringLiteral(":/translations/%1.qm").arg(base))) {
            app.installTranslator(&translator);
            break;
        }
    }

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, [] { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("Omaplayer"), QStringLiteral("Main"));

    // IINA-style usage: `omaplayer <file-or-url>...` (options start with `--`
    // and must be skipped before picking the media path).
    for (int i = 1; i < argc; i++) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg.startsWith(QLatin1String("--")))
            continue;
        MpvCore::instance()->open(arg);
        break;
    }

    return app.exec();
}