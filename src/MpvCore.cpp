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
#include <QIcon>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QWindow>
#include <functional>

#include <cstring>
#include <algorithm>
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
    mpv_observe_property(m_handle, 0, "chapter-list", MPV_FORMAT_NODE);
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

    mpv_observe_property(m_handle, 0, "track-list", MPV_FORMAT_NODE);
    mpv_observe_property(m_handle, 0, "aid", MPV_FORMAT_INT64);
    mpv_observe_property(m_handle, 0, "sid", MPV_FORMAT_INT64);
    mpv_observe_property(m_handle, 0, "video-params", MPV_FORMAT_NODE);
    mpv_observe_property(m_handle, 0, "video-rotate", MPV_FORMAT_INT64);
    mpv_observe_property(m_handle, 0, "deinterlace", MPV_FORMAT_FLAG);
    mpv_observe_property(m_handle, 0, "sub-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_handle, 0, "sub-delay", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_handle, 0, "sub-font-size", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_handle, 0, "hue", MPV_FORMAT_DOUBLE);

    mpv_set_wakeup_callback(m_handle, &MpvCore::wakeupCallback, this);
    mpv_request_log_messages(m_handle, "warn");

    m_skipTimer = new QTimer(this);
    m_skipTimer->setSingleShot(true);
    m_skipTimer->setInterval(8000);
    connect(m_skipTimer, &QTimer::timeout, this, [this] { dismissSkipPrompt(); });

    // Debounce EQ rebuilds: lavfi af rebuilds re-init the audio chain, so only
    // apply after the user has stopped dragging a band for a moment.
    m_eqTimer = new QTimer(this);
    m_eqTimer->setSingleShot(true);
    m_eqTimer->setInterval(220);
    connect(m_eqTimer, &QTimer::timeout, this, [this] { buildAudioEqFilter(); });

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
    if (window)
        setupTray();
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
                    checkSkipPrompt();
                }
            } else if (prop->format == MPV_FORMAT_NODE && std::strcmp(name, "chapter-list") == 0) {
                recomputeSkipRanges(static_cast<mpv_node *>(prop->data));
            } else if (prop->format == MPV_FORMAT_DOUBLE && std::strcmp(name, "duration") == 0) {
                if (value != m_duration) {
                    m_duration = value;
                    Q_EMIT durationChanged(m_duration);
                    rebuildSkipRanges(); // last chapter's end becomes known
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
            } else if (prop->format == MPV_FORMAT_NODE && std::strcmp(name, "track-list") == 0) {
                refreshTracks();
            } else if (prop->format == MPV_FORMAT_INT64 && std::strcmp(name, "aid") == 0) {
                if (prop->data)
                    setCurrentAudioId(static_cast<int>(*static_cast<long long *>(prop->data)));
            } else if (prop->format == MPV_FORMAT_INT64 && std::strcmp(name, "sid") == 0) {
                if (prop->data)
                    setCurrentSubtitleId(static_cast<int>(*static_cast<long long *>(prop->data)));
            } else if (prop->format == MPV_FORMAT_NODE && std::strcmp(name, "video-params") == 0) {
                // Recompute the live video label (resolution/codec).
                refreshVideoLabel();
            } else if (prop->format == MPV_FORMAT_INT64 && std::strcmp(name, "video-rotate") == 0) {
                if (prop->data) {
                    const int rot = static_cast<int>(*static_cast<long long *>(prop->data));
                    if (rot != m_videoRotate) {
                        m_videoRotate = rot;
                        Q_EMIT videoRotateChanged();
                    }
                }
            } else if (prop->format == MPV_FORMAT_FLAG && std::strcmp(name, "deinterlace") == 0) {
                if (flag != m_deinterlaceEnabled) {
                    m_deinterlaceEnabled = flag;
                    Q_EMIT deinterlaceEnabledChanged();
                }
            } else if (prop->format == MPV_FORMAT_DOUBLE && std::strcmp(name, "sub-pos") == 0) {
                if (value != m_subPos) {
                    m_subPos = value;
                    Q_EMIT subPosChanged();
                }
            } else if (prop->format == MPV_FORMAT_DOUBLE && std::strcmp(name, "sub-delay") == 0) {
                if (value != m_subDelay) {
                    m_subDelay = value;
                    Q_EMIT subDelayChanged();
                }
            } else if (prop->format == MPV_FORMAT_DOUBLE && std::strcmp(name, "sub-font-size") == 0) {
                if (value != m_subFontSize) {
                    m_subFontSize = value;
                    Q_EMIT subFontSizeChanged();
                }
            } else if (prop->format == MPV_FORMAT_DOUBLE && std::strcmp(name, "hue") == 0) {
                if (value != m_hue) {
                    m_hue = value;
                    Q_EMIT hueChanged();
                }
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
    readOptions();
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

// --- Intro / recap / credits skipping -------------------------------

void MpvCore::recomputeSkipRanges(const mpv_node *list)
{
    m_rawChapters.clear();
    if (list && list->format == MPV_FORMAT_NODE_ARRAY) {
        for (int i = 0; i < list->u.list->num; ++i) {
            const mpv_node &entry = list->u.list->values[i];
            if (entry.format != MPV_FORMAT_NODE_MAP)
                continue;
            double time = 0.0;
            QString title;
            for (int j = 0; j < entry.u.list->num; ++j) {
                const char *key = entry.u.list->keys[j];
                const mpv_node &v = entry.u.list->values[j];
                if (std::strcmp(key, "time") == 0 && v.format == MPV_FORMAT_DOUBLE)
                    time = v.u.double_;
                else if (std::strcmp(key, "title") == 0
                         && v.format == MPV_FORMAT_STRING)
                    title = QString::fromUtf8(v.u.string);
            }
            m_rawChapters.append({ time, title });
        }
    }
    rebuildSkipRanges();
}

void MpvCore::rebuildSkipRanges()
{
    m_skipRanges.clear();
    setSkipPromptVisible(false);
    m_skipTimer->stop();
    const int n = m_rawChapters.size();
    if (n == 0)
        return;
    const double total = m_duration;
    for (int i = 0; i < n; ++i) {
        const double start = m_rawChapters.at(i).first;
        // mpv reports only the chapter start ("time"); the duration ends at
        // the next chapter, or the file duration for the last one.
        const double next = i + 1 < n ? m_rawChapters.at(i + 1).first : total;
        if (total <= 0.0 && i + 1 == n)
            continue; // last chapter's end is unknown yet
        const double end = next;
        if (end <= start + 0.5)
            continue;
        SkipType type;
        if (!classifyChapter(m_rawChapters.at(i).second, type))
            continue;
        const double len = end - start;
        if (len < 5.0 || len > (type == SkipType::Credits ? 600.0 : 360.0))
            continue;
        // Intro/recap only makes sense early on; credits only near the end.
        if (type == SkipType::Intro || type == SkipType::Recap) {
            if (total > 0.0 && start > total * 0.25)
                continue;
        } else if (total > 0.0 && end < total * 0.75) {
            continue;
        }
        m_skipRanges.append({ start, end, type, false });
    }
}

bool MpvCore::classifyChapter(const QString &title, SkipType &type) const
{
    const QStringList words = title.toLower()
                                  .split(QRegularExpression("\\W+"),
                                         Qt::SkipEmptyParts);
    for (const QString &w : words) {
        if (w == "intro" || w == "opening" || w == "preface"
            || w == "bevezetés" || w == "bevezető"
            || (w.startsWith("op") && w.length() <= 4)) {
            type = SkipType::Intro;
            return true;
        }
        if (w == "recap" || w == "recaps" || w == "previously"
            || w == "visszatekintés" || w == "visszatekintő") {
            type = SkipType::Recap;
            return true;
        }
        if (w == "credits" || w == "credit" || w == "ending" || w == "outro"
            || w == "stáblista" || (w.startsWith("ed") && w.length() <= 4)) {
            type = SkipType::Credits;
            return true;
        }
    }
    return false;
}

void MpvCore::checkSkipPrompt()
{
    if (m_skipRanges.isEmpty())
        return;
    for (int i = 0; i < m_skipRanges.size(); ++i) {
        const SkipRange &r = m_skipRanges.at(i);
        if (r.prompted || m_position < r.start || m_position >= r.end)
            continue;
        m_skipRanges[i].prompted = true;
        m_skipPromptRange = i;
        if (m_autoSkip) {
            skipRange(i);
            return;
        }
        m_skipPromptLabel = promptLabel(r.type);
        setSkipPromptVisible(true);
        Q_EMIT skipPromptChanged();
        m_skipTimer->start();
        return;
    }
}

void MpvCore::skipRange(int index)
{
    if (!m_handle || index < 0 || index >= m_skipRanges.size())
        return;
    const QByteArray sec = QByteArray::number(m_skipRanges.at(index).end);
    const char *cmd[] = { "seek", sec.constData(), "absolute", nullptr };
    mpv_command(m_handle, cmd);
}

void MpvCore::setSkipPromptVisible(bool visible)
{
    if (visible == m_skipPromptVisible)
        return;
    m_skipPromptVisible = visible;
    Q_EMIT skipPromptChanged();
}

QString MpvCore::promptLabel(SkipType type) const
{
    switch (type) {
    case SkipType::Intro:
        return tr("Bevezető kihagyása");
    case SkipType::Recap:
        return tr("Visszatekintés kihagyása");
    case SkipType::Credits:
        return tr("Stáblista kihagyása");
    }
    return {};
}

void MpvCore::skipCurrent()
{
    if (m_skipPromptRange >= 0 && m_skipPromptRange < m_skipRanges.size())
        skipRange(m_skipPromptRange);
    dismissSkipPrompt();
}

void MpvCore::dismissSkipPrompt()
{
    m_skipTimer->stop();
    m_skipPromptRange = -1;
    setSkipPromptVisible(false);
}

void MpvCore::setAutoSkip(bool on)
{
    if (on == m_autoSkip)
        return;
    if (on && m_skipPromptVisible)
        skipCurrent();
    m_autoSkip = on;
    Q_EMIT autoSkipChanged(m_autoSkip);
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

void MpvCore::setupTray()
{
    if (m_tray)
        return;

    m_tray = new QSystemTrayIcon(this);
    QIcon icon = QIcon::fromTheme(QStringLiteral("omaplayer"),
                                  QIcon::fromTheme(QStringLiteral("multimedia-player")));
    m_tray->setIcon(icon);
    m_tray->setToolTip(QStringLiteral("omaplayer"));

    auto *menu = new QMenu();
    auto *actPlayPause = menu->addAction(QStringLiteral("Lejátszás / Szünet"));
    auto *actPrev = menu->addAction(QStringLiteral("Előző"));
    auto *actNext = menu->addAction(QStringLiteral("Következő"));
    menu->addSeparator();
    auto *actShow = menu->addAction(QStringLiteral("Ablak megjelenítése"));
    menu->addSeparator();
    auto *actQuit = menu->addAction(QStringLiteral("Kilépés"));

    connect(actPlayPause, &QAction::triggered, this,
            [this] { togglePause(); });
    connect(actPrev, &QAction::triggered, this, [this] {
        if (hasPrevious())
            playlistPrevious();
    });
    connect(actNext, &QAction::triggered, this, [this] {
        if (hasNext())
            playlistNext();
    });
    connect(actShow, &QAction::triggered, this, [this] {
        if (m_renderWindow && m_renderWindow->visibility() == QWindow::Hidden)
            restoreFromTray();
        else
            hideToTray();
    });
    connect(actQuit, &QAction::triggered,
            [this] { QCoreApplication::quit(); });

    m_tray->setContextMenu(menu);
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger
                    || reason == QSystemTrayIcon::DoubleClick)
                    restoreFromTray();
            });
}

