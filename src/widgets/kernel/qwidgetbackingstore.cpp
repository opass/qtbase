/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtWidgets module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/


#include "qplatformdefs.h"

#include "qwidgetbackingstore_p.h"

#include <QtCore/qglobal.h>
#include <QtCore/qdebug.h>
#include <QtCore/qvarlengtharray.h>
#include <QtGui/qevent.h>
#include <QtWidgets/qapplication.h>
#include <QtGui/qpaintengine.h>
#if QT_CONFIG(graphicsview)
#include <QtWidgets/qgraphicsproxywidget.h>
#endif

#include <private/qwidget_p.h>
#include <private/qapplication_p.h>
#include <private/qpaintengine_raster_p.h>
#if QT_CONFIG(graphicseffect)
#include <private/qgraphicseffect_p.h>
#endif
#include <QtGui/private/qwindow_p.h>

#include <qpa/qplatformbackingstore.h>

#if defined(Q_OS_WIN) && !defined(QT_NO_PAINT_DEBUG)
#  include <QtCore/qt_windows.h>
#  include <qpa/qplatformnativeinterface.h>
#endif

#include <iostream>

QT_BEGIN_NAMESPACE

extern QRegion qt_dirtyRegion(QWidget *);

#ifndef QT_NO_OPENGL
Q_GLOBAL_STATIC(QPlatformTextureList, qt_dummy_platformTextureList)
#endif

/**
 * Flushes the contents of the \a backingStore into the screen area of \a widget.
 * \a region is the region to be updated in \a widget coordinates.
 */
void QWidgetBackingStore::qt_flush(QWidget *widget, const QRegion &region, QBackingStore *backingStore,
                                   QWidget *tlw, QPlatformTextureList *widgetTextures,
                                   QWidgetBackingStore *widgetBackingStore)
{
#ifdef QT_NO_OPENGL
    Q_UNUSED(widgetTextures);
    Q_ASSERT(!region.isEmpty());
#else
    Q_ASSERT(!region.isEmpty() || widgetTextures);
#endif
    Q_ASSERT(widget);
    Q_ASSERT(backingStore);
    Q_ASSERT(tlw);
#if !defined(QT_NO_PAINT_DEBUG)
    static int flushUpdate = qEnvironmentVariableIntValue("QT_FLUSH_UPDATE");
    if (flushUpdate > 0)
        QWidgetBackingStore::showYellowThing(widget, region, flushUpdate * 10, false);
#endif

    if (tlw->testAttribute(Qt::WA_DontShowOnScreen) || widget->testAttribute(Qt::WA_DontShowOnScreen))
        return;
    static bool fpsDebug = qEnvironmentVariableIntValue("QT_DEBUG_FPS");
    if (fpsDebug) {
        if (!widgetBackingStore->perfFrames++)
            widgetBackingStore->perfTime.start();
        if (widgetBackingStore->perfTime.elapsed() > 5000) {
            double fps = double(widgetBackingStore->perfFrames * 1000) / widgetBackingStore->perfTime.restart();
            qDebug("FPS: %.1f\n", fps);
            widgetBackingStore->perfFrames = 0;
        }
    }

    QPoint offset;
    if (widget != tlw)
        offset += widget->mapTo(tlw, QPoint());

    QRegion effectiveRegion = region;
#ifndef QT_NO_OPENGL
    const bool compositionWasActive = widget->d_func()->renderToTextureComposeActive;
    if (!widgetTextures) {
        widget->d_func()->renderToTextureComposeActive = false;
        // Detect the case of falling back to the normal flush path when no
        // render-to-texture widgets are visible anymore. We will force one
        // last flush to go through the OpenGL-based composition to prevent
        // artifacts. The next flush after this one will use the normal path.
        if (compositionWasActive)
            widgetTextures = qt_dummy_platformTextureList;
    } else {
        widget->d_func()->renderToTextureComposeActive = true;
    }
    // When changing the composition status, make sure the dirty region covers
    // the entire widget.  Just having e.g. the shown/hidden render-to-texture
    // widget's area marked as dirty is incorrect when changing flush paths.
    if (compositionWasActive != widget->d_func()->renderToTextureComposeActive)
        effectiveRegion = widget->rect();

    // re-test since we may have been forced to this path via the dummy texture list above
    if (widgetTextures) {
        qt_window_private(tlw->windowHandle())->compositing = true;
        widget->window()->d_func()->sendComposeStatus(widget->window(), false);
        // A window may have alpha even when the app did not request
        // WA_TranslucentBackground. Therefore the compositor needs to know whether the app intends
        // to rely on translucency, in order to decide if it should clear to transparent or opaque.
        const bool translucentBackground = widget->testAttribute(Qt::WA_TranslucentBackground);
        backingStore->handle()->composeAndFlush(widget->windowHandle(), effectiveRegion, offset,
                                                widgetTextures, translucentBackground);
        widget->window()->d_func()->sendComposeStatus(widget->window(), true);
    } else
#endif
        backingStore->flush(effectiveRegion, widget->windowHandle(), offset);
}

#ifndef QT_NO_PAINT_DEBUG
#if defined(Q_OS_WIN) && !defined(Q_OS_WINRT)

static void showYellowThing_win(QWidget *widget, const QRegion &region, int msec)
{
    // We expect to be passed a native parent.
    QWindow *nativeWindow = widget->windowHandle();
    if (!nativeWindow)
        return;
    void *hdcV = QGuiApplication::platformNativeInterface()->nativeResourceForWindow(QByteArrayLiteral("getDC"), nativeWindow);
    if (!hdcV)
        return;
    const HDC hdc = reinterpret_cast<HDC>(hdcV);

    static const COLORREF colors[] = {RGB(255, 255, 0), RGB(255, 200, 55), RGB(200, 255, 55), RGB(200, 200, 0)};

    static size_t i = 0;
    const HBRUSH brush = CreateSolidBrush(colors[i]);
    i = (i + 1) % (sizeof(colors) / sizeof(colors[0]));

    for (const QRect &rect : region) {
        RECT winRect;
        SetRect(&winRect, rect.left(), rect.top(), rect.right(), rect.bottom());
        FillRect(hdc, &winRect, brush);
    }
    DeleteObject(brush);
    QGuiApplication::platformNativeInterface()->nativeResourceForWindow(QByteArrayLiteral("releaseDC"), nativeWindow);
    ::Sleep(msec);
}
#endif //  defined(Q_OS_WIN) && !defined(Q_OS_WINRT)

void QWidgetBackingStore::showYellowThing(QWidget *widget, const QRegion &toBePainted, int msec, bool unclipped)
{
#ifdef Q_OS_WINRT
    Q_UNUSED(msec)
#endif
    QRegion paintRegion = toBePainted;
    QRect widgetRect = widget->rect();

    if (!widget->internalWinId()) {
        QWidget *nativeParent = widget->nativeParentWidget();
        const QPoint offset = widget->mapTo(nativeParent, QPoint(0, 0));
        paintRegion.translate(offset);
        widgetRect.translate(offset);
        widget = nativeParent;
    }

#if defined(Q_OS_WIN) && !defined(Q_OS_WINRT)
    Q_UNUSED(unclipped);
    showYellowThing_win(widget, paintRegion, msec);
#else
    //flags to fool painter
    bool paintUnclipped = widget->testAttribute(Qt::WA_PaintUnclipped);
    if (unclipped && !widget->d_func()->paintOnScreen())
        widget->setAttribute(Qt::WA_PaintUnclipped);

    const bool setFlag = !widget->testAttribute(Qt::WA_WState_InPaintEvent);
    if (setFlag)
        widget->setAttribute(Qt::WA_WState_InPaintEvent);

    //setup the engine
    QPaintEngine *pe = widget->paintEngine();
    if (pe) {
        pe->setSystemClip(paintRegion);
        {
            QPainter p(widget);
            p.setClipRegion(paintRegion);
            static int i = 0;
            switch (i) {
            case 0:
                p.fillRect(widgetRect, QColor(255,255,0));
                break;
            case 1:
                p.fillRect(widgetRect, QColor(255,200,55));
                break;
            case 2:
                p.fillRect(widgetRect, QColor(200,255,55));
                break;
            case 3:
                p.fillRect(widgetRect, QColor(200,200,0));
                break;
            }
            i = (i+1) & 3;
            p.end();
        }
    }

    if (setFlag)
        widget->setAttribute(Qt::WA_WState_InPaintEvent, false);

    //restore
    widget->setAttribute(Qt::WA_PaintUnclipped, paintUnclipped);

    if (pe)
        pe->setSystemClip(QRegion());

#if defined(Q_OS_UNIX)
    ::usleep(1000 * msec);
#endif
#endif // !Q_OS_WIN
}

