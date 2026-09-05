#include "MpvCore.h"

#include <QOpenGLContext>
#include <QQuickWindow>
#include <QQuickItem>
#include <QFile>
#include <QTextStream>
#include <QProcess>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <functional>

#include <cstring>
namespace {

// Qt Quick keeps a QOpenGLContext current on the render thread; the address
// comes from there. Must be a plain function so mpv can call it later.
void *mpvGetProcAddress(void *ctx, const char *name)
{
    Q_UNUSED(ctx)
    return reinterpret_cast<void *>(
        QOpenGLContext::currentContext()->getProcAddress(name));
}

// Qt Quick's renderer needs to know when mpv produced a new frame so it can
// schedule a repaint. mpv calls this from one of its worker threads. The
// window is the one currently hosting the video item — with PiP the item is
// re-parented into a second window, so `focusWindow()` would be wrong.
void onUpdate(void *context)
{
    auto *core = static_cast<MpvCore *>(context);
    QMetaObject::invokeMethod(core, [core] {
        if (QQuickWindow *window = core->m_renderWindow) {
            // The item must be marked dirty itself, otherwise the FBO render
            // pass is not re-run; window->update() alone is not enough.
            if (core->m_renderItem)
                core->m_renderItem->update();
            window->update();
        }
    }, Qt::QueuedConnection);
}

} // namespace

MpvCore *MpvCore::s_instance = nullptr;

MpvCore *MpvCore::instance()
{
    if (!s_instance)
        new MpvCore; // the constructor records itself
    return s_instance;
}

