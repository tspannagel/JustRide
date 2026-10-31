// Touch UI: home, riders, workouts, settings, Wi-Fi, ride, summary. LVGL 9.
// Everything here runs in the LVGL task; ride state comes from app_get_view().
// Look: dark background, numbers on cards, one colour per metric, flat buttons.

#include "ui.h"

#include <dirent.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "app.h"
#include "ble_mgr.h"
#include "bsp.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "input_map.h"
#include "tz.h"
#include "web.h"

static const char *TAG = "ui";

#define WORKOUT_DIR BSP_SD_MOUNT "/workouts"
#define MAX_LIST 40
#define UI_DRAW_LINES 48 // LVGL draw buffer height (two buffers in PSRAM)
#define PAD 12
#define GAP 10

// Palette (matches the web app's dark theme)
// No gradients on dark surfaces: RGB565 has too few dark steps, they band
#define C_BG 0x0A0B12
#define C_CARD 0x151823
#define C_LINE 0x262A38      // 1 px card and button border
#define C_BTN 0x1E2230
#define C_TEXT 0xEEF0F6
#define C_MUTED 0x8A90A3
#define C_ACCENT 0x6574F5    // primary gradient, left to right
#define C_ACCENT_2 0x9A5BF0
#define C_FOCUS 0x58A6FF  // controller highlight, a different hue than the primary so a focused primary shows
#define C_OK 0x3DDC97
#define C_WARN 0xF5B94A
#define C_DANGER 0xE5484D
#define C_DANGER_2 0xB8304F
#define C_POWER 0xF5B83D
#define C_HR 0xF85149
#define C_CAD 0x39C5BB
#define C_SPEED 0xA371F7

// Montserrat Medium 100 px, only "-0123456789" (generated with lv_font_conv, see font_digits_100.c)
LV_FONT_DECLARE(font_digits_100);
// Font Awesome 5 glyphs at 18 px (font_icons_18.c); only these exist in it
LV_FONT_DECLARE(font_icons_18);
#define I_BOLT "\xEF\x83\xA7"
#define I_HEART "\xEF\x80\x84"
#define I_CADENCE "\xEF\x8B\xB1"
#define I_SPEED "\xEF\x8F\xBD"
#define I_TIME "\xEF\x8B\xB2"
#define I_ROAD "\xEF\x80\x98"
#define I_FIRE "\xEF\x81\xAD"
#define I_BARS "\xEF\x82\x80"
#define I_PERCENT "\xEF\x8A\x95"
#define I_LINE "\xEF\x88\x81"

static lv_style_t st_screen, st_card, st_btn, st_primary, st_danger, st_focus;

static lv_obj_t *scr_home, *scr_users, *scr_name, *scr_workouts, *scr_adapt, *scr_preview, *scr_settings, *scr_wifi, *scr_ride,
    *scr_summary;

static lv_obj_t *home_user, *home_dot[4], *home_state[4], *home_warn, *home_test;
static lv_obj_t *users_list, *users_msg;
static lv_obj_t *name_title, *name_ta;
static bool s_name_add;  // name screen: add a rider (true) or rename the active one
static lv_obj_t *wo_list, *wo_msg;
static lv_obj_t *pv_title, *pv_profile, *pv_val[4], *pv_cap[4], *pv_icon[4], *pv_note, *pv_msg, *pv_start;
EXT_RAM_BSS_ATTR static workout_t s_preview;  // workout shown on the preview screen (PSRAM)
EXT_RAM_BSS_ATTR static route_t s_route_pv;   // route shown there instead (s_pv_route)
static bool s_pv_route;
static const char *s_preview_path;   // points into s_paths; NULL: built in memory (adaptive ride, FTP test)
static lv_obj_t *s_preview_from;     // screen the preview's back button returns to
static lv_obj_t *ad_dur, *ad_power, *ad_hr, *ad_avg_label;
static struct {
    int min, watts, hr_zone;  // hr_zone 1..5 (% of max HR, metrics_hr_zone_pct), 0: no HR goal
    bool hold_avg;
} s_adapt;
static lv_obj_t *set_title, *set_ftp, *set_erg, *set_weight, *set_hr;
static lv_obj_t *wifi_status, *wifi_btn, *wifi_btn_label, *wifi_ota_btn, *wifi_ota_label, *wifi_tz, *wifi_time;
static lv_obj_t *r_profile;
static lv_obj_t *r_time, *r_info, *r_power_card, *r_power_cap, *r_power, *r_target, *r_cad, *r_hr, *r_hr_cap, *r_speed,
    *r_stats;
static lv_obj_t *r_pause_label, *r_grade_minus, *r_grade_plus, *r_skip;
static lv_obj_t *sum_saved, *sum_note, *sum_val[9], *sum_ftp_btn, *sum_ftp_label, *sum_hr_btn, *sum_hr_label;

static char s_paths[MAX_LIST][80];

// ---- Helpers ------------------------------------------------------------------

static lv_color_t rgb(uint32_t c) { return lv_color_hex(c); }

static void styles_init(void)
{
    lv_style_init(&st_screen);
    lv_style_set_bg_color(&st_screen, rgb(C_BG));
    lv_style_set_text_color(&st_screen, rgb(C_TEXT));
    lv_style_set_pad_all(&st_screen, PAD);
    lv_style_set_pad_row(&st_screen, GAP);

    lv_style_init(&st_card);
    lv_style_set_bg_color(&st_card, rgb(C_CARD));
    lv_style_set_bg_opa(&st_card, LV_OPA_COVER);
    lv_style_set_radius(&st_card, 18);
    lv_style_set_pad_all(&st_card, 10);
    lv_style_set_pad_row(&st_card, 4);
    lv_style_set_pad_column(&st_card, 8);
    lv_style_set_border_color(&st_card, rgb(C_LINE));
    lv_style_set_border_width(&st_card, 1);

    lv_style_init(&st_btn);
    lv_style_set_bg_color(&st_btn, rgb(C_BTN));
    lv_style_set_radius(&st_btn, 14);
    lv_style_set_shadow_width(&st_btn, 0);
    lv_style_set_border_color(&st_btn, rgb(C_LINE));
    lv_style_set_border_width(&st_btn, 1);
    lv_style_set_text_color(&st_btn, rgb(C_TEXT));

    lv_style_init(&st_primary);
    lv_style_set_bg_color(&st_primary, rgb(C_ACCENT));
    lv_style_set_bg_grad_color(&st_primary, rgb(C_ACCENT_2));
    lv_style_set_bg_grad_dir(&st_primary, LV_GRAD_DIR_HOR);
    lv_style_set_border_width(&st_primary, 0);

    lv_style_init(&st_danger);
    lv_style_set_bg_color(&st_danger, rgb(C_DANGER));
    lv_style_set_bg_grad_color(&st_danger, rgb(C_DANGER_2));
    lv_style_set_bg_grad_dir(&st_danger, LV_GRAD_DIR_HOR);
    lv_style_set_border_width(&st_danger, 0);

    // Controller highlight (LV_STATE_FOCUS_KEY): blue fill plus outline
    lv_style_init(&st_focus);
    lv_style_set_bg_color(&st_focus, rgb(C_FOCUS));
    lv_style_set_bg_grad_dir(&st_focus, LV_GRAD_DIR_NONE);
    lv_style_set_outline_color(&st_focus, rgb(C_TEXT));
    lv_style_set_outline_width(&st_focus, 3);
    lv_style_set_outline_pad(&st_focus, 3);
}

// Object flags for controller navigation
#define NAV_ITEM LV_OBJ_FLAG_USER_1   // can be highlighted and pressed with A
#define NAV_START LV_OBJ_FLAG_USER_2  // starts a ride: only a held A presses it

static void navigable(lv_obj_t *o)
{
    lv_obj_add_flag(o, NAV_ITEM);
    lv_obj_add_style(o, &st_focus, LV_STATE_FOCUS_KEY);
}

static lv_obj_t *screen(const char *title, bool back);

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_label_set_text(l, text);
    return l;
}