bool QWidgetBackingStore::flushPaint(QWidget *widget, const QRegion &rgn)
{
    if (!widget)
        return false;

    int delay = 0;
    if (widget->testAttribute(Qt::WA_WState_InPaintEvent)) {
        static int flushPaintEvent = qEnvironmentVariableIntValue("QT_FLUSH_PAINT_EVENT");
        if (!flushPaintEvent)
            return false;
        delay = flushPaintEvent;
    } else {
        static int flushPaint = qEnvironmentVariableIntValue("QT_FLUSH_PAINT");
        if (!flushPaint)
            return false;
        delay = flushPaint;
    }

    QWidgetBackingStore::showYellowThing(widget, rgn, delay * 10, true);
    return true;
}

void QWidgetBackingStore::unflushPaint(QWidget *widget, const QRegion &rgn)
{
    if (widget->d_func()->paintOnScreen() || rgn.isEmpty())
        return;

    QWidget *tlw = widget->window();
    QTLWExtra *tlwExtra = tlw->d_func()->maybeTopData();
    if (!tlwExtra)
        return;

    qt_flush(widget, rgn, tlwExtra->backingStoreTracker->store, tlw, 0, tlw->d_func()->maybeBackingStore());
}
#endif // QT_NO_PAINT_DEBUG

/*
    Moves the whole rect by (dx, dy) in widget's coordinate system.
    Doesn't generate any updates.
*/
bool QWidgetBackingStore::bltRect(const QRect &rect, int dx, int dy, QWidget *widget)
{
    const QPoint pos(widget->mapTo(tlw, rect.topLeft()));
    const QRect tlwRect(QRect(pos, rect.size()));
    if (dirty.intersects(tlwRect))
        return false; // We don't want to scroll junk.
    return store->scroll(tlwRect, dx, dy);
}

void QWidgetBackingStore::releaseBuffer()
{
    if (store)
        store->resize(QSize());
}

/*!
    Prepares the window surface to paint a\ toClean region of the \a widget and
    updates the BeginPaintInfo struct accordingly.

    The \a toClean region might be clipped by the window surface.
*/
void QWidgetBackingStore::beginPaint(QRegion &toClean, QWidget *widget, QBackingStore *backingStore,
                                     BeginPaintInfo *returnInfo, bool toCleanIsInTopLevelCoordinates)
{
    Q_UNUSED(widget);
    Q_UNUSED(toCleanIsInTopLevelCoordinates);

    // Always flush repainted areas.
    dirtyOnScreen += toClean;

#ifdef QT_NO_PAINT_DEBUG
    backingStore->beginPaint(toClean);
#else
    returnInfo->wasFlushed = QWidgetBackingStore::flushPaint(tlw, toClean);
    // Avoid deadlock with QT_FLUSH_PAINT: the server will wait for
    // the BackingStore lock, so if we hold that, the server will
    // never release the Communication lock that we are waiting for in
    // sendSynchronousCommand
    if (!returnInfo->wasFlushed)
        backingStore->beginPaint(toClean);
#endif

    Q_UNUSED(returnInfo);
}

void QWidgetBackingStore::endPaint(const QRegion &cleaned, QBackingStore *backingStore,
        BeginPaintInfo *beginPaintInfo)
{
#ifndef QT_NO_PAINT_DEBUG
    if (!beginPaintInfo->wasFlushed)
        backingStore->endPaint();
    else
        QWidgetBackingStore::unflushPaint(tlw, cleaned);
#else
    Q_UNUSED(beginPaintInfo);
    Q_UNUSED(cleaned);
    backingStore->endPaint();
#endif

    flush();
}

/*!
    Returns the region (in top-level coordinates) that needs repaint and/or flush.

    If the widget is non-zero, only the dirty region for the widget is returned
    and the region will be in widget coordinates.
*/
QRegion QWidgetBackingStore::dirtyRegion(QWidget *widget) const
{
    const bool widgetDirty = widget && widget != tlw;
    const QRect tlwRect(topLevelRect());
    const QRect surfaceGeometry(tlwRect.topLeft(), store->size());
    if (surfaceGeometry != tlwRect && surfaceGeometry.size() != tlwRect.size()) {
        if (widgetDirty) {
            const QRect dirtyTlwRect = QRect(QPoint(), tlwRect.size());
            const QPoint offset(widget->mapTo(tlw, QPoint()));
            const QRect dirtyWidgetRect(dirtyTlwRect & widget->rect().translated(offset));
            return dirtyWidgetRect.translated(-offset);
        }
        return QRect(QPoint(), tlwRect.size());
    }

    // Calculate the region that needs repaint.
    QRegion r(dirty);
    for (int i = 0; i < dirtyWidgets.size(); ++i) {
        QWidget *w = dirtyWidgets.at(i);
        if (widgetDirty && w != widget && !widget->isAncestorOf(w))
            continue;
        r += w->d_func()->dirty.translated(w->mapTo(tlw, QPoint()));
    }

    // Append the region that needs flush.
    r += dirtyOnScreen;

    if (dirtyOnScreenWidgets) { // Only in use with native child widgets.
        for (int i = 0; i < dirtyOnScreenWidgets->size(); ++i) {
            QWidget *w = dirtyOnScreenWidgets->at(i);
            if (widgetDirty && w != widget && !widget->isAncestorOf(w))
                continue;
            QWidgetPrivate *wd = w->d_func();
            Q_ASSERT(wd->needsFlush);
            r += wd->needsFlush->translated(w->mapTo(tlw, QPoint()));
        }
    }

    if (widgetDirty) {
        // Intersect with the widget geometry and translate to its coordinates.
        const QPoint offset(widget->mapTo(tlw, QPoint()));
        r &= widget->rect().translated(offset);
        r.translate(-offset);
    }
    return r;
}

/*!
    Returns the static content inside the \a parent if non-zero; otherwise the static content
    for the entire backing store is returned. The content will be clipped to \a withinClipRect
    if non-empty.
*/
QRegion QWidgetBackingStore::staticContents(QWidget *parent, const QRect &withinClipRect) const
{
    if (!parent && tlw->testAttribute(Qt::WA_StaticContents)) {
        const QSize surfaceGeometry(store->size());
        QRect surfaceRect(0, 0, surfaceGeometry.width(), surfaceGeometry.height());
        if (!withinClipRect.isEmpty())
            surfaceRect &= withinClipRect;
        return QRegion(surfaceRect);
    }

    QRegion region;
    if (parent && parent->d_func()->children.isEmpty())
        return region;

    const bool clipToRect = !withinClipRect.isEmpty();
    const int count = staticWidgets.count();
    for (int i = 0; i < count; ++i) {
        QWidget *w = staticWidgets.at(i);
        QWidgetPrivate *wd = w->d_func();
        if (!wd->isOpaque || !wd->extra || wd->extra->staticContentsSize.isEmpty()
            || !w->isVisible() || (parent && !parent->isAncestorOf(w))) {
            continue;
        }

        QRect rect(0, 0, wd->extra->staticContentsSize.width(), wd->extra->staticContentsSize.height());
        const QPoint offset = w->mapTo(parent ? parent : tlw, QPoint());
        if (clipToRect)
            rect &= withinClipRect.translated(-offset);
        if (rect.isEmpty())
            continue;

        rect &= wd->clipRect();
        if (rect.isEmpty())
            continue;

        QRegion visible(rect);
        wd->clipToEffectiveMask(visible);
        if (visible.isEmpty())
            continue;
        wd->subtractOpaqueSiblings(visible, 0, /*alsoNonOpaque=*/true);

        visible.translate(offset);
        region += visible;
    }

    return region;
}