void MpvCore::restoreFromTray()
{
    if (m_renderWindow) {
        if (m_renderWindow->visibility() == QWindow::Hidden)
            m_renderWindow->show();
        m_renderWindow->raise();
        m_renderWindow->requestActivate();
    }
    if (m_tray)
        m_tray->hide();
}

void MpvCore::hideToTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        toggleMinimize(); // no tray host — at least get out of the way
        return;
    }
    if (m_tray)
        m_tray->show();
    if (m_renderWindow)
        m_renderWindow->hide();
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
    readOptions();
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

namespace {
void setStringProperty(mpv_handle *handle, const char *name, const QString &value)
{
    if (!handle)
        return;
    const QByteArray text = value.toUtf8();
    mpv_set_property_string(handle, name, text.constData());
}
int mpvNodeInt(const mpv_node &entry, const char *key, int fallback = -1)
{
    if (entry.format != MPV_FORMAT_NODE_MAP)
        return fallback;
    for (int j = 0; j < entry.u.list->num; ++j) {
        if (std::strcmp(entry.u.list->keys[j], key) == 0
            && entry.u.list->values[j].format == MPV_FORMAT_INT64)
            return static_cast<int>(entry.u.list->values[j].u.int64);
    }
    return fallback;
}
QString mpvNodeString(const mpv_node &entry, const char *key)
{
    if (entry.format != MPV_FORMAT_NODE_MAP)
        return {};
    for (int j = 0; j < entry.u.list->num; ++j) {
        if (std::strcmp(entry.u.list->keys[j], key) == 0
            && entry.u.list->values[j].format == MPV_FORMAT_STRING)
            return QString::fromUtf8(entry.u.list->values[j].u.string);
    }
    return {};
}
double mpvNodeDouble(const mpv_node &entry, const char *key, double fallback = 0.0)
{
    if (entry.format != MPV_FORMAT_NODE_MAP)
        return fallback;
    for (int j = 0; j < entry.u.list->num; ++j) {
        if (std::strcmp(entry.u.list->keys[j], key) == 0
            && entry.u.list->values[j].format == MPV_FORMAT_DOUBLE)
            return entry.u.list->values[j].u.double_;
    }
    return fallback;
}
bool mpvNodeInt64(mpv_handle *handle, const char *name, long long *out)
{
    return handle && mpv_get_property(handle, name, MPV_FORMAT_INT64, out) == 0;
}
bool mpvNodeStringOut(mpv_handle *handle, const char *name, QString *out)
{
    if (!handle)
        return false;
    char *str = nullptr;
    if (mpv_get_property(handle, name, MPV_FORMAT_STRING, &str) != 0 || !str)
        return false;
    *out = QString::fromUtf8(str);
    mpv_free(str);
    return true;
}
} // namespace

