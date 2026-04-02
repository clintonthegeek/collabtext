# GPU Rendering and VSync-Paced Repainting for Qt Widget Text Editors

This document describes two advanced approaches to reducing input-to-screen
latency beyond what the `repaint()` trick (described in `LOW_LATENCY_INPUT.md`)
provides. Both can be pursued independently or combined.

**Option B** replaces the CPU raster text rendering path with a GPU glyph
atlas renderer, using Qt's `QRhiWidget` to embed GPU-rendered content inside a
normal QWidget hierarchy.

**Option C** replaces the low-priority `postEvent()` scheduling in
`QWidgetRepaintManager` with display-synced frame delivery via
`QWindow::requestUpdate()`, giving the widget paint cycle vsync awareness
without changing the paint backend.

---

## Option B: GPU Text Rendering via QRhiWidget

### What QRhiWidget Is

`QRhiWidget` is a QWidget subclass (lives in qtbase, not qtdeclarative) that
renders into an offscreen GPU texture via Qt's `QRhi` hardware abstraction.
The texture is then composited into the window alongside normal raster widget
content. No QML, no scene graph, no JavaScript engine.

**Source:** `qtbase/src/widgets/kernel/qrhiwidget.h`
**Examples:** `qtbase/examples/widgets/rhi/simplerhiwidget/`,
`qtbase/examples/widgets/rhi/cuberhiwidget/`

The API surface is small. You subclass it and override two methods:

```cpp
class TextViewport : public QRhiWidget {
protected:
    void initialize(QRhiCommandBuffer *cb) override;
    void render(QRhiCommandBuffer *cb) override;
};
```

`initialize()` is called once on first paint, and again whenever the widget
resizes, the sample count changes, or the QRhi instance changes (which happens
when the widget is reparented to a different top-level window). Create all GPU
resources here.

`render()` is called every paint cycle. A frame is already recording but no
render pass is active. You begin a render pass, draw, and end it.

### How It Fits into the Widget Hierarchy

QRhiWidget uses the `renderToTexture` machinery that already exists in Qt's
repaint manager. The compositing model works as follows:

1. All normal raster widgets paint to the backing store via QPainter as usual.
2. QRhiWidget renders to its own GPU texture (not the backing store).
3. The repaint manager punches a transparent hole in the backing store at the
   QRhiWidget's position.
4. `QBackingStoreDefaultCompositor` composites the raster content and the GPU
   texture together in a final RHI render pass, blending them in Z-order.
5. The composed frame is presented to the window surface.

This means QRhiWidget siblings (scrollbars, line number gutters, status bars)
remain normal raster widgets. Only the text viewport itself is GPU-rendered.

### QRhi Instance Lifecycle

QRhiWidget does not create its own QRhi. The top-level window's backing store
owns a single QRhi instance, shared by all QRhiWidget instances in that
window. You access it via the `rhi()` accessor, which is only valid during
`initialize()` and `render()`.

The backend (Vulkan, Metal, D3D11/12, OpenGL) is selected by calling
`setApi()` before the widget is shown. Defaults: Metal on macOS, D3D11 on
Windows, OpenGL on Linux. All QRhiWidget instances in the same window must
use the same backend.

If the widget is reparented to a different top-level window, `releaseResources()`
is called (drop all GPU resources), the QRhi pointer changes, and
`initialize()` is called again with the new QRhi. Your `initialize()` must
detect this:

```cpp
void TextViewport::initialize(QRhiCommandBuffer *cb)
{
    if (m_rhi != rhi()) {
        // QRhi changed (reparented). Old resources are invalid.
        m_pipeline.reset();
        m_rhi = rhi();
    }

    if (!m_pipeline) {
        // Create all resources from scratch...
    }
}
```

### The Render Target

When `autoRenderTarget` is true (the default), QRhiWidget manages a color
texture and a depth-stencil buffer for you. Access them via:

- `colorTexture()` -- the RGBA8 (or RGBA16F, RGBA32F, RGB10A2) texture you
  render into. Size tracks the widget size times the device pixel ratio,
  unless you set a fixed size via `setFixedColorBufferSize()`.
- `depthStencilBuffer()` -- auto-managed depth-stencil renderbuffer.
- `renderTarget()` -- the render target that binds both. Pass this to
  `cb->beginPass()`.
- `renderTarget()->renderPassDescriptor()` -- needed when creating pipelines.

