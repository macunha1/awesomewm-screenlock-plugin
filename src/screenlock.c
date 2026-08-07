#define _GNU_SOURCE

#include "awesomewm_screenlock.h"
#include "awesomewm_screenlock_capture.h"
#include "awesomewm_screenlock_control.h"

#include <security/pam_appl.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/randr.h>
#include <xcb/xinerama.h>
#include <X11/keysym.h>
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include <signal.h>
#include <stdatomic.h>
#include <pthread.h>
#include <pwd.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    PASSWORD_CAPACITY = 256,
};

struct lock_display {
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
};

struct lock_state {
    xcb_connection_t *connection;
    xcb_screen_t *screen;
    xcb_window_t window;
    xcb_gcontext_t graphics;
    xcb_key_symbols_t *key_symbols;
    pam_handle_t *pam;
    const char *pam_service;
    const char *user;
    char password[PASSWORD_CAPACITY];
    size_t password_length;
    unsigned prompt_inverted;
    char status_message[64];
    Display *text_display;
    XftFont *text_font;
    struct screenlock_capture capture;
    uint8_t *background;
    int background_stride;
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
    int background_rows_drawn;
    atomic_int background_rows_ready;
    atomic_int capture_status;
    pthread_t capture_thread;
    int capture_thread_started;
    int background_ready;
    struct lock_display *displays;
    size_t display_count;
    int lockdown_enabled;
    const uint32_t *wibar_windows;
    size_t wibar_window_count;
    uint16_t lock_width;
    uint16_t lock_height;
    struct screenlock_control control;
    char notification_level[16];
    char notification_title[128];
    char notification_text[1024];
};

struct pam_data {
    const char *password;
    const char *user;
};

static volatile sig_atomic_t interrupted;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    interrupted = 1;
}

static void clear_secret(char *secret, size_t length)
{
    volatile char *cursor = (volatile char *)secret;

    while (length-- > 0)
        *cursor++ = 0;
}

static int pam_conversation(
    int message_count,
    const struct pam_message **messages,
    struct pam_response **responses,
    void *data
)
{
    const struct pam_data *pam_data = data;
    struct pam_response *result;

    if (message_count <= 0 || messages == NULL || responses == NULL)
        return PAM_CONV_ERR;

    result = calloc((size_t)message_count, sizeof(*result));
    if (result == NULL)
        return PAM_BUF_ERR;

    for (int index = 0; index < message_count; index++) {
        switch (messages[index]->msg_style) {
        case PAM_PROMPT_ECHO_OFF:
            result[index].resp = strdup(pam_data->password);
            if (result[index].resp == NULL)
                goto error;
            break;
        case PAM_PROMPT_ECHO_ON:
            result[index].resp = strdup(pam_data->user);
            if (result[index].resp == NULL)
                goto error;
            break;
        case PAM_TEXT_INFO:
        case PAM_ERROR_MSG:
            break;
        }
    }

    *responses = result;
    return PAM_SUCCESS;

error:
    for (int index = 0; index < message_count; index++) {
        if (result[index].resp != NULL) {
            clear_secret(result[index].resp, strlen(result[index].resp));
            free(result[index].resp);
        }
    }
    free(result);
    return PAM_CONV_ERR;
}

static void draw_rect(
    struct lock_state *state,
    int16_t x,
    int16_t y,
    uint16_t width,
    uint16_t height,
    uint32_t foreground
)
{
    xcb_rectangle_t rectangle = { x, y, width, height };

    xcb_change_gc(state->connection, state->graphics, XCB_GC_FOREGROUND, &foreground);
    xcb_poly_fill_rectangle(state->connection, state->window, state->graphics, 1, &rectangle);
}

/*
 * Order pending XCB fills before drawing text through the Xft connection.
 *
 * Contract: the XCB connection and lock window must be initialized. Xft uses
 * a separate Xlib connection, so flushing alone does not establish ordering
 * between the prompt background and the status text. The reply makes the
 * queued XCB rectangles complete before Xft paints over them.
 */
static int synchronize_prompt_background(struct lock_state *state)
{
    xcb_get_input_focus_cookie_t cookie =
        xcb_get_input_focus(state->connection);
    xcb_get_input_focus_reply_t *reply =
        xcb_get_input_focus_reply(state->connection, cookie, NULL);
    int success = reply != NULL;

    free(reply);
    return success;
}

/*
 * Draw a short status message using the prompt's current foreground color.
 *
 * Contract: the Xft display and font must be initialized, and `message` must
 * be UTF-8. The Xft resources are transient because the lock surface can be
 * redrawn after an expose event; the shared prompt state remains in C.
 */