MpvCore::MpvCore(QObject *parent)
    : QObject(parent)
    , m_handle(mpv_create())
{
    if (!s_instance)
        s_instance = this;

    if (!m_handle) {
        qWarning("mpv_create failed");
        return;
    }

    // A conservative, well-supported option suite; the user can override the
    // vo/hwdec via scripts if they want more of the system. RPI and software
    // fallbacks are handled by mpv itself.
    mpv_set_option_string(m_handle, "vo", "libmpv");
    mpv_set_option_string(m_handle, "hwdec", "auto");
    mpv_set_option_string(m_handle, "ytdl", "yes");     // youtube etc.
    mpv_set_option_string(m_handle, "audio-display", "no");
    mpv_set_option_string(m_handle, "osc", "no");       // we draw our own bar
    mpv_set_option_string(m_handle, "keep-open", "yes"); // stay for a last-frame
    mpv_set_option_string(m_handle, "screenshot-directory", "~/Pictures");

    mpv_observe_property(m_handle, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(m_handle, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_handle, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_handle, 0, "volume", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_handle, 0, "mute", MPV_FORMAT_FLAG);
    mpv_observe_property(m_handle, 0, "media-title", MPV_FORMAT_STRING);
    mpv_observe_property(m_handle, 0, "path", MPV_FORMAT_STRING);
    mpv_observe_property(m_handle, 0, "speed", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_handle, 0, "sub-visibility", MPV_FORMAT_FLAG);
    mpv_observe_property(m_handle, 0, "brightness", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_handle, 0, "contrast", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_handle, 0, "saturation", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_handle, 0, "gamma", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_handle, 0, "sub-scale", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_handle, 0, "audio-delay", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_handle, 0, "playlist-pos", MPV_FORMAT_INT64);

    mpv_set_wakeup_callback(m_handle, &MpvCore::wakeupCallback, this);
    mpv_request_log_messages(m_handle, "warn");

    if (mpv_initialize(m_handle) < 0) {
        qWarning("mpv_initialize failed");
    }
}

MpvCore::~MpvCore()
{
    if (m_renderContext)
        mpv_render_context_free(m_renderContext);
    if (m_handle)
        mpv_terminate_destroy(m_handle);
}

mpv_render_context *MpvCore::renderContext()
{
    // Created lazily once an OpenGL context is current (the render thread),
    // which mpv requires for the libmpv OpenGL renderer. A render context is
    // bound to the GL context it was created with; the MpvVideoItem can end up
    // in a different window (PiP), which brings its own GL context, so the
    // render context is rebuilt whenever the current GL context changes.
    QOpenGLContext *current = QOpenGLContext::currentContext();
    if (!current)
        return m_renderContext;

    if (m_renderContext && m_renderGlContext != current) {
        qInfo("rebuilding mpv render context for %p", (void *)current);
        mpv_render_context_set_update_callback(m_renderContext, nullptr, nullptr);
        mpv_render_context_free(m_renderContext);
        m_renderContext = nullptr;
        m_renderGlContext = nullptr;
        // A mid-flight mpv VO cannot be re-initialized into a recreated render
        // context in place, and reloding inside this same pass races the
        // context's first render ("No render context set"). Reboot the
        // playback only after the new context has rendered its first frame.
        m_contextRebootPending = !m_filePath.isEmpty();
        m_contextRebootLocation = m_filePath;
        m_contextRebootPosition = m_position;
    }

    if (m_renderContext)
        return m_renderContext;

    m_renderGlContext = current;

    static const char *api = MPV_RENDER_API_TYPE_OPENGL;
    static mpv_opengl_init_params initParams{};
    initParams.get_proc_address = &mpvGetProcAddress;

    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(api) },
        { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &initParams },
        { MPV_RENDER_PARAM_INVALID, nullptr },
    };

    if (mpv_render_context_create(&m_renderContext, m_handle, params) < 0) {
        qWarning("mpv_render_context_create failed");
        m_renderContext = nullptr;
        m_renderGlContext = nullptr;
        return nullptr;
    }
    mpv_render_context_set_update_callback(m_renderContext, &onUpdate, this);

    // mpv initializes its video output the moment playback starts, and the
    // libmpv VO refuses to come up without a render context ("No render
    // context set"). Defer the file load until the first render gives us a
    // context, otherwise the video stream silently never starts.
    if (m_pendingOpen) {
        m_pendingOpen = false;
        if (!m_pendingFiles.isEmpty()) {
            openList(m_pendingFiles);
            m_pendingFiles.clear();
        } else {
            const QString location = m_pendingLocation;
            m_pendingLocation.clear();
            const QByteArray bytes = location.toUtf8();
            const char *cmd[] = { "loadfile", bytes.constData(), nullptr };
            mpv_command(m_handle, cmd);
            mpv_set_property_string(m_handle, "pause", "no");
        }
    }
    return m_renderContext;
}

void MpvCore::setRenderWindow(QQuickWindow *window)
{
    m_renderWindow = window;
}

void MpvCore::setRenderItem(QQuickItem *item)
{
    m_renderItem = item;
}

void MpvCore::renderFrame(int fbo, int width, int height)
{
    // Runs on the render thread with the scene-graph GL context current; the
    // render context is born here on the first frame.
    if (width <= 0 || height <= 0) {
        qInfo("skipping 0-size FBO render");
        return;
    }
    mpv_render_context *ctx = renderContext();
    if (!ctx)
        return;
    mpv_opengl_fbo mpvFbo{
        fbo,
        width,
        height,
        0, // internal_format, 0 = keep
    };
    int flipY = 0;
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_OPENGL_FBO, &mpvFbo },
        { MPV_RENDER_PARAM_FLIP_Y, &flipY },
        { MPV_RENDER_PARAM_INVALID, nullptr },
    };
    mpv_render_context_render(ctx, params);

    // Re-arm the update request; mpv fires the update callback on demand, so
    // without this it signals a new frame once and then goes quiet.
    mpv_render_context_update(ctx);

    // Deferred reboot after a render-context recreation: the new context is
    // now confirmed ("set") so a fresh loadfile at the saved position can
    // start the video cleanly instead of mpv failing its in-place VO reinit.
    if (m_contextRebootPending) {
        m_contextRebootPending = false;
        const QString location = m_contextRebootLocation;
        m_contextRebootLocation.clear();
        const double pos = m_contextRebootPosition;
        if (!location.isEmpty()) {
            const QByteArray bytes = location.toUtf8();
            const char *cmd[] = { "loadfile", bytes.constData(), "replace", nullptr };
            mpv_command(m_handle, cmd);
            if (pos > 0) {
                const QByteArray sec = QByteArray::number(pos);
                const char *seek[] = { "seek", sec.constData(), "absolute", nullptr };
                mpv_command(m_handle, seek);
            }
        }
    }
}