If MSAA is enabled (`setSampleCount(4)` etc.), the color target becomes a
multisample renderbuffer (`msaaColorBuffer()`), and a single-sampled
`resolveTexture()` is used for compositing. The resolve happens automatically
at the end of the render pass.

### Building a Glyph Atlas Renderer

This is the bulk of the work. You need three things: a glyph atlas texture,
a vertex buffer of positioned quads, and a shader that samples the atlas.

#### The Glyph Atlas

A glyph atlas is a large texture (typically 2048x2048 or 4096x4096) into which
rasterized glyph images are packed. When a character needs to be displayed,
you look up its position in the atlas and emit a textured quad.

QRhi's texture upload API is explicitly designed for atlas use cases. The
documentation says so directly (qrhi.cpp, around line 3198):

> "QRhiTextureUploadDescription also enables specifying batched uploads,
> which are useful for example when generating an atlas or glyph cache
> texture: multiple, partial uploads for the same subresource [...] are
> supported, and can be, depending on the backend and the underlying
> graphics API, more efficient when batched into the same
> QRhiTextureUploadDescription."

Create the atlas texture:

```cpp
// R8 format: single channel, 1 byte per texel.
// For a grayscale glyph atlas this is ideal.
// 4096x4096 at R8 = 16 MB, enough for thousands of glyphs.
m_atlas = m_rhi->newTexture(QRhiTexture::R8, QSize(4096, 4096));
m_atlas->create();
```

Query the device limit if you want to be safe:

```cpp
int maxSize = m_rhi->resourceLimit(QRhi::TextureSizeMax);
// Typically 4096 on OpenGL ES 2, 8192-16384 on desktop.
```

#### Partial Texture Uploads

When a new glyph is rasterized (via Qt's font engine, `QRawFont::alphaMapForGlyph()`
or by painting into a small QImage with QPainter), upload it to a sub-region
of the atlas:

```cpp
void GlyphAtlas::uploadGlyph(QRhi *rhi,
                              QRhiResourceUpdateBatch *batch,
                              const QImage &glyphImage,
                              QPoint atlasPosition)
{
    QRhiTextureSubresourceUploadDescription sub(glyphImage);
    sub.setDestinationTopLeft(atlasPosition);
    // sourceSize defaults to the full glyphImage, which is what we want.

    batch->uploadTexture(m_atlas,
        QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, sub)));
}
```

Multiple glyph uploads can be batched into a single
`QRhiTextureUploadDescription` by passing multiple `QRhiTextureUploadEntry`
objects. The batch is submitted when you pass the `QRhiResourceUpdateBatch`
to `cb->beginPass()` as its third argument.

Atlas packing (deciding where each glyph goes) is your problem. A simple
shelf packer works: maintain a current row Y position and a current X cursor.
Place glyphs left-to-right, advancing to the next row when the current one
is full. For a text editor with a monospace or small set of fonts, this is
more than adequate.

#### Vertex Buffer: Glyph Quads

Each visible glyph on screen becomes a textured quad (two triangles, four
vertices with a triangle strip, or six vertices with plain triangles). The
vertex data contains the screen position and the atlas UV coordinates.

```cpp
struct GlyphVertex {
    float x, y;       // screen position (pixels)
    float u, v;       // atlas texture coordinates (normalized 0..1)
};
```

Use a dynamic vertex buffer since the content changes every frame (scrolling,
typing, cursor movement):

```cpp
m_vbuf = m_rhi->newBuffer(QRhiBuffer::Dynamic,
                           QRhiBuffer::VertexBuffer,
                           MAX_VISIBLE_GLYPHS * 6 * sizeof(GlyphVertex));
m_vbuf->create();
```

Update it in `render()`:

```cpp
QRhiResourceUpdateBatch *batch = m_rhi->nextResourceUpdateBatch();

// Build vertex data for all visible glyphs...
QVector<GlyphVertex> vertices;
for (each visible glyph) {
    QRectF screenRect = ...;  // where the glyph goes on screen
    QRectF atlasRect = ...;   // normalized UV rect in the atlas

    // Two triangles per glyph (6 vertices)
    vertices.append({screenRect.left(),  screenRect.top(),    atlasRect.left(),  atlasRect.top()});
    vertices.append({screenRect.right(), screenRect.top(),    atlasRect.right(), atlasRect.top()});
    vertices.append({screenRect.left(),  screenRect.bottom(), atlasRect.left(),  atlasRect.bottom()});
    vertices.append({screenRect.right(), screenRect.top(),    atlasRect.right(), atlasRect.top()});
    vertices.append({screenRect.right(), screenRect.bottom(), atlasRect.right(), atlasRect.bottom()});
    vertices.append({screenRect.left(),  screenRect.bottom(), atlasRect.left(),  atlasRect.bottom()});
}

batch->updateDynamicBuffer(m_vbuf, 0,
    vertices.size() * sizeof(GlyphVertex), vertices.constData());
```