static void draw_status_message(
    struct lock_state *state,
    int16_t left,
    int16_t top,
    uint16_t width,
    uint32_t color,
    const char *message
)
{
    XftDraw *draw;
    XftColor text_color;
    XGlyphInfo text_extents;
    XRenderColor render_color = {
        .red = color == state->screen->white_pixel ? 0xffff : 0,
        .green = color == state->screen->white_pixel ? 0xffff : 0,
        .blue = color == state->screen->white_pixel ? 0xffff : 0,
        .alpha = 0xffff,
    };
    int screen_number;

    if (state->text_display == NULL || state->text_font == NULL)
        return;
    if (!synchronize_prompt_background(state))
        return;
    screen_number = DefaultScreen(state->text_display);
    draw = XftDrawCreate(
        state->text_display, state->window,
        DefaultVisual(state->text_display, screen_number),
        DefaultColormap(state->text_display, screen_number)
    );
    if (draw == NULL)
        return;
    if (XftColorAllocValue(
            state->text_display,
            DefaultVisual(state->text_display, screen_number),
            DefaultColormap(state->text_display, screen_number),
            &render_color, &text_color
    )) {
        XftTextExtentsUtf8(
            state->text_display, state->text_font,
            (const FcChar8 *)message, strlen(message), &text_extents
        );
        XftDrawStringUtf8(
            draw, &text_color, state->text_font,
            left + (int16_t)(width / 2) - (int16_t)(text_extents.width / 2),
            top + 46, (const FcChar8 *)message, strlen(message)
        );
        XftColorFree(
            state->text_display,
            DefaultVisual(state->text_display, screen_number),
            DefaultColormap(state->text_display, screen_number),
            &text_color
        );
    }
    XftDrawDestroy(draw);
    XFlush(state->text_display);
}

/*
 * Calculate prompt geometry relative to one physical display.
 *
 * The lock window covers the full X11 root, but a prompt belongs to one
 * Xinerama rectangle. Keeping the rectangle in this function prevents a
 * multi-display layout from being treated as one oversized screen.
 */
static void prompt_geometry(
    const struct lock_state *state,
    const struct lock_display *display,
    int16_t *left,
    int16_t *top,
    uint16_t *width,
    uint16_t *height
)
{
    const int16_t center_x = display->x + (int16_t)(display->width / 2);
    const int16_t center_y = display->y + (int16_t)(display->height / 2);

    (void)state;

    *width = 320;
    *height = 72;
    *left = center_x - (int16_t)(*width / 2);
    *top = center_y - (int16_t)(*height / 2);
}

static void draw_background_rows(
    struct lock_state *state,
    int first_row,
    int last_row
)
{
    /*
     * X11 limits the size of one PutImage request. Send a few rows at a time
     * so a 1080p or 4K background cannot invalidate the X connection before
     * the password surface is drawn. The worker publishes completed rows and
     * this X11 thread consumes them in order. These requests are deliberately
     * queued without a per-tile check: checking each tile synchronously would
     * turn the safe tiling into hundreds of X11 round trips and recreate the
     * delay this path is intended to avoid.
     */
    for (int y = first_row; y < last_row; y += 4) {
        uint16_t rows = (uint16_t)(last_row - y);

        if (rows > 4)
            rows = 4;
        xcb_put_image(
            state->connection,
            XCB_IMAGE_FORMAT_Z_PIXMAP,
            state->window,
            state->graphics,
            state->lock_width,
            rows,
            0,
            (int16_t)y,
            0,
            state->screen->root_depth,
            (uint32_t)(state->background_stride * rows),
            state->background + y * state->background_stride
        );
    }
}

/*
 * Draw the shared prompt state on one display.
 *
 * Contract: `state` contains the current password length, flash state, and
 * failure state. The function reads those values only; drawing each display
 * from the same state keeps colors and password dots synchronized.
 */
