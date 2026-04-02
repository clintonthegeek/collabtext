# Low-Latency Input Rendering in Qt Widgets

This document describes how to minimize keyboard-to-screen latency in our
forked QPlainTextEdit widget. The techniques are derived from studying the Zed
editor's rendering pipeline and mapping its core ideas onto Qt's QWidget
internals.

## Background: Where Latency Comes From

In a typical Qt widget, a keystroke follows this path:

1. OS delivers key event to the platform plugin (XCB, Cocoa, Win32).
2. QPA posts it to the window system event queue (asynchronous by default).
3. The event loop picks it up, delivers it to the focused widget's
   `keyPressEvent()`.
4. The widget modifies the document model, then calls `update()`.
5. `update()` posts a `QEvent::UpdateRequest` at **`Qt::LowEventPriority`**.
6. Control returns to the event loop.
7. Eventually, the low-priority UpdateRequest is dequeued.
8. `QWidgetRepaintManager::paintAndFlush()` runs: it calls `paintEvent()`,
   draws to the backing store, and flushes to the window surface.

Steps 5-7 are the main latency source under our control. The paint is deferred
to a future event loop iteration and queued at the lowest priority, meaning any
other posted event (timers, socket notifiers, deferred deletes) will be
serviced first.

Zed avoids this entirely: when a key event arrives, it calls its `draw()`
function **synchronously inside the input handler**, before returning to its
event loop. The frame is rendered and presented as part of handling the
keystroke, not as a separate deferred step.

## The Fix: `repaint()` Instead of `update()`

Qt already supports this pattern. `QWidget::repaint()` and `QWidget::update()`
differ in exactly one way:

| Method      | Internal call                          | Timing         |
|-------------|----------------------------------------|----------------|
| `update()`  | `markDirty(region, widget, UpdateLater)` | Deferred, low priority |
| `repaint()` | `markDirty(region, widget, UpdateNow)`   | Synchronous    |

When `UpdateNow` is used, `QWidgetRepaintManager::sendUpdateRequest()` calls
`QCoreApplication::sendEvent()` (synchronous delivery) instead of
`postEvent()`. The entire pipeline -- `paintEvent()`, backing store
composition, and platform flush -- runs before `repaint()` returns.

**In our forked QPlainTextEdit, any code path triggered by keyboard input
that currently calls `update()` should call `repaint()` instead.**

This means the rendering happens inside the same call stack as input handling:

```
keyPressEvent()
  -> modify document
  -> repaint()
    -> QWidgetRepaintManager::sendUpdateRequest(widget, UpdateNow)
      -> QCoreApplication::sendEvent(widget, UpdateRequest)
        -> paintAndFlush()
          -> paintEvent()       // our code draws with QPainter
          -> flush()            // pixels pushed to window surface
    -> returns
  -> returns
// latency = time spent in the above, no event loop round-trip
```

### Where to Change

Upstream `QPlainTextEdit` calls `viewport()->update()` in several places after
modifying the document. Grep the forked widget for `viewport()->update()` and
`update()` calls on input-driven code paths. Replace those with
`viewport()->repaint()` or `repaint()` respectively.

Do **not** blanket-replace every `update()` call. Only target the paths
triggered by keyboard input (typing, backspace, delete, cut/paste, etc.).
Non-interactive updates (document reloads, theme changes, resize
reflows) should remain as `update()` to avoid redundant synchronous paints.

### Caveat: renderToTexture Demotion

`QWidgetRepaintManager::sendUpdateRequest()` (qtbase
`src/widgets/kernel/qwidgetrepaintmanager.cpp`, around line 342) contains
a throttle: if the widget's top-level window has `textureChildSeen` set (any
QOpenGLWidget in the hierarchy), and the time since the last compose is within
one vsync period, `UpdateNow` is silently **demoted to `UpdateLater`**.

