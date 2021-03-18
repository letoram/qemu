/*
 * QEMU Arcan-shmif display driver
 *
 * Copyright (c) 2016-2021 Bjorn Stahl
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qemu/cutils.h"
#include "qemu/config-file.h"
#include "qemu/typedefs.h"
#include "ui/console.h"
#include "ui/input.h"

#include "ui/egl-context.h"
#include "sysemu/sysemu.h"
#include "sysemu/runstate.h"

/*
 * TODO:
 * [ ] Audio support, set shmif_primary as an accessor to the display if necessary
 *     build from audio/sdlaudio.c as they are mostly similar
 * [ ] VirGL / dma-buf style forwarding, see gtk-egl.c
 * [ ] Map virtio-VGA framebuffer directly into SHMIF segment if formats match (big saving)
 * [ ] (Linux host) Switch input from SDL2+conv to subid-as-linux-keycode
 * [ ] State controls (load / save)
 * [ ] Text console to TUI
 * [ ] Plugging in multiple displays as secondary segments
 * [ ] Resize propagation [ dpy_set_ui_info() ]
 * [ ] Hardware mouse cursor path [ SEGREQ(CURSOR) ]
 * [ ] SHMIF proxying into guest [ handover alloc, semaphores being a real PITA ]
 * [ ] DEBUG segment handler for memory inspection
 * [ ] SEGID_ICON request and if OK, raster icons/qemu.svg into it
 * [ ] Expose OUTPUT segment (at least audio-out)
 */

#define WANT_ARCAN_SHMIF_HELPER
#include <arcan_shmif.h>

enum blitmode {
    BLIT_SHARE, /* try and share vidp with underlying layer directly */
    BLIT_DIRECT, /* format-in == format-out, direct memcpy */
    BLIT_REPACK, /* format-in != format-out, unpack-pack */
#ifdef CONFIG_OPENGL
    BLIT_TXPACK
#endif
};

struct dpy_state {
/* for drawing */
    struct arcan_shmif_cont dpy;
    enum blitmode mode;
    DisplaySurface *surface;
    struct PixelFormat fmt;
    size_t bpp, w, h;
    uint8_t* ptr;
    bool hpass_disable;
    bool hidden;

/* track pressed inputs and reset on lost focus */
    size_t index;
    uint8_t kbd_statetbl[256];

/* for input state */
    int mx, my;

    DisplayChangeListener dcl;
};

#ifndef ARCAN_DISPLAY_LIMIT
#define ARCAN_DISPLAY_LIMIT 4
#endif

static void update_display_titles(void);

/* even though everthing is contained here, keep a static tracking
 * table as well for the "for all x" cases */
static struct {
    struct dpy_state dpy[ARCAN_DISPLAY_LIMIT];
    int vbufc, abufc;
    int ledstate;
    bool gl;
    size_t abuf_sz;
    size_t n_dpy;
#ifdef CONFIG_OPENGL
#endif
} arcan_ctx = {
    .vbufc = 1,
    .abufc = 8,
    .abuf_sz = 4096
};

/* grabbed from input-linux.c
 * this is slightly incorrect as we have:
 * subid - whatever the I/O layer gets us
 * keycode - OS specific keycode if it can be found
 * keysym - SDL1.2 keysym
 * -- since this is primarily used for linux where we [should] have egl-dri
 * platform driver, linux to qcode makes the most sense though this should
 * probably match compile- time probing
 */
