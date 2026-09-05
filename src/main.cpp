#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QLocale>
#include <QLibraryInfo>
#include <QTranslator>
#include <QDir>
#include <clocale>

#include "MpvCore.h"

int main(int argc, char *argv[])
{
    // NVIDIA driver 580.178.04 segfaults (SIGSEGV in libnvidia-eglcore during
    // QRhi::endFrame) when Qt Quick presents over the setup the in-code
    // setGraphicsApi() below selects; the QSG_RHI_BACKEND environment variable
    // is honored earlier and avoids the crash, so set it before the
    // QGuiApplication is constructed.
    qputenv("QSG_RHI_BACKEND", "opengl");

    QGuiApplication app(argc, argv);
    // PiP toggles the window set (one hides while the other maps), which must
    // not end the session just because no window happens to be visible in that
    // instant.
    QGuiApplication::setQuitOnLastWindowClosed(false);

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
    auto baseFor = [](const QString &lang) -> QString {
        return lang == QLatin1String("hu")
                   ? QStringLiteral("hu")
                   : QLocale(lang).name().toLower();
    };

    // Qt's own dialogs/controls text (OK, Cancel, file-picker buttons, …)
    // needs its translations too; load those first so the app's own
    // translator below wins any conflict over the same source text.
    const QString qtTranslationsDir = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    const QStringList qtBases = { QStringLiteral("qtbase_"),
                                  QStringLiteral("qt_"),
                                  QStringLiteral("qtdeclarative_") };
    for (const QString &lang : langs) {
        bool qtLoaded = false;
        for (const QString &base : qtBases) {
            auto *qt = new QTranslator(&app);
            if (qt->load(base + baseFor(lang), qtTranslationsDir)) {
                app.installTranslator(qt);
                qtLoaded = true;
            } else {
                delete qt;
            }
        }
        if (qtLoaded)
            break;
    }

    for (const QString &lang : langs) {
        if (translator.load(QStringLiteral(":/translations/%1.qm").arg(baseFor(lang)))) {
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