// Reads the current mpv option state into our mirrors the first time a file
// loads (before any observe fires for the current value).
void MpvCore::readOptions()
{
    if (!m_handle)
        return;

    QString aspect;
    if (mpvNodeStringOut(m_handle, "video-aspect-override", &aspect))
        m_videoAspect = aspect.isEmpty() ? QStringLiteral("no") : aspect;

    long long rot = 0;
    if (mpvNodeInt64(m_handle, "video-rotate", &rot))
        m_videoRotate = static_cast<int>(rot);

    char *hw = nullptr;
    if (mpv_get_property(m_handle, "hwdec", MPV_FORMAT_STRING, &hw) == 0 && hw) {
        m_hwdecEnabled = (QLatin1String(hw) != QLatin1String("no"));
        mpv_free(hw);
    }

    int deint = 0;
    if (mpv_get_property(m_handle, "deinterlace", MPV_FORMAT_FLAG, &deint) == 0)
        m_deinterlaceEnabled = deint != 0;

    char *tm = nullptr;
    if (mpv_get_property(m_handle, "tone-mapping", MPV_FORMAT_STRING, &tm) == 0 && tm) {
        m_hdrEnabled = (QLatin1String(tm) != QLatin1String("clip"));
        mpv_free(tm);
    }

    long long aid = -1, sid = -1;
    if (mpvNodeInt64(m_handle, "aid", &aid))
        m_currentAudioId = static_cast<int>(aid);
    if (mpvNodeInt64(m_handle, "sid", &sid))
        m_currentSubtitleId = static_cast<int>(sid);

    double subPos = 100, subDelay = 0, subFontSize = 55, hue = 0;
    if (mpv_get_property(m_handle, "sub-pos", MPV_FORMAT_DOUBLE, &subPos) == 0)
        m_subPos = subPos;
    if (mpv_get_property(m_handle, "sub-delay", MPV_FORMAT_DOUBLE, &subDelay) == 0)
        m_subDelay = subDelay;
    if (mpv_get_property(m_handle, "sub-font-size", MPV_FORMAT_DOUBLE, &subFontSize) == 0)
        m_subFontSize = subFontSize;
    if (mpv_get_property(m_handle, "hue", MPV_FORMAT_DOUBLE, &hue) == 0)
        m_hue = hue;

    mpvNodeStringOut(m_handle, "sub-font", &m_subFontFamily);
    mpvNodeStringOut(m_handle, "sub-color", &m_subColor);
    mpvNodeStringOut(m_handle, "sub-border-color", &m_subBorderColor);
    mpvNodeStringOut(m_handle, "sub-back-color", &m_subBackColor);
    double subBorder = 3.0;
    if (mpv_get_property(m_handle, "sub-border-size", MPV_FORMAT_DOUBLE, &subBorder) == 0)
        m_subBorderSize = subBorder;

    refreshVideoLabel();
    refreshTracks();
}