void QWidgetBackingStore::sendUpdateRequest(QWidget *widget, UpdateTime updateTime)
{
    if (!widget)
        return;

#ifndef QT_NO_OPENGL
    // Having every repaint() leading to a sync/flush is bad as it causes
    // compositing and waiting for vsync each and every time. Change to
    // UpdateLater, except for approx. once per frame to prevent starvation in
    // case the control does not get back to the event loop.
    QWidget *w = widget->window();
    if (updateTime == UpdateNow && w && w->windowHandle() && QWindowPrivate::get(w->windowHandle())->compositing) {
        int refresh = 60;
        QScreen *ws = w->windowHandle()->screen();
        if (ws)
            refresh = ws->refreshRate();
        QWindowPrivate *wd = QWindowPrivate::get(w->windowHandle());
        if (wd->lastComposeTime.isValid()) {
            const qint64 elapsed = wd->lastComposeTime.elapsed();
            if (elapsed <= qint64(1000.0f / refresh))
                updateTime = UpdateLater;
       }
    }
#endif

    switch (updateTime) {
    case UpdateLater:
        updateRequestSent = true;
        QApplication::postEvent(widget, new QEvent(QEvent::UpdateRequest), Qt::LowEventPriority);
        break;
    case UpdateNow: {
        QEvent event(QEvent::UpdateRequest);
        QApplication::sendEvent(widget, &event);
        break;
        }
    }
}

/*!
    Marks the region of the widget as dirty (if not already marked as dirty) and
    posts an UpdateRequest event to the top-level widget (if not already posted).

    If updateTime is UpdateNow, the event is sent immediately instead of posted.

    If bufferState is BufferInvalid, all widgets intersecting with the region will be dirty.

    If the widget paints directly on screen, the event is sent to the widget
    instead of the top-level widget, and bufferState is completely ignored.

    ### Qt 4.6: Merge into a template function (after MSVC isn't supported anymore).
*/
void QWidgetBackingStore::markDirty(const QRegion &rgn, QWidget *widget,
                                    UpdateTime updateTime, BufferState bufferState)
{
    Q_ASSERT(tlw->d_func()->extra);
    Q_ASSERT(tlw->d_func()->extra->topextra);
    Q_ASSERT(!tlw->d_func()->extra->topextra->inTopLevelResize);
    Q_ASSERT(widget->isVisible() && widget->updatesEnabled());
    Q_ASSERT(widget->window() == tlw);
    Q_ASSERT(!rgn.isEmpty());

#if QT_CONFIG(graphicseffect)
    widget->d_func()->invalidateGraphicsEffectsRecursively();
#endif // QT_CONFIG(graphicseffect)

    if (widget->d_func()->paintOnScreen()) {
        if (widget->d_func()->dirty.isEmpty()) {
            widget->d_func()->dirty = rgn;
            sendUpdateRequest(widget, updateTime);
            return;
        } else if (qt_region_strictContains(widget->d_func()->dirty, widget->rect())) {
            if (updateTime == UpdateNow)
                sendUpdateRequest(widget, updateTime);
            return; // Already dirty.
        }

        const bool eventAlreadyPosted = !widget->d_func()->dirty.isEmpty();
        widget->d_func()->dirty += rgn;
        if (!eventAlreadyPosted || updateTime == UpdateNow)
            sendUpdateRequest(widget, updateTime);
        return;
    }

    const QPoint offset = widget->mapTo(tlw, QPoint());

    if (QWidgetPrivate::get(widget)->renderToTexture) {
        if (!widget->d_func()->inDirtyList)
            addDirtyRenderToTextureWidget(widget);
        if (!updateRequestSent || updateTime == UpdateNow)
            sendUpdateRequest(tlw, updateTime);
        return;
    }

    const QRect widgetRect = widget->d_func()->effectiveRectFor(widget->rect());
    if (qt_region_strictContains(dirty, widgetRect.translated(offset))) {
        if (updateTime == UpdateNow)
            sendUpdateRequest(tlw, updateTime);
        return; // Already dirty.
    }

    if (bufferState == BufferInvalid) {
        const bool eventAlreadyPosted = !dirty.isEmpty() || updateRequestSent;
#if QT_CONFIG(graphicseffect)
        if (widget->d_func()->graphicsEffect)
            dirty += widget->d_func()->effectiveRectFor(rgn.boundingRect()).translated(offset);
        else
#endif // QT_CONFIG(graphicseffect)
            dirty += rgn.translated(offset);
        if (!eventAlreadyPosted || updateTime == UpdateNow)
            sendUpdateRequest(tlw, updateTime);
        return;
    }

    if (dirtyWidgets.isEmpty()) {
        addDirtyWidget(widget, rgn);
        sendUpdateRequest(tlw, updateTime);
        return;
    }

    if (widget->d_func()->inDirtyList) {
        if (!qt_region_strictContains(widget->d_func()->dirty, widgetRect)) {
#if QT_CONFIG(graphicseffect)
            if (widget->d_func()->graphicsEffect)
                widget->d_func()->dirty += widget->d_func()->effectiveRectFor(rgn.boundingRect());
            else
#endif // QT_CONFIG(graphicseffect)
                widget->d_func()->dirty += rgn;
        }
    } else {
        addDirtyWidget(widget, rgn);
    }

    if (updateTime == UpdateNow)
        sendUpdateRequest(tlw, updateTime);
}

/*!
    This function is equivalent to calling markDirty(QRegion(rect), ...), but
    is more efficient as it eliminates QRegion operations/allocations and can
    use the rect more precisely for additional cut-offs.

    ### Qt 4.6: Merge into a template function (after MSVC isn't supported anymore).
*/
void QWidgetBackingStore::markDirty(const QRect &rect, QWidget *widget,
                                    UpdateTime updateTime, BufferState bufferState)
{
    Q_ASSERT(tlw->d_func()->extra);
    Q_ASSERT(tlw->d_func()->extra->topextra);
    Q_ASSERT(!tlw->d_func()->extra->topextra->inTopLevelResize);
    Q_ASSERT(widget->isVisible() && widget->updatesEnabled());
    Q_ASSERT(widget->window() == tlw);
    Q_ASSERT(!rect.isEmpty());

#if QT_CONFIG(graphicseffect)
    widget->d_func()->invalidateGraphicsEffectsRecursively();
#endif // QT_CONFIG(graphicseffect)

    if (widget->d_func()->paintOnScreen()) {
        if (widget->d_func()->dirty.isEmpty()) {
            widget->d_func()->dirty = QRegion(rect);
            sendUpdateRequest(widget, updateTime);
            return;
        } else if (qt_region_strictContains(widget->d_func()->dirty, rect)) {
            if (updateTime == UpdateNow)
                sendUpdateRequest(widget, updateTime);
            return; // Already dirty.
        }

        const bool eventAlreadyPosted = !widget->d_func()->dirty.isEmpty();
        widget->d_func()->dirty += rect;
        if (!eventAlreadyPosted || updateTime == UpdateNow)
            sendUpdateRequest(widget, updateTime);
        return;
    }

    if (QWidgetPrivate::get(widget)->renderToTexture) {
        if (!widget->d_func()->inDirtyList)
            addDirtyRenderToTextureWidget(widget);
        if (!updateRequestSent || updateTime == UpdateNow)
            sendUpdateRequest(tlw, updateTime);
        return;
    }


    const QRect widgetRect = widget->d_func()->effectiveRectFor(rect);
    QRect translatedRect = widgetRect;
    if (widget != tlw)
        translatedRect.translate(widget->mapTo(tlw, QPoint()));
    // Graphics effects may exceed window size, clamp.
    translatedRect = translatedRect.intersected(QRect(QPoint(), tlw->size()));
    if (qt_region_strictContains(dirty, translatedRect)) {
        if (updateTime == UpdateNow)
            sendUpdateRequest(tlw, updateTime);
        return; // Already dirty
    }

    if (bufferState == BufferInvalid) {
        const bool eventAlreadyPosted = !dirty.isEmpty();
        dirty += translatedRect;
        if (!eventAlreadyPosted || updateTime == UpdateNow)
            sendUpdateRequest(tlw, updateTime);
        return;
    }

    if (dirtyWidgets.isEmpty()) {
        addDirtyWidget(widget, rect);
        sendUpdateRequest(tlw, updateTime);
        return;
    }

    if (widget->d_func()->inDirtyList) {
        if (!qt_region_strictContains(widget->d_func()->dirty, widgetRect))
            widget->d_func()->dirty += widgetRect;
    } else {
        addDirtyWidget(widget, rect);
    }

    if (updateTime == UpdateNow)
        sendUpdateRequest(tlw, updateTime);
}

