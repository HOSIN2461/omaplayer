#include "MpvVideoItem.h"

#include "MpvCore.h"

#include <QOpenGLFramebufferObject>
#include <QOpenGLContext>
#include <QtGui/qopengl.h>

namespace {

class MpvRenderer : public QQuickFramebufferObject::Renderer
{
public:
    void render() override
    {
        const QSize size = framebufferObject()->size();
        const GLuint fbo = framebufferObject()->handle();
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        if (++s_frame % 60 == 1)
            qInfo("render fbo=%u %dx%d ctx=%p", fbo, size.width(), size.height(), (void *)ctx);
        MpvCore::instance()->renderFrame(GLint(fbo), size.width(), size.height());
    }

    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override
    {
        if (++s_created % 10 == 1)
            qInfo("FBO létrehozva: %dx%d", size.width(), size.height());
        return new QOpenGLFramebufferObject(size);
    }

    void synchronize(QQuickFramebufferObject *item) override
    {
        Q_UNUSED(item)
        if (++s_sync % 60 == 1)
            qInfo("synchronize");
    }

    static int s_frame;
    static int s_created;
    static int s_sync;
};

int MpvRenderer::s_frame = 0;
int MpvRenderer::s_created = 0;
int MpvRenderer::s_sync = 0;

} // namespace

MpvVideoItem::MpvVideoItem(QQuickItem *parent)
    : QQuickFramebufferObject(parent)
{
    setMirrorVertically(false);
    setTextureFollowsItemSize(true);
    setAntialiasing(false);
}

// Track the top-level window that hosts the item. PiP re-parents the item into
// a second window; the frame-update callback must repaint that window.
void MpvVideoItem::itemChange(ItemChange change, const ItemChangeData &value)
{
    if (change == ItemSceneChange)
        MpvCore::instance()->setRenderWindow(window());
    QQuickFramebufferObject::itemChange(change, value);
}

MpvVideoItem::Renderer *MpvVideoItem::createRenderer() const
{
    return new MpvRenderer;
}