QRhi handles double/triple buffering internally for dynamic buffers, so you do
not need to manage multiple copies or worry about GPU/CPU synchronization.

#### Alternative: Instanced Rendering

Instead of emitting six vertices per glyph, you can use instanced rendering.
Define a unit quad as a static vertex buffer (4 vertices, draw as triangle
strip) and put per-glyph data (position, atlas UV offset, size) in a
per-instance buffer:

```cpp
struct GlyphInstance {
    float x, y;          // screen position
    float w, h;          // glyph size in pixels
    float atlasU, atlasV; // atlas UV origin
    float atlasW, atlasH; // atlas UV size
};

// Vertex input layout with two bindings:
QRhiVertexInputLayout layout;
layout.setBindings({
    QRhiVertexInputBinding(2 * sizeof(float)),  // binding 0: unit quad, per-vertex
    QRhiVertexInputBinding(sizeof(GlyphInstance),
                           QRhiVertexInputBinding::PerInstance)  // binding 1: per-instance
});
layout.setAttributes({
    { 0, 0, QRhiVertexInputAttribute::Float2, 0 },              // quad corner
    { 1, 1, QRhiVertexInputAttribute::Float2, offsetof(GlyphInstance, x) },
    { 1, 2, QRhiVertexInputAttribute::Float2, offsetof(GlyphInstance, w) },
    { 1, 3, QRhiVertexInputAttribute::Float2, offsetof(GlyphInstance, atlasU) },
    { 1, 4, QRhiVertexInputAttribute::Float2, offsetof(GlyphInstance, atlasW) },
});

// Draw all glyphs in one call:
cb->draw(4, glyphCount);
```

This reduces CPU-side vertex generation and GPU vertex throughput. For a text
editor showing thousands of glyphs, the difference is measurable.

#### Shaders

Shaders are written in GLSL and compiled to `.qsb` (Qt Shader Binary) format
using the `qsb` tool, which ships with Qt. The `.qsb` file contains
pre-compiled variants for all backends (SPIR-V for Vulkan, MSL for Metal,
HLSL for Direct3D, GLSL for OpenGL).

Vertex shader (for the non-instanced path):

```glsl
#version 440

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texcoord;

layout(location = 0) out vec2 vUV;

layout(std140, binding = 0) uniform Block {
    mat4 projection;
    vec4 textColor;
};

void main()
{
    vUV = texcoord;
    gl_Position = projection * vec4(position, 0.0, 1.0);
}
```

Fragment shader:

```glsl
#version 440

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Block {
    mat4 projection;
    vec4 textColor;
};

layout(binding = 1) uniform sampler2D glyphAtlas;

void main()
{
    float coverage = texture(glyphAtlas, vUV).r;
    fragColor = textColor * coverage;
}
```

The fragment shader samples the single-channel atlas and multiplies by the
text color. Coverage acts as alpha. This produces correct antialiased text
over any background.

Compile during the build:

```
qsb --glsl 100es,120,150 --hlsl 50 --msl 12 -o text.vert.qsb text.vert
qsb --glsl 100es,120,150 --hlsl 50 --msl 12 -o text.frag.qsb text.frag
```

Load at runtime:

```cpp
static QShader loadShader(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll())
                                       : QShader();
}
```

#### Pipeline Setup

```cpp
m_pipeline = m_rhi->newGraphicsPipeline();

// Alpha blending for text compositing
QRhiGraphicsPipeline::TargetBlend blend;
blend.enable = true;
blend.srcColor = QRhiGraphicsPipeline::One;
blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
blend.srcAlpha = QRhiGraphicsPipeline::One;
blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
m_pipeline->setTargetBlends({ blend });

// No depth testing for 2D text
m_pipeline->setDepthTest(false);
m_pipeline->setDepthWrite(false);

m_pipeline->setShaderStages({
    { QRhiShaderStage::Vertex,   loadShader(":/text.vert.qsb") },
    { QRhiShaderStage::Fragment, loadShader(":/text.frag.qsb") },
});

// Vertex layout (non-instanced)
QRhiVertexInputLayout layout;
layout.setBindings({{ sizeof(GlyphVertex) }});
layout.setAttributes({
    { 0, 0, QRhiVertexInputAttribute::Float2, 0 },
    { 0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float) },
});
m_pipeline->setVertexInputLayout(layout);

m_pipeline->setShaderResourceBindings(m_srb);
m_pipeline->setRenderPassDescriptor(
    renderTarget()->renderPassDescriptor());
m_pipeline->create();
```

