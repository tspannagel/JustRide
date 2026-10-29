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
#include "app.h"
#include "ble_mgr.h"
#include "bsp.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "input_map.h"
#include "web.h"

static const char *TAG = "ui";

#define WORKOUT_DIR BSP_SD_MOUNT "/workouts"
#define MAX_LIST 40
#define UI_DRAW_LINES 48 // LVGL draw buffer height (two buffers in PSRAM)
#define PAD 12
#define GAP 10

// Palette
#define C_BG 0x0D1117
#define C_CARD 0x161B22
#define C_BTN 0x262C36
#define C_TEXT 0xE6EDF3
#define C_MUTED 0x8B949E
#define C_ACCENT 0x2F81F7
#define C_FOCUS 0x58A6FF  // controller highlight, lighter than the accent so a focused primary shows
#define C_OK 0x3FB950
#define C_WARN 0xE3A21A
#define C_DANGER 0xDA3633
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

static lv_obj_t *scr_home, *scr_users, *scr_name, *scr_workouts, *scr_preview, *scr_settings, *scr_wifi, *scr_ride, *scr_summary;

static lv_obj_t *home_user, *home_dot[3], *home_state[3], *home_warn;
static lv_obj_t *users_list, *users_msg;
static lv_obj_t *name_title, *name_ta;
static bool s_name_add;  // name screen: add a rider (true) or rename the active one
static lv_obj_t *wo_list, *wo_msg;
static lv_obj_t *pv_title, *pv_profile, *pv_val[4], *pv_note, *pv_msg;
static workout_t s_preview;          // workout shown on the preview screen
static const char *s_preview_path;   // points into s_paths
static lv_obj_t *set_title, *set_ftp, *set_erg, *set_weight, *set_hr;
static lv_obj_t *wifi_status, *wifi_btn, *wifi_btn_label, *wifi_ota_btn, *wifi_ota_label;
static lv_obj_t *r_profile;
static lv_obj_t *r_time, *r_info, *r_power_card, *r_power_cap, *r_power, *r_target, *r_cad, *r_hr, *r_speed, *r_stats;
static lv_obj_t *r_pause_label, *r_grade_minus, *r_grade_plus, *r_skip;
static lv_obj_t *sum_saved, *sum_note, *sum_val[9], *sum_ftp_btn, *sum_ftp_label;

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
    lv_style_set_radius(&st_card, 14);
    lv_style_set_pad_all(&st_card, 10);
    lv_style_set_pad_row(&st_card, 4);
    lv_style_set_pad_column(&st_card, 8);
    lv_style_set_border_color(&st_card, rgb(C_WARN));
    lv_style_set_border_width(&st_card, 0);

    lv_style_init(&st_btn);
    lv_style_set_bg_color(&st_btn, rgb(C_BTN));
    lv_style_set_radius(&st_btn, 12);
    lv_style_set_shadow_width(&st_btn, 0);
    lv_style_set_border_width(&st_btn, 0);
    lv_style_set_text_color(&st_btn, rgb(C_TEXT));

    lv_style_init(&st_primary);
    lv_style_set_bg_color(&st_primary, rgb(C_ACCENT));

    lv_style_init(&st_danger);
    lv_style_set_bg_color(&st_danger, rgb(C_DANGER));

    // Controller highlight (LV_STATE_FOCUS_KEY): blue fill plus outline
    lv_style_init(&st_focus);
    lv_style_set_bg_color(&st_focus, rgb(C_FOCUS));
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

// Zwift-style zone colours by % of FTP
static uint32_t zone_color(uint16_t pm)
{
    return pm < 600 ? 0x8B949E : pm < 760 ? 0x2F81F7 : pm < 900 ? 0x3FB950 : pm < 1050 ? 0xE3B341 : pm < 1180 ? 0xF0883E : 0xF85149;
}