#include "standard-headers/linux/input.h"
static int xlate_lut[65536] = {
    [KEY_ESC]            = Q_KEY_CODE_ESC,
    [KEY_1]              = Q_KEY_CODE_1,
    [KEY_2]              = Q_KEY_CODE_2,
    [KEY_3]              = Q_KEY_CODE_3,
    [KEY_4]              = Q_KEY_CODE_4,
    [KEY_5]              = Q_KEY_CODE_5,
    [KEY_6]              = Q_KEY_CODE_6,
    [KEY_7]              = Q_KEY_CODE_7,
    [KEY_8]              = Q_KEY_CODE_8,
    [KEY_9]              = Q_KEY_CODE_9,
    [KEY_0]              = Q_KEY_CODE_0,
    [KEY_MINUS]          = Q_KEY_CODE_MINUS,
    [KEY_EQUAL]          = Q_KEY_CODE_EQUAL,
    [KEY_BACKSPACE]      = Q_KEY_CODE_BACKSPACE,
    [KEY_TAB]            = Q_KEY_CODE_TAB,
    [KEY_Q]              = Q_KEY_CODE_Q,
    [KEY_W]              = Q_KEY_CODE_W,
    [KEY_E]              = Q_KEY_CODE_E,
    [KEY_R]              = Q_KEY_CODE_R,
    [KEY_T]              = Q_KEY_CODE_T,
    [KEY_Y]              = Q_KEY_CODE_Y,
    [KEY_U]              = Q_KEY_CODE_U,
    [KEY_I]              = Q_KEY_CODE_I,
    [KEY_O]              = Q_KEY_CODE_O,
    [KEY_P]              = Q_KEY_CODE_P,
    [KEY_LEFTBRACE]      = Q_KEY_CODE_BRACKET_LEFT,
    [KEY_RIGHTBRACE]     = Q_KEY_CODE_BRACKET_RIGHT,
    [KEY_ENTER]          = Q_KEY_CODE_RET,
    [KEY_LEFTCTRL]       = Q_KEY_CODE_CTRL,
    [KEY_A]              = Q_KEY_CODE_A,
    [KEY_S]              = Q_KEY_CODE_S,
    [KEY_D]              = Q_KEY_CODE_D,
    [KEY_F]              = Q_KEY_CODE_F,
    [KEY_G]              = Q_KEY_CODE_G,
    [KEY_H]              = Q_KEY_CODE_H,
    [KEY_J]              = Q_KEY_CODE_J,
    [KEY_K]              = Q_KEY_CODE_K,
    [KEY_L]              = Q_KEY_CODE_L,
    [KEY_SEMICOLON]      = Q_KEY_CODE_SEMICOLON,
    [KEY_APOSTROPHE]     = Q_KEY_CODE_APOSTROPHE,
    [KEY_GRAVE]          = Q_KEY_CODE_GRAVE_ACCENT,
    [KEY_LEFTSHIFT]      = Q_KEY_CODE_SHIFT,
    [KEY_BACKSLASH]      = Q_KEY_CODE_BACKSLASH,
    [KEY_102ND]          = Q_KEY_CODE_LESS,
    [KEY_Z]              = Q_KEY_CODE_Z,
    [KEY_X]              = Q_KEY_CODE_X,
    [KEY_C]              = Q_KEY_CODE_C,
    [KEY_V]              = Q_KEY_CODE_V,
    [KEY_B]              = Q_KEY_CODE_B,
    [KEY_N]              = Q_KEY_CODE_N,
    [KEY_M]              = Q_KEY_CODE_M,
    [KEY_COMMA]          = Q_KEY_CODE_COMMA,
    [KEY_DOT]            = Q_KEY_CODE_DOT,
    [KEY_SLASH]          = Q_KEY_CODE_SLASH,
    [KEY_RIGHTSHIFT]     = Q_KEY_CODE_SHIFT_R,
    [KEY_LEFTALT]        = Q_KEY_CODE_ALT,
    [KEY_SPACE]          = Q_KEY_CODE_SPC,
    [KEY_CAPSLOCK]       = Q_KEY_CODE_CAPS_LOCK,
    [KEY_F1]             = Q_KEY_CODE_F1,
    [KEY_F2]             = Q_KEY_CODE_F2,
    [KEY_F3]             = Q_KEY_CODE_F3,
    [KEY_F4]             = Q_KEY_CODE_F4,
    [KEY_F5]             = Q_KEY_CODE_F5,
    [KEY_F6]             = Q_KEY_CODE_F6,
    [KEY_F7]             = Q_KEY_CODE_F7,
    [KEY_F8]             = Q_KEY_CODE_F8,
    [KEY_F9]             = Q_KEY_CODE_F9,
    [KEY_F10]            = Q_KEY_CODE_F10,
    [KEY_NUMLOCK]        = Q_KEY_CODE_NUM_LOCK,
    [KEY_SCROLLLOCK]     = Q_KEY_CODE_SCROLL_LOCK,
    [KEY_KP0]            = Q_KEY_CODE_KP_0,
    [KEY_KP1]            = Q_KEY_CODE_KP_1,
    [KEY_KP2]            = Q_KEY_CODE_KP_2,
    [KEY_KP3]            = Q_KEY_CODE_KP_3,
    [KEY_KP4]            = Q_KEY_CODE_KP_4,
    [KEY_KP5]            = Q_KEY_CODE_KP_5,
    [KEY_KP6]            = Q_KEY_CODE_KP_6,
    [KEY_KP7]            = Q_KEY_CODE_KP_7,
    [KEY_KP8]            = Q_KEY_CODE_KP_8,
    [KEY_KP9]            = Q_KEY_CODE_KP_9,
    [KEY_KPMINUS]        = Q_KEY_CODE_KP_SUBTRACT,
    [KEY_KPPLUS]         = Q_KEY_CODE_KP_ADD,
    [KEY_KPDOT]          = Q_KEY_CODE_KP_DECIMAL,
    [KEY_KPENTER]        = Q_KEY_CODE_KP_ENTER,
    [KEY_KPSLASH]        = Q_KEY_CODE_KP_DIVIDE,
    [KEY_KPASTERISK]     = Q_KEY_CODE_KP_MULTIPLY,
    [KEY_F11]            = Q_KEY_CODE_F11,
    [KEY_F12]            = Q_KEY_CODE_F12,
    [KEY_RIGHTCTRL]      = Q_KEY_CODE_CTRL_R,
    [KEY_SYSRQ]          = Q_KEY_CODE_SYSRQ,
    [KEY_RIGHTALT]       = Q_KEY_CODE_ALT_R,
    [KEY_HOME]           = Q_KEY_CODE_HOME,
    [KEY_UP]             = Q_KEY_CODE_UP,
    [KEY_PAGEUP]         = Q_KEY_CODE_PGUP,
    [KEY_LEFT]           = Q_KEY_CODE_LEFT,
    [KEY_RIGHT]          = Q_KEY_CODE_RIGHT,
    [KEY_END]            = Q_KEY_CODE_END,
    [KEY_DOWN]           = Q_KEY_CODE_DOWN,
    [KEY_PAGEDOWN]       = Q_KEY_CODE_PGDN,
    [KEY_INSERT]         = Q_KEY_CODE_INSERT,
    [KEY_DELETE]         = Q_KEY_CODE_DELETE,
    [KEY_LEFTMETA]       = Q_KEY_CODE_META_L,
    [KEY_RIGHTMETA]      = Q_KEY_CODE_META_R,
    [KEY_MENU]           = Q_KEY_CODE_MENU,
};