void MpvCore::refreshVideoLabel()
{
    if (!m_handle) {
        m_videoTrackLabel.clear();
        Q_EMIT videoTrackLabelChanged();
        return;
    }
    mpv_node node;
    QString label;
    if (mpv_get_property(m_handle, "video-params", MPV_FORMAT_NODE, &node) == 0) {
        int w = mpvNodeInt(node, "w", 0);
        int h = mpvNodeInt(node, "h", 0);
        QString codec = mpvNodeString(node, "codec");
        QString pixfmt = mpvNodeString(node, "pixelformat");
        if (w > 0 && h > 0) {
            label = QStringLiteral("%1×%2").arg(w).arg(h);
            if (!codec.isEmpty())
                label += QStringLiteral(" · %3").arg(codec);
            if (!pixfmt.isEmpty())
                label += QStringLiteral(" · %4").arg(pixfmt);
        }
        mpv_free_node_contents(&node);
    }
    if (label != m_videoTrackLabel) {
        m_videoTrackLabel = label;
        Q_EMIT videoTrackLabelChanged();
    }
}

void MpvCore::refreshTracks()
{
    if (!m_handle)
        return;

    // Recompute the human-readable label of the currently selected audio track
    // from the live track list (id/language/codec).
    mpv_node root{};
    QString label = m_audioTrackLabel;
    if (mpv_get_property(m_handle, "track-list", MPV_FORMAT_NODE, &root) == 0) {
        if (root.format == MPV_FORMAT_NODE_ARRAY) {
            for (int i = 0; i < root.u.list->num; ++i) {
                const mpv_node &entry = root.u.list->values[i];
                if (entry.format != MPV_FORMAT_NODE_MAP)
                    continue;
                if (mpvNodeString(entry, "type") != QLatin1String("audio"))
                    continue;
                if (mpvNodeInt(entry, "selected", 0) != 1)
                    continue;
                const int id = mpvNodeInt(entry, "id", -1);
                QString title = mpvNodeString(entry, "title");
                QString lang = mpvNodeString(entry, "lang");
                QString codec = mpvNodeString(entry, "codec");
                if (lang.isEmpty() && title.isEmpty())
                    title = tr("Hangsáv %1").arg(id);
                label = title;
                if (!lang.isEmpty())
                    label += QStringLiteral(" [%1]").arg(lang.toUpper());
                if (!codec.isEmpty() && codec != QLatin1String("unknown"))
                    label += QStringLiteral(" · %2").arg(codec);
                break;
            }
        }
        mpv_free_node_contents(&root);
    }

    if (label != m_audioTrackLabel) {
        m_audioTrackLabel = label;
        Q_EMIT currentAudioTrackChanged();
    }
}