/*!
    Marks the \a region of the \a widget as dirty on screen. The \a region will be copied from
    the backing store to the \a widget's native parent next time flush() is called.

    Paint on screen widgets are ignored.
*/
void QWidgetBackingStore::markDirtyOnScreen(const QRegion &region, QWidget *widget, const QPoint &topLevelOffset)
{
    if (!widget || widget->d_func()->paintOnScreen() || region.isEmpty())
        return;

#if 0 // Used to be included in Qt4 for Q_WS_MAC
    if (!widget->testAttribute(Qt::WA_WState_InPaintEvent))
        dirtyOnScreen += region.translated(topLevelOffset);
    return;
#endif

    // Top-level.
    if (widget == tlw) {
        if (!widget->testAttribute(Qt::WA_WState_InPaintEvent))
            dirtyOnScreen += region;
        return;
    }

    // Alien widgets.
    if (!widget->internalWinId() && !widget->isWindow()) {
        QWidget *nativeParent = widget->nativeParentWidget();        // Alien widgets with the top-level as the native parent (common case).
        if (nativeParent == tlw) {
            if (!widget->testAttribute(Qt::WA_WState_InPaintEvent))
                dirtyOnScreen += region.translated(topLevelOffset);
            return;
        }

        // Alien widgets with native parent != tlw.
        QWidgetPrivate *nativeParentPrivate = nativeParent->d_func();
        if (!nativeParentPrivate->needsFlush)
            nativeParentPrivate->needsFlush = new QRegion;
        const QPoint nativeParentOffset = widget->mapTo(nativeParent, QPoint());
        *nativeParentPrivate->needsFlush += region.translated(nativeParentOffset);
        appendDirtyOnScreenWidget(nativeParent);
        return;
    }

    // Native child widgets.
    QWidgetPrivate *widgetPrivate = widget->d_func();
    if (!widgetPrivate->needsFlush)
        widgetPrivate->needsFlush = new QRegion;
    *widgetPrivate->needsFlush += region;
    appendDirtyOnScreenWidget(widget);
}

void QWidgetBackingStore::removeDirtyWidget(QWidget *w)
{
    if (!w)
        return;

    dirtyWidgetsRemoveAll(w);
    dirtyOnScreenWidgetsRemoveAll(w);
    dirtyRenderToTextureWidgets.removeAll(w);
    resetWidget(w);

    QWidgetPrivate *wd = w->d_func();
    const int n = wd->children.count();
    for (int i = 0; i < n; ++i) {
        if (QWidget *child = qobject_cast<QWidget*>(wd->children.at(i)))
            removeDirtyWidget(child);
    }
}

void QWidgetBackingStore::updateLists(QWidget *cur)
{
    if (!cur)
        return;

    QList<QObject*> children = cur->children();
    for (int i = 0; i < children.size(); ++i) {
        QWidget *child = qobject_cast<QWidget*>(children.at(i));
        if (!child)
            continue;

        updateLists(child);
    }

    if (cur->testAttribute(Qt::WA_StaticContents))
        addStaticWidget(cur);
}

QWidgetBackingStore::QWidgetBackingStore(QWidget *topLevel)
    : tlw(topLevel),
      dirtyOnScreenWidgets(0),
      updateRequestSent(0),
      textureListWatcher(0),
      perfFrames(0)
{
    store = tlw->backingStore();
    Q_ASSERT(store);

    // Ensure all existing subsurfaces and static widgets are added to their respective lists.
    updateLists(topLevel);
}

QWidgetBackingStore::~QWidgetBackingStore()
{
    for (int c = 0; c < dirtyWidgets.size(); ++c)
        resetWidget(dirtyWidgets.at(c));
    for (int c = 0; c < dirtyRenderToTextureWidgets.size(); ++c)
        resetWidget(dirtyRenderToTextureWidgets.at(c));

    delete dirtyOnScreenWidgets;
}

//parent's coordinates; move whole rect; update parent and widget
//assume the screen blt has already been done, so we don't need to refresh that part
void QWidgetPrivate::moveRect(const QRect &rect, int dx, int dy)
{
    Q_Q(QWidget);
    if (!q->isVisible() || (dx == 0 && dy == 0))
        return;

    QWidget *tlw = q->window();
    QTLWExtra* x = tlw->d_func()->topData();
    if (x->inTopLevelResize)
        return;

    static const bool accelEnv = qEnvironmentVariableIntValue("QT_NO_FAST_MOVE") == 0;

    QWidget *pw = q->parentWidget();
    QPoint toplevelOffset = pw->mapTo(tlw, QPoint());
    QWidgetPrivate *pd = pw->d_func();
    QRect clipR(pd->clipRect());
    const QRect newRect(rect.translated(dx, dy));
    QRect destRect = rect.intersected(clipR);
    if (destRect.isValid())
        destRect = destRect.translated(dx, dy).intersected(clipR);
    const QRect sourceRect(destRect.translated(-dx, -dy));
    const QRect parentRect(rect & clipR);
    const bool nativeWithTextureChild = textureChildSeen && q->internalWinId();

    bool accelerateMove = accelEnv && isOpaque && !nativeWithTextureChild
#if QT_CONFIG(graphicsview)
                          // No accelerate move for proxy widgets.
                          && !tlw->d_func()->extra->proxyWidget
#endif
                          && !isOverlapped(sourceRect) && !isOverlapped(destRect);

    if (!accelerateMove) {
        QRegion parentR(effectiveRectFor(parentRect));
        if (!extra || !extra->hasMask) {
            parentR -= newRect;
        } else {
            // invalidateBuffer() excludes anything outside the mask
            parentR += newRect & clipR;
        }
        pd->invalidateBuffer(parentR);
        invalidateBuffer((newRect & clipR).translated(-data.crect.topLeft()));
    } else {

        QWidgetBackingStore *wbs = x->backingStoreTracker.data();
        QRegion childExpose(newRect & clipR);

        if (sourceRect.isValid() && wbs->bltRect(sourceRect, dx, dy, pw))
            childExpose -= destRect;

        if (!pw->updatesEnabled())
            return;

        const bool childUpdatesEnabled = q->updatesEnabled();
        if (childUpdatesEnabled && !childExpose.isEmpty()) {
            childExpose.translate(-data.crect.topLeft());
            wbs->markDirty(childExpose, q);
            isMoved = true;
        }

        QRegion parentExpose(parentRect);
        parentExpose -= newRect;
        if (extra && extra->hasMask)
            parentExpose += QRegion(newRect) - extra->mask.translated(data.crect.topLeft());

        if (!parentExpose.isEmpty()) {
            wbs->markDirty(parentExpose, pw);
            pd->isMoved = true;
        }

        if (childUpdatesEnabled) {
            QRegion needsFlush(sourceRect);
            needsFlush += destRect;
            wbs->markDirtyOnScreen(needsFlush, pw, toplevelOffset);
        }
    }
}

