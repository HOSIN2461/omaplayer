#include "MpvVideoItem.h"

#include "MpvCore.h"

#include <QOpenGLFramebufferObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QtGui/qopengl.h>

namespace {

class MpvRenderer : public QQuickFramebufferObject::Renderer
{
public:
    void render() override
    {
        const QSize size = framebufferObject()->size();
        const GLuint fbo = framebufferObject()->handle();
        QOpenGLFunctions *gl = QOpenGLContext::currentContext()
                                   ? QOpenGLContext::currentContext()->functions()
                                   : nullptr;
        if (!gl)
            return;
        // Bind our framebuffer explicitly and leave the previous binding
        // untouched; Qt does not guarantee the FBO is bound on entry.
        GLint previousFbo = 0;
        gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFbo);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl->glViewport(0, 0, size.width(), size.height());
        MpvCore::instance()->renderFrame(GLint(fbo), size.width(), size.height());
        gl->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previousFbo));
    }
};

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
    if (change == ItemSceneChange) {
        MpvCore::instance()->setRenderWindow(window());
        MpvCore::instance()->setRenderItem(this);
    }
    QQuickFramebufferObject::itemChange(change, value);
}

MpvVideoItem::Renderer *MpvVideoItem::createRenderer() const
{
    return new MpvRenderer;
}