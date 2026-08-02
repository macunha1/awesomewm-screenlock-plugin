#define _GNU_SOURCE

#include "awesomewm_screenlock.h"
#include "awesomewm_screenlock_capture.h"

#include <security/pam_appl.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <X11/keysym.h>

#include <signal.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    PASSWORD_CAPACITY = 256,
    FAILURE_DISPLAY_TICKS = 20,
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
    unsigned failure_ticks;
    struct screenlock_capture capture;
    uint8_t *background;
    int background_stride;
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

static void draw(struct lock_state *state)
{
    const int16_t center_x = (int16_t)(state->screen->width_in_pixels / 2);
    const int16_t center_y = (int16_t)(state->screen->height_in_pixels / 2);
    const uint32_t white = state->screen->white_pixel;
    const uint32_t black = state->screen->black_pixel;
    const uint32_t red = state->screen->white_pixel;
    const uint16_t box_width = 320;
    const uint16_t box_height = 72;
    const int16_t left = center_x - (int16_t)(box_width / 2);
    const int16_t top = center_y - (int16_t)(box_height / 2);

    xcb_clear_area(state->connection, 0, state->window, 0, 0,
                   state->screen->width_in_pixels, state->screen->height_in_pixels);

    xcb_put_image(
        state->connection,
        XCB_IMAGE_FORMAT_Z_PIXMAP,
        state->window,
        state->graphics,
        state->screen->width_in_pixels,
        state->screen->height_in_pixels,
        0,
        0,
        0,
        state->screen->root_depth,
        (uint32_t)(state->background_stride * state->screen->height_in_pixels),
        state->background
    );

    draw_rect(state, left, top, box_width, box_height, white);
    draw_rect(state, left + 4, top + 4, box_width - 8, box_height - 8,
              state->failure_ticks > 0 ? red : black);

    for (size_t index = 0; index < state->password_length; index++) {
        int16_t dot_x = left + 18 + (int16_t)(index % 24) * 12;
        int16_t dot_y = top + 18 + (int16_t)(index / 24) * 16;
        draw_rect(state, dot_x, dot_y, 7, 7, white);
    }

    xcb_flush(state->connection);
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

static int prepare_background(struct lock_state *state)
{
    char resolution[32];
    const char *display = getenv("DISPLAY");
    xcb_visualtype_t *visual;
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;

    snprintf(
        resolution, sizeof(resolution), "%ux%u",
        state->screen->width_in_pixels,
        state->screen->height_in_pixels
    );
    if (awesomewm_screenlock_capture(
            display == NULL ? ":0" : display, resolution, &state->capture
        ) < 0)
        return 0;

    visual = root_visual_type(state->connection, state->screen);
    if (visual == NULL)
        return 0;
    red_mask = visual->red_mask;
    green_mask = visual->green_mask;
    blue_mask = visual->blue_mask;
    state->background_stride = state->screen->width_in_pixels * 4;
    state->background = calloc(
        (size_t)state->background_stride,
        state->screen->height_in_pixels
    );
    if (state->background == NULL)
        return 0;

    for (int y = 0; y < state->screen->height_in_pixels; y++) {
        uint8_t *source = state->capture.pixels + y * state->capture.stride;
        uint32_t *destination = (uint32_t *)(state->background + y * state->background_stride);

        for (int x = 0; x < state->screen->width_in_pixels; x++) {
            destination[x] = visual_component(source[x * 3], red_mask)
                | visual_component(source[x * 3 + 1], green_mask)
                | visual_component(source[x * 3 + 2], blue_mask);
        }
    }
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

    state->window = xcb_generate_id(state->connection);
    xcb_create_window(
        state->connection,
        XCB_COPY_FROM_PARENT,
        state->window,
        state->screen->root,
        0,
        0,
        state->screen->width_in_pixels,
        state->screen->height_in_pixels,
        0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        state->screen->root_visual,
        mask,
        values
    );

    state->graphics = xcb_generate_id(state->connection);
    xcb_create_gc(state->connection, state->graphics, state->window, 0, NULL);
    xcb_map_window(state->connection, state->window);
    xcb_set_input_focus(state->connection, XCB_INPUT_FOCUS_PARENT,
                        state->window, XCB_CURRENT_TIME);

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
}

int awesomewm_screenlock_run(const char *pam_service, const char *user)
{
    struct lock_state state = { 0 };
    int screen_number;
    xcb_screen_iterator_t iterator;
    struct sigaction action = { .sa_handler = handle_signal };

    state.pam_service = pam_service == NULL ? "login" : pam_service;
    state.user = user == NULL ? getenv("USER") : user;
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
    if (state.screen == NULL || state.key_symbols == NULL || !prepare_background(&state)
        || !create_lock_window(&state))
        goto error;

    draw(&state);
    while (!interrupted) {
        xcb_generic_event_t *event = xcb_poll_for_event(state.connection);
        uint8_t response_type;

        if (event == NULL) {
            if (xcb_connection_has_error(state.connection) != 0)
                break;
            usleep(10000);
            continue;
        }
        response_type = event->response_type & 0x7f;
        if (response_type == XCB_EXPOSE) {
            draw(&state);
        } else if (response_type == XCB_KEY_PRESS) {
            xcb_key_press_event_t *key_event = (xcb_key_press_event_t *)event;
            xcb_keysym_t key = xcb_key_symbols_get_keysym(
                state.key_symbols, key_event->detail, 0
            );

            if (key == XK_BackSpace) {
                if (state.password_length > 0)
                    state.password[--state.password_length] = 0;
            } else if (key == XK_Escape) {
                clear_secret(state.password, state.password_length);
                state.password_length = 0;
            } else if (key == XK_Return || key == XK_KP_Enter) {
                if (authenticate(&state)) {
                    free(event);
                    destroy_lock_window(&state);
                    xcb_key_symbols_free(state.key_symbols);
                    xcb_disconnect(state.connection);
                    return 0;
                }
                clear_secret(state.password, state.password_length);
                state.password_length = 0;
                state.failure_ticks = FAILURE_DISPLAY_TICKS;
            } else if (state.password_length < PASSWORD_CAPACITY - 1) {
                char character = key_to_character(key, key_event->state);
                if (character != '\0')
                    state.password[state.password_length++] = character;
            }
            if (state.failure_ticks > 0)
                state.failure_ticks--;
            draw(&state);
        }
        free(event);
    }

    clear_secret(state.password, state.password_length);
    destroy_lock_window(&state);
    xcb_key_symbols_free(state.key_symbols);
    xcb_disconnect(state.connection);
    return 1;

error:
    if (state.key_symbols != NULL)
        xcb_key_symbols_free(state.key_symbols);
    destroy_lock_window(&state);
    if (state.connection != NULL)
        xcb_disconnect(state.connection);
    return 2;
}