void MpvCore::setCurrentAudioId(int id)
{
    if (id == m_currentAudioId)
        return;
    m_currentAudioId = id;
    // Update the human-readable label from the live track list.
    m_audioTrackLabel.clear();
    Q_EMIT currentAudioTrackChanged();
}

void MpvCore::setCurrentSubtitleId(int id)
{
    if (id == m_currentSubtitleId)
        return;
    m_currentSubtitleId = id;
    Q_EMIT currentSubtitleTrackChanged();
}

// --- video in/out -----------------------------------------------------

void MpvCore::setVideoAspect(const QString &aspect)
{
    const QString v = aspect.isEmpty() ? QStringLiteral("no") : aspect;
    if (v == m_videoAspect)
        return;
    m_videoAspect = v;
    Q_EMIT videoAspectChanged();
    if (m_handle) {
        const QByteArray bytes = v.toUtf8();
        mpv_set_property_string(m_handle, "video-aspect-override", bytes.constData());
    }
}

void MpvCore::setVideoCropAspect(const QString &aspect)
{
    if (!m_handle)
        return;
    // video-crop needs concrete pixel dimensions; read the current frame and
    // clip it to the largest centered rectangle matching the requested aspect.
    mpv_node node;
    int w = 0, h = 0;
    if (mpv_get_property(m_handle, "video-params", MPV_FORMAT_NODE, &node) == 0) {
        w = mpvNodeInt(node, "w", 0);
        h = mpvNodeInt(node, "h", 0);
        mpv_free_node_contents(&node);
    }
    if (w <= 0 || h <= 0)
        return;

    int num = 0, den = 0;
    const QStringList parts = aspect.split(':');
    if (parts.size() == 2) {
        num = parts[0].toInt();
        den = parts[1].toInt();
    }
    if (num <= 0 || den <= 0)
        return;

    int cw = w, ch = h;
    if (w * den > h * num) {
        // Wider than the target: crop the sides.
        cw = h * num / den;
    } else {
        // Taller than the target: crop the top/bottom.
        ch = w * den / num;
    }
    if (cw <= 0 || ch <= 0)
        return;
    const int x = (w - cw) / 2;
    const int y = (h - ch) / 2;
    const QString crop = QStringLiteral("%1x%2+%3+%4").arg(cw).arg(ch).arg(x).arg(y);
    const QByteArray bytes = crop.toUtf8();
    mpv_set_property_string(m_handle, "video-crop", bytes.constData());
}