static void draw_prompt_on_display(
    struct lock_state *state,
    const struct lock_display *display
)
{
    int16_t left;
    int16_t top;
    uint16_t box_width;
    uint16_t box_height;
    const uint32_t white = state->screen->white_pixel;
    const uint32_t black = state->screen->black_pixel;
    const uint32_t prompt_foreground = state->prompt_inverted ? black : white;
    const uint32_t prompt_background = state->prompt_inverted ? white : black;

    prompt_geometry(
        state, display, &left, &top, &box_width, &box_height
    );
    xcb_clear_area(state->connection, 0, state->window, left, top,
                   box_width, box_height);
    draw_rect(state, left, top, box_width, box_height, prompt_foreground);
    draw_rect(state, left + 4, top + 4, box_width - 8, box_height - 8,
              prompt_background);

    if (state->status_message[0] != '\0')
        draw_status_message(
            state, left, top, box_width, prompt_foreground,
            state->status_message
        );
    else
        for (size_t index = 0; index < state->password_length; index++) {
            int16_t dot_x = left + 18 + (int16_t)(index % 24) * 12;
            int16_t dot_y = top + 18 + (int16_t)(index / 24) * 16;
            draw_rect(state, dot_x, dot_y, 7, 7, prompt_foreground);
        }

    if (state->notification_text[0] != '\0') {
        int16_t notification_left = display->x + 8;
        int16_t notification_top = display->y + 8;
        uint16_t notification_width = display->width - 16;

        draw_rect(
            state, notification_left, notification_top,
            notification_width, 48, prompt_background
        );
        draw_status_message(
            state, notification_left, notification_top,
            notification_width, prompt_foreground,
            state->notification_title[0] != '\0'
                ? state->notification_title
                : state->notification_text
        );
    }
}

/*
 * Draw the prompt on every active Xinerama display.
 *
 * Xinerama reports monitor rectangles in root-window coordinates, so one lock
 * window can contain all prompts. If display discovery is unavailable, the
 * initialized fallback rectangle still gives the user one centered prompt.
 */
static void draw_prompt(struct lock_state *state)
{
    for (size_t index = 0; index < state->display_count; index++)
        draw_prompt_on_display(state, &state->displays[index]);
}

/*
 * Use the root rectangle as the one-display fallback.
 *
 * Contract: `state->screen` must be initialized. The allocated layout is
 * owned by `state` and is released by destroy_lock_window(). This fallback is
 * also the normal path on X servers without the Xinerama extension.
 */
static int use_root_display_layout(struct lock_state *state)
{
    state->displays = calloc(1, sizeof(*state->displays));
    if (state->displays == NULL)
        return 0;
    state->displays[0] = (struct lock_display){
        .width = state->lock_width,
        .height = state->lock_height,
    };
    state->display_count = 1;
    return 1;
}

/*
 * Extend one root-axis dimension with a display rectangle.
 *
 * Contract: `extent` contains the current non-negative root dimension and
 * `origin` is an X11 RandR/Xinerama origin. Negative origins are already
 * covered by the root dimension and must not be converted to unsigned values.
 * X11 dimensions are 16-bit, so larger calculated extents are capped instead
 * of wrapping into a small window.
 */
static void extend_root_extent(
    uint16_t *extent,
    int16_t origin,
    uint16_t size
)
{
    uint32_t end;

    if (origin < 0)
        return;
    end = (uint32_t)origin + size;
    if (end > UINT16_MAX)
        end = UINT16_MAX;
    if (end > *extent)
        *extent = (uint16_t)end;
}

/*
 * Discover active monitor rectangles for prompt placement.
 *
 * Xinerama coordinates are root-window coordinates, which means the lock
 * surface can remain a single fullscreen window while each prompt is centered
 * on its own display. Failure to query the extension is non-fatal: the root
 * rectangle preserves the existing single-prompt behavior.
 */