void MpvCore::wakeupCallback(void *context)
{
    auto *self = static_cast<MpvCore *>(context);
    QMetaObject::invokeMethod(self, &MpvCore::handleWakeup, Qt::QueuedConnection);
}

void MpvCore::handleWakeup()
{
    // mpv may wake us more than once per event; drain everything.
    while (m_handle) {
        mpv_event *event = mpv_wait_event(m_handle, 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;
        switch (event->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto *prop = static_cast<mpv_event_property *>(event->data);
            const char *name = prop->name;
            const bool flag = prop->data && prop->format == MPV_FORMAT_FLAG
                                  ? *static_cast<int *>(prop->data) != 0
                                  : false;
            const double value = prop->data && prop->format == MPV_FORMAT_DOUBLE
                                     ? *static_cast<double *>(prop->data)
                                     : 0.0;
            const char *str = prop->data && prop->format == MPV_FORMAT_STRING
                                  ? *static_cast<char **>(prop->data)
                                  : nullptr;

            if (prop->format == MPV_FORMAT_FLAG && std::strcmp(name, "pause") == 0) {
                const bool playing = !flag;
                if (playing != m_playing) {
                    m_playing = playing;
                    Q_EMIT playingChanged(m_playing);
                }
            } else if (prop->format == MPV_FORMAT_DOUBLE && std::strcmp(name, "time-pos") == 0) {
                if (value != m_position) {
                    m_position = value;
                    Q_EMIT positionChanged(m_position);
                }
            } else if (prop->format == MPV_FORMAT_DOUBLE && std::strcmp(name, "duration") == 0) {
                if (value != m_duration) {
                    m_duration = value;
                    Q_EMIT durationChanged(m_duration);
                }
            } else if (prop->format == MPV_FORMAT_DOUBLE && std::strcmp(name, "volume") == 0) {
                if (value != m_volume) {
                    m_volume = value;
                    Q_EMIT volumeChanged(m_volume);
                }
            } else if (prop->format == MPV_FORMAT_FLAG && std::strcmp(name, "mute") == 0) {
                if (flag != m_muted) {
                    m_muted = flag;
                    Q_EMIT mutedChanged(m_muted);
                }
            } else if (prop->format == MPV_FORMAT_STRING && std::strcmp(name, "media-title") == 0) {
                if (str) {
                    const QString title = QString::fromUtf8(str);
                    if (title != m_mediaTitle) {
                        m_mediaTitle = title;
                        Q_EMIT mediaTitleChanged(m_mediaTitle);
                    }
                }
            } else if (prop->format == MPV_FORMAT_STRING && std::strcmp(name, "path") == 0) {
                if (str) {
                    const QString path = QString::fromUtf8(str);
                    if (path != m_filePath) {
                        m_filePath = path;
                        Q_EMIT filePathChanged(m_filePath);
                    }
                }
            } else if (prop->format == MPV_FORMAT_DOUBLE && std::strcmp(name, "speed") == 0) {
                if (value != m_speed) {
                    m_speed = value;
                    Q_EMIT speedChanged(m_speed);
                }
            } else if (prop->format == MPV_FORMAT_FLAG && std::strcmp(name, "sub-visibility") == 0) {
                if (flag != m_subVisible) {
                    m_subVisible = flag;
                    Q_EMIT subtitlesVisibleChanged(m_subVisible);
                }
            } else if (prop->format == MPV_FORMAT_DOUBLE && std::strcmp(name, "brightness") == 0) {
                if (value != m_brightness) {
                    m_brightness = value;
                    Q_EMIT brightnessChanged(m_brightness);
                }
            } else if (prop->format == MPV_FORMAT_DOUBLE && std::strcmp(name, "contrast") == 0) {
                if (value != m_contrast) {
                    m_contrast = value;
                    Q_EMIT contrastChanged(m_contrast);
                }
            } else if (prop->format == MPV_FORMAT_DOUBLE && std::strcmp(name, "saturation") == 0) {
                if (value != m_saturation) {
                    m_saturation = value;
                    Q_EMIT saturationChanged(m_saturation);
                }
            } else if (prop->format == MPV_FORMAT_DOUBLE && std::strcmp(name, "gamma") == 0) {
                if (value != m_gamma) {
                    m_gamma = value;
                    Q_EMIT gammaChanged(m_gamma);
                }
            } else if (prop->format == MPV_FORMAT_DOUBLE && std::strcmp(name, "sub-scale") == 0) {
                if (value != m_subScale) {
                    m_subScale = value;
                    Q_EMIT subScaleChanged(m_subScale);
                }
            } else if (prop->format == MPV_FORMAT_DOUBLE && std::strcmp(name, "audio-delay") == 0) {
                if (value != m_audioDelay) {
                    m_audioDelay = value;
                    Q_EMIT audioDelayChanged(m_audioDelay);
                }
            } else if (prop->format == MPV_FORMAT_INT64 && std::strcmp(name, "playlist-pos") == 0) {
                if (prop->data)
                    Q_EMIT currentIndexChanged(static_cast<int>(*static_cast<long long *>(prop->data)));
            }
            break;
        }
        case MPV_EVENT_LOG_MESSAGE: {
            auto *log = static_cast<mpv_event_log_message *>(event->data);
            if (log && log->prefix && log->text)
                qInfo("[mpv:%s] %s", log->prefix, log->text);
            break;
        }
        case MPV_EVENT_END_FILE: {
            // With keep-open=yes, mpv pauses at end-of-file instead of
            // advancing, so drive the playlist forward ourselves: jump to the
            // next entry whenever a file ends normally and one exists.
            auto *ef = static_cast<mpv_event_end_file *>(event->data);
            if (ef->reason == MPV_END_FILE_REASON_EOF) {
                int64_t count = 0;
                int64_t pos = 0;
                if (mpv_get_property(m_handle, "playlist-count", MPV_FORMAT_INT64, &count) == 0
                    && mpv_get_property(m_handle, "playlist-pos", MPV_FORMAT_INT64, &pos) == 0
                    && count > 0 && pos >= 0 && pos + 1 < count) {
                    mpv_command_string(m_handle, "playlist-next");
                }
            }
            break;
        }
        default:
            break;
        }
    }
}