#### The Sampler

```cpp
m_sampler = m_rhi->newSampler(
    QRhiSampler::Linear,       // mag filter: smooth scaling
    QRhiSampler::Linear,       // min filter
    QRhiSampler::None,         // no mipmaps
    QRhiSampler::ClampToEdge,  // prevent atlas edge bleeding
    QRhiSampler::ClampToEdge);
m_sampler->create();
```

Linear filtering is important for smooth glyph rendering at non-integer
positions (subpixel glyph placement).

#### Shader Resource Bindings

```cpp
m_srb = m_rhi->newShaderResourceBindings();
m_srb->setBindings({
    QRhiShaderResourceBinding::uniformBuffer(
        0,
        QRhiShaderResourceBinding::VertexStage |
            QRhiShaderResourceBinding::FragmentStage,
        m_ubuf),
    QRhiShaderResourceBinding::sampledTexture(
        1,
        QRhiShaderResourceBinding::FragmentStage,
        m_atlas, m_sampler),
});
m_srb->create();
```

#### The render() Method, Assembled

```cpp
void TextViewport::render(QRhiCommandBuffer *cb)
{
    QRhiResourceUpdateBatch *batch = m_rhi->nextResourceUpdateBatch();

    // 1. Upload any new glyphs to the atlas
    for (auto &pending : m_pendingGlyphs) {
        QRhiTextureSubresourceUploadDescription sub(pending.image);
        sub.setDestinationTopLeft(pending.atlasPos);
        batch->uploadTexture(m_atlas,
            QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, sub)));
    }
    m_pendingGlyphs.clear();

    // 2. Build vertex data for visible text
    auto vertices = buildVisibleGlyphVertices();

    // 3. Update dynamic buffers
    batch->updateDynamicBuffer(m_vbuf, 0,
        vertices.size() * sizeof(GlyphVertex), vertices.constData());

    QMatrix4x4 proj = m_rhi->clipSpaceCorrMatrix();
    proj.ortho(0, width() * devicePixelRatioF(),
               height() * devicePixelRatioF(), 0,
               -1, 1);
    // Pack projection + color into uniform buffer
    batch->updateDynamicBuffer(m_ubuf, 0, 64, proj.constData());
    QVector4D color(m_textColor.redF(), m_textColor.greenF(),
                    m_textColor.blueF(), m_textColor.alphaF());
    batch->updateDynamicBuffer(m_ubuf, 64, 16, &color);

    // 4. Render
    cb->beginPass(renderTarget(), m_backgroundColor, { 1.0f, 0 }, batch);
    cb->setGraphicsPipeline(m_pipeline);
    cb->setViewport({ 0, 0,
        float(renderTarget()->pixelSize().width()),
        float(renderTarget()->pixelSize().height()) });
    cb->setShaderResources();

    const QRhiCommandBuffer::VertexInput vbufBinding(m_vbuf, 0);
    cb->setVertexInput(0, 1, &vbufBinding);
    cb->draw(vertices.size());

    cb->endPass();
}
```

### What This Gets You

The GPU path eliminates CPU rasterization from the hot loop. QPainter's text
rendering path goes through the font engine's glyph cache, but it still
blits each glyph to the backing store image on the CPU. With the QRhiWidget
approach, glyph rasterization only happens once (on cache miss), and all
per-frame work is a buffer upload + a single draw call.

For a text editor, the practical benefit is most visible during fast scrolling,
where the CPU path must re-render thousands of glyphs per frame. The GPU path
just updates the vertex buffer (new positions) and issues one draw call.

### What This Costs You

1. **You must reimplement text layout.** QPlainTextEdit delegates to
   QTextLayout, which produces QPainter draw commands. A QRhiWidget renderer
   needs its own layout engine (or a way to extract glyph positions from
   QTextLayout without the QPainter step).