// Small upper-case label above a value
static lv_obj_t *caption(lv_obj_t *parent, const char *text, uint32_t color)
{
    lv_obj_t *l = label(parent, &lv_font_montserrat_14, text);
    lv_obj_set_style_text_color(l, rgb(color), 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    return l;
}

static lv_obj_t *muted(lv_obj_t *parent, const lv_font_t *font, const char *text)
{
    lv_obj_t *l = label(parent, font, text);
    lv_obj_set_style_text_color(l, rgb(C_MUTED), 0);
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, int w, int h, lv_event_cb_t cb, void *data)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_add_style(b, &st_btn, 0);
    lv_obj_set_size(b, w, h);
    lv_obj_t *l = label(b, &lv_font_montserrat_20, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
    navigable(b);
    return b;
}

static lv_obj_t *primary(lv_obj_t *b)
{
    lv_obj_add_style(b, &st_primary, 0);
    return b;
}

static lv_obj_t *danger(lv_obj_t *b)
{
    lv_obj_add_style(b, &st_danger, 0);
    return b;
}

// Fires only after a long press: for actions that must not happen by accident
static lv_obj_t *hold_button(lv_obj_t *parent, const char *text, int w, int h, lv_event_cb_t cb, void *data)
{
    lv_obj_t *b = danger(button(parent, text, w, h, cb, data));
    lv_obj_remove_event_cb(b, cb);
    lv_obj_add_event_cb(b, cb, LV_EVENT_LONG_PRESSED, data);
    return b;
}

static lv_obj_t *grow(lv_obj_t *o)
{
    lv_obj_set_flex_grow(o, 1);
    return o;
}

static lv_obj_t *box(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_set_style_pad_column(o, GAP, 0);
    lv_obj_set_style_pad_row(o, 4, 0);
    return o;
}

// Children side by side, vertically centred
static lv_obj_t *row(lv_obj_t *parent)
{
    lv_obj_t *r = box(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return r;
}

static lv_obj_t *card(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *c = box(parent, flow);
    lv_obj_add_style(c, &st_card, 0);
    return c;
}

// Icon + caption in one colour; returns the caption label
static lv_obj_t *icon_caption(lv_obj_t *parent, const char *icon, const char *name, uint32_t color)
{
    lv_obj_t *r = row(parent);
    lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(r, 6, 0);
    lv_obj_t *i = label(r, &font_icons_18, icon);
    lv_obj_set_style_text_color(i, rgb(color), 0);
    return caption(r, name, color);
}

// Card with an icon, a coloured caption and a value + unit: returns the value label
static lv_obj_t *metric_font(lv_obj_t *parent, const char *icon, const char *name, uint32_t color, const char *unit,
                             const lv_font_t *font)
{
    lv_obj_t *c = grow(card(parent, LV_FLEX_FLOW_COLUMN));
    lv_obj_set_width(c, 0);  // with grow: equal widths
    icon_caption(c, icon, name, color);
    lv_obj_t *r = row(c);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(r, 4, 0);
    lv_obj_t *v = label(r, font, "-");
    if (unit) {
        lv_obj_set_style_pad_bottom(muted(r, &lv_font_montserrat_14, unit), 4, 0);
    }
    return v;
}

static lv_obj_t *metric(lv_obj_t *parent, const char *icon, const char *name, uint32_t color, const char *unit)
{
    return metric_font(parent, icon, name, color, unit, &lv_font_montserrat_28);
}

// ---- Workout profile ------------------------------------------------------------

// Zone colours by % of FTP, same zones and colours as the web app
static uint32_t zone_color(uint16_t pm)
{
    return pm < 550 ? 0x6B7385 : pm < 750 ? 0x3987E5 : pm < 900 ? 0x1FB57F : pm < 1050 ? 0xE8B100
         : pm < 1200 ? 0xEB6834 : pm < 1500 ? 0xE34948 : 0xA07CF0;
}

typedef struct {
    const workout_t *w;
    int32_t pos_s;  // workout time to mark, < 0: none
    const route_t *route;  // instead of w: the route's elevation
    int32_t pos_m;         // distance to mark, < 0: none
} profile_t;

static profile_t s_pv_prof, s_ride_prof;

static void fill(lv_layer_t *layer, int x1, int y1, int x2, int y2, uint32_t color, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = rgb(color);
    d.bg_opa = opa;
    lv_area_t a = {x1, y1, x2, y2};
    lv_draw_rect(layer, &d, &a);
}

// Two box blurs: steps become a short S-curve, ramps stay straight
static void blur(const int16_t *in, int16_t *out, int n, int r)
{
    for (int x = 0; x < n; x++) {
        int32_t sum = 0;
        for (int k = x - r; k <= x + r; k++) {
            sum += in[k < 0 ? 0 : k >= n ? n - 1 : k];
        }
        out[x] = (int16_t)(sum / (2 * r + 1));
    }
}

// Gradient colours for routes, the web app's ramp: < 2, 4, 6, 8, 10 %, steeper
static uint32_t grade_color(int16_t g)
{
    return g < 200 ? 0x3987E5 : g < 400 ? 0x1FB57F : g < 600 ? 0xE8B100 : g < 800 ? 0xEB6834 : g < 1000 ? 0xE34948
                                                                                                 : 0xA07CF0;
}

// A route's elevation, one column per pixel coloured by gradient; done part dimmed,
// marker at pos_m. Solid fills (no gradients on the dark surface: RGB565 bands).
static void draw_route(lv_obj_t *o, lv_layer_t *layer, const route_t *r, int32_t pos_m)
{
    lv_area_t a;
    lv_obj_get_content_coords(o, &a);
    int width = lv_area_get_width(&a), height = lv_area_get_height(&a);
    uint32_t len = route_length_m(r);
    if (width <= 0 || width > BSP_LCD_W || height <= 0 || len == 0) {
        return;
    }
    int32_t lo = r->ele_cm[0], hi = r->ele_cm[0];
    for (uint32_t i = 1; i < r->n; i++) {
        lo = r->ele_cm[i] < lo ? r->ele_cm[i] : lo;
        hi = r->ele_cm[i] > hi ? r->ele_cm[i] : hi;
    }
    int32_t range = hi - lo < 5000 ? 5000 : hi - lo;  // at least 50 m: flat routes stay flat
    int marker = pos_m < 0 ? -1 : (int)((uint64_t)(pos_m > (int32_t)len ? len : (uint32_t)pos_m) * width / len);
    for (int x = 0; x < width; x++) {
        uint32_t d = (uint32_t)(((2ULL * x + 1) * len) / (2ULL * width));
        int y = a.y2 - (int)((int64_t)(route_ele_cm(r, d) - lo) * (height - 2) / range);
        bool done = x < marker;
        fill(layer, a.x1 + x, y, a.x1 + x, a.y2, grade_color(route_grade(r, d)), done ? LV_OPA_20 : LV_OPA_60);
        fill(layer, a.x1 + x, y, a.x1 + x, y + 1, C_TEXT, done ? LV_OPA_30 : LV_OPA_80);
    }
    if (marker >= 0) {
        fill(layer, a.x1 + marker - 1, a.y1, a.x1 + marker, a.y2, C_TEXT, LV_OPA_COVER);
    }
}

// The workout as one continuous curve, as in the web app: one column per pixel,
// softened at steps, zone colours blended at zone changes, fading towards the
// floor, with a light line along the top. Free segments are grey. Done part
// dimmed, marker at pos_s. Routes: draw_route.
static void draw_profile(lv_event_t *e)
{
    const profile_t *p = lv_event_get_user_data(e);
    if (p->route) {
        draw_route(lv_event_get_target_obj(e), lv_event_get_layer(e), p->route, p->pos_m);
        return;
    }
    const workout_t *w = p->w;
    if (!w || w->total_s == 0) {
        return;
    }
    lv_obj_t *o = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_content_coords(o, &a);
    int width = lv_area_get_width(&a), height = lv_area_get_height(&a);
    if (width <= 0 || width > BSP_LCD_W || height <= 0) {
        return;
    }
    static int16_t col[BSP_LCD_W], tmp[BSP_LCD_W], y[BSP_LCD_W];
    static uint8_t rgb3[3][BSP_LCD_W];
    int top = 1000;
    for (int x = 0; x < width; x++) {
        uint32_t t = (uint32_t)(((2ULL * x + 1) * w->total_s) / (2ULL * width));
        workout_pos_t pos;
        workout_at(w, t, &pos);
        col[x] = (int16_t)workout_profile_at(w, t);
        uint32_t c = pos.kind == SEG_FREE ? C_MUTED : zone_color((uint16_t)col[x]);
        rgb3[0][x] = c >> 16;
        rgb3[1][x] = c >> 8;
        rgb3[2][x] = c;
        top = col[x] > top ? col[x] : top;
    }
    top = top + top / 10;  // headroom
    int r = width >= 200 ? 2 : 1;
    blur(col, tmp, width, r);
    blur(tmp, col, width, r);
    for (int x = 0; x < width; x++) {
        y[x] = (int16_t)(a.y2 - col[x] * (height - 2) / top);
    }
    // Blend colours over a few pixels at zone changes
    for (int c = 0; c < 3; c++) {
        for (int x = 0; x < width; x++) {
            tmp[x] = rgb3[c][x];
        }
        blur(tmp, col, width, 3);
        for (int x = 0; x < width; x++) {
            rgb3[c][x] = (uint8_t)col[x];
        }
    }
    int marker = p->pos_s < 0 ? -1 : (int)((uint64_t)p->pos_s * width / w->total_s);
    lv_draw_rect_dsc_t d;
    for (int x = 0; x < width; x++) {
        lv_color_t c = lv_color_make(rgb3[0][x], rgb3[1][x], rgb3[2][x]);
        lv_opa_t opa = x < marker ? LV_OPA_40 : LV_OPA_COVER;
        lv_draw_rect_dsc_init(&d);
        d.bg_opa = opa;
        d.bg_grad.dir = LV_GRAD_DIR_VER;
        d.bg_grad.stops_count = 2;
        // Fade by screen row, not per column: start at this column's share of the full fade
        lv_opa_t top_opa = (lv_opa_t)(LV_OPA_10 + (LV_OPA_90 - LV_OPA_10) * (a.y2 - y[x]) / height);
        d.bg_grad.stops[0] = (lv_grad_stop_t){.color = c, .opa = top_opa, .frac = 0};
        d.bg_grad.stops[1] = (lv_grad_stop_t){.color = c, .opa = LV_OPA_10, .frac = 255};
        lv_area_t col_a = {a.x1 + x, y[x], a.x1 + x, a.y2};
        lv_draw_rect(layer, &d, &col_a);
        // Top line, joined to the neighbour so steep steps have no holes
        int y1 = y[x], y2 = y[x] + 1;
        if (x > 0) {
            y1 = LV_MIN(y1, y[x - 1] + 1);
            y2 = LV_MAX(y2, y[x - 1]);
        }
        lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_mix(lv_color_white(), c, 90);
        d.bg_opa = opa;
        col_a = (lv_area_t){a.x1 + x, y1, a.x1 + x, y2};
        lv_draw_rect(layer, &d, &col_a);
    }
    int ftp_y = a.y2 - 1000 * (height - 2) / top;  // 100 % FTP line
    fill(layer, a.x1, ftp_y, a.x2, ftp_y, C_TEXT, LV_OPA_20);
    if (marker >= 0) {
        fill(layer, a.x1 + marker - 1, a.y1, a.x1 + marker, a.y2, C_TEXT, LV_OPA_COVER);
    }
}

static lv_obj_t *profile(lv_obj_t *parent, profile_t *p, int h)
{
    lv_obj_t *o = card(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_height(o, h);
    lv_obj_set_style_pad_all(o, 6, 0);
    lv_obj_add_event_cb(o, draw_profile, LV_EVENT_DRAW_MAIN_END, p);
    return o;
}

static void fmt_time(char *buf, size_t n, uint32_t s)
{
    snprintf(buf, n, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
}

static void show(lv_obj_t *o, bool on)
{
    if (on) {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static void nav_clear(void);

static void go(lv_obj_t *scr)
{
    nav_clear();
    lv_screen_load(scr);
}

static void on_back(lv_event_t *e) { go(scr_home); }

static void on_action(lv_event_t *e) { app_post_action((action_t)(intptr_t)lv_event_get_user_data(e)); }

// ---- Home ---------------------------------------------------------------------

static void open_users(lv_event_t *e);
static void open_workouts(lv_event_t *e);
static void open_settings(lv_event_t *e);
static void open_wifi(lv_event_t *e);
static void open_ramp_test(lv_event_t *e);

static lv_obj_t *tile(lv_obj_t *parent, const char *icon, const char *text, lv_event_cb_t cb, void *data)
{
    lv_obj_t *b = grow(button(parent, "", 0, LV_PCT(100), cb, data));
    lv_obj_clean(b);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(b, 8, 0);
    label(b, &lv_font_montserrat_28, icon);
    label(b, &lv_font_montserrat_20, text);
    return b;
}

static void build_home(void)
{
    scr_home = screen(NULL, false);
    lv_obj_t *top = row(scr_home);
    grow(label(top, &lv_font_montserrat_28, "JustRide"));
    lv_obj_t *u = button(top, "", LV_SIZE_CONTENT, 44, open_users, NULL);
    lv_obj_set_style_pad_hor(u, 16, 0);
    home_user = lv_obj_get_child(u, 0);

    static const char *const names[4] = {"Trainer", "Heart rate", "Zwift Ride", "Watch"};
    lv_obj_t *c = card(scr_home, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 6, 0);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *r = row(c);
        home_dot[i] = lv_obj_create(r);
        lv_obj_remove_style_all(home_dot[i]);
        lv_obj_set_size(home_dot[i], 10, 10);
        lv_obj_set_style_radius(home_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(home_dot[i], LV_OPA_COVER, 0);
        grow(label(r, &lv_font_montserrat_20, names[i]));
        home_state[i] = muted(r, &lv_font_montserrat_16, "");
    }
    home_warn = label(c, &lv_font_montserrat_16, "");
    lv_obj_set_style_text_color(home_warn, rgb(C_WARN), 0);
    // New riders: FTP is only a guess until the first test
    home_test = button(scr_home, LV_SYMBOL_CHARGE "  FTP not tested yet: take the ramp test", LV_PCT(100), 52, open_ramp_test, NULL);

    lv_obj_t *r1 = grow(row(scr_home)), *r2 = grow(row(scr_home));  // tiles fill the rest of the screen
    lv_obj_add_flag(primary(tile(r1, LV_SYMBOL_PLAY, "Free ride", on_action, (void *)ACT_START_FREE)), NAV_START);
    tile(r1, LV_SYMBOL_LIST, "Workouts", open_workouts, NULL);
    tile(r2, LV_SYMBOL_SETTINGS, "Settings", open_settings, NULL);
    tile(r2, LV_SYMBOL_WIFI, "Wi-Fi", open_wifi, NULL);
}

static void update_home(const ride_view_t *v, const app_status_t *st)
{
    settings_t s;
    app_get_settings(&s);
    lv_label_set_text_fmt(home_user, "%s  " LV_SYMBOL_DOWN, settings_user(&s)->name);
    const bool on[4] = {st->trainer, st->hr, st->controller, st->receivers > 0};
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(home_dot[i], rgb(on[i] ? C_OK : C_MUTED), 0);
        lv_label_set_text(home_state[i], on[i] ? "Connected" : i == 0 ? "Searching..." : "Not connected");
    }
    if (st->receivers > 1) {
        lv_label_set_text_fmt(home_state[3], "%u connected", st->receivers);  // watch + bike computer
    }
    lv_label_set_text_fmt(home_warn, "%s%s%s", st->sd_ok ? "" : LV_SYMBOL_WARNING " No SD card: rides are not saved",
                          !st->sd_ok && !st->clock_ok ? "\n" : "",
                          st->clock_ok ? "" : LV_SYMBOL_WARNING " Clock not set: open the web app");
    show(home_warn, !st->sd_ok || !st->clock_ok);
    show(home_test, !settings_user(&s)->ftp_known);
}

// ---- Riders -------------------------------------------------------------------

static lv_obj_t *list_item(lv_obj_t *l, const char *icon, const char *text, lv_event_cb_t cb, void *data);

static char s_user_ids[SETTINGS_MAX_USERS][8];  // ids behind the riders list

static void on_user(lv_event_t *e)
{
    app_select_user(s_user_ids[(int)(intptr_t)lv_event_get_user_data(e)]);
    go(scr_home);
}

static void open_users(lv_event_t *e)
{
    settings_t s;
    app_get_settings(&s);
    lv_obj_clean(users_list);
    lv_label_set_text(users_msg, "");
    for (int i = 0; i < s.n_users; i++) {
        const user_t *u = &s.users[i];
        memcpy(s_user_ids[i], u->id, sizeof(s_user_ids[i]));
        char text[48];
        snprintf(text, sizeof(text), "%s\nFTP %u W  -  %u kg", u->name, u->ftp_w, u->weight_kg);
        list_item(users_list, i == s.user ? LV_SYMBOL_OK : " ", text, on_user, (void *)(intptr_t)i);
    }
    go(scr_users);
}

static void open_name(bool add)
{
    settings_t s;
    app_get_settings(&s);
    s_name_add = add;
    lv_label_set_text(name_title, add ? "New rider name" : "Rename rider");
    lv_textarea_set_text(name_ta, add ? "" : settings_user(&s)->name);
    go(scr_name);
}

static void on_add_user(lv_event_t *e)
{
    settings_t s;
    app_get_settings(&s);
    if (s.n_users >= SETTINGS_MAX_USERS) {
        lv_label_set_text_fmt(users_msg, "At most %d riders", SETTINGS_MAX_USERS);
        return;
    }
    open_name(true);
}

static void on_rename_user(lv_event_t *e) { open_name(false); }

static void on_delete_user(lv_event_t *e)
{
    settings_t s;
    app_get_settings(&s);
    if (!app_delete_user(settings_user(&s)->id)) {
        lv_label_set_text(users_msg, "The last rider can't be deleted");
        return;
    }
    open_users(NULL);
    lv_label_set_text(users_msg, "Deleted. Their rides stay on the SD card.");
}

static lv_obj_t *list(lv_obj_t *parent, int h)
{
    lv_obj_t *l = lv_list_create(parent);
    lv_obj_add_style(l, &st_card, 0);
    lv_obj_set_style_pad_all(l, 8, 0);
    lv_obj_set_style_pad_row(l, 8, 0);
    lv_obj_set_size(l, LV_PCT(100), h);
    return l;
}

// List row styled like a button, icon in the accent colour
static lv_obj_t *list_item(lv_obj_t *l, const char *icon, const char *text, lv_event_cb_t cb, void *data)
{
    lv_obj_t *b = lv_list_add_button(l, icon, text);
    lv_obj_add_style(b, &st_btn, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(b, 0), rgb(C_ACCENT), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
    navigable(b);
    return b;
}

static void build_users(void)
{
    scr_users = screen("Riders", true);
    users_list = list(scr_users, 268);
    lv_obj_t *r = row(scr_users);
    lv_obj_set_flex_grow(primary(button(r, LV_SYMBOL_PLUS " Add", 0, 60, on_add_user, NULL)), 3);
    lv_obj_set_flex_grow(button(r, "Rename", 0, 60, on_rename_user, NULL), 3);
    // Deleting a rider (and their paired HR strap) by accident would be annoying
    lv_obj_set_flex_grow(hold_button(r, "Hold to delete", 0, 60, on_delete_user, NULL), 4);
    users_msg = muted(scr_users, &lv_font_montserrat_16, "");
}

static void on_name_done(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) {
        settings_t s;
        app_get_settings(&s);
        user_t u = *settings_user(&s);
        const char *name = lv_textarea_get_text(name_ta);
        bool ok = settings_valid_name(name);
        if (ok && s_name_add) {
            ok = app_add_user(name);
        } else if (ok) {
            snprintf(u.name, sizeof(u.name), "%s", name);
            ok = app_edit_user(&u);
        }
        if (!ok) {
            lv_label_set_text(name_title, "Use 1-15 letters, digits or spaces");
            return;
        }
    }
    open_users(NULL);
}

static void build_name(void)
{
    scr_name = screen(NULL, false);
    name_title = label(scr_name, &lv_font_montserrat_20, "");
    name_ta = lv_textarea_create(scr_name);
    lv_obj_add_style(name_ta, &st_card, 0);
    lv_obj_set_style_text_font(name_ta, &lv_font_montserrat_28, 0);
    lv_textarea_set_one_line(name_ta, true);
    lv_textarea_set_max_length(name_ta, SETTINGS_NAME_LEN - 1);
    lv_obj_set_width(name_ta, LV_PCT(100));
    lv_obj_t *kb = lv_keyboard_create(scr_name);
    lv_obj_set_size(kb, LV_PCT(100), 300);
    lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(kb, 0, 0);
    lv_keyboard_set_textarea(kb, name_ta);
    // The keyboard's OK and close keys send READY / CANCEL
    lv_obj_add_event_cb(kb, on_name_done, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, on_name_done, LV_EVENT_CANCEL, NULL);
}

// ---- Workouts -----------------------------------------------------------------

static void show_preview(void);
static void open_adapt(lv_event_t *e);

static void open_preview(const char *path)
{
    if (!app_read_workout(path, &s_preview)) {
        lv_label_set_text(wo_msg, "Could not load this workout");
        return;
    }
    s_preview_path = path;
    s_preview_from = scr_workouts;
    show_preview();
}

static void open_ramp_test(lv_event_t *e)
{
    app_ramp_test(&s_preview);
    s_preview_path = NULL;
    s_preview_from = lv_screen_active();
    show_preview();
}

static void preview_captions(const char *const names[4], const char *const icons[4], const char *start)
{
    for (int i = 0; i < 4; i++) {
        lv_label_set_text(pv_cap[i], names[i]);
        lv_label_set_text(pv_icon[i], icons[i]);
    }
    lv_label_set_text(pv_start, start);
}

static const char *const PV_NAMES[4] = {"TIME", "AVG", "IF", "TSS"};
static const char *const PV_ICONS[4] = {I_TIME, I_BOLT, I_PERCENT, I_BARS};

// A .route: elevation profile, distance, climbing, steepest 100 m, highest point
static void open_route_preview(const char *path)
{
    if (!app_read_route(path, &s_route_pv)) {
        lv_label_set_text(wo_msg, "Could not load this route");
        return;
    }
    static const char *const names[4] = {"DISTANCE", "CLIMBING", "STEEPEST", "HIGHEST"};
    static const char *const icons[4] = {I_ROAD, I_LINE, I_PERCENT, I_BARS};
    preview_captions(names, icons, LV_SYMBOL_PLAY " Start route");
    route_info_t i;
    route_info(&s_route_pv, &i);
    lv_label_set_text(pv_title, s_route_pv.name);
    lv_label_set_text_fmt(pv_val[0], "%lu.%lu km", (unsigned long)(i.length_m / 1000), (unsigned long)(i.length_m % 1000 / 100));
    lv_label_set_text_fmt(pv_val[1], "%lu m", (unsigned long)i.climb_m);
    lv_label_set_text_fmt(pv_val[2], "%d.%d %%", i.max_grade / 100, i.max_grade % 100 / 10);
    lv_label_set_text_fmt(pv_val[3], "%ld m", (long)(i.max_cm / 100));
    lv_label_set_text(pv_note, "Free ride along the route: the trainer follows its gradient by distance "
                               "(-10 to +20 %), your virtual gears work as usual. Flat road after the end.");
    lv_label_set_text(pv_msg, "");
    s_pv_route = true;
    s_preview_path = path;
    s_preview_from = scr_workouts;
    s_pv_prof = (profile_t){.route = &s_route_pv, .pos_m = -1};
    lv_obj_invalidate(pv_profile);
    go(scr_preview);
}

// Fills the preview screen from s_preview and shows it
static void show_preview(void)
{
    s_pv_route = false;
    preview_captions(PV_NAMES, PV_ICONS, LV_SYMBOL_PLAY " Start workout");
    settings_t s;
    app_get_settings(&s);
    uint32_t ftp = settings_user(&s)->ftp_w;
    workout_est_t est;
    workout_estimate(&s_preview, &est);
    char t[16];
    fmt_time(t, sizeof(t), est.total_s);
    lv_label_set_text(pv_title, s_preview.name);
    lv_label_set_text(pv_val[0], t);
    lv_label_set_text_fmt(pv_val[1], "%lu W", (unsigned long)((ftp * est.avg_pm + 500) / 1000));
    lv_label_set_text_fmt(pv_val[2], "%u.%02u", est.if_pm / 1000, est.if_pm % 1000 / 10);
    lv_label_set_text_fmt(pv_val[3], "%u", (est.tss10 + 5) / 10);
    lv_label_set_text_fmt(pv_note, "%s, FTP %lu W  -  %u intervals  -  peak %lu W", settings_user(&s)->name,
                          (unsigned long)ftp, s_preview.n, (unsigned long)((ftp * est.max_pm + 500) / 1000));
    if (s_preview.ramp_test) {
        // Nobody rides it to the end: no average, IF or TSS
        uint16_t start, step;
        workout_ramp_steps(settings_user(&s)->weight_kg, &start, &step);
        for (int i = 1; i < 4; i++) {
            lv_label_set_text(pv_val[i], "-");
        }
        lv_label_set_text_fmt(pv_note, "From %u W, +%u W every minute until you can't hold it. "
                                       "FTP = 75 %% of your best minute. Based on %u kg.",
                              start, step, settings_user(&s)->weight_kg);
    }
    lv_label_set_text(pv_msg, "");
    s_pv_prof = (profile_t){.w = &s_preview, .pos_s = -1};
    lv_obj_invalidate(pv_profile);
    go(scr_preview);
}

static bool is_route(const char *path)
{
    size_t len = strlen(path);
    return len > 6 && strcasecmp(path + len - 6, ".route") == 0;
}

static void on_workout(lv_event_t *e)
{
    const char *path = lv_event_get_user_data(e);
    if (is_route(path)) {
        open_route_preview(path);
    } else {
        open_preview(path);
    }
}

static void on_preview_start(lv_event_t *e)
{
    if (s_pv_route) {
        if (!app_start_route_mem(&s_route_pv)) {
            lv_label_set_text(pv_msg, "Could not start this route");
        }
    } else if (s_preview_path ? !app_start_workout(s_preview_path) : !app_start_workout_mem(&s_preview)) {
        lv_label_set_text(pv_msg, "Could not start this workout");
    }
}

static void on_preview_back(lv_event_t *e) { go(s_preview_from); }

static void build_preview(void)
{
    scr_preview = screen(NULL, false);
    lv_obj_t *top = row(scr_preview);
    lv_obj_set_user_data(scr_preview, button(top, LV_SYMBOL_LEFT, 56, 44, on_preview_back, NULL));
    pv_title = grow(label(top, &lv_font_montserrat_20, ""));
    lv_label_set_long_mode(pv_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(pv_title, 0);
    pv_profile = profile(scr_preview, &s_pv_prof, 190);
    lv_obj_t *r = row(scr_preview);
    for (int i = 0; i < 4; i++) {
        pv_val[i] = metric_font(r, PV_ICONS[i], PV_NAMES[i], i == 1 ? C_POWER : C_MUTED, NULL, &lv_font_montserrat_20);
        // Card: icon + caption row, then the value row (metric_font)
        lv_obj_t *cap_row = lv_obj_get_child(lv_obj_get_parent(lv_obj_get_parent(pv_val[i])), 0);
        pv_icon[i] = lv_obj_get_child(cap_row, 0);
        pv_cap[i] = lv_obj_get_child(cap_row, 1);
    }
    pv_note = muted(scr_preview, &lv_font_montserrat_14, "");
    lv_obj_set_width(pv_note, LV_PCT(100));
    lv_label_set_long_mode(pv_note, LV_LABEL_LONG_WRAP);
    pv_msg = label(scr_preview, &lv_font_montserrat_16, "");
    lv_obj_set_style_text_color(pv_msg, rgb(C_DANGER), 0);
    lv_obj_t *start = primary(button(scr_preview, LV_SYMBOL_PLAY " Start workout", LV_PCT(100), 64, on_preview_start, NULL));
    lv_obj_add_flag(start, NAV_START);
    pv_start = lv_obj_get_child(start, 0);
}

// Training-plan workouts ("Plan 2026-11-02 ...", written by the web app) first, by date; then A-Z
static int cmp_workout(const void *a, const void *b)
{
    const char *x = strrchr(a, '/') + 1, *y = strrchr(b, '/') + 1;
    bool px = strncmp(x, "Plan ", 5) == 0, py = strncmp(y, "Plan ", 5) == 0;
    return px != py ? py - px : strcasecmp(x, y);
}

static void open_workouts(lv_event_t *e)
{
    lv_obj_clean(wo_list);
    lv_label_set_text(wo_msg, "");
    DIR *d = opendir(WORKOUT_DIR);
    int n = 0;
    struct dirent *de;
    while (d && n < MAX_LIST && (de = readdir(d))) {
        size_t len = strlen(de->d_name);
        // Longer names would be cut off in s_paths (and are refused by the upload page too)
        bool zwo = len >= 5 && strcasecmp(de->d_name + len - 4, ".zwo") == 0;
        if (de->d_name[0] == '.' || len > 60 || !(zwo || is_route(de->d_name))) {
            continue;
        }
        snprintf(s_paths[n++], sizeof(s_paths[0]), WORKOUT_DIR "/%.60s", de->d_name);
    }
    if (d) {
        closedir(d);
    }
    qsort(s_paths, n, sizeof(s_paths[0]), cmp_workout);
    list_item(wo_list, LV_SYMBOL_CHARGE, "FTP ramp test", open_ramp_test, NULL);
    list_item(wo_list, LV_SYMBOL_SHUFFLE, "Zone ride: HR zone, power, average", open_adapt, NULL);
    for (int i = 0; i < n; i++) {
        char name[64];
        snprintf(name, sizeof(name), "%s", strrchr(s_paths[i], '/') + 1);
        bool route = is_route(name);
        name[strlen(name) - (route ? 6 : 4)] = 0;  // without ".zwo" / ".route"
        for (char *c = name; *c; c++) {
            *c = *c == '_' ? ' ' : *c;
        }
        list_item(wo_list, route ? LV_SYMBOL_GPS : LV_SYMBOL_PLAY, name, on_workout, s_paths[i]);
    }
    if (n == 0) {
        lv_label_set_text(wo_msg, "No workouts or routes in /workouts on the SD card yet");
    }
    go(scr_workouts);
}

static void build_workouts(void)
{
    scr_workouts = screen("Workouts", true);
    wo_list = list(scr_workouts, 340);
    wo_msg = muted(scr_workouts, &lv_font_montserrat_16, "");
}

// ---- Settings -----------------------------------------------------------------

static void update_settings(void)
{
    settings_t s;
    app_get_settings(&s);
    const user_t *u = settings_user(&s);
    lv_label_set_text_fmt(set_title, "Settings: %s", u->name);
    lv_label_set_text_fmt(set_ftp, "%u W", u->ftp_w);
    lv_label_set_text_fmt(set_erg, "%u W", u->max_erg_w);
    lv_label_set_text_fmt(set_weight, "%u kg", u->weight_kg);
    lv_label_set_text_fmt(set_hr, "%u bpm", u->max_hr);
}

// One handler for all +/- buttons: user data = field * 1000 + 500 + step
enum { F_FTP, F_ERG, F_WEIGHT, F_HR };
#define STEP(field, step) ((void *)(intptr_t)((field) * 1000 + 500 + (step)))

static void on_step(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    int field = v / 1000, step = v % 1000 - 500;
    settings_t s;
    app_get_settings(&s);
    user_t u = *settings_user(&s);
    if (field == F_FTP) {
        u.ftp_w = (uint16_t)(u.ftp_w + step);
    } else if (field == F_ERG) {
        u.max_erg_w = (uint16_t)(u.max_erg_w + step);
    } else if (field == F_WEIGHT) {
        u.weight_kg = (uint8_t)(u.weight_kg + step);
    } else {
        u.max_hr = (uint8_t)(u.max_hr + step);
    }
    app_edit_user(&u);  // refuses values out of range
    update_settings();
}

// Card: caption + value on the left, - and + on the right
static void stepper(lv_obj_t *parent, lv_obj_t **value, const char *name, lv_event_cb_t cb, int field, int step)
{
    lv_obj_t *c = card(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(c, 6, 0);
    lv_obj_t *col = grow(box(c, LV_FLEX_FLOW_COLUMN));
    lv_obj_set_style_pad_row(col, 0, 0);
    caption(col, name, C_MUTED);
    *value = label(col, &lv_font_montserrat_28, "");
    button(c, LV_SYMBOL_MINUS, 76, 52, cb, STEP(field, -step));
    button(c, LV_SYMBOL_PLUS, 76, 52, cb, STEP(field, step));
}

static void on_forget(lv_event_t *e)
{
    ble_mgr_forget();
    lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0), "Forgotten");
}

static void open_settings(lv_event_t *e)
{
    update_settings();
    go(scr_settings);
}

static void build_settings(void)
{
    scr_settings = screen(NULL, true);
    set_title = label(lv_obj_get_child(scr_settings, 0), &lv_font_montserrat_28, "");
    stepper(scr_settings, &set_ftp, "FTP", on_step, F_FTP, 5);
    stepper(scr_settings, &set_erg, "MAX ERG", on_step, F_ERG, 50);
    stepper(scr_settings, &set_weight, "WEIGHT", on_step, F_WEIGHT, 1);
    stepper(scr_settings, &set_hr, "MAX HEART RATE", on_step, F_HR, 1);
    lv_obj_t *r = row(scr_settings);
    grow(button(r, "Forget paired devices", 0, 52, on_forget, NULL));
    muted(r, &lv_font_montserrat_14, esp_app_get_description()->version);
}

// ---- Zone ride (adaptive quick start) ---------------------------------------------
// Warm-up, one adaptive part with these goals, cool-down. "Next" shows it on the
// preview screen, which starts it. The ride steers the power (ride.c, goal_t). The
// HR zone is stored as % of max HR, so it follows the rider's max HR.

enum { AD_DUR, AD_POWER, AD_HR };

static void update_adapt(void)
{
    settings_t s;
    app_get_settings(&s);
    const user_t *u = settings_user(&s);
    lv_label_set_text_fmt(ad_dur, "%d min", s_adapt.min);
    lv_label_set_text_fmt(ad_power, "%d W  Z%d", s_adapt.watts, metrics_zone(s_adapt.watts, u->ftp_w) + 1);
    if (s_adapt.hr_zone) {
        uint8_t lo, hi;
        metrics_hr_zone_pct(s_adapt.hr_zone - 1, &lo, &hi);
        lv_label_set_text_fmt(ad_hr, "Z%d  %u-%u bpm", s_adapt.hr_zone, metrics_hr_of_pct(lo, u->max_hr),
                              metrics_hr_of_pct(hi, u->max_hr));
    } else {
        lv_label_set_text(ad_hr, "Off");
    }
    lv_label_set_text(ad_avg_label, s_adapt.hold_avg ? LV_SYMBOL_OK " Hold average" : "Hold average: off");
}

static void on_adapt_step(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    int field = v / 1000, step = v % 1000 - 500;
    if (field == AD_DUR) {
        s_adapt.min = LV_CLAMP(20, s_adapt.min + step, 180);
    } else if (field == AD_POWER) {
        s_adapt.watts = LV_CLAMP(50, s_adapt.watts + step, RIDE_ERG_CAP_W);  // Max ERG still applies in the ride
    } else {
        s_adapt.hr_zone = LV_CLAMP(0, s_adapt.hr_zone + (step > 0 ? 1 : -1), METRICS_N_HR_ZONES);  // 0: off
    }
    update_adapt();
}

static void on_adapt_avg(lv_event_t *e)
{
    s_adapt.hold_avg = !s_adapt.hold_avg;
    update_adapt();
}

static void open_adapt(lv_event_t *e)
{
    if (s_adapt.min == 0) {
        settings_t s;
        app_get_settings(&s);
        const user_t *u = settings_user(&s);
        s_adapt.min = 60;
        s_adapt.watts = (u->ftp_w * 65 / 100 + 2) / 5 * 5;    // endurance
        s_adapt.hr_zone = 2;                                   // 60-70 % of max HR
    }
    update_adapt();
    go(scr_adapt);
}

static void on_adapt_next(lv_event_t *e)
{
    settings_t s;
    app_get_settings(&s);
    uint32_t ftp = settings_user(&s)->ftp_w;
    uint16_t pm = (uint16_t)LV_MIN(s_adapt.watts * 1000 / ftp, 3000);
    workout_t *w = &s_preview;
    memset(w, 0, sizeof(*w));
    if (s_adapt.hr_zone) {
        snprintf(w->name, sizeof(w->name), "HR zone %d, %d min", s_adapt.hr_zone, s_adapt.min);
    } else {
        snprintf(w->name, sizeof(w->name), "Zone %d, %d min", metrics_zone(s_adapt.watts, (uint16_t)ftp) + 1,
                 s_adapt.min);
    }
    uint8_t hr_lo = 0, hr_hi = 0;
    if (s_adapt.hr_zone) {
        metrics_hr_zone_pct(s_adapt.hr_zone - 1, &hr_lo, &hr_hi);
    }
    uint16_t easy = (uint16_t)LV_MIN(450, pm * 7 / 10);
    w->seg[0] = (segment_t){.dur_s = 600, .p_start = easy, .p_end = pm, .kind = SEG_ERG};
    w->seg[1] = (segment_t){.dur_s = (uint16_t)((s_adapt.min - 15) * 60), .p_start = pm, .p_end = pm, .kind = SEG_ERG};
    w->seg[1].goal = workout_add_goal(w, pm, (goal_t){.hr_lo = hr_lo, .hr_hi = hr_hi, .hr_pct = true,
                                                      .hold_avg = s_adapt.hold_avg});
    w->seg[2] = (segment_t){.dur_s = 300, .p_start = pm, .p_end = easy, .kind = SEG_ERG};
    w->n = 3;
    w->total_s = 600 + w->seg[1].dur_s + 300;
    s_preview_path = NULL;
    s_preview_from = scr_adapt;
    show_preview();
}

static void on_adapt_back(lv_event_t *e) { go(scr_workouts); }

static void build_adapt(void)
{
    scr_adapt = screen(NULL, false);
    lv_obj_t *top = row(scr_adapt);
    lv_obj_set_user_data(scr_adapt, button(top, LV_SYMBOL_LEFT, 56, 44, on_adapt_back, NULL));
    label(top, &lv_font_montserrat_28, "Zone ride");
    stepper(scr_adapt, &ad_dur, "DURATION", on_adapt_step, AD_DUR, 5);
    stepper(scr_adapt, &ad_power, "POWER", on_adapt_step, AD_POWER, 5);
    stepper(scr_adapt, &ad_hr, "HEART-RATE ZONE", on_adapt_step, AD_HR, 1);
    lv_obj_t *r = row(scr_adapt);
    ad_avg_label = lv_obj_get_child(grow(button(r, "", 0, 56, on_adapt_avg, NULL)), 0);
    primary(grow(button(r, "Next " LV_SYMBOL_RIGHT, 0, 56, on_adapt_next, NULL)));
    lv_obj_t *note = muted(scr_adapt, &lv_font_montserrat_14,
                           "HR zones are % of your max HR. HR steers the power slowly, within -30 / +15 % "
                           "of it. Hold average makes up for easier minutes later. Max ERG always applies.");
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
}

// ---- Wi-Fi --------------------------------------------------------------------

static void update_wifi(void)
{
    char buf[160];
    web_status(buf, sizeof(buf));
    lv_label_set_text(wifi_status, buf);
    bool on = web_running();
    lv_label_set_text(wifi_btn_label, on ? LV_SYMBOL_CLOSE " Turn off" : LV_SYMBOL_WIFI " Turn on");
    if (on) {
        lv_obj_remove_style(wifi_btn, &st_primary, 0);
    } else {
        primary(wifi_btn);
    }
    show(wifi_ota_btn, on);
    lv_label_set_text(wifi_ota_label, web_ota_allowed() ? LV_SYMBOL_OK " Update allowed (5 min)"
                                                        : LV_SYMBOL_DOWNLOAD " Allow firmware update");
}

// Time zone: only used for ride file names and the FIT file's local time
static void update_tz(void)
{
    settings_t s;
    app_get_settings(&s);
    int i = tz_preset_index(s.tz);
    lv_label_set_text(wifi_tz, i >= 0 ? tz_presets[i].name : s.tz);
    time_t now = time(NULL);
    if (now > 1735689600) {  // clock set
        int64_t local = (int64_t)now + tz_offset(s.tz, (uint32_t)now);
        lv_label_set_text_fmt(wifi_time, "Local time %02d:%02d", (int)(local / 3600 % 24), (int)(local / 60 % 60));
    } else {
        lv_label_set_text(wifi_time, "Clock not set yet");
    }
}

static void on_tz(lv_event_t *e)
{
    int step = (int)(intptr_t)lv_event_get_user_data(e);
    settings_t s;
    app_get_settings(&s);
    int i = tz_preset_index(s.tz);
    i = i < 0 ? 0 : (i + step + tz_n_presets) % tz_n_presets;
    app_set_tz(tz_presets[i].posix);
    update_tz();
}

static void on_allow_ota(lv_event_t *e)
{
    web_allow_ota();
    update_wifi();
}

static void on_wifi_toggle(lv_event_t *e)
{
    if (web_running()) {
        web_stop();
    } else {
        app_web_start();
    }
    update_wifi();
}

static void open_wifi(lv_event_t *e)
{
    update_wifi();
    update_tz();
    go(scr_wifi);
}

static void build_wifi(void)
{
    scr_wifi = screen("Wi-Fi and time", true);
    lv_obj_t *c = card(scr_wifi, LV_FLEX_FLOW_COLUMN);
    caption(c, "STATUS", C_MUTED);
    wifi_status = label(c, &lv_font_montserrat_20, "");
    wifi_btn = button(scr_wifi, "", LV_PCT(100), 64, on_wifi_toggle, NULL);
    wifi_btn_label = lv_obj_get_child(wifi_btn, 0);
    wifi_ota_btn = button(scr_wifi, "", LV_PCT(100), 64, on_allow_ota, NULL);
    wifi_ota_label = lv_obj_get_child(wifi_ota_btn, 0);
    muted(scr_wifi, &lv_font_montserrat_16, "Wi-Fi turns off automatically when a ride starts.");

    lv_obj_t *t = card(scr_wifi, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(t, 6, 0);
    lv_obj_t *col = grow(box(t, LV_FLEX_FLOW_COLUMN));
    lv_obj_set_style_pad_row(col, 0, 0);
    caption(col, "TIME ZONE", C_MUTED);
    wifi_tz = label(col, &lv_font_montserrat_20, "");
    lv_label_set_long_mode(wifi_tz, LV_LABEL_LONG_DOT);
    lv_obj_set_width(wifi_tz, LV_PCT(100));
    wifi_time = muted(col, &lv_font_montserrat_14, "");
    button(t, LV_SYMBOL_LEFT, 76, 52, on_tz, (void *)(intptr_t)-1);
    button(t, LV_SYMBOL_RIGHT, 76, 52, on_tz, (void *)(intptr_t)1);
}

// ---- Ride ---------------------------------------------------------------------

static void build_ride(void)
{
    scr_ride = screen(NULL, false);
    lv_obj_set_style_pad_row(scr_ride, 8, 0);  // tighter: everything must fit with the workout strip
    lv_obj_set_style_pad_top(scr_ride, 8, 0);
    lv_obj_set_style_pad_bottom(scr_ride, 8, 0);
    lv_obj_t *top = row(scr_ride);
    r_time = grow(label(top, &lv_font_montserrat_28, ""));
    r_info = muted(top, &lv_font_montserrat_20, "");
    r_profile = profile(scr_ride, &s_ride_prof, 34);  // workouts only

    // Power: the number that matters most, on its own card
    r_power_card = card(scr_ride, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(r_power_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    r_power_cap = icon_caption(r_power_card, I_BOLT, "", C_POWER);
    lv_obj_t *pr = row(r_power_card);
    lv_obj_set_size(pr, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(pr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(pr, 6, 0);
    r_power = label(pr, &font_digits_100, "");
    lv_obj_set_style_pad_bottom(muted(pr, &lv_font_montserrat_28, "W"), 16, 0);
    r_target = muted(r_power_card, &lv_font_montserrat_20, "");
    lv_obj_set_style_pad_ver(r_power_card, 6, 0);
    lv_obj_set_style_pad_row(r_power_card, 2, 0);

    lv_obj_t *mid = row(scr_ride);
    r_cad = metric(mid, I_CADENCE, "CADENCE", C_CAD, "rpm");
    r_hr = metric(mid, I_HEART, "HEART RATE", C_HR, "bpm");
    r_hr_cap = lv_obj_get_child(lv_obj_get_child(lv_obj_get_parent(lv_obj_get_parent(r_hr)), 0), 1);
    r_speed = metric(mid, I_SPEED, "SPEED", C_SPEED, "km/h");

    r_stats = muted(scr_ride, &lv_font_montserrat_16, "");
    lv_obj_set_width(r_stats, LV_PCT(100));
    lv_obj_set_style_text_align(r_stats, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *b1 = row(scr_ride);
    grow(button(b1, LV_SYMBOL_MINUS, 0, 60, on_action, (void *)ACT_MINUS));
    lv_obj_t *pause = grow(button(b1, "", 0, 60, on_action, (void *)ACT_PAUSE_TOGGLE));
    lv_obj_set_flex_grow(pause, 2);
    r_pause_label = lv_obj_get_child(pause, 0);
    grow(button(b1, "Lap", 0, 60, on_action, (void *)ACT_LAP));
    grow(button(b1, LV_SYMBOL_PLUS, 0, 60, on_action, (void *)ACT_PLUS));

    lv_obj_t *b2 = row(scr_ride);
    r_grade_minus = grow(button(b2, LV_SYMBOL_DOWN " Grade", 0, 56, on_action, (void *)ACT_GRADE_MINUS));
    r_skip = grow(button(b2, LV_SYMBOL_NEXT " Skip", 0, 56, on_action, (void *)ACT_SKIP));
    // Stop needs a long press so it can't be hit by accident mid-interval
    lv_obj_set_flex_grow(hold_button(b2, "Hold to stop", 0, 56, on_action, (void *)ACT_STOP), 2);
    r_grade_plus = grow(button(b2, LV_SYMBOL_UP " Grade", 0, 56, on_action, (void *)ACT_GRADE_PLUS));
}

// "12 s ahead of your best", "1:05 behind your best"; finished: "23 s faster than your best"
static void race_text(char *buf, size_t n, const ride_view_t *v)
{
    int32_t s = v->race_gap_s < 0 ? -v->race_gap_s : v->race_gap_s;
    char t[16];
    if (s < 60) {
        snprintf(t, sizeof(t), "%ld s", (long)s);
    } else {
        snprintf(t, sizeof(t), "%ld:%02ld", (long)(s / 60), (long)(s % 60));
    }
    if (v->race_gap_s == 0) {
        snprintf(buf, n, v->route_done ? "as fast as your best" : "level with your best");
    } else if (v->route_done) {
        snprintf(buf, n, "%s %s than your best", t, v->race_gap_s > 0 ? "faster" : "slower");
    } else {
        snprintf(buf, n, "%s %s your best", t, v->race_gap_s > 0 ? "ahead of" : "behind");
    }
}

static void update_ride(const ride_view_t *v)
{
    char t[16];
    fmt_time(t, sizeof(t), v->timer_s);
    lv_label_set_text(r_time, t);

    bool erg = v->mode == MODE_ERG;
    bool paused = v->state == RIDE_PAUSED;
    bool in_workout = v->has_workout && !v->wo_done;
    show(r_profile, in_workout || v->route);
    if (v->route) {
        const route_t *rt = app_route();
        if (rt && (s_ride_prof.route != rt || s_ride_prof.pos_m != (int32_t)v->route_dist_m)) {
            s_ride_prof = (profile_t){.route = rt, .pos_m = (int32_t)v->route_dist_m};
            lv_obj_invalidate(r_profile);
        }
    } else if (in_workout) {
        // Workout time from the segment position (Skip jumps ahead of the ride timer)
        const workout_t *w = app_workout();
        int32_t pos = v->seg_idx < w->n ? (int32_t)(workout_seg_start(w, v->seg_idx) + w->seg[v->seg_idx].dur_s -
                                                    v->seg_left_s)
                                        : -1;
        if (s_ride_prof.w != w || s_ride_prof.pos_s != pos) {
            s_ride_prof = (profile_t){.w = w, .pos_s = pos};
            lv_obj_invalidate(r_profile);
        }
    }
    if (in_workout && v->ramp_step) {
        lv_label_set_text_fmt(r_info, "FTP test  -  step %u", v->ramp_step);
    } else if (in_workout) {
        char left[16];
        fmt_time(left, sizeof(left), v->seg_left_s);
        lv_label_set_text_fmt(r_info, "Interval %d/%d  %s", v->seg_idx + 1, v->seg_count, left);
    } else if (v->route && !v->route_done) {
        uint32_t left = v->route_len_m > v->route_dist_m ? v->route_len_m - v->route_dist_m : 0;
        lv_label_set_text_fmt(r_info, "Gear %u/%u  -  %lu.%lu km to go", v->gear, v->gears, (unsigned long)(left / 1000),
                              (unsigned long)(left % 1000 / 100));
    } else if (v->route) {
        lv_label_set_text_fmt(r_info, "Gear %u/%u  -  route done", v->gear, v->gears);
    } else {
        lv_label_set_text_fmt(r_info, "Gear %u/%u", v->gear, v->gears);
    }

    // Paused: orange frame and caption, so it's obvious from the saddle
    if (v->auto_paused) {
        lv_label_set_text(r_power_cap, "PAUSED  -  PEDAL TO RESUME");
    } else if (paused) {
        lv_label_set_text(r_power_cap, "PAUSED");
    } else if (v->zone >= 0) {
        // Zone of the shown power and the time spent in it this ride
        char z[16];
        fmt_time(z, sizeof(z), v->zone_s);
        lv_label_set_text_fmt(r_power_cap, "POWER  -  Z%d  %s", v->zone + 1, z);
    } else {
        lv_label_set_text(r_power_cap, "POWER");
    }
    lv_obj_set_style_text_color(r_power_cap, rgb(paused ? C_WARN : C_POWER), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(lv_obj_get_parent(r_power_cap), 0), rgb(paused ? C_WARN : C_POWER), 0);
    lv_obj_set_style_border_width(r_power_card, paused ? 3 : 1, 0);
    lv_obj_set_style_border_color(r_power_card, rgb(paused ? C_WARN : C_LINE), 0);

    // ERG: the number turns green when within 5 % (at least 5 W) of the target
    uint32_t color = C_TEXT;
    if (v->power_w >= 0) {
        lv_label_set_text_fmt(r_power, "%d", v->power_w);
        int tol = v->target_w / 20 > 5 ? v->target_w / 20 : 5;
        if (erg && !paused && v->target_w > 0 && abs(v->power_w - v->target_w) <= tol) {
            color = C_OK;
        }
    } else {
        lv_label_set_text(r_power, "-");
    }
    lv_obj_set_style_text_color(r_power, rgb(color), 0);

    if (erg && v->adaptive) {
        // Adaptive: the steered target and the goals
        char goals[48] = "";
        if (v->hold_avg) {
            snprintf(goals, sizeof(goals), "  -  avg %d of %d W", v->seg_avg_w < 0 ? 0 : v->seg_avg_w, v->avg_goal_w);
        }
        if (v->hr_hi && v->hr_goal_zone >= 0) {
            size_t n = strlen(goals);
            snprintf(goals + n, sizeof(goals) - n, "  -  HR Z%d %u-%u", v->hr_goal_zone + 1, v->hr_lo, v->hr_hi);
        } else if (v->hr_hi) {
            size_t n = strlen(goals);
            snprintf(goals + n, sizeof(goals) - n, "  -  HR %u-%u", v->hr_lo, v->hr_hi);
        }
        lv_label_set_text_fmt(r_target, "Target %d W%s", v->target_w, goals);
    } else if (erg && v->ramp_step) {
        lv_label_set_text_fmt(r_target, "Target %d W  -  hold it as long as you can", v->target_w);
    } else if (erg) {
        if (v->cadence_target) {
            lv_label_set_text_fmt(r_target, "Target %d W  -  %u %%  -  %u rpm", v->target_w, v->intensity,
                                  v->cadence_target);
        } else {
            lv_label_set_text_fmt(r_target, "Target %d W  -  %u %%", v->target_w, v->intensity);
        }
    } else if (v->race) {
        // Racing the best earlier ride on this route: the gap instead of the sent grade
        char gap[40];
        race_text(gap, sizeof(gap), v);
        lv_label_set_text_fmt(r_target, "Road %s%d.%d %%  -  %s", v->base_grade < 0 ? "-" : "+", abs(v->base_grade) / 100,
                              abs(v->base_grade % 100) / 10, gap);
    } else {
        lv_label_set_text_fmt(r_target, "Road %+d.%d %%  -  sent %+d.%02d %%", v->base_grade / 100,
                              abs(v->base_grade % 100) / 10, v->grade / 100, abs(v->grade % 100));
    }
    lv_obj_set_style_text_color(r_target, rgb(!v->race || erg ? C_MUTED : v->race_gap_s >= 0 ? C_OK : C_WARN), 0);

    if (v->cadence_rpm10 >= 0) {
        lv_label_set_text_fmt(r_cad, "%d", (v->cadence_rpm10 + 5) / 10);
    } else {
        lv_label_set_text(r_cad, "-");
    }
    if (v->hr_bpm > 0) {
        lv_label_set_text_fmt(r_hr, "%d", v->hr_bpm);
    } else {
        lv_label_set_text(r_hr, "-");
    }
    // HR zone (% of the rider's max HR)
    if (v->hr_zone >= 0) {
        lv_label_set_text_fmt(r_hr_cap, "HR  -  Z%d", v->hr_zone + 1);
    } else {
        lv_label_set_text(r_hr_cap, "HEART RATE");
    }
    // Adaptive goals: green in range, orange outside
    bool hr_goal = v->adaptive && v->hr_hi && v->hr_bpm > 0, cad_goal = v->adaptive && v->cad_hi && v->cadence_rpm10 > 0;
    lv_obj_set_style_text_color(r_hr, rgb(!hr_goal ? C_TEXT : v->hr_cue ? C_WARN : C_OK), 0);
    lv_obj_set_style_text_color(r_cad, rgb(!cad_goal ? C_TEXT : v->cad_cue ? C_WARN : C_OK), 0);
    if (v->speed_kmh100 >= 0) {
        lv_label_set_text_fmt(r_speed, "%ld.%ld", (long)(v->speed_kmh100 / 100), (long)(v->speed_kmh100 % 100 / 10));
    } else {
        lv_label_set_text(r_speed, "-");
    }
    const ride_last_block_t *b = &v->last_blk;
    if (b->kind != BLOCK_NONE) {
        // A lap or interval just ended: how it went, for a few seconds
        char dur[16], hr[16] = "";
        fmt_time(dur, sizeof(dur), b->s);
        if (b->avg_hr > 0) {
            snprintf(hr, sizeof(hr), "    %d bpm", b->avg_hr);
        }
        lv_label_set_text_fmt(r_stats, "%s %u done:  %s    %d W avg%s", b->kind == BLOCK_LAP ? "Lap" : "Interval",
                              b->no, dur, b->avg_w, hr);
    } else {
        lv_label_set_text_fmt(r_stats, "%lu.%lu km    Avg %d W    NP %d W    %lu kJ    Lap %u",
                              (unsigned long)(v->sum.distance_m / 1000),
                              (unsigned long)(v->sum.distance_m % 1000 / 100), v->sum.avg_w, v->sum.np_w,
                              (unsigned long)v->sum.kj, v->lap + 1);
    }
    lv_obj_set_style_text_color(r_stats, rgb(b->kind != BLOCK_NONE ? C_TEXT : C_MUTED), 0);

    lv_label_set_text(r_pause_label, paused ? LV_SYMBOL_PLAY " Resume" : LV_SYMBOL_PAUSE " Pause");
    show(r_grade_minus, !erg && !v->route);  // a route sets the grade itself
    show(r_grade_plus, !erg && !v->route);
    show(r_skip, v->has_workout && !v->wo_done);
}

// ---- Summary ------------------------------------------------------------------

static void on_ftp_btn(lv_event_t *e)
{
    ride_view_t v;
    app_status_t st;
    app_get_view(&v, &st);
    if (st.ftp_prev_w) {
        app_keep_old_ftp();
    } else {
        app_accept_ftp();
    }
}

static void on_max_hr_btn(lv_event_t *e) { app_accept_max_hr(); }

static void build_summary(void)
{
    scr_summary = screen(NULL, false);
    lv_obj_t *top = row(scr_summary);
    grow(label(top, &lv_font_montserrat_28, "Ride finished"));
    sum_saved = label(top, &lv_font_montserrat_16, "");

    static const char *const names[9] = {"TIME", "DISTANCE", "ENERGY", "AVG POWER", "NP",
                                         "MAX POWER", "TSS", "IF", "AVG HR"};
    static const uint32_t colors[9] = {C_MUTED, C_SPEED, C_POWER, C_POWER, C_POWER, C_POWER, C_MUTED, C_MUTED, C_HR};
    static const char *const icons[9] = {I_TIME, I_ROAD, I_FIRE, I_BOLT, I_LINE, I_BOLT, I_BARS, I_PERCENT, I_HEART};
    for (int r = 0; r < 3; r++) {
        lv_obj_t *rw = row(scr_summary);
        for (int c = 0; c < 3; c++) {
            sum_val[r * 3 + c] = metric(rw, icons[r * 3 + c], names[r * 3 + c], colors[r * 3 + c], NULL);
        }
    }
    sum_note = muted(scr_summary, &lv_font_montserrat_16, "");

    lv_obj_t *b = row(scr_summary);
    sum_ftp_btn = grow(button(b, "", 0, 64, on_ftp_btn, NULL));
    lv_obj_set_flex_grow(sum_ftp_btn, 3);
    sum_ftp_label = lv_obj_get_child(sum_ftp_btn, 0);
    sum_hr_btn = grow(button(b, "", 0, 64, on_max_hr_btn, NULL));
    lv_obj_set_flex_grow(sum_hr_btn, 3);
    sum_hr_label = lv_obj_get_child(sum_hr_btn, 0);
    lv_obj_set_flex_grow(primary(button(b, LV_SYMBOL_OK " Done", 0, 64, on_action, (void *)ACT_DISMISS)), 2);
}

static void update_summary(const ride_view_t *v, const app_status_t *st)
{
    const metrics_summary_t *m = &v->sum;
    char t[16];
    fmt_time(t, sizeof(t), m->duration_s);
    lv_label_set_text(sum_val[0], t);
    lv_label_set_text_fmt(sum_val[1], "%lu.%lu km", (unsigned long)(m->distance_m / 1000),
                          (unsigned long)(m->distance_m % 1000 / 100));
    lv_label_set_text_fmt(sum_val[2], "%lu kJ", (unsigned long)m->kj);
    lv_label_set_text_fmt(sum_val[3], "%d W", m->avg_w);
    lv_label_set_text_fmt(sum_val[4], "%d W", m->np_w);
    lv_label_set_text_fmt(sum_val[5], "%d W", m->max_w);
    lv_label_set_text_fmt(sum_val[6], "%u.%u", m->tss10 / 10, m->tss10 % 10);
    lv_label_set_text_fmt(sum_val[7], "%u.%02u", m->if_1000 / 1000, m->if_1000 % 1000 / 10);
    if (m->avg_hr > 0) {
        lv_label_set_text_fmt(sum_val[8], "%d", m->avg_hr);
    } else {
        lv_label_set_text(sum_val[8], "-");
    }

    lv_label_set_text(sum_saved, st->saving ? "Saving..." : st->saved ? LV_SYMBOL_OK " Saved" : LV_SYMBOL_WARNING " Not saved");
    lv_obj_set_style_text_color(sum_saved, rgb(st->saving ? C_MUTED : st->saved ? C_OK : C_DANGER), 0);
    if (!st->saving && !st->saved) {
        lv_label_set_text(sum_note, "Shorter than 1 min, or SD card problem.");
    } else if (st->ftp_test && st->ftp_test_w) {
        lv_label_set_text_fmt(sum_note, "FTP test: best minute %d W, your FTP is now %u W.", st->ftp_test_60s_w,
                              st->ftp_test_w);
    } else if (st->ftp_test) {
        lv_label_set_text(sum_note, st->saving ? "" : "FTP test: under 2 min of the ramp, no result.");
    } else if (v->race && v->route_done) {
        char gap[40];
        race_text(gap, sizeof(gap), v);
        lv_label_set_text_fmt(sum_note, "Route: %s%s", gap, v->race_gap_s > 0 ? ". New best!" : ".");
    } else if (v->route_done) {
        lv_label_set_text(sum_note, "Route done. Next time you race this ride.");
    }
    show(sum_note, !st->saving && (!st->saved || st->ftp_test || v->route_done));

    // 95 % of this ride's best 20 min beat the FTP: offer it, never apply it silently.
    // A test's result is already applied: offer the old value back.
    settings_t s;
    app_get_settings(&s);
    bool ftp = st->ftp_suggest_w > 0 || st->ftp_prev_w > 0, hr = st->max_hr_suggest > 0;
    show(sum_ftp_btn, ftp);
    if (st->ftp_prev_w) {
        lv_label_set_text_fmt(sum_ftp_label, hr ? LV_SYMBOL_LOOP " FTP %u W" : LV_SYMBOL_LOOP " Keep old FTP %u W",
                              st->ftp_prev_w);
    } else {
        lv_label_set_text_fmt(sum_ftp_label, hr ? "%s FTP %u W" : "%s Set FTP to %u W",
                              st->ftp_suggest_w > settings_user(&s)->ftp_w ? LV_SYMBOL_UP : LV_SYMBOL_DOWN,
                              st->ftp_suggest_w);
    }
    // This ride's 10 s HR peak beat the max HR the HR zones use: offer it, never apply it silently
    show(sum_hr_btn, hr);
    lv_label_set_text_fmt(sum_hr_label, ftp ? LV_SYMBOL_UP " Max HR %u" : LV_SYMBOL_UP " Set max HR to %u bpm",
                          st->max_hr_suggest);
}

// ---- Screens + refresh --------------------------------------------------------

static lv_obj_t *screen(const char *title, bool back)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_add_style(s, &st_screen, 0);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (title || back) {
        lv_obj_t *r = row(s);
        if (back) {
            lv_obj_set_user_data(s, button(r, LV_SYMBOL_LEFT, 56, 44, on_back, NULL));  // B presses it
        }
        if (title) {
            label(r, &lv_font_montserrat_28, title);
        }
    }
    return s;
}

// ---- Controller navigation ----------------------------------------------------
// Arrows move a highlight over the buttons of the active screen, A presses it, B presses
// the screen's back button (its user_data). Keys arrive from the control task through a
// single-producer ring and are handled here, in the LVGL task.

#define NAV_MAX 48
#define NAV_RING 8

static lv_obj_t *s_focus;  // compared only, never dereferenced: list rows get deleted
static uint8_t s_ring[NAV_RING];
static atomic_uint s_ring_head, s_ring_tail;

static void nav_collect(lv_obj_t *o, lv_obj_t **list, int *n)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (lv_obj_has_flag(o, NAV_ITEM) && *n < NAV_MAX) {
        list[(*n)++] = o;
    }
    uint32_t c = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < c; i++) {
        nav_collect(lv_obj_get_child(o, i), list, n);
    }
}

static int nav_items(lv_obj_t **list, int *cur)
{
    int n = 0;
    nav_collect(lv_screen_active(), list, &n);
    *cur = -1;
    for (int i = 0; i < n; i++) {
        if (list[i] == s_focus) {
            *cur = i;
        }
    }
    return n;
}

static void nav_clear(void)
{
    lv_obj_t *list[NAV_MAX];
    int cur;
    if (nav_items(list, &cur) && cur >= 0) {
        lv_obj_remove_state(list[cur], LV_STATE_FOCUS_KEY);
    }
    s_focus = NULL;
}

static void nav_key(int key)
{
    lv_obj_t *list[NAV_MAX];
    int cur;
    int n = nav_items(list, &cur);
    lv_obj_t *f = cur >= 0 ? list[cur] : NULL;
    switch (key) {
    case NAV_PREV:
    case NAV_NEXT:
        if (n == 0) {
            return;
        }
        if (f) {
            lv_obj_remove_state(f, LV_STATE_FOCUS_KEY);
        }
        s_focus = list[cur < 0 ? 0 : (cur + (key == NAV_NEXT ? 1 : n - 1)) % n];
        lv_obj_add_state(s_focus, LV_STATE_FOCUS_KEY);
        lv_obj_scroll_to_view_recursive(s_focus, LV_ANIM_ON);
        break;
    case NAV_ENTER:  // hold buttons only react to a long touch, so they stay touch-only
        if (f && !lv_obj_has_flag(f, NAV_START)) {
            lv_obj_send_event(f, LV_EVENT_CLICKED, NULL);
        }
        break;
    case NAV_HOLD:
        if (f && lv_obj_has_flag(f, NAV_START)) {
            lv_obj_send_event(f, LV_EVENT_CLICKED, NULL);
        } else if (!f) {
            app_post_action(ACT_START_FREE);
        }
        break;
    case NAV_BACK: {
        lv_obj_t *back = lv_obj_get_user_data(lv_screen_active());
        if (back) {
            lv_obj_send_event(back, LV_EVENT_CLICKED, NULL);
        }
        break;
    }
    default:
        break;
    }
}

static void nav_poll(lv_timer_t *t)
{
    unsigned tail = atomic_load(&s_ring_tail);
    while (tail != atomic_load(&s_ring_head)) {
        nav_key(s_ring[tail % NAV_RING]);
        atomic_store(&s_ring_tail, ++tail);
    }
}

void ui_nav(int key)
{
    unsigned head = atomic_load(&s_ring_head);
    if (head - atomic_load(&s_ring_tail) < NAV_RING) {  // full: drop the key
        s_ring[head % NAV_RING] = (uint8_t)key;
        atomic_store(&s_ring_head, head + 1);
    }
}

static void refresh(lv_timer_t *t)
{
    ride_view_t v;
    app_status_t st;
    app_get_view(&v, &st);
    lv_obj_t *active = lv_screen_active();

    lv_obj_t *want = active;
    if (v.state == RIDE_RUNNING || v.state == RIDE_PAUSED) {
        want = scr_ride;
    } else if (v.state == RIDE_DONE) {
        want = scr_summary;
    } else if (active == scr_ride || active == scr_summary || active == scr_workouts) {
        want = active == scr_workouts ? scr_workouts : scr_home;
    }
    if (want != active) {
        go(want);
    }

    if (want == scr_ride) {
        update_ride(&v);
    } else if (want == scr_summary) {
        update_summary(&v, &st);
    } else if (want == scr_home) {
        update_home(&v, &st);
    } else if (want == scr_wifi) {
        update_wifi();
    }
}

static void build_all(lv_display_t *disp)
{
    lv_theme_default_init(disp, rgb(C_ACCENT), rgb(C_WARN), true, &lv_font_montserrat_20);
    styles_init();
    build_home();
    build_users();
    build_name();
    build_workouts();
    build_preview();
    build_settings();
    build_adapt();
    build_wifi();
    build_ride();
    build_summary();
}

void ui_start(void)
{
    lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
    port.task_affinity = 1;
    port.task_stack = 8192;
    ESP_ERROR_CHECK(lvgl_port_init(&port));

    lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = bsp_panel(),
        .buffer_size = BSP_LCD_W * UI_DRAW_LINES,
        .double_buffer = true,
        .hres = BSP_LCD_W,
        .vres = BSP_LCD_H,
        .color_format = LV_COLOR_FORMAT_RGB565,
        // esp_lvgl_port sets the panel mirror from this at start-up (the RGB driver mirrors
        // while copying into the frame buffer); touch is flipped in bsp.c.
        .rotation = {.mirror_x = BSP_ROTATE_180, .mirror_y = BSP_ROTATE_180},
        .flags = {.buff_spiram = true},
    };
    // Partial rendering copied into the panel's single frame buffer, as Waveshare's V4
    // BSP does. Direct mode on two frame buffers (avoid_tearing) showed a shifted,
    // flickering second image on the V4 board.
    lvgl_port_display_rgb_cfg_t rgb_cfg = {0};
    lv_display_t *disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    if (bsp_touch()) {
        lvgl_port_touch_cfg_t touch = {.disp = disp, .handle = bsp_touch()};
        lvgl_port_add_touch(&touch);
    } else {
        ESP_LOGW(TAG, "no touch controller");
    }

    lvgl_port_lock(0);
    build_all(disp);
    go(scr_home);
    lv_timer_create(refresh, 250, NULL);
    lv_timer_create(nav_poll, 30, NULL);
    lvgl_port_unlock();
}