void MpvCore::open(const QString &location)
{
    if (!m_handle)
        return;
    if (!m_renderContext) {
        // No render context yet (no first paint). Queue the file; renderContext()
        // will start playback once the mpv VO can attach to a context.
        m_pendingOpen = true;
        m_pendingLocation = location;
        return;
    }
    const QByteArray bytes = location.toUtf8();
    const char *cmd[] = { "loadfile", bytes.constData(), nullptr };
    mpv_command(m_handle, cmd);
    mpv_set_property_string(m_handle, "pause", "no");
}

void MpvCore::play()
{
    if (!m_handle)
        return;
    mpv_set_property_string(m_handle, "pause", "no");
}

void MpvCore::pause()
{
    if (!m_handle)
        return;
    mpv_set_property_string(m_handle, "pause", "yes");
}

void MpvCore::stop()
{
    if (!m_handle)
        return;
    mpv_command_string(m_handle, "stop");
}

void MpvCore::togglePause()
{
    if (m_playing)
        pause();
    else
        play();
}

void MpvCore::seek(double seconds)
{
    if (!m_handle)
        return;
    const QByteArray sec = QByteArray::number(seconds);
    const char *cmd[] = { "seek", sec.constData(), "absolute", nullptr };
    mpv_command(m_handle, cmd);
}