2. **Subpixel rendering is harder.** LCD subpixel antialiasing (ClearType on
   Windows, RGB subpixel on Linux) requires a three-channel atlas (R, G, B
   coverage values) and a more complex fragment shader that reads the
   destination color. This is doable but not trivial, and the compositing
   model (QRhiWidget renders to an offscreen texture, not directly to the
   window surface) means you cannot read the final destination pixels at
   fragment shader time. Most GPU text renderers either use grayscale
   antialiasing or signed distance field rendering instead.

3. **The compositing overhead.** QRhiWidget output is composited with the
   raster backing store in a separate pass by
   `QBackingStoreDefaultCompositor`. This is one extra texture blit per
   frame. It's fast, but it's not zero.

4. **The vsync demotion.** As documented in `LOW_LATENCY_INPUT.md`,
   `QWidgetRepaintManager::sendUpdateRequest()` (qtbase
   `src/widgets/kernel/qwidgetrepaintmanager.cpp` ~line 342) demotes
   `UpdateNow` to `UpdateLater` when the widget tree contains
   `textureChildSeen` (which QRhiWidget sets). This means `repaint()` from
   `keyPressEvent()` no longer guarantees synchronous rendering. You would
   need to either patch this throttle or combine with Option C.

### QRhiWidget Frame Pacing

QRhiWidget does not use `QWindow::requestUpdate()` internally. It relies on
the normal widget update mechanism (`update()` posts a low-priority event,
eventually `paintEvent()` fires). Vsync throttling is implicit: the GPU
driver throttles `endFrame()` / presentation to the display refresh rate, and
the event loop doesn't spin faster than that in practice.

For continuous rendering (animation, cursor blink), call `update()` at the end
of `render()`. Qt compresses redundant update requests, so this produces one
paint per compositor frame, naturally vsync-paced.

For input-driven rendering (typing), the trigger is `keyPressEvent()` calling
`update()` on the QRhiWidget. Without Option C, this is still subject to the
low-priority event queue delay. The combination of Option B + Option C solves
this.

---

## Option C: VSync-Paced Frame Delivery via requestUpdate()

### The Problem with Qt's Default Widget Paint Scheduling

When a QWidget calls `update()`, the repaint manager posts a
`QEvent::UpdateRequest` at `Qt::LowEventPriority`. This means:

1. The event goes into the posted event queue behind everything else.
2. Other events (timers, socket notifiers, deferred deletes, zero-timers from
   Qt internals) are serviced first.
3. The paint eventually fires, with no relationship to the display's refresh
   cycle.

This scheduling policy makes sense for general-purpose GUI apps where
over-rendering wastes power. It does not make sense for a text editor where
the user is watching for their keystroke to appear.

### What requestUpdate() Does

`QWindow::requestUpdate()` is a method on `QWindow` (not `QWidget`) that
schedules a `QEvent::UpdateRequest` for delivery synchronized to the
display's refresh cycle. The Qt documentation says:

> "The event is delivered in sync with the display vsync on platforms where
> this is possible. Otherwise, the event is delivered after a delay of at
> most 5 ms."

It delegates to `QPlatformWindow::requestUpdate()`, which each platform
backend can override. The implementations:

#### Platform Implementations

**Default (all platforms):**
`QPlatformWindow::requestUpdate()` at
`qtbase/src/gui/kernel/qplatformwindow.cpp` ~line 762. Starts a
`QBasicTimer` with `Qt::PreciseTimer` and a 5ms interval, scaled for high
refresh rates:

```
Base interval: 5ms
For 120 Hz: 5 / (120/60) = 2.5ms
For 240 Hz: 5 / (240/60) = 1.25ms
```

The timer fires into `QPlatformWindow::windowEvent()`, which calls
`deliverUpdateRequest()`. This sends the `QEvent::UpdateRequest`
**synchronously** via `QCoreApplication::sendEvent()` (not `postEvent()`).

**Windows:**
`QWindowsWindow::requestUpdate()` at
`qtbase/src/plugins/platforms/windows/qwindowswindow.cpp` ~line 4034.
Integrates with `QDxgiVSyncService`, which runs a dedicated thread calling
`IDXGIOutput::WaitForVBlank()`. The callback fires at the exact display
vsync moment, delivers the update request to the GUI thread via
`QMetaObject::invokeMethod()`. Uses an atomic state machine
(Ready/Requested/Posted) to avoid redundant deliveries.

