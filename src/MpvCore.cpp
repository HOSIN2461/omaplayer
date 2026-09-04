#include "MpvCore.h"

#include <QOpenGLContext>
#include <QQuickWindow>
#include <QTimer>

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
            if (++core->m_updateCount % 60 == 1)
                qInfo("onUpdate -> window=%p title=\"%s\" visible=%d", (void *)window,
                      qPrintable(window->title()), int(window->isVisible()));
            window->update();
        } else {
            qInfo("onUpdate: m_renderWindow NULL");
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

    mpv_set_wakeup_callback(m_handle, &MpvCore::wakeupCallback, this);

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
        mpv_render_context_set_update_callback(m_renderContext, nullptr, nullptr);
        mpv_render_context_free(m_renderContext);
        m_renderContext = nullptr;
        m_renderGlContext = nullptr;
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
    return m_renderContext;
}

void MpvCore::setRenderWindow(QQuickWindow *window)
{
    m_renderWindow = window;
}

void MpvCore::renderFrame(int fbo, int width, int height)
{
    if (!m_renderContext)
        return;
    mpv_opengl_fbo mpvFbo{
        fbo,
        width,
        height,
        0, // internal_format, 0 = keep
    };
    int flipY = 1;
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_OPENGL_FBO, &mpvFbo },
        { MPV_RENDER_PARAM_FLIP_Y, &flipY },
        { MPV_RENDER_PARAM_INVALID, nullptr },
    };
    mpv_render_context_render(m_renderContext, params);
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
    const char *cmd[] = { "loadfile", location.toUtf8().constData(), nullptr };
    mpv_command(m_handle, cmd);
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