void MpvCore::seekRelative(double seconds)
{
    if (!m_handle)
        return;
    const QByteArray sec = QByteArray::number(seconds);
    const char *cmd[] = { "seek", sec.constData(), "relative", nullptr };
    mpv_command(m_handle, cmd);
}

void MpvCore::setVolume(double volume)
{
    if (!m_handle)
        return;
    const QByteArray vol = QByteArray::number(volume);
    mpv_set_property_string(m_handle, "volume", vol.constData());
}

void MpvCore::toggleMute()
{
    if (!m_handle)
        return;
    mpv_set_property_string(m_handle, "mute", m_muted ? "no" : "yes");
}

void MpvCore::frameStep()
{
    if (!m_handle)
        return;
    mpv_command_string(m_handle, "frame-step");
}

void MpvCore::toggleFullscreen()
{
    if (!m_handle)
        return;
    mpv_command_string(m_handle, "cycle fullscreen");
}

namespace {
void dispatchHypr(const QStringList &args)
{
    QProcess::startDetached("hyprctl", args);
}
} // namespace

// Runs `hyprctl -j activewindow`, waits until the key holds the expected
// value, then invokes `then`. Polls every 50 ms; gives up after `timeoutMs`.
void MpvCore::whenHyprState(const QString &key, const char *value, bool isBool,
                            int timeoutMs, const std::function<void()> &then)
{
    auto *p = new QProcess;
    p->setProcessChannelMode(QProcess::MergedChannels);
    QObject::connect(p, &QProcess::finished, this, [this, p, key, value, isBool, timeoutMs, then] {
        const QJsonObject o = QJsonDocument::fromJson(p->readAll()).object();
        p->deleteLater();
        bool matched = false;
        if (isBool) {
            const bool expectTrue = (QLatin1String(value) == QLatin1String("true"));
            matched = o.value(key).toBool() == expectTrue;
        } else {
            matched = o.value(key).toVariant().toString() == QLatin1String(value);
        }
        if (matched) {
            then();
        } else if (timeoutMs <= 0) {
            m_fsTransitioning = false; // give up quietly and unlock
        } else {
            QTimer::singleShot(50, this, [this, key, value, isBool, timeoutMs, then] {
                whenHyprState(key, value, isBool, timeoutMs - 50, then);
            });
        }
    });
    p->start("hyprctl", { "-j", "activewindow" });
}

void MpvCore::windowFullscreen(bool on)
{
    if (!m_renderWindow)
        return;
    if (m_fsTransitioning)
        return;
    m_fsTransitioning = true;
    if (on) {
        // omaplayer is a *pinned* ("always on top") floating window by window
        // rule. Hyprland refuses to fullscreen a pinned window, and our float
        // ignores the plain QWindow::FullScreen request, so we drive both the
        // pin and the fullscreen through the Hyprland IPC (new DSL). Each step
        // waits for the compositor to actually apply the previous one, since
        // the dispatches can otherwise race and undo each other.
        dispatchHypr({ "dispatch", "hl.dsp.window.pin(false)" });
        whenHyprState(QStringLiteral("pinned"), "false", true, 1000, [this] {
            dispatchHypr({ "dispatch", "hl.dsp.window.fullscreen()" });
        });
        whenHyprState(QStringLiteral("fullscreen"), "2", false, 1500, [this] {
            m_fsTransitioning = false;
        });
    } else {
        dispatchHypr({ "dispatch", "hl.dsp.window.fullscreen()" });
        whenHyprState(QStringLiteral("fullscreen"), "0", false, 1000, [this] {
            dispatchHypr({ "dispatch", "hl.dsp.window.pin(true)" });
            m_fsTransitioning = false;
        });
    }
}