**macOS (Cocoa):**
`QCocoaWindow::requestUpdate()` at
`qtbase/src/plugins/platforms/cocoa/qcocoawindow.mm` ~line 1766. Uses
`CVDisplayLink` for vsync synchronization when the surface format has
a non-zero swap interval. Falls back to the default timer otherwise.

**Wayland:**
`QWaylandWindow::requestUpdate()` at
`qtbase/src/plugins/platforms/wayland/qwaylandwindow.cpp` ~line 1756. Uses
Wayland's native `wl_surface_frame()` callback, which fires when the
compositor is ready for a new frame. This is true compositor-synchronized
frame pacing.

**X11 (XCB):**
No override. Uses the default 5ms timer. X11 has no native vsync callback
mechanism. The 5ms heuristic gives the event loop idle time to gather
system events while still delivering updates promptly.

#### How deliverUpdateRequest() Delivers

`QPlatformWindow::deliverUpdateRequest()` at
`qtbase/src/gui/kernel/qplatformwindow.cpp` ~line 815:

```cpp
void QPlatformWindow::deliverUpdateRequest()
{
    Q_ASSERT(hasPendingUpdateRequest());
    QWindow *w = window();
    QWindowPrivate *wp = qt_window_private(w);
    wp->updateRequestPending = false;
    QEvent request(QEvent::UpdateRequest);
    QCoreApplication::sendEvent(w, &request);  // synchronous
}
```

The event is delivered synchronously to the QWindow. It does not go through
the posted event queue. It does not have low priority. It runs immediately
when the platform decides it's time (at vsync, or after the timer fires).

### The Bridge: QWidgetWindow

Every top-level QWidget has a corresponding `QWidgetWindow` (a QWindow
subclass) that bridges the QWindow and QWidget worlds. Its event handler for
`QEvent::UpdateRequest` at
`qtbase/src/widgets/kernel/qwidgetwindow.cpp` line 378:

```cpp
case QEvent::UpdateRequest:
    m_widget->repaint();
    return true;
```

It calls `repaint()`, which is the synchronous paint path. So the chain is:

```
QPlatformWindow::deliverUpdateRequest()   // vsync-timed, synchronous
  -> QWidgetWindow::event(UpdateRequest)  // synchronous dispatch
    -> m_widget->repaint()                // synchronous paint
      -> paintAndFlush()                  // paint + flush in one call
```

This is already wired up and working. The missing piece is getting the
widget repaint manager to call `requestUpdate()` instead of posting a
low-priority event.

### The Integration Point

`QWidgetRepaintManager::sendUpdateRequest()` at
`qtbase/src/widgets/kernel/qwidgetrepaintmanager.cpp` ~line 331 is the
single point where paint scheduling is decided. Currently:

```cpp
void QWidgetRepaintManager::sendUpdateRequest(QWidget *widget, UpdateTime updateTime)
{
    // ... vsync demotion logic for renderToTexture ...

    switch (updateTime) {
    case UpdateLater:
        updateRequestSent = true;
        QCoreApplication::postEvent(widget,
            new QEvent(QEvent::UpdateRequest),
            Qt::LowEventPriority);                // <-- THIS
        break;
    case UpdateNow:
        QEvent event(QEvent::UpdateRequest);
        QCoreApplication::sendEvent(widget, &event);
        break;
    }
}
```

The change is to replace the `UpdateLater` path with a call to
`requestUpdate()` on the top-level window's QWindow:

```cpp
case UpdateLater: {
    QWidget *tlw = widget->window();
    if (QWindow *win = tlw->windowHandle()) {
        win->requestUpdate();
    } else {
        // Fallback if no platform window yet
        QCoreApplication::postEvent(widget,
            new QEvent(QEvent::UpdateRequest),
            Qt::LowEventPriority);
    }
    break;
}
```

This routes the paint request through the platform's vsync-aware delivery
mechanism instead of the low-priority event queue.

### Redundancy Guards

Both the old and new paths have guards against redundant requests:

- **Old path:** `updateRequestSent` flag in `QWidgetRepaintManager` prevents
  posting multiple events. The event compressor in `QApplicationPrivate::compressEvent()`
  (qapplication.cpp ~line 790) also merges duplicate `UpdateRequest` events
  and unions dirty regions for `UpdateLater` events.

- **New path:** `QWindow::requestUpdate()` checks `updateRequestPending`
  (`qwindow_p.h:152`) and returns immediately if a request is already
  pending. The platform backends also guard against duplicates (e.g., the
  Windows DXGI path uses an atomic state machine).

