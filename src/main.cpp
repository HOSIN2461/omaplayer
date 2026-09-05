#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickWindow>
#include <QDBusConnection>
#include <QLocale>
#include <QLibraryInfo>
#include <QTranslator>
#include <QDir>
#include <clocale>

#include "MpvCore.h"
#include "MprisPlayer.h"

using namespace Qt::Literals::StringLiterals;

int main(int argc, char *argv[])
{
    fprintf(stderr, "BOOT-START\n");
    fflush(stderr);
    // NVIDIA driver 580.178.04 segfaults (SIGSEGV in libnvidia-eglcore during
    // QRhi::endFrame) when Qt Quick presents over the setup the in-code
    // setGraphicsApi() below selects; the QSG_RHI_BACKEND environment variable
    // is honored earlier and avoids the crash, so set it before the
    // QGuiApplication is constructed.
    qputenv("QSG_RHI_BACKEND", "opengl");

    QGuiApplication app(argc, argv);
    fprintf(stderr, "BOOT app-ctor done\n");
    fflush(stderr);
    // The player is a single floating window (the PiP window-set toggle was
    // removed), so the default "quit when the (last) window closes" applies —
    // closing the window stops playback and exits the app.

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
        &app, [] { qWarning() << "MAIN: objectCreationFailed"; QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    QObject::connect(&engine, &QQmlEngine::warnings,
                     [](const QList<QQmlError> &errs) {
                         for (const QQmlError &e : errs)
                             qWarning().noquote() << e.toString();
                     });
    engine.loadFromModule(QStringLiteral("Omaplayer"), QStringLiteral("Main"));
    fprintf(stderr, "BOOT after loadFromModule, rootObjects=%d\n", int(engine.rootObjects().size()));
    fflush(stderr);

    // Test hook: OMAPLAYER_WIN_W/H lets us launch the floating window at a
    // given size (hyprctl resize is unusable on the Lua config). Overrides the
    // normal size gracefully — set both, or neither is used.
    if (auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0))) {
        const int tw = qEnvironmentVariableIntValue("OMAPLAYER_WIN_W");
        const int th = qEnvironmentVariableIntValue("OMAPLAYER_WIN_H");
        if (tw > 0 && th > 0)
            win->resize(tw, th);
    }

    // --- MPRIS (org.mpris.MediaPlayer2) over session D-Bus — media keys,
    // mixer strips and the desktop shell's media widget drive the player.
    MpvCore::instance(); // ensure the singleton exists before adaptors attach
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (bus.isConnected()
            && bus.registerService(QStringLiteral("org.mpris.MediaPlayer2.omaplayer"))) {
            // The QDBusAbstractAdaptor pattern requires one "host" object that
            // owns the adaptors; registering the host exports every adaptor on
            // the same object path.
            static QObject mprisHost;
            static MprisRoot rootAdaptor(MpvCore::instance(), &mprisHost);
            static MprisPlayer playerAdaptor(MpvCore::instance(), &mprisHost);
            bus.registerObject(QStringLiteral("/org/mpris/MediaPlayer2"),
                               &mprisHost,
                               QDBusConnection::ExportAdaptors);
        } else {
            qInfo("MPRIS: session bus unavailable or name already taken — "
                  "disabled (another player instance may be running)");
        }
    }

    // IINA-style usage: `omaplayer <file-or-url>...` (options start with `--`
    // and must be skipped before picking the media path). The first file is
    // played, the rest are appended to the playlist.
    QStringList args;
    for (int i = 1; i < argc; i++) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg.startsWith(QLatin1String("--")))
            continue;
        args << arg;
    }
    if (!args.isEmpty())
        MpvCore::instance()->openList(args);

    return app.exec();
}