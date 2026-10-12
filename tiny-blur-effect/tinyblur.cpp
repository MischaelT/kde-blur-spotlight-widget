/*
    SPDX-FileCopyrightText: 2010 Fredrik Höglund <fredrik@kde.org>
    SPDX-FileCopyrightText: 2011 Philipp Knechtges <philipp-dev@knechtges.com>
    SPDX-FileCopyrightText: 2018 Alex Nemeth <alex.nemeth329@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "tinyblur.h"

#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/glplatform.h"
#include "scene/backgroundeffectitem.h"
#include "scene/decorationitem.h"
#include "scene/scene.h"
#include "scene/surfaceitem.h"
#include "scene/windowitem.h"
#include "wayland/backgroundeffect_v1.h"
#include "wayland/display.h"
#include "wayland/surface.h"
#include "wayland_server.h"
#include "window.h"

#include <QGuiApplication>
#include <QDBusConnection>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QMatrix4x4>
#include <QSettings>
#include <QTimer>
#include <QWindow>

#include <array>

Q_LOGGING_CATEGORY(KWIN_TINYBLUR, "kwin_effect_tinyblur", QtWarningMsg);

static void ensureResources()
{
    Q_INIT_RESOURCE(tinyblur);
}

namespace KWin
{

static bool isBlurRectangleWindow(EffectWindow *w)
{
    // Menus share the overlay's application ID, but their destruction must not
    // clear its zoom state or apply the overlay's regions to a popup surface.
    if (!w || w->isPopupWindow() || w->isPopupMenu() || w->isMenu() || w->isTooltip()) {
        return false;
    }

    const QString windowClass = w->windowClass();
    if (windowClass == QStringLiteral("blur-rectangle")
        || windowClass == QStringLiteral("org.local.blurrectangle")) {
        return true;
    }

    if (w->caption() == QStringLiteral("Blur Rectangle")) {
        return true;
    }

    if (Window *window = w->window()) {
        return window->desktopFileName() == QStringLiteral("blur-rectangle")
            || window->desktopFileName() == QStringLiteral("org.local.blurrectangle")
            || window->resourceClass() == QStringLiteral("blur-rectangle")
            || window->resourceClass() == QStringLiteral("org.local.blurrectangle")
            || window->resourceName() == QStringLiteral("blur-rectangle")
            || window->resourceName() == QStringLiteral("org.local.blurrectangle");
    }

    return false;
}

static QMatrix4x4 colorTransformMatrix(qreal saturation, qreal contrast)
{
    QMatrix4x4 saturationMatrix;
    QMatrix4x4 contrastMatrix;

    if (!qFuzzyCompare(saturation, 1.0)) {
        const qreal rval = (1.0 - saturation) * 0.2126;
        const qreal gval = (1.0 - saturation) * 0.7152;
        const qreal bval = (1.0 - saturation) * 0.0722;

        saturationMatrix = QMatrix4x4(rval + saturation, rval, rval, 0.0,
                                      gval, gval + saturation, gval, 0.0,
                                      bval, bval, bval + saturation, 0.0,
                                      0.0, 0.0, 0.0, 1.0);
    }

    if (!qFuzzyCompare(contrast, 1.0)) {
        const float transl = (1.0 - contrast) / 2.0;

        contrastMatrix = QMatrix4x4(contrast, 0.0, 0.0, 0.0,
                                    0.0, contrast, 0.0, 0.0,
                                    0.0, 0.0, contrast, 0.0,
                                    transl, transl, transl, 1.0);
    }

    return contrastMatrix * saturationMatrix;
}

TinyBlurEffect::TinyBlurEffect()
{
    ensureResources();

    m_onscreenPass.shader = ShaderManager::instance()->generateShaderFromFile(ShaderTrait::MapTexture,
                                                                              QStringLiteral(":/effects/stableblur/shaders/vertex.vert"),
                                                                              QStringLiteral(":/effects/stableblur/shaders/onscreen.frag"));
    if (!m_onscreenPass.shader
        || m_onscreenPass.shader->uniformLocation("zoomFactors[0]") < 0
        || m_onscreenPass.shader->uniformLocation("originalTexture") < 0) {
        qCWarning(KWIN_TINYBLUR) << "Failed to load onscreen pass shader";
        return;
    } else {
        m_onscreenPass.mvpMatrixLocation = m_onscreenPass.shader->uniformLocation("modelViewProjectionMatrix");
        m_onscreenPass.colorMatrixLocation = m_onscreenPass.shader->uniformLocation("colorMatrix");
        m_onscreenPass.offsetLocation = m_onscreenPass.shader->uniformLocation("offset");
        m_onscreenPass.halfpixelLocation = m_onscreenPass.shader->uniformLocation("halfpixel");
    }

    m_downsamplePass.shader = ShaderManager::instance()->generateShaderFromFile(ShaderTrait::MapTexture,
                                                                                QStringLiteral(":/effects/stableblur/shaders/vertex.vert"),
                                                                                QStringLiteral(":/effects/stableblur/shaders/downsample.frag"));
    if (!m_downsamplePass.shader) {
        qCWarning(KWIN_TINYBLUR) << "Failed to load downsampling pass shader";
        return;
    } else {
        m_downsamplePass.mvpMatrixLocation = m_downsamplePass.shader->uniformLocation("modelViewProjectionMatrix");
        m_downsamplePass.offsetLocation = m_downsamplePass.shader->uniformLocation("offset");
        m_downsamplePass.halfpixelLocation = m_downsamplePass.shader->uniformLocation("halfpixel");
    }

    m_upsamplePass.shader = ShaderManager::instance()->generateShaderFromFile(ShaderTrait::MapTexture,
                                                                              QStringLiteral(":/effects/stableblur/shaders/vertex.vert"),
                                                                              QStringLiteral(":/effects/stableblur/shaders/upsample.frag"));
    if (!m_upsamplePass.shader) {
        qCWarning(KWIN_TINYBLUR) << "Failed to load upsampling pass shader";
        return;
    } else {
        m_upsamplePass.mvpMatrixLocation = m_upsamplePass.shader->uniformLocation("modelViewProjectionMatrix");
        m_upsamplePass.offsetLocation = m_upsamplePass.shader->uniformLocation("offset");
        m_upsamplePass.halfpixelLocation = m_upsamplePass.shader->uniformLocation("halfpixel");
    }

    reconfigure(ReconfigureAll);

    waylandServer()->backgroundEffectManager()->addBlurCapability();

    connect(effects, &EffectsHandler::windowAdded, this, &TinyBlurEffect::slotWindowAdded);
    connect(effects, &EffectsHandler::windowDeleted, this, &TinyBlurEffect::slotWindowDeleted);
    connect(effects, &EffectsHandler::viewRemoved, this, &TinyBlurEffect::slotViewRemoved);
    // Fetch the blur regions for all windows
    const auto stackingOrder = effects->stackingOrder();
    for (EffectWindow *window : stackingOrder) {
        slotWindowAdded(window);
    }

    m_valid = true;
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/org/local/StableBlur"), this,
                                                 QDBusConnection::ExportScriptableSlots);
}

TinyBlurEffect::~TinyBlurEffect()
{
    QDBusConnection::sessionBus().unregisterObject(QStringLiteral("/org/local/StableBlur"));
    waylandServer()->backgroundEffectManager()->removeBlurCapability();
}

void TinyBlurEffect::setZoomRegions(const QString &json)
{
    const QJsonObject payload = QJsonDocument::fromJson(json.toUtf8()).object();
    m_zoomRegions.clear();
    m_zoomFactors.clear();
    m_zoomCircle = payload.value(QStringLiteral("circle")).toBool();
    m_zoomBlurred = payload.value(QStringLiteral("blurred")).toBool();
    for (const QJsonValue &value : payload.value(QStringLiteral("regions")).toArray()) {
        const QJsonArray rect = value.toArray();
        if (m_zoomBlurred || (rect.size() != 4 && rect.size() != 5) || m_zoomRegions.size() >= 16) {
            continue;
        }
        const QRectF bounds(rect[0].toDouble(), rect[1].toDouble(), rect[2].toDouble(), rect[3].toDouble());
        if (bounds.isValid()) {
            m_zoomRegions.append(bounds);
            m_zoomFactors.append(rect.size() == 5 ? float(qBound(1.0, rect[4].toDouble(1.15), 3.0)) : 1.15f);
        }
    }
    for (EffectWindow *window : effects->stackingOrder()) {
        if (isBlurRectangleWindow(window)) {
            updateBlurRegion(window);
        }
    }
    effects->addRepaintFull();
}

void TinyBlurEffect::reconfigure(ReconfigureFlags flags)
{
    Q_UNUSED(flags)

    struct BlurTuning {
        size_t iterations;
        float offset;
    };
    static constexpr std::array<BlurTuning, 10> tuning = {{
        {1, 0.60f},
        {1, 1.00f},
        {1, 1.60f},
        {2, 1.20f},
        {2, 2.00f},
        {3, 1.60f},
        {3, 2.40f},
        {4, 2.00f},
        {4, 2.80f},
        {4, 3.60f},
    }};

    QSettings settings(QStringLiteral("local"), QStringLiteral("blur-rectangle"));
    const int level = qBound(1, settings.value(QStringLiteral("blurLevel"), 4).toInt(), 10);
    m_iterationCount = tuning.at(level - 1).iterations;
    m_offset = tuning.at(level - 1).offset;
    m_expandSize = 0;
    m_colorMatrix = colorTransformMatrix(1.0, 1.0);
    for (auto &[window, data] : m_windows) {
        data.blurItem->setPixelsToExpandRepaintsBelowOpaqueRegions(m_expandSize);
    }

    // Update all windows for the blur to take effect
    effects->addRepaintFull();
}

void TinyBlurEffect::updateBlurRegion(EffectWindow *w)
{
    std::optional<RegionF> content;
    std::optional<RegionF> frame;
    bool hasExplicitBlurRegion = false;

    if (SurfaceInterface *surface = w->surface()) {
        if (!surface->blurRegion().isEmpty()) {
            content = surface->blurRegion();
            hasExplicitBlurRegion = true;
        }
    }

    if (auto internal = w->internalWindow()) {
        const auto property = internal->property("kwin_blur");
        if (property.isValid()) {
            content = property.value<RegionF>();
            hasExplicitBlurRegion = true;
        }
    }

    const bool isMaskWindow = isBlurRectangleWindow(w);
    if (isMaskWindow && w->window()) {
        // xdg-shell does not expose Qt's WindowStaysOnTopHint on Wayland.
        w->window()->setKeepAbove(true);
    }
    if (!hasExplicitBlurRegion && !isMaskWindow) {
        if (auto it = m_windows.find(w); it != m_windows.end()) {
            effects->makeOpenGLContextCurrent();
            m_windows.erase(it);
        }
        return;
    }

    if (!content.has_value()) {
        content = RegionF();
    }

    {
        TinyBlurEffectData &data = m_windows[w];
        data.content = content;
        data.frame = frame;
        if (!data.blurItem) {
            data.blurItem = std::make_unique<BackgroundEffectItem>(w->windowItem());
        }
        data.blurItem->setPixelsToExpandRepaintsBelowOpaqueRegions(m_expandSize);
        data.blurItem->setEffectBoundingRect(blurRegion(w).boundingRect());
    }

    if (isMaskWindow) {
        qCWarning(KWIN_TINYBLUR) << "Blur Rectangle registered; explicit region:" << hasExplicitBlurRegion
                                 << "region:" << blurRegion(w).boundingRect();
    }
}

void TinyBlurEffect::slotWindowAdded(EffectWindow *w)
{
    SurfaceInterface *surf = w->surface();

    if (surf) {
        windowBlurChangedConnections[w] = connect(surf, &SurfaceInterface::blurChanged, this, [this, w]() {
            if (w) {
                updateBlurRegion(w);
            }
        });
    }
    if (auto internal = w->internalWindow()) {
        internal->installEventFilter(this);
    }

    updateBlurRegion(w);
}

void TinyBlurEffect::slotWindowDeleted(EffectWindow *w)
{
    if (isBlurRectangleWindow(w)) {
        m_zoomRegions.clear();
        m_zoomFactors.clear();
    }
    if (auto it = m_windows.find(w); it != m_windows.end()) {
        effects->makeOpenGLContextCurrent();
        m_windows.erase(it);
    }
    if (auto it = windowBlurChangedConnections.find(w); it != windowBlurChangedConnections.end()) {
        disconnect(*it);
        windowBlurChangedConnections.erase(it);
    }
}

void TinyBlurEffect::slotViewRemoved(KWin::RenderView *view)
{
    for (auto &[window, data] : m_windows) {
        if (auto it = data.render.find(view); it != data.render.end()) {
            effects->makeOpenGLContextCurrent();
            data.render.erase(it);
        }
    }
}

bool TinyBlurEffect::eventFilter(QObject *watched, QEvent *event)
{
    auto internal = qobject_cast<QWindow *>(watched);
    if (internal && event->type() == QEvent::DynamicPropertyChange) {
        QDynamicPropertyChangeEvent *pe = static_cast<QDynamicPropertyChangeEvent *>(event);
        if (pe->propertyName() == "kwin_blur") {
            if (auto w = effects->findWindow(internal)) {
                updateBlurRegion(w);
            }
        }
    }
    return false;
}

bool TinyBlurEffect::enabledByDefault()
{
    const auto context = effects->openglContext();
    if (!context || context->isSoftwareRenderer()) {
        return false;
    }
    GLPlatform *gl = context->glPlatform();

    if (gl->isIntel() && gl->chipClass() < SandyBridge) {
        return false;
    }
    if (gl->isPanfrost() && gl->chipClass() <= MaliT8XX) {
        return false;
    }
    // The blur effect works, but is painfully slow (FPS < 5) on Mali and VideoCore
    if (gl->isLima() || gl->isVideoCore4() || gl->isVideoCore3D()) {
        return false;
    }
    return true;
}

bool TinyBlurEffect::supported()
{
    return effects->isOpenGLCompositing();
}

RegionF TinyBlurEffect::blurRegion(EffectWindow *w) const
{
    RegionF region;

    if (auto it = m_windows.find(w); it != m_windows.end()) {
        const std::optional<RegionF> &content = it->second.content;
        const std::optional<RegionF> &frame = it->second.frame;
        if (content.has_value()) {
            if (content->isEmpty()) {
                // An empty region means that the blur effect should be enabled
                // for the whole window.
                region = w->contentsRect();
            } else {
                region = content->translated(w->contentsRect().topLeft()) & w->contentsRect();
            }
            if (frame.has_value()) {
                region += frame.value();
            }
        } else if (frame.has_value()) {
            region = frame.value();
        }
    }

    if (isBlurRectangleWindow(w)) {
        for (const QRectF &bounds : m_zoomRegions) {
            region += RegionF(QRegion(bounds.toAlignedRect(), m_zoomCircle ? QRegion::Ellipse : QRegion::Rectangle));
        }
    }
    return region;
}

void TinyBlurEffect::prePaintScreen(ScreenPrePaintData &data)
{
    m_currentView = data.view;

    effects->prePaintScreen(data);
}

void TinyBlurEffect::postPaintScreen()
{
    effects->postPaintScreen();
    if (!m_zoomRegions.isEmpty()) {
        // Schedule after this frame's damage is consumed, so stationary lenses
        // continue sampling live content without waiting for pointer motion.
        effects->addRepaintFull();
    }
}

bool TinyBlurEffect::shouldBlur(const EffectWindow *w, int mask, const WindowPaintData &data) const
{
    if (effects->activeFullScreenEffect() && !w->data(WindowForceBlurRole).toBool()) {
        return false;
    }

    if (w->isDesktop()) {
        return false;
    }

    bool scaled = !qFuzzyCompare(data.xScale(), 1.0) && !qFuzzyCompare(data.yScale(), 1.0);
    bool translated = data.xTranslation() || data.yTranslation();

    if ((scaled || (translated || (mask & PAINT_WINDOW_TRANSFORMED))) && !w->data(WindowForceBlurRole).toBool()) {
        return false;
    }

    return true;
}

void TinyBlurEffect::drawWindow(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *w, int mask, const Region &deviceRegion, WindowPaintData &data)
{
    blur(renderTarget, viewport, w, mask, deviceRegion, data);

    // Draw the window over the blurred area
    effects->drawWindow(renderTarget, viewport, w, mask, deviceRegion, data);
}

void TinyBlurEffect::blur(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *w, int mask, const Region &deviceRegion, WindowPaintData &data)
{
    auto it = m_windows.find(w);
    if (it == m_windows.end()) {
        return;
    }

    TinyBlurEffectData &blurInfo = it->second;
    TinyBlurRenderData &renderInfo = blurInfo.render[m_currentView];
    if (!shouldBlur(w, mask, data)) {
        return;
    }

    // Compute the effective blur shape. Note that if the window is transformed, so will be the blur shape.
    RegionF blurShape = blurRegion(w);
    if (data.xScale() != 1 || data.yScale() != 1) {
        blurShape.scale(data.xScale(), data.yScale());
    }
    if (data.xTranslation() || data.yTranslation()) {
        blurShape.translate(data.xTranslation(), data.yTranslation());
    }

    blurShape.translate(w->pos());

    const Rect backgroundRect = blurShape.boundingRect().rounded();
    const Rect scaledBackgroundRect = backgroundRect.scaled(viewport.scale()).rounded();
    const Rect deviceBackgroundRect = viewport.mapToDeviceCoordinates(backgroundRect).rounded();
    const auto opacity = w->opacity() * data.opacity();

    // Get the effective shape that will be actually blurred. It's possible that all of it will be clipped.
    QList<RectF> effectiveShape;
    effectiveShape.reserve(blurShape.rects().size());
    if (deviceRegion != Region::infinite()) {
        for (const Rect &clipRect : deviceRegion.rects()) {
            const RectF deviceClipRect = clipRect.translated(-deviceBackgroundRect.topLeft());
            for (const RectF &shapeRect : blurShape.rects()) {
                const RectF deviceShapeRect = shapeRect.translated(-backgroundRect.topLeft()).scaled(viewport.scale()).rounded();
                if (const RectF intersected = deviceClipRect.intersected(deviceShapeRect); !intersected.isEmpty()) {
                    effectiveShape.append(intersected);
                }
            }
        }
    } else {
        for (const RectF &rect : blurShape.rects()) {
            effectiveShape.append(rect.translated(-backgroundRect.topLeft()).scaled(viewport.scale()).rounded());
        }
    }
    if (effectiveShape.isEmpty()) {
        return;
    }

    // Maybe reallocate offscreen render targets. Keep in mind that the first one contains
    // original background behind the window, it's not blurred.
    GLenum textureFormat = GL_RGBA8;
    if (renderTarget.texture()) {
        textureFormat = renderTarget.texture()->internalFormat();
    }

    size_t iterationCount = m_iterationCount;
    while (iterationCount > 1
           && (backgroundRect.width() / (1 << iterationCount) < 1
               || backgroundRect.height() / (1 << iterationCount) < 1)) {
        --iterationCount;
    }

    if (renderInfo.framebuffers.size() != (iterationCount + 1) || renderInfo.textures[0]->size() != backgroundRect.size() || renderInfo.textures[0]->internalFormat() != textureFormat) {
        renderInfo.framebuffers.clear();
        renderInfo.textures.clear();

        glClearColor(0, 0, 0, 0);
        for (size_t i = 0; i <= iterationCount; ++i) {
            auto texture = GLTexture::allocate(textureFormat, backgroundRect.size() / (1 << i));
            if (!texture) {
                qCWarning(KWIN_TINYBLUR) << "Failed to allocate an offscreen texture";
                return;
            }
            texture->setFilter(GL_LINEAR);
            texture->setWrapMode(GL_CLAMP_TO_EDGE);

            auto framebuffer = std::make_unique<GLFramebuffer>(texture.get());
            if (!framebuffer->valid()) {
                qCWarning(KWIN_TINYBLUR) << "Failed to create an offscreen framebuffer";
                return;
            }
            EglContext::currentContext()->pushFramebuffer(framebuffer.get());
            glClear(GL_COLOR_BUFFER_BIT);
            EglContext::currentContext()->popFramebuffer();
            renderInfo.textures.push_back(std::move(texture));
            renderInfo.framebuffers.push_back(std::move(framebuffer));
        }
    }

    // Fetch the pixels behind the shape that is going to be blurred.
    const Region dirtyRegion = viewport.mapFromDeviceCoordinatesContained(deviceRegion) & backgroundRect;
    for (const Rect &dirtyRect : dirtyRegion.rects()) {
        renderInfo.framebuffers[0]->blitFromRenderTarget(renderTarget, viewport, dirtyRect, dirtyRect.translated(-backgroundRect.topLeft()));
    }

    // Upload the geometry: the first 6 vertices are used when downsampling and upsampling offscreen,
    // the remaining vertices are used when rendering on the screen.
    GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
    vbo->reset();
    vbo->setAttribLayout(std::span(GLVertexBuffer::GLVertex2DLayout), sizeof(GLVertex2D));

    const int vertexCount = effectiveShape.size() * 6;
    if (auto result = vbo->map<GLVertex2D>(6 + vertexCount)) {
        auto map = *result;

        size_t vboIndex = 0;

        // The geometry that will be blurred offscreen, in logical pixels.
        {
            const RectF localRect = RectF(0, 0, backgroundRect.width(), backgroundRect.height());

            const float x0 = localRect.left();
            const float y0 = localRect.top();
            const float x1 = localRect.right();
            const float y1 = localRect.bottom();

            const float u0 = x0 / backgroundRect.width();
            const float v0 = 1.0f - y0 / backgroundRect.height();
            const float u1 = x1 / backgroundRect.width();
            const float v1 = 1.0f - y1 / backgroundRect.height();

            // first triangle
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x0, y0),
                .texcoord = QVector2D(u0, v0),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x1, y1),
                .texcoord = QVector2D(u1, v1),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x0, y1),
                .texcoord = QVector2D(u0, v1),
            };

            // second triangle
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x0, y0),
                .texcoord = QVector2D(u0, v0),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x1, y0),
                .texcoord = QVector2D(u1, v0),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x1, y1),
                .texcoord = QVector2D(u1, v1),
            };
        }

        // The geometry that will be painted on screen, in device pixels.
        for (const RectF &rect : effectiveShape) {
            const float x0 = rect.left();
            const float y0 = rect.top();
            const float x1 = rect.right();
            const float y1 = rect.bottom();

            const float u0 = x0 / scaledBackgroundRect.width();
            const float v0 = 1.0f - y0 / scaledBackgroundRect.height();
            const float u1 = x1 / scaledBackgroundRect.width();
            const float v1 = 1.0f - y1 / scaledBackgroundRect.height();

            // first triangle
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x0, y0),
                .texcoord = QVector2D(u0, v0),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x1, y1),
                .texcoord = QVector2D(u1, v1),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x0, y1),
                .texcoord = QVector2D(u0, v1),
            };

            // second triangle
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x0, y0),
                .texcoord = QVector2D(u0, v0),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x1, y0),
                .texcoord = QVector2D(u1, v0),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x1, y1),
                .texcoord = QVector2D(u1, v1),
            };
        }

        vbo->unmap();
    } else {
        qCWarning(KWIN_TINYBLUR) << "Failed to map vertex buffer";
        return;
    }

    vbo->bindArrays();

    // The downsample pass of the dual Kawase algorithm: the background will be scaled down 50% every iteration.
    {
        ShaderManager::instance()->pushShader(m_downsamplePass.shader.get());

        QMatrix4x4 projectionMatrix;
        projectionMatrix.ortho(QRectF(0.0, 0.0, backgroundRect.width(), backgroundRect.height()));

        m_downsamplePass.shader->setUniform(m_downsamplePass.mvpMatrixLocation, projectionMatrix);
        m_downsamplePass.shader->setUniform(m_downsamplePass.offsetLocation, float(m_offset));

        for (size_t i = 1; i < renderInfo.framebuffers.size(); ++i) {
            const auto &read = renderInfo.framebuffers[i - 1];
            const auto &draw = renderInfo.framebuffers[i];

            const QVector2D halfpixel(0.5 / read->colorAttachment()->width(),
                                      0.5 / read->colorAttachment()->height());
            m_downsamplePass.shader->setUniform(m_downsamplePass.halfpixelLocation, halfpixel);

            read->colorAttachment()->bind();

            GLFramebuffer::pushFramebuffer(draw.get());
            vbo->draw(GL_TRIANGLES, 0, 6);
        }

        ShaderManager::instance()->popShader();
    }

    // The upsample pass of the dual Kawase algorithm: the background will be scaled up 200% every iteration.
    {
        ShaderManager::instance()->pushShader(m_upsamplePass.shader.get());

        QMatrix4x4 projectionMatrix;
        projectionMatrix.ortho(QRectF(0.0, 0.0, backgroundRect.width(), backgroundRect.height()));

        m_upsamplePass.shader->setUniform(m_upsamplePass.mvpMatrixLocation, projectionMatrix);
        m_upsamplePass.shader->setUniform(m_upsamplePass.offsetLocation, float(m_offset));

        for (size_t i = renderInfo.framebuffers.size() - 1; i > 1; --i) {
            GLFramebuffer::popFramebuffer();
            const auto &read = renderInfo.framebuffers[i];

            const QVector2D halfpixel(0.5 / read->colorAttachment()->width(),
                                      0.5 / read->colorAttachment()->height());
            m_upsamplePass.shader->setUniform(m_upsamplePass.halfpixelLocation, halfpixel);

            read->colorAttachment()->bind();

            vbo->draw(GL_TRIANGLES, 0, 6);
        }

        ShaderManager::instance()->popShader();
    }

    // The kernel itself is already intentionally tiny. Do not mix the original
    // image back in here: that made the minimal setting visually identical to
    // an ordinary transparent window.
    constexpr float subtleBlendStrength = 1.0f;
    const float modulation = float(opacity * opacity) * subtleBlendStrength;

    if (const BorderRadius cornerRadius = w->window()->borderRadius(); false && !cornerRadius.isNull()) {
        ShaderManager::instance()->pushShader(m_roundedOnscreenPass.shader.get());

        QMatrix4x4 projectionMatrix = viewport.projectionMatrix();
        projectionMatrix.translate(scaledBackgroundRect.x(), scaledBackgroundRect.y());

        GLFramebuffer::popFramebuffer();
        const auto &read = renderInfo.framebuffers[1];

        const QVector2D halfpixel(0.5 / read->colorAttachment()->width(),
                                  0.5 / read->colorAttachment()->height());

        const RectF transformedRect = RectF{
            w->frameGeometry().x() + data.xTranslation(),
            w->frameGeometry().y() + data.yTranslation(),
            w->frameGeometry().width() * data.xScale(),
            w->frameGeometry().height() * data.yScale(),
        };
        const RectF nativeBox = transformedRect
                                    .scaled(viewport.scale())
                                    .rounded()
                                    .translated(-scaledBackgroundRect.topLeft());
        const BorderRadius nativeCornerRadius = cornerRadius.scaled(viewport.scale()).rounded();

        m_roundedOnscreenPass.shader->setUniform(m_roundedOnscreenPass.mvpMatrixLocation, projectionMatrix);
        m_roundedOnscreenPass.shader->setUniform(m_roundedOnscreenPass.colorMatrixLocation, m_colorMatrix);
        m_roundedOnscreenPass.shader->setUniform(m_roundedOnscreenPass.halfpixelLocation, halfpixel);
        m_roundedOnscreenPass.shader->setUniform(m_roundedOnscreenPass.offsetLocation, float(m_offset));
        m_roundedOnscreenPass.shader->setUniform(m_roundedOnscreenPass.boxLocation, QVector4D(nativeBox.horizontalCenter(), nativeBox.verticalCenter(), nativeBox.width() * 0.5, nativeBox.height() * 0.5));
        m_roundedOnscreenPass.shader->setUniform(m_roundedOnscreenPass.cornerRadiusLocation, nativeCornerRadius.toVector());
        m_roundedOnscreenPass.shader->setUniform(m_roundedOnscreenPass.opacityLocation, modulation);

        read->colorAttachment()->bind();

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        vbo->draw(GL_TRIANGLES, 6, vertexCount);

        glDisable(GL_BLEND);

        ShaderManager::instance()->popShader();
    } else {
        ShaderManager::instance()->pushShader(m_onscreenPass.shader.get());

        QMatrix4x4 projectionMatrix = viewport.projectionMatrix();
        projectionMatrix.translate(scaledBackgroundRect.x(), scaledBackgroundRect.y());

        GLFramebuffer::popFramebuffer();
        const auto &read = renderInfo.framebuffers[1];

        const QVector2D halfpixel(0.5 / read->colorAttachment()->width(),
                                  0.5 / read->colorAttachment()->height());

        m_onscreenPass.shader->setUniform(m_onscreenPass.mvpMatrixLocation, projectionMatrix);
        m_onscreenPass.shader->setUniform(m_onscreenPass.colorMatrixLocation, m_colorMatrix);
        m_onscreenPass.shader->setUniform(m_onscreenPass.halfpixelLocation, halfpixel);
        m_onscreenPass.shader->setUniform(m_onscreenPass.offsetLocation, float(m_offset));

        const int zoomCount = isBlurRectangleWindow(w) ? m_zoomRegions.size() : 0;
        m_onscreenPass.shader->setUniform("zoomCount", zoomCount);
        m_onscreenPass.shader->setUniform("zoomCircle", int(m_zoomCircle));
        m_onscreenPass.shader->setUniform("originalTexture", 1);
        for (int i = 0; i < zoomCount; ++i) {
            const QRectF bounds = m_zoomRegions.at(i).translated(w->pos().x(), w->pos().y());
            const QVector4D normalized((bounds.center().x() - backgroundRect.x()) / backgroundRect.width(),
                                      1.0 - (bounds.center().y() - backgroundRect.y()) / backgroundRect.height(),
                                      bounds.width() / backgroundRect.width() / 2,
                                      bounds.height() / backgroundRect.height() / 2);
            m_onscreenPass.shader->setUniform(QStringLiteral("zoomRegions[%1]").arg(i).toLatin1().constData(), normalized);
            m_onscreenPass.shader->setUniform(QStringLiteral("zoomFactors[%1]").arg(i).toLatin1().constData(), m_zoomFactors.at(i));
        }
        glActiveTexture(GL_TEXTURE1);
        renderInfo.textures[0]->bind();
        glActiveTexture(GL_TEXTURE0);

        read->colorAttachment()->bind();

        if (modulation < 1.0) {
            glEnable(GL_BLEND);
            glBlendColor(0, 0, 0, modulation);
            glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
        }

        vbo->draw(GL_TRIANGLES, 6, vertexCount);

        if (modulation < 1.0) {
            glDisable(GL_BLEND);
        }

        ShaderManager::instance()->popShader();
    }

    vbo->unbindArrays();
}

bool TinyBlurEffect::isActive() const
{
    return m_valid && !effects->isScreenLocked();
}

bool TinyBlurEffect::blocksDirectScanout() const
{
    // A fullscreen video may otherwise bypass KWin's compositing pipeline,
    // leaving no framebuffer for the mask to blur.
    for (const auto &[window, data] : m_windows) {
        Q_UNUSED(data)
        if (isBlurRectangleWindow(window)) {
            return true;
        }
    }
    return false;
}

} // namespace KWin

#include "moc_tinyblur.cpp"