static void arcan_update(DisplayChangeListener *dcl,
                         int x, int y, int w, int h)
{
    struct dpy_state *dst = container_of(dcl, struct dpy_state, dcl);
    struct arcan_shmif_cont *dpy = &dst->dpy;

/* if we favor latency instead of bandwidth */
    if (arcan_ctx.vbufc > 1){
        x = 0;
        y = 0;
        w = dpy->w;
        h = dpy->h;
    }

/* shouldn't need a color-space conversion */
    switch(dst->mode){
    case BLIT_SHARE:
/* should be possible with qemu_create_displaysurface_from and then somehow
 * switch out the old one (or simply patch it), but we need to avoid the buffer
 * being free()d or changed somehow */
    break;
    case BLIT_DIRECT:
    case BLIT_REPACK:{
    shmif_pixel *dp = &dpy->vidp[y * dpy->pitch];
    pixman_image_t *src = dst->surface->image;
    size_t stride = pixman_image_get_stride(src);

/* we are expecting BGRx, hence *4 */
    uint8_t *sp = (uint8_t*)pixman_image_get_data(src) + y * stride + x * 4;
    for (size_t cy = 0; cy < h; cy++){
        for (size_t cx = 0; cx < w; cx++){
            uint8_t r = sp[cx*4+2];
            uint8_t g = sp[cx*4+1];
            uint8_t b = sp[cx*4+0];
            uint8_t a = 255;
            dp[cx] = SHMIF_RGBA(r, g, b, a);
        }
        sp += stride;
        dp += dpy->pitch;
    }
    break;
    }
/* in this mode, we do the texture packing ourselves using a litle trick,
 * i.e. the shmifext_signal uses cont->vidp as basis for packing into texture
 */
#ifdef CONFIG_OPENGL
    case BLIT_TXPACK:

    break;
#endif
    }

/*
 * set the actual dirty region, if vbufc > 1 this will always be full display
 * when vbufc > 1 due to the internal double/triple/... buffering */
    dpy->dirty.x1 = x;
    dpy->dirty.x2 = x + w;
    dpy->dirty.y1 = y;
    dpy->dirty.y2 = y + h;
    arcan_shmif_signal(dpy, SHMIF_SIGVID);
}