void MpvCore::setCustomVideoCrop(int w, int h)
{
    if (!m_handle || w <= 0 || h <= 0)
        return;
    // Clamp the requested crop to the source frame size, centered.
    mpv_node node;
    int vw = 0, vh = 0;
    if (mpv_get_property(m_handle, "video-params", MPV_FORMAT_NODE, &node) == 0) {
        vw = mpvNodeInt(node, "w", 0);
        vh = mpvNodeInt(node, "h", 0);
        mpv_free_node_contents(&node);
    }
    if (vw <= 0 || vh <= 0)
        return;
    const int cw = std::min(w, vw);
    const int ch = std::min(h, vh);
    const int x = (vw - cw) / 2;
    const int y = (vh - ch) / 2;
    const QString crop = QStringLiteral("%1x%2+%3+%4").arg(cw).arg(ch).arg(x).arg(y);
    const QByteArray bytes = crop.toUtf8();
    mpv_set_property_string(m_handle, "video-crop", bytes.constData());
}

void MpvCore::clearVideoCrop()
{
    if (m_handle)
        mpv_set_property_string(m_handle, "video-crop", "no");
}

void MpvCore::setVideoRotate(int deg)
{
    deg = ((deg % 360) + 360) % 360;
    if (deg == m_videoRotate)
        return;
    m_videoRotate = deg;
    Q_EMIT videoRotateChanged();
    if (m_handle) {
        const QByteArray bytes = QByteArray::number(deg);
        mpv_set_property_string(m_handle, "video-rotate", bytes.constData());
    }
}

void MpvCore::setHwdecEnabled(bool on)
{
    if (on == m_hwdecEnabled)
        return;
    m_hwdecEnabled = on;
    Q_EMIT hwdecEnabledChanged();
    if (m_handle)
        mpv_set_property_string(m_handle, "hwdec", on ? "auto" : "no");
}

void MpvCore::setDeinterlaceEnabled(bool on)
{
    if (on == m_deinterlaceEnabled)
        return;
    m_deinterlaceEnabled = on;
    Q_EMIT deinterlaceEnabledChanged();
    if (m_handle)
        mpv_set_property_string(m_handle, "deinterlace", on ? "yes" : "no");
}

void MpvCore::setHdrEnabled(bool on)
{
    if (on == m_hdrEnabled)
        return;
    m_hdrEnabled = on;
    Q_EMIT hdrEnabledChanged();
    if (!m_handle)
        return;
    // HDR off = force plain SDR tone clamp (no HDR peak detection).
    if (on) {
        mpv_set_property_string(m_handle, "tone-mapping", "auto");
        mpv_set_property_string(m_handle, "hdr-compute-peak", "auto");
    } else {
        mpv_set_property_string(m_handle, "tone-mapping", "clip");
        mpv_set_property_string(m_handle, "hdr-compute-peak", "no");
    }
}