static int discover_display_layout(struct lock_state *state)
{
    const xcb_query_extension_reply_t *extension;
    xcb_xinerama_query_screens_reply_t *reply;
    xcb_xinerama_screen_info_t *screens;
    struct lock_display *displays;
    int screen_count;

    state->lock_width = state->screen->width_in_pixels;
    state->lock_height = state->screen->height_in_pixels;

    xcb_randr_get_screen_resources_current_reply_t *resources =
        xcb_randr_get_screen_resources_current_reply(
            state->connection,
            xcb_randr_get_screen_resources_current(
                state->connection, state->screen->root
            ),
            NULL
        );
    if (resources != NULL) {
        int crtc_count =
            xcb_randr_get_screen_resources_current_crtcs_length(resources);
        xcb_randr_crtc_t *crtcs =
            xcb_randr_get_screen_resources_current_crtcs(resources);
        for (int index = 0; index < crtc_count; index++) {
            xcb_randr_get_crtc_info_reply_t *crtc =
                xcb_randr_get_crtc_info_reply(
                    state->connection,
                    xcb_randr_get_crtc_info(
                        state->connection, crtcs[index], XCB_CURRENT_TIME
                    ),
                    NULL
            );
            if (crtc != NULL && crtc->mode != XCB_NONE) {
                extend_root_extent(&state->lock_width, crtc->x, crtc->width);
                extend_root_extent(&state->lock_height, crtc->y, crtc->height);
            }
            free(crtc);
        }
        free(resources);
    }

    if (!use_root_display_layout(state))
        return 0;
    extension = xcb_get_extension_data(
        state->connection, &xcb_xinerama_id
    );
    if (extension == NULL || !extension->present)
        return 1;
    reply = xcb_xinerama_query_screens_reply(
        state->connection,
        xcb_xinerama_query_screens(state->connection),
        NULL
    );
    if (reply == NULL)
        return 1;
    screen_count = xcb_xinerama_query_screens_screen_info_length(reply);
    screens = xcb_xinerama_query_screens_screen_info(reply);
    if (screen_count <= 0 || screens == NULL) {
        free(reply);
        return 1;
    }
    displays = calloc((size_t)screen_count, sizeof(*displays));
    if (displays == NULL) {
        free(reply);
        return 0;
    }
    free(state->displays);
    for (int index = 0; index < screen_count; index++) {
        displays[index] = (struct lock_display){
            .x = screens[index].x_org,
            .y = screens[index].y_org,
            .width = screens[index].width,
            .height = screens[index].height,
        };
        extend_root_extent(
            &state->lock_width, screens[index].x_org, screens[index].width
        );
        extend_root_extent(
            &state->lock_height, screens[index].y_org, screens[index].height
        );
    }
    state->displays = displays;
    state->display_count = (size_t)screen_count;
    free(reply);
    return 1;
}

static uint32_t visual_component(uint8_t value, uint32_t mask);

static void draw(struct lock_state *state)
{
    draw_background_rows(
        state, 0, state->lock_height
    );
    draw_prompt(state);
    xcb_flush(state->connection);
}

static void *capture_worker(void *opaque)
{
    struct lock_state *state = opaque;
    struct screenlock_capture capture = { 0 };
    char resolution[32];
    const char *display = getenv("DISPLAY");
    int result;

    /*
     * Capture runs away from the XCB event loop. The lock surface is mapped
     * only after this worker finishes, so FFmpeg can take its time without
     * capturing the lock surface or exposing an unfiltered desktop.
     */
    snprintf(
        resolution, sizeof(resolution), "%ux%u",
        state->lock_width,
        state->lock_height
    );
    result = awesomewm_screenlock_capture(
        display == NULL ? ":0" : display, resolution, &capture
    );

    if (result >= 0 && (capture.width != state->lock_width
                        || capture.height != state->lock_height)) {
        fprintf(
            stderr,
            "awesomewm-screenlock: clipping filtered frame from %dx%d to %ux%u\n",
            capture.width,
            capture.height,
            state->lock_width,
            state->lock_height
        );
    }

    if (result >= 0) {
        /*
         * XCB must stay on the owner thread, but pixel conversion does not.
         * Publish each completed row only after all of its pixels are written;
         * the acquire load in update_background() then makes that row visible
         * to the renderer without copying the whole frame again.
         */
        for (int y = 0; y < state->lock_height; y++) {
            uint8_t *source = y < capture.height
                ? capture.pixels + y * capture.stride
                : NULL;
            uint32_t *destination = (uint32_t *)
                (state->background + y * state->background_stride);

            for (int x = 0; x < state->lock_width; x++) {
                if (source != NULL && x < capture.width) {
                    destination[x] = visual_component(
                        source[x * 3], state->red_mask
                    ) | visual_component(
                        source[x * 3 + 1], state->green_mask
                    ) | visual_component(
                        source[x * 3 + 2], state->blue_mask
                    );
                } else {
                    destination[x] = state->screen->black_pixel;
                }
            }
            atomic_store_explicit(
                &state->background_rows_ready, y + 1, memory_order_release
            );
        }
    }
    awesomewm_screenlock_capture_free(&capture);
    atomic_store_explicit(
        &state->capture_status, result < 0 ? -1 : 1, memory_order_release
    );
    return NULL;
}

static int start_capture_worker(struct lock_state *state)
{
    atomic_init(&state->background_rows_ready, 0);
    atomic_init(&state->capture_status, 0);
    if (pthread_create(&state->capture_thread, NULL, capture_worker, state) != 0)
        return 0;
    state->capture_thread_started = 1;
    return 1;
}