#ifdef CONFIG_OPENGL
static unsigned context_mask;
static QEMUGLContext arcan_egl_create_context(DisplayChangeListener *dcl,
                                                         QEMUGLParams *params)
{
    struct dpy_state *dst = container_of(dcl, struct dpy_state, dcl);
    struct arcan_shmifext_setup defs = arcan_shmifext_defaults(&dst->dpy);
    defs.major = params->major_ver;
    defs.minor = params->minor_ver;
    defs.builtin_fbo = false;
/* FIXME: populate defs from "qemu_egl_config" */
    uintptr_t i = 0;
    for (i = 0; i < 64; i++)
      if (!(context_mask & (1 << i))){
        context_mask = 1 << i;
        break;
      }
    arcan_shmifext_setup(&dst->dpy, defs);

    return (QEMUGLContext) i;
}

static void arcan_gl_scanout_disable(DisplayChangeListener *dcl)
{
}

static void arcan_gl_scanout_texture(DisplayChangeListener *dcl,
                                  uint32_t tex_id,
                                  bool y_0_top,
                                  uint32_t backing_width,
                                  uint32_t backing_height,
                                  uint32_t x, uint32_t y,
                                  uint32_t w, uint32_t h)
{
    struct dpy_state *dst = container_of(dcl, struct dpy_state, dcl);
    struct arcan_shmif_cont *dpy = &dst->dpy;

    dpy->dirty.x1 = x;
    dpy->dirty.x2 = x + w;
    dpy->dirty.y1 = y;
    dpy->dirty.y2 = y + h;

    if (context_mask)
        arcan_shmifext_signal(dpy, 0, SHMIF_SIGVID, tex_id);
}

static void arcan_egl_destroy_context(DisplayChangeListener *dcl, QEMUGLContext ctx)
{
    struct dpy_state *dst = container_of(dcl, struct dpy_state, dcl);
    arcan_shmifext_drop_context(&dst->dpy);
    context_mask &= ~(1 << (uintptr_t)ctx);
}

static int arcan_egl_make_context_current(DisplayChangeListener *dcl,
                                 QEMUGLContext ctx)
{
    struct dpy_state *dst = container_of(dcl, struct dpy_state, dcl);
    return arcan_shmifext_make_current(&dst->dpy);
}

/*
 * Because https://github.com/qemu/qemu/commit/c110d949b8166a633179edcf3390a42673ac843c
 *
static QEMUGLContext arcan_egl_get_current_context(DisplayChangeListener *dcl)
{
    struct dpy_state *dst = container_of(dcl, struct dpy_state, dcl);
    uintptr_t context = 0;
    arcan_shmifext_egl_meta(&dst->dpy, NULL, NULL, &context);
    return (QEMUGLContext) context;
}
 */

static void arcan_gl_update(DisplayChangeListener* dcl,
                                uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h)
{
 /*
  * struct dpy_state *dst = container_of(dcl, struct dpy_state, dcl);
    struct arcan_shmif_cont *dpy = &dst->dpy;
  */
/* sdl just goes BindFramebuffer, GetWindowSize, Viewport, Blit, Bind, Swap
 * qemu does qemu_spice_glblock, spice_qxl_gl_draw_async */
}
#endif

static void reset_dpykbd(struct dpy_state* dpy)
{
    for (int i = 0; i < 256; i++){
        if (dpy->kbd_statetbl[i]){
            qemu_input_event_send_key_qcode(NULL, xlate_lut[i], 0);
            dpy->kbd_statetbl[i] = 0;
        }
    }
}