You need to keep `updateRequestSent` in sync if other code in the repaint
manager checks it. Alternatively, replace its checks with
`win->d_func()->updateRequestPending`.

### What This Gets You

**On Windows:** True vsync-synchronized paint delivery. The DXGI service wakes
the GUI thread at the exact vertical blank interval.

**On macOS:** CVDisplayLink-synchronized delivery when the surface format
enables vsync (which is the default).

**On Wayland:** Compositor-synchronized delivery via frame callbacks. This is
actually the most correct behavior -- you paint exactly when the compositor
wants a new frame.

**On X11:** A 5ms timer with `Qt::PreciseTimer`. This is still better than the
old path: the old path uses `Qt::LowEventPriority`, which means the paint
event sits behind everything else in the queue. The 5ms timer fires with
normal priority and delivers synchronously. In practice, on a 60 Hz display
(16.67ms frame interval), a 5ms delay after dirtying is well within one
frame.

### What This Costs You

1. **Patching Qt.** This is a modification to
   `QWidgetRepaintManager::sendUpdateRequest()`, which is a private
   implementation detail. You are already forking QPlainTextEdit, so forking
   one more file is not a major architectural change. But it's a change to
   the widget paint scheduling path, which means you must be careful about
   interactions with the rest of the repaint manager's state.

2. **Latency floor.** Even on platforms with true vsync delivery, there is a
   minimum latency of "time until next vsync." On a 60 Hz display, if a
   keystroke arrives 1ms after a vsync, the next vsync is ~15.67ms away.
   Compare to the `repaint()` trick, which renders immediately (0ms
   scheduling delay, though the frame won't be scanned out until the next
   vsync regardless). So `requestUpdate()` adds scheduling latency up to one
   frame period, while `repaint()` adds zero scheduling latency but may
   produce a frame that sits in the backbuffer until the next vsync anyway.

3. **Interaction with repaint() on input.** If you implement both the
   `repaint()` trick from `LOW_LATENCY_INPUT.md` (synchronous paint on
   keyboard input) and the `requestUpdate()` path (vsync-paced paint for
   non-input-driven updates), you need to decide which path each update
   takes. The recommended design:

   - **Keyboard input path:** `keyPressEvent()` -> modify document ->
     `repaint()` (synchronous, immediate, no scheduling delay).
   - **Everything else** (scrollbar interaction, external document changes,
     resize, theme changes, timer-driven cursor blink): `update()` ->
     `requestUpdate()` path (vsync-paced).

   This gives you the best of both worlds: zero-delay rendering for typing,
   vsync-paced rendering for everything else.

### The Complete Flow with requestUpdate()

For a non-input update (e.g., cursor blink timer):

```
QTimer fires
  -> widget->update()
    -> QWidgetRepaintManager::markDirty(region, widget, UpdateLater)
      -> sendUpdateRequest()
        -> widget->window()->windowHandle()->requestUpdate()
          -> QPlatformWindow::requestUpdate()

[Platform delivers at vsync:]
  -> QPlatformWindow::deliverUpdateRequest()
    -> QCoreApplication::sendEvent(window, UpdateRequest)  // synchronous
      -> QWidgetWindow::event(UpdateRequest)
        -> m_widget->repaint()
          -> QWidgetRepaintManager::sync()
            -> paintAndFlush()
              -> paintEvent()
              -> flush()
```

For a keyboard input:

```
keyPressEvent()
  -> modify document
  -> viewport()->repaint()   // synchronous, bypasses requestUpdate entirely
    -> QWidgetRepaintManager::markDirty(region, widget, UpdateNow)
      -> sendUpdateRequest(widget, UpdateNow)
        -> QCoreApplication::sendEvent(widget, UpdateRequest)  // synchronous
          -> sync()
            -> paintAndFlush()
              -> paintEvent()
              -> flush()
```

---

## Combining Option B and Option C

The two options address different parts of the pipeline:

- **Option B** makes the paint itself faster (GPU rendering vs CPU
  rasterization).
- **Option C** makes the paint arrive sooner (vsync-paced vs low-priority
  deferred).

They compose naturally with one caveat. QRhiWidget sets
`renderToTexture = true` on the widget, which causes the repaint manager's
`textureChildSeen` flag to be set on the top-level window. This triggers the
vsync demotion logic in `sendUpdateRequest()` (line ~342), which demotes
`UpdateNow` to `UpdateLater` if the last compose was within one vsync period.