void MpvCore::toggleMinimize()
{
    if (m_renderWindow)
        m_renderWindow->setVisibility(QWindow::Minimized);
}

bool MpvCore::isFullscreen()
{
    return false; // handled by the window, kept here for symmetry
}

void MpvCore::takeScreenshot()
{
    if (!m_handle)
        return;
    mpv_command_string(m_handle, "screenshot-to-file ~/Pictures/omaplayer-${file-name}");
}

void MpvCore::openList(const QStringList &files)
{
    if (!m_handle || files.isEmpty())
        return;
    if (!m_renderContext) {
        // No render context yet (no first paint); remember all files so the
        // renderContext() path can start the full list once painting begins.
        m_pendingOpen = true;
        m_pendingFiles = files;
        return;
    }
    for (int i = 0; i < files.size(); ++i) {
        const QByteArray fn = files.at(i).toUtf8();
        const char *cmd[] = { "loadfile", fn.constData(), i == 0 ? nullptr : "append-play", nullptr };
        mpv_command(m_handle, cmd);
    }
    mpv_set_property_string(m_handle, "pause", "no");
}

void MpvCore::appendToPlaylist(const QStringList &files)
{
    if (!m_handle || files.isEmpty())
        return;
    if (!m_renderContext) {
        m_pendingOpen = true;
        m_pendingFiles = m_pendingFiles + files;
        return;
    }
    // "append-play" adds each file to the playlist without touching whatever
    // is currently playing; if nothing is loaded yet, the first file starts.
    for (const QString &f : files) {
        const QByteArray fn = f.toUtf8();
        const char *cmd[] = { "loadfile", fn.constData(), "append-play", nullptr };
        mpv_command(m_handle, cmd);
    }
}

QVariantList MpvCore::playlistItems()
{
    QVariantList out;
    if (!m_handle)
        return out;

    int64_t count = 0;
    if (mpv_get_property(m_handle, "playlist-count", MPV_FORMAT_INT64, &count) < 0 || count <= 0)
        return out;

    int64_t pos = 0;
    mpv_get_property(m_handle, "playlist-pos", MPV_FORMAT_INT64, &pos);

    for (int64_t i = 0; i < count; ++i) {
        const QByteArray base = "playlist/" + QByteArray::number(i) + "/";
        QVariantMap item;

        char *title = nullptr;
        if (mpv_get_property(m_handle, base + "title", MPV_FORMAT_STRING, &title) == 0 && title) {
            item["title"] = QString::fromUtf8(title);
            mpv_free(title);
        }

        char *filename = nullptr;
        if (mpv_get_property(m_handle, base + "filename", MPV_FORMAT_STRING, &filename) == 0 && filename) {
            QString path = QString::fromUtf8(filename);
            mpv_free(filename);
            item["path"] = path;
            if (!item.contains("title")) {
                const int slash = path.lastIndexOf('/');
                item["title"] = slash >= 0 ? path.mid(slash + 1) : path;
            }
        }

        if (item.contains("path")) {
            item["current"] = (i == pos);
            out.append(item);
        }
    }
    return out;
}

void MpvCore::toggleSubtitles()
{
    if (!m_handle)
        return;
    mpv_set_property_string(m_handle, "sub-visibility", m_subVisible ? "no" : "yes");
}

namespace {
void setDoubleProperty(mpv_handle *handle, const char *name, double value)
{
    if (!handle)
        return;
    const QByteArray text = QByteArray::number(value);
    mpv_set_property_string(handle, name, text.constData());
}
} // namespace

void MpvCore::setSpeed(double speed)
{
    if (m_speed != speed) {
        m_speed = speed;
        Q_EMIT speedChanged(m_speed);
    }
    setDoubleProperty(m_handle, "speed", m_speed);
}