static bool input_event(DisplayChangeListener *dcl, struct dpy_state* dpy,
                        struct arcan_shmif_cont* con, arcan_ioevent *iev)
{
/* we could use this to provide a new usb device, character device and so
 * on */
    if (iev->devkind != EVENT_IDEVKIND_KEYBOARD &&
        iev->devkind != EVENT_IDEVKIND_MOUSE)
        return false;

/* FIXME: this has changed - if we can know it is a linux keycode etc. there
 * is automatic translation / interpreation / forwarding now through the use
 * of qemu_input_linux_to_keycode and then qemu_input_key_number_to_qcode.
 *
 * Due to changes practically imposed by the wayland bridge, we can assume
 * that even for SDL2 (the other possible source) the subid will be a linux
 * keycode. The situation is slightly different on BSDs.
 */
    if (iev->datatype == EVENT_IDATATYPE_TRANSLATED){
        dpy->kbd_statetbl[iev->input.translated.scancode] =
                                                iev->input.translated.active;
        qemu_input_event_send_key_qcode(NULL,
                                   xlate_lut[iev->input.translated.scancode],
                                   iev->input.translated.active);
    }
    else if (iev->datatype == EVENT_IDATATYPE_DIGITAL){
        int btnout = 0;
        switch (iev->subid){
        case MBTN_LEFT_IND: btnout = INPUT_BUTTON_LEFT; break;
        case MBTN_MIDDLE_IND: btnout = INPUT_BUTTON_MIDDLE; break;
        case MBTN_RIGHT_IND: btnout = INPUT_BUTTON_RIGHT; break;
        case MBTN_WHEEL_UP_IND: btnout = INPUT_BUTTON_WHEEL_UP; break;
        case MBTN_WHEEL_DOWN_IND: btnout = INPUT_BUTTON_WHEEL_DOWN; break;
        default:
            return false;
        break;
        }
        qemu_input_queue_btn(dcl->con, btnout, iev->input.digital.active);
        return true;
    }
    else if (iev->datatype == EVENT_IDATATYPE_ANALOG){
        int did = iev->subid == 0 ? INPUT_AXIS_X : INPUT_AXIS_Y;
        int av = iev->input.analog.axisval[0];
        if (iev->input.analog.gotrel){
            qemu_input_queue_rel(dcl->con, did, av);
        }
        else
            qemu_input_queue_abs(dcl->con, did, av, 0,
                                 iev->subid==0 ? con->w : con->h);
        return true;
    }
/* open question: how to map tablet input, IDATATYPE_TOUCH and other
 * game- devices? is there an existing joystick interface or do we have to
 * create our own virtual one? */
    return false;
}

static void system_event(DisplayChangeListener *dcl, struct dpy_state *dpy,
                         struct arcan_shmif_cont* con, arcan_tgtevent *iev)
{
    struct dpy_state *dst = container_of(dcl, struct dpy_state, dcl);
    struct arcan_shmif_cont *acon = &dst->dpy;

    switch (iev->kind){
    case TARGET_COMMAND_EXIT:
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
    break;
    case TARGET_COMMAND_RESET:
        switch(iev->ioevs[0].iv){
        case 0:
        case 1:
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_RESET);
        break;
        case 2:
        case 3:
/* re- query for clipboard, mouse cursor, output segment, ... */
        break;
        }

    /* send a new complete frame immediately as this might come from migration
     * where the other end won't create local resources until new contents has
      * arrived, while the guest might not update until there is activity */
        acon->dirty.x1 = 0;
        acon->dirty.y1 = 0;
        acon->dirty.x2 = acon->w;
        acon->dirty.y2 = acon->h;
        arcan_shmif_signal(acon, SHMIF_SIGVID);
    break;
    case TARGET_COMMAND_NEWSEGMENT:
/* Check ID for requested display or special (clipboard, mouse cursor),
 * if it's an output segment, set it in the primary slot so we can use
 * it as an audio source and as an emulated video capture device */
    break;
    case TARGET_COMMAND_PAUSE:
        if (runstate_is_running()){}
             /* qmp_stop? sweep vidp and greyscale? */
    break;
    case TARGET_COMMAND_UNPAUSE:
        if (!runstate_is_running()){}
             /* qmp_disable? */
    break;
    case TARGET_COMMAND_SETIODEV:
/* no real defined behavior here unless we start supporting joystick hotplug */
    break;
    case TARGET_COMMAND_STORE:
/* use descriptor and try to save snapshot,
 * black-and-white current buffer and draw progression into it, becomes more
 * annoying with GL as we would have to switch to readback temporarily */
    break;
    case TARGET_COMMAND_RESTORE:
/* use descriptor and try to restore from snapshot */
    break;
    case TARGET_COMMAND_DISPLAYHINT:
        if (!(iev->ioevs[2].iv & 128)){
            if (iev->ioevs[2].iv & 2){ /* invisible */
                update_displaychangelistener(dcl, 500);
                dpy->hidden = true;
            }
            else if (dpy->hidden){
                update_displaychangelistener(dcl, GUI_REFRESH_INTERVAL_DEFAULT);
                dpy->hidden = false;
            }
            if (iev->ioevs[2].iv & 4){ /* no focus */
                reset_dpykbd(dpy);
            }
        }
/* something to request / initiate window resize? */
    break;
    case TARGET_COMMAND_OUTPUTHINT:
/* FIXME: update refresh rate */
    break;
    case TARGET_COMMAND_DEVICE_NODE:
/*
 * active rendernode migration in 3D, there seem to be qemu mechanisms in
 * place for handling that
 */
    break;
    default:
    break;
    }
}