This means: if the widget tree contains any QOpenGLWidget, the `repaint()`
trick is partially defeated. Our editor widget is pure raster (QPainter on
QWidget), so this does not apply as long as no QOpenGLWidget is added to the
same top-level window.

## Optional: Input Rate Tracking

Zed implements an `InputRateTracker` that detects sustained high-frequency
input (keyboard repeat, fast typing) and keeps the rendering loop running at
full speed for one second after the burst ends. This prevents the display from
underclocking during typing and avoids a visible "settling" lag when typing
stops.

A simple Qt equivalent:

```cpp
// In the editor widget class
QElapsedTimer m_lastInputTime;
int m_inputCount = 0;
bool m_sustainedInput = false;
QTimer m_sustainTimer;

void handleInputBurst()
{
    if (!m_lastInputTime.isValid())
        m_lastInputTime.start();

    ++m_inputCount;

    // Check rate over the last 100ms
    if (m_lastInputTime.elapsed() > 100) {
        m_sustainedInput = (m_inputCount > 6); // ~60+ inputs/sec
        m_inputCount = 0;
        m_lastInputTime.restart();
    }

    if (m_sustainedInput) {
        // Restart the sustain window: keep repainting for 1s after
        // the last input event, even if nothing changes.
        m_sustainTimer.start(1000);
    }
}
```

Call `handleInputBurst()` at the top of `keyPressEvent()`. While
`m_sustainedInput` is true, use `repaint()`. When the sustain timer fires
(1 second of silence), clear the flag and revert to normal `update()` behavior.

This is a minor optimization. The `repaint()` change alone is the dominant
improvement.

## What NOT to Do

### Don't add GPU text rendering via QOpenGLWidget

Zed renders text on the GPU because it is a from-scratch GPU-native
application. In Qt's QWidget pipeline, QOpenGLWidget does not replace the
backing store -- it punches a transparent hole in the raster backing store and
renders to a separate texture, which is then composited via RHI. This
compositing step adds overhead and, as noted above, triggers the vsync
demotion logic that defeats `repaint()`. For a text editor doing glyph
rendering, the raster QPainter path with font engine glyph caching is already
efficient.

### Don't try to bypass QWidgetRepaintManager

It is possible to obtain the backing store's paint device and draw to it
directly, skipping the repaint manager. This breaks: child widget clipping,
opaque region optimization, expose event handling, and high-DPI scaling.
The repaint manager's overhead is minimal -- the synchronous `repaint()` path
runs the same `paintAndFlush()` function with negligible dispatch cost.

### Don't add platform-specific display link integration

Zed uses CVDisplayLink on macOS for vsync-aware frame scheduling. Qt widgets
have no vsync awareness and adding it would require patching the platform
plugin layer. The benefit is marginal for a text editor: vsync matters when
you're animating at 60+ fps. For keystroke response, the goal is to render
as fast as possible after input, not to synchronize with the display refresh.
`repaint()` achieves this.

## Summary

The single most impactful change is replacing `update()` with `repaint()` on
keyboard-driven code paths. This eliminates the event loop round-trip and
low-priority queue delay, rendering the frame synchronously during input
handling -- the same architectural trick that makes Zed feel fast.

## Reference

Relevant Qt source files (relative to qtbase):

- `src/widgets/kernel/qwidget.cpp` -- `update()` at line ~11327, `repaint()`
  at line ~11249, `drawWidget()` at line ~5514
- `src/widgets/kernel/qwidgetrepaintmanager.cpp` -- `markDirty()` at line ~168,
  `sendUpdateRequest()` at line ~331, `paintAndFlush()` at line ~709
- `src/widgets/kernel/qapplication.cpp` -- event compression at line ~790
- `src/gui/kernel/qwindowsysteminterface.cpp` -- QPA event delivery at
  line ~76
- `src/plugins/platforms/xcb/qxcbkeyboard.cpp` -- platform key event entry at
  line ~925