void MpvCore::setHue(double value)
{
    if (value != m_hue) {
        m_hue = value;
        Q_EMIT hueChanged();
    }
    setDoubleProperty(m_handle, "hue", m_hue);
}

// --- audio ------------------------------------------------------------

QVariantList MpvCore::audioTracks()
{
    QVariantList out;
    if (!m_handle)
        return out;
    mpv_node root{};
    if (mpv_get_property(m_handle, "track-list", MPV_FORMAT_NODE, &root) != 0) {
        return out;
    }
    if (root.format != MPV_FORMAT_NODE_ARRAY) {
        mpv_free_node_contents(&root);
        return out;
    }
    for (int i = 0; i < root.u.list->num; ++i) {
        const mpv_node &entry = root.u.list->values[i];
        if (entry.format != MPV_FORMAT_NODE_MAP)
            continue;
        if (mpvNodeString(entry, "type") != QLatin1String("audio"))
            continue;
        const int id = mpvNodeInt(entry, "id", -1);
        QString title = mpvNodeString(entry, "title");
        QString lang = mpvNodeString(entry, "lang");
        QString codec = mpvNodeString(entry, "codec");
        if (lang.isEmpty() && title.isEmpty())
            title = tr("Hangsáv %1").arg(id);
        QString label = title;
        if (!lang.isEmpty())
            label += QStringLiteral(" [%1]").arg(lang.toUpper());
        if (!codec.isEmpty() && codec != QLatin1String("unknown"))
            label += QStringLiteral(" · %2").arg(codec);
        QVariantMap t;
        t["id"] = id;
        t["title"] = label;
        t["selected"] = (id == m_currentAudioId);
        out.append(t);
    }
    mpv_free_node_contents(&root);
    return out;
}

void MpvCore::setAudioTrack(int id)
{
    if (!m_handle)
        return;
    const QByteArray bytes = QByteArray::number(id);
    mpv_set_property_string(m_handle, "aid", bytes.constData());
}

void MpvCore::setAudioEqBand(int index, double gain)
{
    if (index < 0 || index >= m_audioEqGains.size())
        return;
    m_audioEqGains[index] = gain;
    Q_EMIT audioEqGainsChanged();
    if (m_eqTimer)
        m_eqTimer->start();
}

void MpvCore::setAudioEqGains(const QVariantList &gains)
{
    for (int i = 0; i < gains.size() && i < m_audioEqGains.size(); ++i)
        m_audioEqGains[i] = gains.at(i);
    Q_EMIT audioEqGainsChanged();
    if (m_eqTimer)
        m_eqTimer->start();
}

void MpvCore::resetAudioEq()
{
    setAudioEqGains(QVariantList() << 0.0 << 0.0 << 0.0 << 0.0 << 0.0
                                   << 0.0 << 0.0 << 0.0 << 0.0 << 0.0);
}

void MpvCore::applyAudioEq()
{
    buildAudioEqFilter();
}

void MpvCore::buildAudioEqFilter()
{
    if (!m_handle)
        return;
    // 10-band graphic EQ via lavfi peaking filters. Frequencies follow the
    // ISO one-third-octave ladder from 31 Hz to 16 kHz.
    static const double freqs[10] = {
        31.0, 62.0, 125.0, 250.0, 500.0,
        1000.0, 2000.0, 4000.0, 8000.0, 16000.0,
    };
    QString chain = QStringLiteral("lavfi=[");
    for (int i = 0; i < 10; ++i) {
        const double g = m_audioEqGains.at(i).toDouble();
        if (i > 0)
            chain += QLatin1Char(',');
        chain += QStringLiteral("equalizer=f=%1:t=o:w=1:g=%2")
                     .arg(freqs[i], 0, 'f', 0)
                     .arg(g, 0, 'f', 2);
    }
    chain += QLatin1Char(']');
    setStringProperty(m_handle, "af", chain);
}

void MpvCore::loadExternalAudio(const QString &path)
{
    if (!m_handle || path.isEmpty())
        return;
    const QByteArray bytes = path.toUtf8();
    const char *cmd[] = { "audio-add", bytes.constData(), "auto", nullptr };
    mpv_command(m_handle, cmd);
}

