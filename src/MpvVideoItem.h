#pragma once

#include <QQuickFramebufferObject>

// A Qt Quick item whose texture is the mpv OpenGL render output. Creating the
// FBO happens on the render thread; MpvCore::renderContext() lazily builds the
// mpv render context from that thread's OpenGL state, which is exactly when
// the context is current.
class MpvVideoItem : public QQuickFramebufferObject
{
    Q_OBJECT
    QML_ELEMENT

public:
    MpvVideoItem(QQuickItem *parent = nullptr);

    Renderer *createRenderer() const override;

protected:
    void itemChange(ItemChange change, const ItemChangeData &value) override;
};