//widget's coordinates; scroll within rect;  only update widget
void QWidgetPrivate::scrollRect(const QRect &rect, int dx, int dy)
{
    Q_Q(QWidget);
    QWidget *tlw = q->window();
    QTLWExtra* x = tlw->d_func()->topData();
    if (x->inTopLevelResize)
        return;

    QWidgetBackingStore *wbs = x->backingStoreTracker.data();
    if (!wbs)
        return;

    static const bool accelEnv = qEnvironmentVariableIntValue("QT_NO_FAST_SCROLL") == 0;

    QRect scrollRect = rect & clipRect();
    bool overlapped = false;
    bool accelerateScroll = accelEnv && isOpaque && !q_func()->testAttribute(Qt::WA_WState_InPaintEvent)
                            && !(overlapped = isOverlapped(scrollRect.translated(data.crect.topLeft())));

    if (!accelerateScroll) {
        if (overlapped) {
            QRegion region(scrollRect);
            subtractOpaqueSiblings(region);
            invalidateBuffer(region);
        }else {
            invalidateBuffer(scrollRect);
        }
    } else {
        const QPoint toplevelOffset = q->mapTo(tlw, QPoint());
        const QRect destRect = scrollRect.translated(dx, dy) & scrollRect;
        const QRect sourceRect = destRect.translated(-dx, -dy);

        QRegion childExpose(scrollRect);
        if (sourceRect.isValid()) {
            if (wbs->bltRect(sourceRect, dx, dy, q))
                childExpose -= destRect;
        }

        if (inDirtyList) {
            if (rect == q->rect()) {
                dirty.translate(dx, dy);
            } else {
                QRegion dirtyScrollRegion = dirty.intersected(scrollRect);
                if (!dirtyScrollRegion.isEmpty()) {
                    dirty -= dirtyScrollRegion;
                    dirtyScrollRegion.translate(dx, dy);
                    dirty += dirtyScrollRegion;
                }
            }
        }

        if (!q->updatesEnabled())
            return;

        if (!childExpose.isEmpty()) {
            wbs->markDirty(childExpose, q);
            isScrolled = true;
        }

        // Instead of using native scroll-on-screen, we copy from
        // backingstore, giving only one screen update for each
        // scroll, and a solid appearance
        wbs->markDirtyOnScreen(destRect, q, toplevelOffset);
    }
}

#ifndef QT_NO_OPENGL
static void findTextureWidgetsRecursively(QWidget *tlw, QWidget *widget, QPlatformTextureList *widgetTextures, QVector<QWidget *> *nativeChildren)
{
    QWidgetPrivate *wd = QWidgetPrivate::get(widget);
    if (wd->renderToTexture) {
        QPlatformTextureList::Flags flags = wd->textureListFlags();
        const QRect rect(widget->mapTo(tlw, QPoint()), widget->size());
        widgetTextures->appendTexture(widget, wd->textureId(), rect, wd->clipRect(), flags);
    }

    for (int i = 0; i < wd->children.size(); ++i) {
        QWidget *w = qobject_cast<QWidget *>(wd->children.at(i));
        // Stop at native widgets but store them. Stop at hidden widgets too.
        if (w && !w->isWindow() && w->internalWinId())
            nativeChildren->append(w);
        if (w && !w->isWindow() && !w->internalWinId() && !w->isHidden() && QWidgetPrivate::get(w)->textureChildSeen)
            findTextureWidgetsRecursively(tlw, w, widgetTextures, nativeChildren);
    }
}

static void findAllTextureWidgetsRecursively(QWidget *tlw, QWidget *widget)
{
    // textureChildSeen does not take native child widgets into account and that's good.
    if (QWidgetPrivate::get(widget)->textureChildSeen) {
        QVector<QWidget *> nativeChildren;
        QScopedPointer<QPlatformTextureList> tl(new QPlatformTextureList);
        // Look for texture widgets (incl. widget itself) from 'widget' down,
        // but skip subtrees with a parent of a native child widget.
        findTextureWidgetsRecursively(tlw, widget, tl.data(), &nativeChildren);
        // tl may be empty regardless of textureChildSeen if we have native or hidden children.
        if (!tl->isEmpty())
            QWidgetPrivate::get(tlw)->topData()->widgetTextures.append(tl.take());
        // Native child widgets, if there was any, get their own separate QPlatformTextureList.
        foreach (QWidget *ncw, nativeChildren) {
            if (QWidgetPrivate::get(ncw)->textureChildSeen)
                findAllTextureWidgetsRecursively(tlw, ncw);
        }
    }
}

static QPlatformTextureList *widgetTexturesFor(QWidget *tlw, QWidget *widget)
{
    foreach (QPlatformTextureList *tl, QWidgetPrivate::get(tlw)->topData()->widgetTextures) {
        Q_ASSERT(!tl->isEmpty());
        for (int i = 0; i < tl->count(); ++i) {
            QWidget *w = static_cast<QWidget *>(tl->source(i));
            if ((w->internalWinId() && w == widget) || (!w->internalWinId() && w->nativeParentWidget() == widget))
                return tl;
        }
    }

    if (QWidgetPrivate::get(tlw)->textureChildSeen) {
        // No render-to-texture widgets in the (sub-)tree due to hidden or native
        // children. Returning null results in using the normal backingstore flush path
        // without OpenGL-based compositing. This is very desirable normally. However,
        // some platforms cannot handle switching between the non-GL and GL paths for
        // their windows so it has to be opt-in.
        static bool switchableWidgetComposition =
            QGuiApplicationPrivate::instance()->platformIntegration()
                ->hasCapability(QPlatformIntegration::SwitchableWidgetComposition);
        if (!switchableWidgetComposition
// The Windows compositor handles fullscreen OpenGL window specially. Besides
// having trouble with popups, it also has issues with flip-flopping between
// OpenGL-based and normal flushing. Therefore, stick with GL for fullscreen
// windows (QTBUG-53515). Similary, translucent windows should not switch to
// layered native windows (QTBUG-54734).
#if defined(Q_OS_WIN) && !defined(Q_OS_WINRT) && !defined(Q_OS_WINCE)
                || tlw->windowState().testFlag(Qt::WindowFullScreen)
                || tlw->testAttribute(Qt::WA_TranslucentBackground)
#endif
                )
        {
            return qt_dummy_platformTextureList();
        }
    }

    return 0;
}

// Watches one or more QPlatformTextureLists for changes in the lock state and
// triggers a backingstore sync when all the registered lists turn into
// unlocked state. This is essential when a custom composeAndFlush()
// implementation in a platform plugin is not synchronous and keeps
// holding on to the textures for some time even after returning from there.
QPlatformTextureListWatcher::QPlatformTextureListWatcher(QWidgetBackingStore *backingStore)
    : m_backingStore(backingStore)
{
}

void QPlatformTextureListWatcher::watch(QPlatformTextureList *textureList)
{
    connect(textureList, SIGNAL(locked(bool)), SLOT(onLockStatusChanged(bool)));
    m_locked[textureList] = textureList->isLocked();
}