static void arcan_refresh(DisplayChangeListener *dcl)
{
/* flush input event loop */
    struct dpy_state *dpy = container_of(dcl, struct dpy_state, dcl);
    struct arcan_shmif_cont *con = &dpy->dpy;

    graphic_hw_update(NULL);

    arcan_event ev;
    bool queue_flush = false;

    while(arcan_shmif_poll(con, &ev) > 0){
    if (ev.category == EVENT_IO)
        queue_flush |= input_event(dcl, dpy, con, &ev.io);
    else if (ev.category == EVENT_TARGET)
        system_event(dcl, dpy, con, &ev.tgt);
    }

    if (queue_flush)
        qemu_input_event_sync();


/* runstate_is_running may change between invocations here */
}

static void arcan_switch(DisplayChangeListener *dcl,
                         DisplaySurface *new_surface)
{
    struct dpy_state *dst = container_of(dcl, struct dpy_state, dcl);
    struct arcan_shmif_cont *dpy = &dst->dpy;

/* FIXME:
 * toggle back and forth between tpack format here
 */

    if (dpy->addr){
        dpy->hints = SHMIF_RHINT_SUBREGION | SHMIF_RHINT_IGNORE_ALPHA;
#ifdef CONFIG_OPENGL
        if (arcan_ctx.gl){
          dpy->hints |= SHMIF_RHINT_ORIGO_UL;
        }
#endif
        struct shmif_resize_ext ext = {
            .vbuf_cnt = arcan_ctx.vbufc,
            .abuf_cnt = arcan_ctx.abufc,
            .abuf_sz = arcan_ctx.abuf_sz
        };

        arcan_shmif_lock(&dst->dpy);
        arcan_shmif_resize_ext(dpy, surface_width(new_surface),
                           surface_height(new_surface), ext);
        arcan_shmif_unlock(&dst->dpy);
    }

    if (new_surface){
/* FIXME: compare color space with the segment native, if they match, run with
 * BLIT_DIRECT rather than repack (or even SHARED if we figure that one out)
 * for BLIT_DIRECT we may get away with doing GL_RGB on arcan-side then with
 * qemu_create_displaysurface_from pointing to our vidp (can only do with
 * vbufc = 1), is_surface_bgr is also useful, something with is_buffer_shared,
 *
 */
        dst->ptr = surface_data(new_surface);
        dst->bpp = surface_bits_per_pixel(new_surface);
        dst->fmt = qemu_pixelformat_from_pixman(new_surface->format);
        dst->surface = new_surface;
        dst->mode = BLIT_REPACK;
    }
}

static bool arcan_check_format(DisplayChangeListener *dcl,
                               pixman_format_code_t format)
{
/* FIXME: this is not correct in regards to shmif, we should either reject
 * all "non-platform-default" formats or provide a swizzle- flag in
 * shmif */
    return format == PIXMAN_b8g8r8x8 || format == PIXMAN_b8g8r8a8
        || format == PIXMAN_x8r8g8b8 || format == PIXMAN_a8r8g8b8;
}