// --- subtitles --------------------------------------------------------

QVariantList MpvCore::subtitleTracks()
{
    QVariantList out;
    if (!m_handle)
        return out;
    mpv_node root{};
    if (mpv_get_property(m_handle, "track-list", MPV_FORMAT_NODE, &root) != 0) {
        return out;
    }
    if (root.format != MPV_FORMAT_NODE_ARRAY) {
        mpv_free_node_contents(&root);
        return out;
    }
    for (int i = 0; i < root.u.list->num; ++i) {
        const mpv_node &entry = root.u.list->values[i];
        if (entry.format != MPV_FORMAT_NODE_MAP)
            continue;
        if (mpvNodeString(entry, "type") != QLatin1String("sub"))
            continue;
        const int id = mpvNodeInt(entry, "id", -1);
        QString title = mpvNodeString(entry, "title");
        QString lang = mpvNodeString(entry, "lang");
        if (lang.isEmpty() && title.isEmpty())
            title = tr("Felirat %1").arg(id);
        QString label = title;
        if (!lang.isEmpty())
            label += QStringLiteral(" [%1]").arg(lang.toUpper());
        QVariantMap t;
        t["id"] = id;
        t["title"] = label;
        t["selected"] = (id == m_currentSubtitleId);
        out.append(t);
    }
    mpv_free_node_contents(&root);
    return out;
}

void MpvCore::setSubtitleTrack(int id)
{
    if (!m_handle)
        return;
    const QByteArray bytes = QByteArray::number(id);
    mpv_set_property_string(m_handle, "sid", bytes.constData());
}

void MpvCore::setSubDelay(double value)
{
    if (value != m_subDelay) {
        m_subDelay = value;
        Q_EMIT subDelayChanged();
    }
    setDoubleProperty(m_handle, "sub-delay", m_subDelay);
}

void MpvCore::setSubPos(double value)
{
    value = std::max(0.0, std::min(150.0, value));
    if (value != m_subPos) {
        m_subPos = value;
        Q_EMIT subPosChanged();
    }
    setDoubleProperty(m_handle, "sub-pos", m_subPos);
}

void MpvCore::setSubFontSize(double value)
{
    if (value != m_subFontSize) {
        m_subFontSize = value;
        Q_EMIT subFontSizeChanged();
    }
    setDoubleProperty(m_handle, "sub-font-size", m_subFontSize);
}

void MpvCore::setSubFontFamily(const QString &family)
{
    const QString v = family.isEmpty() ? QStringLiteral("Sans") : family;
    if (v != m_subFontFamily) {
        m_subFontFamily = v;
        Q_EMIT subFontFamilyChanged();
    }
    setStringProperty(m_handle, "sub-font", m_subFontFamily);
}

void MpvCore::setSubColor(const QString &color)
{
    if (color != m_subColor) {
        m_subColor = color;
        Q_EMIT subColorChanged();
    }
    setStringProperty(m_handle, "sub-color", m_subColor);
}

void MpvCore::setSubBorderColor(const QString &color)
{
    if (color != m_subBorderColor) {
        m_subBorderColor = color;
        Q_EMIT subBorderColorChanged();
    }
    setStringProperty(m_handle, "sub-border-color", m_subBorderColor);
}

void MpvCore::setSubBorderSize(double value)
{
    if (value != m_subBorderSize) {
        m_subBorderSize = value;
        Q_EMIT subBorderSizeChanged();
    }
    setDoubleProperty(m_handle, "sub-border-size", m_subBorderSize);
}

void MpvCore::setSubBackColor(const QString &color)
{
    if (color != m_subBackColor) {
        m_subBackColor = color;
        Q_EMIT subBackColorChanged();
    }
    setStringProperty(m_handle, "sub-back-color", m_subBackColor);
}

void MpvCore::setSubtitleColor(const QString &color)
{
    setSubColor(color);
}

void MpvCore::loadExternalSubtitle(const QString &path)
{
    if (!m_handle || path.isEmpty())
        return;
    const QByteArray bytes = path.toUtf8();
    const char *cmd[] = { "sub-add", bytes.constData(), "auto", nullptr };
    mpv_command(m_handle, cmd);
}

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