bool QPlatformTextureListWatcher::isLocked() const
{
    foreach (bool v, m_locked) {
        if (v)
            return true;
    }
    return false;
}

void QPlatformTextureListWatcher::onLockStatusChanged(bool locked)
{
    QPlatformTextureList *tl = static_cast<QPlatformTextureList *>(sender());
    m_locked[tl] = locked;
    if (!isLocked())
        m_backingStore->sync();
}

#else

static QPlatformTextureList *widgetTexturesFor(QWidget *tlw, QWidget *widget)
{
    Q_UNUSED(tlw);
    Q_UNUSED(widget);
    return nullptr;
}

#endif // QT_NO_OPENGL

static inline bool discardSyncRequest(QWidget *tlw, QTLWExtra *tlwExtra)
{
    if (!tlw || !tlwExtra || !tlw->testAttribute(Qt::WA_Mapped) || !tlw->isVisible())
        return true;

    return false;
}

bool QWidgetBackingStore::syncAllowed()
{
#ifndef QT_NO_OPENGL
    QTLWExtra *tlwExtra = tlw->d_func()->maybeTopData();
    if (textureListWatcher && !textureListWatcher->isLocked()) {
        textureListWatcher->deleteLater();
        textureListWatcher = 0;
    } else if (!tlwExtra->widgetTextures.isEmpty()) {
        bool skipSync = false;
        foreach (QPlatformTextureList *tl, tlwExtra->widgetTextures) {
            if (tl->isLocked()) {
                if (!textureListWatcher)
                    textureListWatcher = new QPlatformTextureListWatcher(this);
                if (!textureListWatcher->isLocked())
                    textureListWatcher->watch(tl);
                skipSync = true;
            }
        }
        if (skipSync)  // cannot compose due to widget textures being in use
            return false;
    }
#endif
    return true;
}

/*!
    Synchronizes the \a exposedRegion of the \a exposedWidget with the backing store.

    If there's nothing to repaint, the area is flushed and painting does not occur;
    otherwise the area is marked as dirty on screen and will be flushed right after
    we are done with all painting.
*/
void QWidgetBackingStore::sync(QWidget *exposedWidget, const QRegion &exposedRegion)
{

    QTLWExtra *tlwExtra = tlw->d_func()->maybeTopData();
    if (!tlw->isVisible() || !tlwExtra || tlwExtra->inTopLevelResize)
    {
        return;
    }

    if (!exposedWidget || !exposedWidget->internalWinId() || !exposedWidget->isVisible() || !exposedWidget->testAttribute(Qt::WA_Mapped)
        || !exposedWidget->updatesEnabled() || exposedRegion.isEmpty()) {
        return;
    }

    // Nothing to repaint.
    if (!isDirty() && store->size().isValid()) {
        QPlatformTextureList *tl = widgetTexturesFor(tlw, exposedWidget);
        qt_flush(exposedWidget, tl ? QRegion() : exposedRegion, store, tlw, tl, this);
        return;
    }

    if (exposedWidget != tlw)
        markDirtyOnScreen(exposedRegion, exposedWidget, exposedWidget->mapTo(tlw, QPoint()));
    else
        markDirtyOnScreen(exposedRegion, exposedWidget, QPoint());

    if (syncAllowed())
        doSync();

}

/*!
    Synchronizes the backing store, i.e. dirty areas are repainted and flushed.
*/
void QWidgetBackingStore::sync()
{
    updateRequestSent = false;
    QTLWExtra *tlwExtra = tlw->d_func()->maybeTopData();
    if (discardSyncRequest(tlw, tlwExtra)) {
        // If the top-level is minimized, it's not visible on the screen so we can delay the
        // update until it's shown again. In order to do that we must keep the dirty states.
        // These will be cleared when we receive the first expose after showNormal().
        // However, if the widget is not visible (isVisible() returns false), everything will
        // be invalidated once the widget is shown again, so clear all dirty states.
        if (!tlw->isVisible()) {
            dirty = QRegion();
            for (int i = 0; i < dirtyWidgets.size(); ++i)
                resetWidget(dirtyWidgets.at(i));
            dirtyWidgets.clear();
        }
        return;
    }

    if (syncAllowed())
        doSync();

}