static void arcan_mouse_warp(DisplayChangeListener *dcl,
                           int x, int y, int on)
{
/* FIXME: if we have a dedicated cursor segment, run the events on
    arcan_shmif_enqueue(dpy, &(arcan_event){
        .category = EVENT_EXTERNAL,
        .ext.kind = ARCAN_EVENT(CURSORINPUT),
        .ext.cursor.id = 0,
        .ext.cursor.x = x,
        .ext.cursor.y = y
    });
*/
/* FIXME: cursorhint */
}

static void arcan_mouse_define(DisplayChangeListener *dcl,
                               QEMUCursor *c)
{
}

static void arcan_kbd_leds(void *opaque, int state)
{
    arcan_ctx.ledstate = state;
    update_display_titles();
}

static void arcan_vmstate_chg(void *opaque, int running, RunState state)
{
    update_display_titles();
}

/*
static void arcan_text_update(DisplayChangeListener *dcl,
                              int x, int y, int w, int h)
{
    printf("text update: %d, %d, %d * %d\n", x, y, w, h);
}

static void arcan_text_cursor(DisplayChangeListener *dcl,
		                          int x, int y)
{
	printf("move to %d, %d\n", x, y);
}

static void arcan_text_resize(DisplayChangeListener *dcl,
		                          int w, int h)
{
	printf("rext to  %d, %d\n", w, h);
}
 */

static const DisplayChangeListenerOps dcl_ops = {
    .dpy_name             = "arcan",
    .dpy_gfx_update       = arcan_update,
    .dpy_gfx_switch       = arcan_switch,
    .dpy_gfx_check_format = arcan_check_format,
    .dpy_refresh          = arcan_refresh,
    .dpy_mouse_set        = arcan_mouse_warp,
    .dpy_cursor_define    = arcan_mouse_define
/*
 *  .dpy_text_resize      = arcan_text_resize,
    .dpy_text_update      = arcan_text_update,
    .dpy_text_cursor      = arcan_text_cursor
 */
#ifdef CONFIG_OPENGL
		,
    .dpy_gl_ctx_make_current = arcan_egl_make_context_current,
/*    .dpy_gl_ctx_get_current  = arcan_egl_get_current_context, */
    .dpy_gl_ctx_create   = arcan_egl_create_context,
    .dpy_gl_ctx_destroy  = arcan_egl_destroy_context,
    .dpy_gl_scanout_texture = arcan_gl_scanout_texture,
    .dpy_gl_scanout_disable = arcan_gl_scanout_disable,
    .dpy_gl_update       = arcan_gl_update,
/* new ones:
 *  .dpy_gl_cursor_position,
 *  .dpy_gl_release_dmabuf,
 *  .dpy_gl_update,
 *  .dpy_gl_has_dmabuf,
 *  .dpy_gl_scanout_dmabuf,
 *  .dpy_gl_cursor_dmabuf,
 *  .dpy_gl_cursor_position
 */
#endif
};

static int wait_for_subseg(struct arcan_shmif_cont *cont)
{
/* request a subsegment to use for 2nd, 3rd, ... console */
    return 0;
}

static void update_display_titles(void)
{
/* for emoji- titles:
    char scroll_lock[] = {0xe2, 0xa4, 0x93, 0x00};
    char num_lock[] = {0xe2, 0x87, 0xad, 0x00};
    char caps_lock[] = {0xe2, 0x87, 0xaa, 0x00};
 */
    const char* scroll_lock = "S";
    const char* caps_lock = "C";
    const char* num_lock = "N";

    for (int i = 0; i < ARCAN_DISPLAY_LIMIT; i++){
        if (!arcan_ctx.dpy[i].dpy.vidp)
            continue;

        struct arcan_event ev = {
            .ext.kind = ARCAN_EVENT(IDENT)
        };

        size_t lim = sizeof(ev.ext.message.data) /
                     sizeof(ev.ext.message.data[0]);

        snprintf((char*)ev.ext.message.data,
               lim, "QEMU[%d][%s%s%s]:%s(%s)", i,
               (arcan_ctx.ledstate & QEMU_SCROLL_LOCK_LED) ? scroll_lock : "",
               (arcan_ctx.ledstate & QEMU_NUM_LOCK_LED) ? num_lock : "",
               (arcan_ctx.ledstate & QEMU_CAPS_LOCK_LED) ? caps_lock : "",
               qemu_name ? qemu_name : "",
               runstate_is_running() ? "Running" : "Suspended"
        );
        arcan_shmif_enqueue(&arcan_ctx.dpy[i].dpy, &ev);
    }
}