static void stop_capture_worker(struct lock_state *state)
{
    if (state->capture_thread_started)
        pthread_join(state->capture_thread, NULL);
    state->capture_thread_started = 0;
}

static int finish_capture_before_lock(struct lock_state *state)
{
    int rows_ready;
    int status;

    /*
     * x11grab captures the composited desktop. Waiting here is intentional:
     * mapping the lock window first would make the capture contain the
     * locker's own black surface instead of the desktop being protected.
     */
    if (state->capture_thread_started) {
        pthread_join(state->capture_thread, NULL);
        state->capture_thread_started = 0;
    }
    rows_ready = atomic_load_explicit(
        &state->background_rows_ready, memory_order_acquire
    );
    status = atomic_load_explicit(
        &state->capture_status, memory_order_acquire
    );
    if (status < 0 || rows_ready != state->lock_height)
        return 0;
    state->background_rows_drawn = rows_ready;
    state->background_ready = 1;
    return 1;
}

static unsigned mask_shift(uint32_t mask)
{
    unsigned shift = 0;

    while (mask != 0 && (mask & 1) == 0) {
        mask >>= 1;
        shift++;
    }
    return shift;
}

static xcb_visualtype_t *root_visual_type(
    xcb_connection_t *connection,
    xcb_screen_t *screen
)
{
    xcb_depth_iterator_t depths = xcb_screen_allowed_depths_iterator(screen);

    for (; depths.rem != 0; xcb_depth_next(&depths)) {
        xcb_visualtype_iterator_t visuals = xcb_depth_visuals_iterator(depths.data);

        for (; visuals.rem != 0; xcb_visualtype_next(&visuals)) {
            if (visuals.data->visual_id == screen->root_visual)
                return visuals.data;
        }
    }
    (void)connection;
    return NULL;
}

static uint32_t visual_component(uint8_t value, uint32_t mask)
{
    uint32_t maximum = mask >> mask_shift(mask);

    return (((uint32_t)value * maximum + 127) / 255) << mask_shift(mask);
}

static int prepare_background_storage(struct lock_state *state)
{
    xcb_visualtype_t *visual;

    visual = root_visual_type(state->connection, state->screen);
    if (visual == NULL)
        return 0;
    state->red_mask = visual->red_mask;
    state->green_mask = visual->green_mask;
    state->blue_mask = visual->blue_mask;
    state->background_stride = state->lock_width * 4;
    state->background = calloc(
        (size_t)state->background_stride,
        state->lock_height
    );
    if (state->background == NULL)
        return 0;
    return 1;
}

static char key_to_character(xcb_keysym_t key, uint16_t key_state)
{
    int shifted = (key_state & XCB_MOD_MASK_SHIFT) != 0;
    int caps_locked = (key_state & XCB_MOD_MASK_LOCK) != 0;
    int uppercase = shifted != caps_locked;

    if (key >= XK_a && key <= XK_z)
        return (char)(key - XK_a + (uppercase ? 'A' : 'a'));
    if (key >= XK_0 && key <= XK_9) {
        static const char shifted_digits[] = ")!@#$%^&*(";
        return shifted ? shifted_digits[key - XK_0] : (char)(key - XK_0 + '0');
    }

    if (shifted) {
        switch (key) {
        case XK_minus: return '_';
        case XK_equal: return '+';
        case XK_bracketleft: return '{';
        case XK_bracketright: return '}';
        case XK_backslash: return '|';
        case XK_semicolon: return ':';
        case XK_apostrophe: return '"';
        case XK_grave: return '~';
        case XK_comma: return '<';
        case XK_period: return '>';
        case XK_slash: return '?';
        default: break;
        }
    } else {
        switch (key) {
        case XK_minus: return '-';
        case XK_equal: return '=';
        case XK_bracketleft: return '[';
        case XK_bracketright: return ']';
        case XK_backslash: return '\\';
        case XK_semicolon: return ';';
        case XK_apostrophe: return '\'';
        case XK_grave: return '`';
        case XK_comma: return ',';
        case XK_period: return '.';
        case XK_slash: return '/';
        default: break;
        }
    }

    if (key == XK_space)
        return ' ';

    return '\0';
}

static int authenticate(struct lock_state *state)
{
    struct pam_data pam_data = { state->password, state->user };
    struct pam_conv conversation = { pam_conversation, &pam_data };
    int result = pam_start(state->pam_service, state->user, &conversation, &state->pam);

    if (result == PAM_SUCCESS)
        result = pam_authenticate(state->pam, 0);
    if (result == PAM_SUCCESS)
        result = pam_acct_mgmt(state->pam, 0);
    if (result != PAM_SUCCESS)
        fprintf(stderr, "awesomewm-screenlock: PAM authentication failed for %s: %s\n",
                state->user, pam_strerror(state->pam, result));
    if (state->pam != NULL)
        pam_end(state->pam, result);
    state->pam = NULL;
    return result == PAM_SUCCESS;
}