void QWidgetBackingStore::doSync()
{
    const bool updatesDisabled = !tlw->updatesEnabled();
    bool repaintAllWidgets = false;

    const bool inTopLevelResize = tlw->d_func()->maybeTopData()->inTopLevelResize;
    const QRect tlwRect(topLevelRect());
    const QRect surfaceGeometry(tlwRect.topLeft(), store->size());
    if ((inTopLevelResize || surfaceGeometry.size() != tlwRect.size()) && !updatesDisabled) {
        if (hasStaticContents() && !store->size().isEmpty() ) {
            // Repaint existing dirty area and newly visible area.
            const QRect clipRect(0, 0, surfaceGeometry.width(), surfaceGeometry.height());
            const QRegion staticRegion(staticContents(0, clipRect));
            QRegion newVisible(0, 0, tlwRect.width(), tlwRect.height());
            newVisible -= staticRegion;
            dirty += newVisible;
            store->setStaticContents(staticRegion);
        } else {
            // Repaint everything.
            dirty = QRegion(0, 0, tlwRect.width(), tlwRect.height());
            for (int i = 0; i < dirtyWidgets.size(); ++i)
                resetWidget(dirtyWidgets.at(i));
            dirtyWidgets.clear();
            repaintAllWidgets = true;
        }
    }

    if (inTopLevelResize || surfaceGeometry.size() != tlwRect.size())
        store->resize(tlwRect.size());

    if (updatesDisabled)
        return;

    // Contains everything that needs repaint.
    QRegion toClean(dirty);

    // Loop through all update() widgets and remove them from the list before they are
    // painted (in case someone calls update() in paintEvent). If the widget is opaque
    // and does not have transparent overlapping siblings, append it to the
    // opaqueNonOverlappedWidgets list and paint it directly without composition.
    QVarLengthArray<QWidget *, 32> opaqueNonOverlappedWidgets;
    for (int i = 0; i < dirtyWidgets.size(); ++i) {
        QWidget *w = dirtyWidgets.at(i);
        QWidgetPrivate *wd = w->d_func();
        if (wd->data.in_destructor)
            continue;

        // Clip with mask() and clipRect().
        wd->dirty &= wd->clipRect();
        wd->clipToEffectiveMask(wd->dirty);

        // Subtract opaque siblings and children.
        bool hasDirtySiblingsAbove = false;
        // We know for sure that the widget isn't overlapped if 'isMoved' is true.
        if (!wd->isMoved)
            wd->subtractOpaqueSiblings(wd->dirty, &hasDirtySiblingsAbove);

        // Make a copy of the widget's dirty region, to restore it in case there is an opaque
        // render-to-texture child that completely covers the widget, because otherwise the
        // render-to-texture child won't be visible, due to its parent widget not being redrawn
        // with a proper blending mask.
        const QRegion dirtyBeforeSubtractedOpaqueChildren = wd->dirty;

        // Scrolled and moved widgets must draw all children.
        if (!wd->isScrolled && !wd->isMoved)
            wd->subtractOpaqueChildren(wd->dirty, w->rect());

        if (wd->dirty.isEmpty() && wd->textureChildSeen)
            wd->dirty = dirtyBeforeSubtractedOpaqueChildren;

        if (wd->dirty.isEmpty()) {
            resetWidget(w);
            continue;
        }

        const QRegion widgetDirty(w != tlw ? wd->dirty.translated(w->mapTo(tlw, QPoint()))
                                           : wd->dirty);
        toClean += widgetDirty;

#if QT_CONFIG(graphicsview)
        if (tlw->d_func()->extra->proxyWidget) {
            resetWidget(w);
            continue;
        }
#endif

        if (!hasDirtySiblingsAbove && wd->isOpaque && !dirty.intersects(widgetDirty.boundingRect())) {
            opaqueNonOverlappedWidgets.append(w);
        } else {
            resetWidget(w);
            dirty += widgetDirty;
        }
    }
    dirtyWidgets.clear();

#ifndef QT_NO_OPENGL
    // Find all render-to-texture child widgets (including self).
    // The search is cut at native widget boundaries, meaning that each native child widget
    // has its own list for the subtree below it.
    QTLWExtra *tlwExtra = tlw->d_func()->topData();
    qDeleteAll(tlwExtra->widgetTextures);
    tlwExtra->widgetTextures.clear();
    findAllTextureWidgetsRecursively(tlw, tlw);
    qt_window_private(tlw->windowHandle())->compositing = false; // will get updated in qt_flush()
#endif

    if (toClean.isEmpty()) {
        // Nothing to repaint. However renderToTexture widgets are handled
        // specially, they are not in the regular dirty list, in order to
        // prevent triggering unnecessary backingstore painting when only the
        // OpenGL content changes. Check if we have such widgets in the special
        // dirty list.
        QVarLengthArray<QWidget *, 16> paintPending;
        const int numPaintPending = dirtyRenderToTextureWidgets.count();
        paintPending.reserve(numPaintPending);
        for (int i = 0; i < numPaintPending; ++i) {
            QWidget *w = dirtyRenderToTextureWidgets.at(i);
            paintPending << w;
            resetWidget(w);
        }
        dirtyRenderToTextureWidgets.clear();
        for (int i = 0; i < numPaintPending; ++i) {
            QWidget *w = paintPending[i];
            w->d_func()->sendPaintEvent(w->rect());
            if (w != tlw) {
                QWidget *npw = w->nativeParentWidget();
                if (w->internalWinId() || (npw && npw != tlw)) {
                    if (!w->internalWinId())
                        w = npw;
                    QWidgetPrivate *wPrivate = w->d_func();
                    if (!wPrivate->needsFlush)
                        wPrivate->needsFlush = new QRegion;
                    appendDirtyOnScreenWidget(w);
                }
            }
        }

        // We might have newly exposed areas on the screen if this function was
        // called from sync(QWidget *, QRegion)), so we have to make sure those
        // are flushed. We also need to composite the renderToTexture widgets.
        flush();

        return;
    }

#ifndef QT_NO_OPENGL
    foreach (QPlatformTextureList *tl, tlwExtra->widgetTextures) {
        for (int i = 0; i < tl->count(); ++i) {
            QWidget *w = static_cast<QWidget *>(tl->source(i));
            if (dirtyRenderToTextureWidgets.contains(w)) {
                const QRect rect = tl->geometry(i); // mapped to the tlw already
                // Set a flag to indicate that the paint event for this
                // render-to-texture widget must not to be optimized away.
                w->d_func()->renderToTextureReallyDirty = 1;
                dirty += rect;
                toClean += rect;
            }
        }
    }
    for (int i = 0; i < dirtyRenderToTextureWidgets.count(); ++i)
        resetWidget(dirtyRenderToTextureWidgets.at(i));
    dirtyRenderToTextureWidgets.clear();
#endif

#if QT_CONFIG(graphicsview)
    if (tlw->d_func()->extra->proxyWidget) {
        updateStaticContentsSize();
        dirty = QRegion();
        updateRequestSent = false;
        for (const QRect &rect : toClean)
            tlw->d_func()->extra->proxyWidget->update(rect);
        return;
    }
#endif

    BeginPaintInfo beginPaintInfo;
    beginPaint(toClean, tlw, store, &beginPaintInfo);
    if (beginPaintInfo.nothingToPaint) {
        for (int i = 0; i < opaqueNonOverlappedWidgets.size(); ++i)
            resetWidget(opaqueNonOverlappedWidgets[i]);
        dirty = QRegion();
        updateRequestSent = false;
        return;
    }

    // Must do this before sending any paint events because
    // the size may change in the paint event.
    updateStaticContentsSize();
    const QRegion dirtyCopy(dirty);
    dirty = QRegion();
    updateRequestSent = false;

    // Paint opaque non overlapped widgets.
    for (int i = 0; i < opaqueNonOverlappedWidgets.size(); ++i) {
        QWidget *w = opaqueNonOverlappedWidgets[i];
        QWidgetPrivate *wd = w->d_func();

        int flags = QWidgetPrivate::DrawRecursive;
        // Scrolled and moved widgets must draw all children.
        if (!wd->isScrolled && !wd->isMoved)
            flags |= QWidgetPrivate::DontDrawOpaqueChildren;
        if (w == tlw)
            flags |= QWidgetPrivate::DrawAsRoot;

        QRegion toBePainted(wd->dirty);
        resetWidget(w);

        QPoint offset;
        if (w != tlw)
            offset += w->mapTo(tlw, QPoint());
        wd->drawWidget(store->paintDevice(), toBePainted, offset, flags, 0, this);
    }

    // Paint the rest with composition.
    if (repaintAllWidgets || !dirtyCopy.isEmpty()) {
        const int flags = QWidgetPrivate::DrawAsRoot | QWidgetPrivate::DrawRecursive;
        tlw->d_func()->drawWidget(store->paintDevice(), dirtyCopy, QPoint(), flags, 0, this);
    }

    endPaint(toClean, store, &beginPaintInfo);
}

/*!
    Flushes the contents of the backing store into the top-level widget.
    If the \a widget is non-zero, the content is flushed to the \a widget.
    If the \a surface is non-zero, the content of the \a surface is flushed.
*/
void QWidgetBackingStore::flush(QWidget *widget)
{
    const bool hasDirtyOnScreenWidgets = dirtyOnScreenWidgets && !dirtyOnScreenWidgets->isEmpty();
    bool flushed = false;

    // Flush the region in dirtyOnScreen.
    if (!dirtyOnScreen.isEmpty()) {
        QWidget *target = widget ? widget : tlw;
        qt_flush(target, dirtyOnScreen, store, tlw, widgetTexturesFor(tlw, tlw), this);
        dirtyOnScreen = QRegion();
        flushed = true;
    }

    // Render-to-texture widgets are not in dirtyOnScreen so flush if we have not done it above.
    if (!flushed && !hasDirtyOnScreenWidgets) {
#ifndef QT_NO_OPENGL
        if (!tlw->d_func()->topData()->widgetTextures.isEmpty()) {
            QPlatformTextureList *tl = widgetTexturesFor(tlw, tlw);
            if (tl) {
                QWidget *target = widget ? widget : tlw;
                qt_flush(target, QRegion(), store, tlw, tl, this);
            }
        }
#endif
    }

    if (!hasDirtyOnScreenWidgets)
        return;

    for (int i = 0; i < dirtyOnScreenWidgets->size(); ++i) {
        QWidget *w = dirtyOnScreenWidgets->at(i);
        QWidgetPrivate *wd = w->d_func();
        Q_ASSERT(wd->needsFlush);
        QPlatformTextureList *widgetTexturesForNative = wd->textureChildSeen ? widgetTexturesFor(tlw, w) : 0;
        qt_flush(w, *wd->needsFlush, store, tlw, widgetTexturesForNative, this);
        *wd->needsFlush = QRegion();
    }
    dirtyOnScreenWidgets->clear();
}

static inline bool discardInvalidateBufferRequest(QWidget *widget, QTLWExtra *tlwExtra)
{
    Q_ASSERT(widget);
    if (QApplication::closingDown())
        return true;

    if (!tlwExtra || tlwExtra->inTopLevelResize || !tlwExtra->backingStore)
        return true;

    if (!widget->isVisible() || !widget->updatesEnabled())
        return true;

    return false;
}