static void arcan_display_early_init(DisplayOptions *o)
{
    assert(o->type == DISPLAY_TYPE_ARCAN);
    if (o->has_gl && o->gl){
#ifdef CONFIG_OPENGL
        display_opengl = 1;
#endif
    }
}

static void arcan_display_init(DisplayState *ds, DisplayOptions *o)
{
    struct arg_arr* args;
/*
 * Though audio/ video shouldn't rely on initialization order, we silently
 * nop audio until the primary segment can be accessed so we don't have to
 * have the connect/init setup available on both sides.
 */
    struct arcan_shmif_cont prim = arcan_shmif_open(SEGID_VM,
                                                    SHMIF_ACQUIRE_FATALFAIL,
                                                    &args);
    prim.hints = SHMIF_RHINT_SUBREGION;
    arcan_shmif_setprimary(SHMIF_INPUT, &prim);

/* FIXME:
 * there is an embeddable io/icons/qemu.svg that we could translate / draw
 * into an icon segment here (so request SEGID_ICON, if OK - draw into it)
 */

/* FIXME:
 * we can also send a custom cursor segment request and attach that to the
 * display structure in order for the pointer to be correct
 */
    arcan_shmif_enqueue(&prim, &(arcan_event){
        .category = EVENT_EXTERNAL,
        .ext.kind = ARCAN_EVENT(CURSORHINT),
        .ext.message.data = "hidden"
    });

    size_t nd;
    for (nd = 0;; nd++){
        QemuConsole *cons = qemu_console_lookup_by_index(nd);
        if (!cons)
            break;

/*
 * We support non-graphical consoles when we have TUI finished, another
 * possibility is to forward that connection to afsrv_terminal and have
 * it run the state machine as well. Should only need a mechanism to set
 * the descriptor to be used for input..
 */
        if (!qemu_console_is_graphic(cons)){
            continue;
        }

/* ask for a valid subwindow to assign it to */
        struct dpy_state *disp = &arcan_ctx.dpy[nd];
        disp->index = nd;
        if (nd > 0){
            int rc = wait_for_subseg(&prim);
            if (rc == -1){
                arcan_shmif_drop(&prim);
                return;
            }
            else if (rc == 0)
                break;
            disp->dpy = arcan_shmif_acquire(&prim, NULL, SEGID_VM, 0);
        }
        else
            disp->dpy = prim;
        DisplayChangeListener *dcl = g_new0(DisplayChangeListener, 1);
        if (!dcl){
            arcan_shmif_drop(&disp->dpy);
            if (nd == 0)
                return;
            else
                break;
        }
        else
            disp->dcl = *dcl;

        disp->dcl.con = cons;
#ifdef CONFIG_OPENGL
        if (display_opengl){
          struct arcan_shmifext_setup defs = arcan_shmifext_defaults(&disp->dpy);
          defs.builtin_fbo = false;
          arcan_shmifext_setup(&disp->dpy, defs);

          arcan_ctx.gl = true;
        }
#endif
        disp->dcl.ops = &dcl_ops;

/* this will likely invalidate prim, don't use it after this point */
        register_displaychangelistener(&disp->dcl);
    }
    arcan_ctx.n_dpy = nd;

    const char* argstr;
    if (arg_lookup(args, "vbufc", 0, &argstr))
        arcan_ctx.vbufc = strtoul(argstr, NULL, 10);

    if (arg_lookup(args, "abufc", 0, &argstr))
        arcan_ctx.abufc = strtoul(argstr, NULL, 10);

    if (arg_lookup(args, "abuf_sz", 0, &argstr))
        arcan_ctx.abuf_sz = strtoul(argstr, NULL, 10);

    qemu_add_led_event_handler(arcan_kbd_leds, NULL);
    qemu_add_vm_change_state_handler(arcan_vmstate_chg, NULL);

    update_display_titles();
}

static QemuDisplay qemu_display_arcan = {
    .type       = DISPLAY_TYPE_ARCAN,
    .early_init = arcan_display_early_init,
    .init       = arcan_display_init
};

static void register_arcan(void)
{
    qemu_display_register(&qemu_display_arcan);
}

type_init(register_arcan);