/*
 * Open the font renderer used for native status messages.
 *
 * Contract: the XCB connection and screen must already exist. The renderer
 * is separate from the XCB connection because Xft owns its Xlib resources;
 * both connections target the same X11 server and the Xft calls stay on the
 * lock event-loop thread.
 */
static int prepare_text_renderer(struct lock_state *state)
{
    state->text_display = XOpenDisplay(NULL);
    if (state->text_display == NULL)
        return 0;
    state->text_font = XftFontOpenName(
        state->text_display, DefaultScreen(state->text_display), "Sans-14"
    );
    if (state->text_font == NULL) {
        XCloseDisplay(state->text_display);
        state->text_display = NULL;
        return 0;
    }
    return 1;
}

/*
 * Clear a previous authentication status before accepting new input.
 *
 * The failed submission is rendered with the next foreground/background
 * combination. The next key press returns the prompt to its default colors,
 * which prevents the error state from leaking into the next password entry.
 */
static void reset_status_message(struct lock_state *state)
{
    if (state->status_message[0] == '\0')
        return;
    state->status_message[0] = '\0';
    state->prompt_inverted = 0;
}

/*
 * Apply one validated notification from AwesomeWM to the lock surface.
 *
 * Notification text is display state, not a command. It is copied into fixed
 * buffers so the helper never retains pointers into the control socket's
 * receive buffer and cannot accept an unbounded allocation from the WM.
 */
static void apply_notification(
    struct lock_state *state,
    const struct screenlock_control_notification *notification
)
{
    if (notification->text[0] == '\0')
        return;
    snprintf(
        state->notification_level, sizeof(state->notification_level),
        "%s", notification->level
    );
    snprintf(
        state->notification_title, sizeof(state->notification_title),
        "%s", notification->title
    );
    snprintf(
        state->notification_text, sizeof(state->notification_text),
        "%s", notification->text
    );
}

static int is_integrated_wibar(
    const struct lock_state *state,
    xcb_window_t window
)
{
    for (size_t index = 0; index < state->wibar_window_count; index++) {
        if (state->wibar_windows[index] == window)
            return 1;
    }
    return 0;
}

/*
 * Keep the lock above every current root child, while leaving all specified
 * Awesome wibars above the lock. Querying the complete root tree is important:
 * menus, notifications, and widget popups are separate windows from their
 * originating wibar and may be mapped after the lock starts.
 *
 * X11 may clear or expose the lock surface while these stacking requests are
 * processed. The caller must repaint the captured background immediately
 * afterward; otherwise the underlying desktop can briefly become visible or
 * the surface can remain black.
 */
static void restack_integrated_windows(struct lock_state *state)
{
    xcb_window_t topmost_non_wibar = XCB_NONE;
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(
        state->connection,
        xcb_query_tree(state->connection, state->screen->root),
        NULL
    );

    if (tree != NULL) {
        int child_count = xcb_query_tree_children_length(tree);
        xcb_window_t *children = xcb_query_tree_children(tree);
        for (int index = 0; index < child_count; index++) {
            if (children[index] == state->window
                || is_integrated_wibar(state, children[index]))
                continue;
            /* Query-tree order is bottom-to-top, so retain only the top one. */
            topmost_non_wibar = children[index];
        }
        free(tree);
    }

    if (topmost_non_wibar != XCB_NONE) {
        uint32_t stacking[] = {
            topmost_non_wibar,
            XCB_STACK_MODE_ABOVE,
        };
        xcb_configure_window(
            state->connection,
            state->window,
            XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE,
            stacking
        );
    }

    for (size_t index = 0; index < state->wibar_window_count; index++) {
        uint32_t stacking[] = { XCB_STACK_MODE_ABOVE };
        xcb_configure_window(
            state->connection,
            state->wibar_windows[index],
            XCB_CONFIG_WINDOW_STACK_MODE,
            stacking
        );
    }

    if (state->wibar_window_count > 0) {
        uint32_t stacking[] = {
            state->wibar_windows[0],
            XCB_STACK_MODE_BELOW,
        };
        xcb_configure_window(
            state->connection,
            state->window,
            XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE,
            stacking
        );
    }
}