typedef struct {
    const workout_t *w;
    int32_t pos_s;  // workout time to mark, < 0: none
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

// One bar per pixel column, height = target power, colour = zone. Free
// segments are drawn flat and dark. Done part dimmed, marker at pos_s.
static void draw_profile(lv_event_t *e)
{
    const profile_t *p = lv_event_get_user_data(e);
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
    static uint16_t col[BSP_LCD_W];
    uint16_t top = 1000;
    for (int x = 0; x < width; x++) {
        uint32_t t = (uint32_t)(((2ULL * x + 1) * w->total_s) / (2ULL * width));
        col[x] = workout_profile_at(w, t);
        top = col[x] > top ? col[x] : top;
    }
    top = top + top / 10;  // headroom
    int marker = p->pos_s < 0 ? -1 : (int)((uint64_t)p->pos_s * width / w->total_s);
    for (int x = 0; x < width; x++) {
        workout_pos_t pos;
        workout_at(w, (uint32_t)(((2ULL * x + 1) * w->total_s) / (2ULL * width)), &pos);
        int h = col[x] * height / top;
        uint32_t c = pos.kind == SEG_FREE ? C_BTN : zone_color(col[x]);
        fill(layer, a.x1 + x, a.y2 - h + 1, a.x1 + x, a.y2, c, x < marker ? LV_OPA_40 : LV_OPA_COVER);
    }
    int ftp_y = a.y2 - 1000 * height / top;  // 100 % FTP line
    fill(layer, a.x1, ftp_y, a.x2, ftp_y, C_TEXT, LV_OPA_30);
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

    static const char *const names[3] = {"Trainer", "Heart rate", "Zwift Ride"};
    lv_obj_t *c = card(scr_home, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 6, 0);
    for (int i = 0; i < 3; i++) {
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
    const bool on[3] = {st->trainer, st->hr, st->controller};
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(home_dot[i], rgb(on[i] ? C_OK : C_MUTED), 0);
        lv_label_set_text(home_state[i], on[i] ? "Connected" : i == 0 ? "Searching..." : "Not connected");
    }
    lv_label_set_text_fmt(home_warn, "%s%s%s", st->sd_ok ? "" : LV_SYMBOL_WARNING " No SD card: rides are not saved",
                          !st->sd_ok && !st->clock_ok ? "\n" : "",
                          st->clock_ok ? "" : LV_SYMBOL_WARNING " Clock not set: needs home Wi-Fi");
    show(home_warn, !st->sd_ok || !st->clock_ok);
}

// ---- Riders -------------------------------------------------------------------

static lv_obj_t *list_item(lv_obj_t *l, const char *icon, const char *text, lv_event_cb_t cb, void *data);

static void on_user(lv_event_t *e)
{
    app_select_user((int)(intptr_t)lv_event_get_user_data(e));
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
    if (!app_delete_user()) {
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

static void open_preview(const char *path)
{
    if (!app_read_workout(path, &s_preview)) {
        lv_label_set_text(wo_msg, "Could not load this workout");
        return;
    }
    s_preview_path = path;
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
    lv_label_set_text(pv_msg, "");
    s_pv_prof = (profile_t){.w = &s_preview, .pos_s = -1};
    lv_obj_invalidate(pv_profile);
    go(scr_preview);
}

static void on_workout(lv_event_t *e) { open_preview(lv_event_get_user_data(e)); }

static void on_preview_start(lv_event_t *e)
{
    if (!s_preview_path || !app_start_workout(s_preview_path)) {
        lv_label_set_text(pv_msg, "Could not start this workout");
    }
}

static void on_preview_back(lv_event_t *e) { go(scr_workouts); }

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
    static const char *const names[4] = {"TIME", "AVG", "IF", "TSS"};
    static const char *const icons[4] = {I_TIME, I_BOLT, I_PERCENT, I_BARS};
    for (int i = 0; i < 4; i++) {
        pv_val[i] = metric_font(r, icons[i], names[i], i == 1 ? C_POWER : C_MUTED, NULL, &lv_font_montserrat_20);
    }
    pv_note = muted(scr_preview, &lv_font_montserrat_14, "");
    pv_msg = label(scr_preview, &lv_font_montserrat_16, "");
    lv_obj_set_style_text_color(pv_msg, rgb(C_DANGER), 0);
    lv_obj_add_flag(primary(button(scr_preview, LV_SYMBOL_PLAY " Start workout", LV_PCT(100), 64, on_preview_start, NULL)),
                    NAV_START);
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
        if (de->d_name[0] == '.' || len < 5 || len > 60 || strcasecmp(de->d_name + len - 4, ".zwo") != 0) {
            continue;
        }
        snprintf(s_paths[n], sizeof(s_paths[n]), WORKOUT_DIR "/%.60s", de->d_name);
        char name[64];
        snprintf(name, sizeof(name), "%.*s", (int)(len - 4), de->d_name);  // without ".zwo"
        for (char *c = name; *c; c++) {
            *c = *c == '_' ? ' ' : *c;
        }
        list_item(wo_list, LV_SYMBOL_PLAY, name, on_workout, s_paths[n]);
        n++;
    }
    if (d) {
        closedir(d);
    }
    if (n == 0) {
        lv_label_set_text(wo_msg, "No .zwo files in /workouts on the SD card");
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
static void stepper(lv_obj_t *parent, lv_obj_t **value, const char *name, int field, int step)
{
    lv_obj_t *c = card(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(c, 6, 0);
    lv_obj_t *col = grow(box(c, LV_FLEX_FLOW_COLUMN));
    lv_obj_set_style_pad_row(col, 0, 0);
    caption(col, name, C_MUTED);
    *value = label(col, &lv_font_montserrat_28, "");
    button(c, LV_SYMBOL_MINUS, 76, 52, on_step, STEP(field, -step));
    button(c, LV_SYMBOL_PLUS, 76, 52, on_step, STEP(field, step));
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
    stepper(scr_settings, &set_ftp, "FTP", F_FTP, 5);
    stepper(scr_settings, &set_erg, "MAX ERG", F_ERG, 50);
    stepper(scr_settings, &set_weight, "WEIGHT", F_WEIGHT, 1);
    stepper(scr_settings, &set_hr, "MAX HEART RATE", F_HR, 1);
    lv_obj_t *r = row(scr_settings);
    grow(button(r, "Forget paired devices", 0, 52, on_forget, NULL));
    muted(r, &lv_font_montserrat_14, esp_app_get_description()->version);
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
    go(scr_wifi);
}

static void build_wifi(void)
{
    scr_wifi = screen("Wi-Fi", true);
    lv_obj_t *c = card(scr_wifi, LV_FLEX_FLOW_COLUMN);
    caption(c, "STATUS", C_MUTED);
    wifi_status = label(c, &lv_font_montserrat_20, "");
    wifi_btn = button(scr_wifi, "", LV_PCT(100), 64, on_wifi_toggle, NULL);
    wifi_btn_label = lv_obj_get_child(wifi_btn, 0);
    wifi_ota_btn = button(scr_wifi, "", LV_PCT(100), 64, on_allow_ota, NULL);
    wifi_ota_label = lv_obj_get_child(wifi_ota_btn, 0);
    muted(scr_wifi, &lv_font_montserrat_16, "Wi-Fi turns off automatically when a ride starts.");
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

static void update_ride(const ride_view_t *v)
{
    char t[16];
    fmt_time(t, sizeof(t), v->timer_s);
    lv_label_set_text(r_time, t);

    bool erg = v->mode == MODE_ERG;
    bool paused = v->state == RIDE_PAUSED;
    bool in_workout = v->has_workout && !v->wo_done;
    show(r_profile, in_workout);
    if (in_workout) {
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
    if (in_workout) {
        char left[16];
        fmt_time(left, sizeof(left), v->seg_left_s);
        lv_label_set_text_fmt(r_info, "Interval %d/%d  %s", v->seg_idx + 1, v->seg_count, left);
    } else {
        lv_label_set_text_fmt(r_info, "Gear %u/%u", v->gear, v->gears);
    }

    // Paused: orange frame and caption, so it's obvious from the saddle
    lv_label_set_text(r_power_cap, paused ? "PAUSED" : "POWER");
    lv_obj_set_style_text_color(r_power_cap, rgb(paused ? C_WARN : C_POWER), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(lv_obj_get_parent(r_power_cap), 0), rgb(paused ? C_WARN : C_POWER), 0);
    lv_obj_set_style_border_width(r_power_card, paused ? 3 : 0, 0);

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

    if (erg) {
        if (v->cadence_target) {
            lv_label_set_text_fmt(r_target, "Target %d W  -  %u %%  -  %u rpm", v->target_w, v->intensity,
                                  v->cadence_target);
        } else {
            lv_label_set_text_fmt(r_target, "Target %d W  -  %u %%", v->target_w, v->intensity);
        }
    } else {
        lv_label_set_text_fmt(r_target, "Road %+d.%d %%  -  sent %+d.%02d %%", v->base_grade / 100,
                              abs(v->base_grade % 100) / 10, v->grade / 100, abs(v->grade % 100));
    }

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
    if (v->speed_kmh100 >= 0) {
        lv_label_set_text_fmt(r_speed, "%ld.%ld", (long)(v->speed_kmh100 / 100), (long)(v->speed_kmh100 % 100 / 10));
    } else {
        lv_label_set_text(r_speed, "-");
    }
    lv_label_set_text_fmt(r_stats, "Avg %d W     NP %d W     %lu kJ     Lap %u", v->sum.avg_w, v->sum.np_w,
                          (unsigned long)v->sum.kj, v->lap + 1);

    lv_label_set_text(r_pause_label, paused ? LV_SYMBOL_PLAY " Resume" : LV_SYMBOL_PAUSE " Pause");
    show(r_grade_minus, !erg);
    show(r_grade_plus, !erg);
    show(r_skip, v->has_workout && !v->wo_done);
}

// ---- Summary ------------------------------------------------------------------

static void on_accept_ftp(lv_event_t *e) { app_accept_ftp(); }

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
    sum_ftp_btn = grow(button(b, "", 0, 64, on_accept_ftp, NULL));
    lv_obj_set_flex_grow(sum_ftp_btn, 3);
    sum_ftp_label = lv_obj_get_child(sum_ftp_btn, 0);
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
    lv_label_set_text(sum_note, st->saving || st->saved ? "" : "Shorter than 1 min, or SD card problem.");
    show(sum_note, !st->saving && !st->saved);

    // 95 % of this ride's best 20 min beat the FTP: offer it, never apply it silently
    show(sum_ftp_btn, st->ftp_suggest_w > 0);
    lv_label_set_text_fmt(sum_ftp_label, LV_SYMBOL_UP " Set FTP to %u W", st->ftp_suggest_w);
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