/*!
    Invalidates the buffer when the widget is resized.
    Static areas are never invalidated unless absolutely needed.
*/
void QWidgetPrivate::invalidateBuffer_resizeHelper(const QPoint &oldPos, const QSize &oldSize)
{
    Q_Q(QWidget);
    Q_ASSERT(!q->isWindow());
    Q_ASSERT(q->parentWidget());

    const bool staticContents = q->testAttribute(Qt::WA_StaticContents);
    const bool sizeDecreased = (data.crect.width() < oldSize.width())
                               || (data.crect.height() < oldSize.height());

    const QPoint offset(data.crect.x() - oldPos.x(), data.crect.y() - oldPos.y());
    const bool parentAreaExposed = !offset.isNull() || sizeDecreased;
    const QRect newWidgetRect(q->rect());
    const QRect oldWidgetRect(0, 0, oldSize.width(), oldSize.height());

    if (!staticContents || graphicsEffect) {
        QRegion staticChildren;
        QWidgetBackingStore *bs = 0;
        if (offset.isNull() && (bs = maybeBackingStore()))
            staticChildren = bs->staticContents(q, oldWidgetRect);
        const bool hasStaticChildren = !staticChildren.isEmpty();

        if (hasStaticChildren) {
            QRegion dirty(newWidgetRect);
            dirty -= staticChildren;
            invalidateBuffer(dirty);
        } else {
            // Entire widget needs repaint.
            invalidateBuffer(newWidgetRect);
        }

        if (!parentAreaExposed)
            return;

        // Invalidate newly exposed area of the parent.
        if (!graphicsEffect && extra && extra->hasMask) {
            QRegion parentExpose(extra->mask.translated(oldPos));
            parentExpose &= QRect(oldPos, oldSize);
            if (hasStaticChildren)
                parentExpose -= data.crect; // Offset is unchanged, safe to do this.
            q->parentWidget()->d_func()->invalidateBuffer(parentExpose);
        } else {
            if (hasStaticChildren && !graphicsEffect) {
                QRegion parentExpose(QRect(oldPos, oldSize));
                parentExpose -= data.crect; // Offset is unchanged, safe to do this.
                q->parentWidget()->d_func()->invalidateBuffer(parentExpose);
            } else {
                q->parentWidget()->d_func()->invalidateBuffer(effectiveRectFor(QRect(oldPos, oldSize)));
            }
        }
        return;
    }

    // Move static content to its new position.
    if (!offset.isNull()) {
        if (sizeDecreased) {
            const QSize minSize(qMin(oldSize.width(), data.crect.width()),
                                qMin(oldSize.height(), data.crect.height()));
            moveRect(QRect(oldPos, minSize), offset.x(), offset.y());
        } else {
            moveRect(QRect(oldPos, oldSize), offset.x(), offset.y());
        }
    }

    // Invalidate newly visible area of the widget.
    if (!sizeDecreased || !oldWidgetRect.contains(newWidgetRect)) {
        QRegion newVisible(newWidgetRect);
        newVisible -= oldWidgetRect;
        invalidateBuffer(newVisible);
    }

    if (!parentAreaExposed)
        return;

    // Invalidate newly exposed area of the parent.
    const QRect oldRect(oldPos, oldSize);
    if (extra && extra->hasMask) {
        QRegion parentExpose(oldRect);
        parentExpose &= extra->mask.translated(oldPos);
        parentExpose -= (extra->mask.translated(data.crect.topLeft()) & data.crect);
        q->parentWidget()->d_func()->invalidateBuffer(parentExpose);
    } else {
        QRegion parentExpose(oldRect);
        parentExpose -= data.crect;
        q->parentWidget()->d_func()->invalidateBuffer(parentExpose);
    }
}

/*!
    Invalidates the \a rgn (in widget's coordinates) of the backing store, i.e.
    all widgets intersecting with the region will be repainted when the backing store
    is synced.

    ### Qt 4.6: Merge into a template function (after MSVC isn't supported anymore).
*/
void QWidgetPrivate::invalidateBuffer(const QRegion &rgn)
{
    Q_Q(QWidget);

    QTLWExtra *tlwExtra = q->window()->d_func()->maybeTopData();
    if (discardInvalidateBufferRequest(q, tlwExtra) || rgn.isEmpty())
        return;

    QRegion wrgn(rgn);
    wrgn &= clipRect();
    if (!graphicsEffect && extra && extra->hasMask)
        wrgn &= extra->mask;
    if (wrgn.isEmpty())
        return;

    tlwExtra->backingStoreTracker->markDirty(wrgn, q,
            QWidgetBackingStore::UpdateLater, QWidgetBackingStore::BufferInvalid);
}

/*!
    This function is equivalent to calling invalidateBuffer(QRegion(rect), ...), but
    is more efficient as it eliminates QRegion operations/allocations and can
    use the rect more precisely for additional cut-offs.

    ### Qt 4.6: Merge into a template function (after MSVC isn't supported anymore).
*/
void QWidgetPrivate::invalidateBuffer(const QRect &rect)
{
    Q_Q(QWidget);

    QTLWExtra *tlwExtra = q->window()->d_func()->maybeTopData();
    if (discardInvalidateBufferRequest(q, tlwExtra) || rect.isEmpty())
        return;

    QRect wRect(rect);
    wRect &= clipRect();
    if (wRect.isEmpty())
        return;

    if (graphicsEffect || !extra || !extra->hasMask) {
        tlwExtra->backingStoreTracker->markDirty(wRect, q,
                QWidgetBackingStore::UpdateLater, QWidgetBackingStore::BufferInvalid);
        return;
    }

    QRegion wRgn(extra->mask);
    wRgn &= wRect;
    if (wRgn.isEmpty())
        return;

    tlwExtra->backingStoreTracker->markDirty(wRgn, q,
            QWidgetBackingStore::UpdateLater, QWidgetBackingStore::BufferInvalid);
}

void QWidgetPrivate::repaint_sys(const QRegion &rgn)
{
    if (data.in_destructor)
        return;

    Q_Q(QWidget);
    if (discardSyncRequest(q, maybeTopData()))
        return;

    if (q->testAttribute(Qt::WA_StaticContents)) {
        if (!extra)
            createExtra();
        extra->staticContentsSize = data.crect.size();
    }

    QPaintEngine *engine = q->paintEngine();

    // QGLWidget does not support partial updates if:
    // 1) The context is double buffered
    // 2) The context is single buffered and auto-fill background is enabled.
    const bool noPartialUpdateSupport = (engine && (engine->type() == QPaintEngine::OpenGL
                                                || engine->type() == QPaintEngine::OpenGL2))
                                        && (usesDoubleBufferedGLContext || q->autoFillBackground());
    QRegion toBePainted(noPartialUpdateSupport ? q->rect() : rgn);

#if 0 // Used to be included in Qt4 for Q_WS_MAC
    // No difference between update() and repaint() on the Mac.
    update_sys(toBePainted);
    return;
#endif

    toBePainted &= clipRect();
    clipToEffectiveMask(toBePainted);
    if (toBePainted.isEmpty())
        return; // Nothing to repaint.

#ifndef QT_NO_PAINT_DEBUG
    bool flushed = QWidgetBackingStore::flushPaint(q, toBePainted);
#endif

    drawWidget(q, toBePainted, QPoint(), QWidgetPrivate::DrawAsRoot | QWidgetPrivate::DrawPaintOnScreen, 0);

#ifndef QT_NO_PAINT_DEBUG
    if (flushed)
        QWidgetBackingStore::unflushPaint(q, toBePainted);
#endif

    abort();

    if (Q_UNLIKELY(q->paintingActive()))
        qWarning("QWidget::repaint: It is dangerous to leave painters active on a widget outside of the PaintEvent");

}


QT_END_NAMESPACE

#include "moc_qwidgetbackingstore_p.cpp"