void MpvCore::setBrightness(double value) { setDoubleProperty(m_handle, "brightness", value); }
void MpvCore::setContrast(double value) { setDoubleProperty(m_handle, "contrast", value); }
void MpvCore::setSaturation(double value) { setDoubleProperty(m_handle, "saturation", value); }
void MpvCore::setGamma(double value) { setDoubleProperty(m_handle, "gamma", value); }
void MpvCore::setSubScale(double value) { setDoubleProperty(m_handle, "sub-scale", value); }
void MpvCore::setAudioDelay(double value) { setDoubleProperty(m_handle, "audio-delay", value); }

bool MpvCore::hasNext()
{
    if (!m_handle)
        return false;
    int64_t count = 0;
    int64_t pos = 0;
    if (mpv_get_property(m_handle, "playlist-count", MPV_FORMAT_INT64, &count) < 0)
        return false;
    if (mpv_get_property(m_handle, "playlist-pos", MPV_FORMAT_INT64, &pos) < 0)
        return false;
    return pos >= 0 && pos + 1 < count;
}

bool MpvCore::hasPrevious()
{
    if (!m_handle)
        return false;
    int64_t pos = 0;
    if (mpv_get_property(m_handle, "playlist-pos", MPV_FORMAT_INT64, &pos) < 0)
        return false;
    return pos > 0;
}

void MpvCore::playlistNext()
{
    if (m_handle)
        mpv_command_string(m_handle, "playlist-next");
}

void MpvCore::playlistPrevious()
{
    if (m_handle)
        mpv_command_string(m_handle, "playlist-prev");
}

void MpvCore::playAt(int index)
{
    if (!m_handle)
        return;
    // Switch to the entry at that index in place — the rest of the playlist
    // stays intact (loadfile would have collapsed it to that one file).
    qint64 pos = index;
    mpv_set_property(m_handle, "playlist-pos", MPV_FORMAT_INT64, &pos);
    mpv_set_property_string(m_handle, "pause", "no");
}

void MpvCore::setLoopStatus(const QString &status)
{
    if (!m_handle)
        return;
    const QString s = status == QLatin1String("Track") ? QStringLiteral("inf")
                    : status == QLatin1String("Playlist") ? QStringLiteral("force")
                    : QStringLiteral("no");
    const QByteArray bytes = s.toUtf8();
    mpv_set_property_string(m_handle, "loop-playlist", bytes.constData());
}

QString MpvCore::loopStatus()
{
    return QStringLiteral("None");
}

void MpvCore::removePlaylistItem(int index)
{
    if (!m_handle)
        return;
    const QByteArray idx = QByteArray::number(index);
    const char *cmd[] = { "playlist-remove", idx.constData(), nullptr };
    mpv_command(m_handle, cmd);
}

void MpvCore::movePlaylistItem(int from, int to)
{
    if (!m_handle || from == to)
        return;
    // mpv's playlist-move takes <index1> <index2>: move the entry at index1
    // to just before index2 (negative numbers count from the end).
    const QByteArray f = QByteArray::number(from);
    const QByteArray t = QByteArray::number(to);
    const char *cmd[] = { "playlist-move", f.constData(), t.constData(), nullptr };
    mpv_command(m_handle, cmd);
}

QString MpvCore::savePlaylist(const QString &filePath)
{
    if (!m_handle || filePath.isEmpty())
        return QString();
    const QVariantList items = playlistItems();
    if (items.isEmpty())
        return QString();

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return QString();
    QTextStream out(&file);
    out << "#EXTM3U\n";
    for (const QVariant &v : items) {
        const QVariantMap m = v.toMap();
        const QString title = m.value("title").toString();
        QString path = m.value("path").toString();
        if (!title.isEmpty())
            out << "#EXTINF:-1," << title << "\n";
        // Store local files as absolute paths; remote URLs verbatim.
        out << path << "\n";
    }
    return filePath;
}