static int create_lock_window(struct lock_state *state)
{
    uint32_t values[] = {
        state->screen->black_pixel,
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_EXPOSURE,
        1,
    };
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK | XCB_CW_OVERRIDE_REDIRECT;
    xcb_grab_keyboard_reply_t *keyboard_reply;
    xcb_grab_pointer_reply_t *pointer_reply;
    xcb_void_cookie_t cookie;

    if (!state->lockdown_enabled && state->wibar_window_count == 0) {
        fprintf(
            stderr,
            "awesomewm-screenlock: integrated mode requires at least one wibar window\n"
        );
        return 0;
    }

    if (!state->lockdown_enabled) {
        uint32_t event_mask = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
        xcb_change_window_attributes(
            state->connection,
            state->screen->root,
            XCB_CW_EVENT_MASK,
            &event_mask
        );
    }

    state->window = xcb_generate_id(state->connection);
    xcb_create_window(
        state->connection,
        XCB_COPY_FROM_PARENT,
        state->window,
        state->screen->root,
        0,
        0,
        state->lock_width,
        state->lock_height,
        0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        state->screen->root_visual,
        mask,
        values
    );

    state->graphics = xcb_generate_id(state->connection);
    xcb_create_gc(state->connection, state->graphics, state->window, 0, NULL);
    xcb_map_window(state->connection, state->window);
    if (!state->lockdown_enabled) {
        restack_integrated_windows(state);
    } else {
        uint32_t lock_stacking[] = { XCB_STACK_MODE_ABOVE };
        xcb_configure_window(
            state->connection, state->window,
            XCB_CONFIG_WINDOW_STACK_MODE, lock_stacking
        );
    }
    xcb_set_input_focus(state->connection, XCB_INPUT_FOCUS_PARENT,
                        state->window, XCB_CURRENT_TIME);

    if (!state->lockdown_enabled)
        return xcb_connection_has_error(state->connection) == 0;

    keyboard_reply = xcb_grab_keyboard_reply(
        state->connection,
        xcb_grab_keyboard(
            state->connection, 1, state->window, XCB_CURRENT_TIME,
            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC
        ),
        NULL
    );
    if (keyboard_reply == NULL || keyboard_reply->status != XCB_GRAB_STATUS_SUCCESS) {
        free(keyboard_reply);
        return 0;
    }
    free(keyboard_reply);

    pointer_reply = xcb_grab_pointer_reply(
        state->connection,
        xcb_grab_pointer(
            state->connection, 1, state->window,
            XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
            state->window, XCB_NONE, XCB_CURRENT_TIME
        ),
        NULL
    );
    if (pointer_reply == NULL || pointer_reply->status != XCB_GRAB_STATUS_SUCCESS) {
        free(pointer_reply);
        return 0;
    }
    free(pointer_reply);

    cookie = xcb_change_window_attributes_checked(
        state->connection, state->window, XCB_CW_EVENT_MASK, values + 1
    );
    if (xcb_request_check(state->connection, cookie) != NULL)
        return 0;

    return xcb_connection_has_error(state->connection) == 0;
}

static void destroy_lock_window(struct lock_state *state)
{
    screenlock_control_close(&state->control);
    free(state->displays);
    state->displays = NULL;
    state->display_count = 0;
    if (state->connection == NULL)
        return;

    xcb_ungrab_pointer(state->connection, XCB_CURRENT_TIME);
    xcb_ungrab_keyboard(state->connection, XCB_CURRENT_TIME);
    if (state->window != XCB_NONE)
        xcb_destroy_window(state->connection, state->window);
    if (state->graphics != XCB_NONE)
        xcb_free_gc(state->connection, state->graphics);
    xcb_flush(state->connection);
    free(state->background);
    state->background = NULL;
    awesomewm_screenlock_capture_free(&state->capture);
    if (state->text_font != NULL)
        XftFontClose(state->text_display, state->text_font);
    if (state->text_display != NULL)
        XCloseDisplay(state->text_display);
    state->text_font = NULL;
    state->text_display = NULL;
}