If you implement Option C, this demotion is less problematic because the
`UpdateLater` path now goes through `requestUpdate()` (vsync-paced delivery)
instead of a low-priority posted event. The demotion just means "deliver at
the next vsync" instead of "deliver whenever the event loop gets around to
it."

But for the keyboard input path, you want true synchronous rendering. You
can either:

1. **Remove the demotion logic.** It exists to prevent excessive compositing
   for renderToTexture widgets (the comment in the source says "Having every
   repaint() leading to a sync/flush is bad as it causes compositing and
   waiting for vsync each and every time"). For a text editor, one
   synchronous composite per keystroke is acceptable.

2. **Bypass the repaint manager for keyboard input.** Call
   `QRhiWidget::render()` directly from `keyPressEvent()` and manually
   trigger a flush. This is fragile and not recommended.

3. **Separate the text viewport update from the top-level widget update.**
   The QRhiWidget's `update()` call is what marks the renderToTexture widget
   dirty. If you can arrange for the immediate input path to go through
   `repaint()` on the QRhiWidget itself (not on the top-level window), the
   repaint manager will use the `UpdateNow` path for the widget, and the
   compositor will run synchronously.

Option 1 is the simplest. The demotion logic is a performance heuristic, not
a correctness requirement.

---

## Decision Framework

| Consideration | Option B (GPU rendering) | Option C (requestUpdate) |
|---|---|---|
| **Implementation effort** | Large: glyph atlas, shaders, layout engine | Small: one function in the repaint manager |
| **Benefit for typing latency** | Indirect (faster paint, same scheduling) | Direct (better scheduling) |
| **Benefit for scrolling** | Large (GPU draws thousands of glyphs in one call) | Moderate (vsync-paced but still CPU-rendered) |
| **Qt patch surface** | None if keeping forked widget only | One method in QWidgetRepaintManager |
| **Platform dependency** | QRhi abstracts backends | Depends on platform vsync support |
| **Risk** | Subpixel rendering loss, compositing overhead | Minimal; well-tested code path |

For typing latency alone, Option C combined with the `repaint()` trick from
`LOW_LATENCY_INPUT.md` is sufficient and low-risk. Option B becomes
compelling when scrolling performance matters or when the editor must handle
very large visible areas (tens of thousands of glyphs on screen).

---

## Reference

### QRhiWidget

- Header: `qtbase/src/widgets/kernel/qrhiwidget.h`
- Implementation: `qtbase/src/widgets/kernel/qrhiwidget.cpp`
  - `ensureRhi()` ~line 489
  - `ensureTexture()` ~line 536
  - `invokeInitialize()` ~line 648
- Private: `qtbase/src/widgets/kernel/qrhiwidget_p.h`
- Simple example: `qtbase/examples/widgets/rhi/simplerhiwidget/`
- Cube example: `qtbase/examples/widgets/rhi/cuberhiwidget/`

### QRhi

- Header: `qtbase/src/gui/rhi/qrhi.h`
  - Texture formats: ~line 928
  - Texture upload API: ~line 661
  - Buffer API: ~line 856
  - Pipeline API: ~line 1310
  - Resource limits: ~line 1987
- Implementation: `qtbase/src/gui/rhi/qrhi.cpp`
  - Atlas upload documentation: ~line 3198

### QWindow::requestUpdate()

- Implementation: `qtbase/src/gui/kernel/qwindow.cpp` ~line 2878
- Default timer: `qtbase/src/gui/kernel/qplatformwindow.cpp` ~line 762
- Delivery: `qtbase/src/gui/kernel/qplatformwindow.cpp` ~line 815
- Windows DXGI: `qtbase/src/plugins/platforms/windows/qwindowswindow.cpp` ~line 4034
- macOS CVDisplayLink: `qtbase/src/plugins/platforms/cocoa/qcocoawindow.mm` ~line 1766
- Wayland frame callbacks: `qtbase/src/plugins/platforms/wayland/qwaylandwindow.cpp` ~line 1756

### Widget Repaint Manager

- Implementation: `qtbase/src/widgets/kernel/qwidgetrepaintmanager.cpp`
  - `sendUpdateRequest()` ~line 331 (the integration point for Option C)
  - `paintAndFlush()` ~line 709
- Header: `qtbase/src/widgets/kernel/qwidgetrepaintmanager_p.h`

### QWidgetWindow

- Event handling: `qtbase/src/widgets/kernel/qwidgetwindow.cpp` ~line 378