int awesomewm_screenlock_run_with_options(
    const char *pam_service,
    const char *user,
    const struct awesomewm_screenlock_options *options
)
{
    struct lock_state state = { 0 };
    int screen_number;
    xcb_screen_iterator_t iterator;
    struct sigaction action = { .sa_handler = handle_signal };

    state.pam_service = pam_service == NULL ? "login" : pam_service;
    state.user = user == NULL ? getenv("USER") : user;
    state.control.listener = -1;
    state.control.client = -1;
    state.lockdown_enabled = options == NULL || options->lockdown_enabled;
    if (options != NULL) {
        state.wibar_windows = options->wibar_windows;
        state.wibar_window_count = options->wibar_window_count;
    }
    if (state.user == NULL || state.user[0] == '\0')
        return 2;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    state.connection = xcb_connect(NULL, &screen_number);
    if (state.connection == NULL || xcb_connection_has_error(state.connection) != 0)
        goto error;

    iterator = xcb_setup_roots_iterator(xcb_get_setup(state.connection));
    for (int index = 0; index < screen_number; index++)
        xcb_screen_next(&iterator);
    state.screen = iterator.data;
    state.key_symbols = xcb_key_symbols_alloc(state.connection);
    if (state.screen == NULL || state.key_symbols == NULL
        || (options != NULL && options->control_socket != NULL
            && !screenlock_control_open(&state.control, options->control_socket))
        || !discover_display_layout(&state)
        || !prepare_text_renderer(&state)
        || !prepare_background_storage(&state)
        || !start_capture_worker(&state)
        || !finish_capture_before_lock(&state)
        || !create_lock_window(&state))
        goto error;

    /* The captured background is ready before either mode becomes visible. */
    draw(&state);

    while (!interrupted) {
        struct screenlock_control_notification notification;

        screenlock_control_poll(&state.control, &notification);
        if (notification.text[0] != '\0') {
            apply_notification(&state, &notification);
            draw(&state);
        }
        xcb_generic_event_t *event = xcb_poll_for_event(state.connection);
        uint8_t response_type;
        if (event == NULL) {
            if (xcb_connection_has_error(state.connection) != 0)
                break;
            usleep(10000);
            continue;
        }
        response_type = event->response_type & 0x7f;
        if (!state.lockdown_enabled && response_type == XCB_MAP_NOTIFY) {
            xcb_window_t window = ((xcb_map_notify_event_t *)event)->window;
            if (window != state.window
                && !is_integrated_wibar(&state, window)) {
                restack_integrated_windows(&state);
                /* Required after X11 restacking; see restack_integrated_windows. */
                draw(&state);
            }
        } else if (response_type == XCB_EXPOSE) {
            draw(&state);
        } else if (response_type == XCB_KEY_PRESS) {
            xcb_key_press_event_t *key_event = (xcb_key_press_event_t *)event;
            xcb_keysym_t key = xcb_key_symbols_get_keysym(
                state.key_symbols, key_event->detail, 0
            );

            reset_status_message(&state);

            if (key == XK_BackSpace) {
                if (state.password_length > 0) {
                    state.password[--state.password_length] = 0;
                    state.prompt_inverted = !state.prompt_inverted;
                }
            } else if (key == XK_Escape) {
                clear_secret(state.password, state.password_length);
                state.password_length = 0;
            } else if (key == XK_Return || key == XK_KP_Enter) {
                if (authenticate(&state)) {
                    free(event);
                    stop_capture_worker(&state);
                    destroy_lock_window(&state);
                    xcb_key_symbols_free(state.key_symbols);
                    xcb_disconnect(state.connection);
                    return 0;
                }
                clear_secret(state.password, state.password_length);
                state.password_length = 0;
                snprintf(
                    state.status_message, sizeof(state.status_message),
                    "Authentication failed"
                );
                state.prompt_inverted = !state.prompt_inverted;
            } else if (state.password_length < PASSWORD_CAPACITY - 1) {
                char character = key_to_character(key, key_event->state);
                if (character != '\0') {
                    state.password[state.password_length++] = character;
                    state.prompt_inverted = !state.prompt_inverted;
                }
            }
            draw_prompt(&state);
            xcb_flush(state.connection);
        }
        free(event);
    }

    clear_secret(state.password, state.password_length);
    stop_capture_worker(&state);
    destroy_lock_window(&state);
    xcb_key_symbols_free(state.key_symbols);
    xcb_disconnect(state.connection);
    return 1;

error:
    stop_capture_worker(&state);
    if (state.key_symbols != NULL)
        xcb_key_symbols_free(state.key_symbols);
    destroy_lock_window(&state);
    if (state.connection != NULL)
        xcb_disconnect(state.connection);
    return 2;
}

int awesomewm_screenlock_run(const char *pam_service, const char *user)
{
    struct awesomewm_screenlock_options options = { .lockdown_enabled = 1 };

    return awesomewm_screenlock_run_with_options(pam_service, user, &options);
}
