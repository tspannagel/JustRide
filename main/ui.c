// Touch UI: home, riders, workouts, settings, Wi-Fi, ride, summary. LVGL 9.
// Everything here runs in the LVGL task; ride state comes from app_get_view().

#include "ui.h"

#include <dirent.h>
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
#include "web.h"

static const char *TAG = "ui";

#define WORKOUT_DIR BSP_SD_MOUNT "/workouts"
#define MAX_LIST 40
#define PAD 12

static lv_obj_t *scr_home, *scr_users, *scr_name, *scr_workouts, *scr_settings, *scr_wifi, *scr_ride, *scr_summary;

static lv_obj_t *home_status, *home_user;
static lv_obj_t *users_list, *users_msg;
static lv_obj_t *name_title, *name_ta;
static bool s_name_add;  // name screen: add a rider (true) or rename the active one
static lv_obj_t *wo_list, *wo_msg;
static lv_obj_t *set_title, *set_ftp, *set_erg, *set_weight, *set_hr;
static lv_obj_t *wifi_status, *wifi_btn_label, *wifi_ota_btn, *wifi_ota_label;
static lv_obj_t *r_time, *r_info, *r_power, *r_target, *r_cad, *r_hr, *r_speed, *r_stats, *r_paused;
static lv_obj_t *r_pause_label, *r_grade_minus, *r_grade_plus, *r_skip;
static lv_obj_t *sum_text, *sum_ftp_btn, *sum_ftp_label;

static char s_paths[MAX_LIST][80];

// ---- Helpers ------------------------------------------------------------------

static lv_obj_t *screen(const char *title, bool back);

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, int w, int h, lv_event_cb_t cb, void *data)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_t *l = label(b, &lv_font_montserrat_20, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
    return b;
}

static lv_obj_t *row(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(r, PAD, 0);
    return r;
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

static void go(lv_obj_t *scr) { lv_screen_load(scr); }

static void on_back(lv_event_t *e) { go(scr_home); }

static void on_action(lv_event_t *e) { app_post_action((action_t)(intptr_t)lv_event_get_user_data(e)); }

// ---- Home ---------------------------------------------------------------------

static void open_users(lv_event_t *e);
static void open_workouts(lv_event_t *e);
static void open_settings(lv_event_t *e);
static void open_wifi(lv_event_t *e);

static void build_home(void)
{
    scr_home = screen("JustRide", false);
    lv_obj_t *u = button(scr_home, "", 420, 60, open_users, NULL);
    home_user = lv_obj_get_child(u, 0);
    home_status = label(scr_home, &lv_font_montserrat_20, "");
    lv_obj_t *r1 = row(scr_home), *r2 = row(scr_home);
    button(r1, "Free ride", 200, 110, on_action, (void *)ACT_START_FREE);
    button(r1, "Workouts", 200, 110, open_workouts, NULL);
    button(r2, "Settings", 200, 110, open_settings, NULL);
    button(r2, "Wi-Fi", 200, 110, open_wifi, NULL);
}

static void update_home(const ride_view_t *v, const app_status_t *st)
{
    settings_t s;
    app_get_settings(&s);
    lv_label_set_text_fmt(home_user, LV_SYMBOL_HOME "  Rider: %s", settings_user(&s)->name);
    lv_label_set_text_fmt(home_status, "Trainer: %s\nHeart rate: %s\nZwift Ride: %s%s%s",
                          st->trainer ? "connected" : "searching...", st->hr ? "connected" : "-",
                          st->controller ? "connected" : "-",
                          st->sd_ok ? "" : "\nNo SD card: rides are not saved",
                          st->clock_ok ? "" : "\nClock not set: turn on Wi-Fi once");
}

// ---- Riders -------------------------------------------------------------------

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
        lv_obj_t *b = lv_list_add_button(users_list, i == s.user ? LV_SYMBOL_OK : NULL, s.users[i].name);
        lv_obj_add_event_cb(b, on_user, LV_EVENT_CLICKED, (void *)(intptr_t)i);
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

static void build_users(void)
{
    scr_users = screen("Riders", true);
    users_list = lv_list_create(scr_users);
    lv_obj_set_size(users_list, LV_PCT(100), 240);
    lv_obj_t *r = row(scr_users);
    button(r, "Add", 120, 60, on_add_user, NULL);
    button(r, "Rename", 120, 60, on_rename_user, NULL);
    // Long press: deleting a rider (and their paired HR strap) by accident would be annoying
    lv_obj_t *del = button(r, "Hold to delete", 170, 60, on_delete_user, NULL);
    lv_obj_remove_event_cb(del, on_delete_user);
    lv_obj_add_event_cb(del, on_delete_user, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_set_style_bg_color(del, lv_palette_main(LV_PALETTE_RED), 0);
    users_msg = label(scr_users, &lv_font_montserrat_16, "");
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
    lv_textarea_set_one_line(name_ta, true);
    lv_textarea_set_max_length(name_ta, SETTINGS_NAME_LEN - 1);
    lv_obj_set_width(name_ta, LV_PCT(100));
    lv_obj_t *kb = lv_keyboard_create(scr_name);
    lv_obj_set_size(kb, LV_PCT(100), 280);
    lv_keyboard_set_textarea(kb, name_ta);
    // The keyboard's OK and close keys send READY / CANCEL
    lv_obj_add_event_cb(kb, on_name_done, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, on_name_done, LV_EVENT_CANCEL, NULL);
}

// ---- Workouts -----------------------------------------------------------------

static void on_workout(lv_event_t *e)
{
    const char *path = lv_event_get_user_data(e);
    if (!app_start_workout(path)) {
        lv_label_set_text(wo_msg, "Could not load this workout");
    }
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
        lv_obj_t *b = lv_list_add_button(wo_list, NULL, de->d_name);
        lv_obj_add_event_cb(b, on_workout, LV_EVENT_CLICKED, s_paths[n]);
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
    wo_msg = label(scr_workouts, &lv_font_montserrat_20, "");
    wo_list = lv_list_create(scr_workouts);
    lv_obj_set_size(wo_list, LV_PCT(100), 330);
}

// ---- Settings -----------------------------------------------------------------

static void update_settings(void)
{
    settings_t s;
    app_get_settings(&s);
    const user_t *u = settings_user(&s);
    lv_label_set_text_fmt(set_title, "Settings: %s", u->name);
    lv_label_set_text_fmt(set_ftp, "FTP %u W", u->ftp_w);
    lv_label_set_text_fmt(set_erg, "Max ERG %u W", u->max_erg_w);
    lv_label_set_text_fmt(set_weight, "Weight %u kg", u->weight_kg);
    lv_label_set_text_fmt(set_hr, "Max HR %u", u->max_hr);
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

static void stepper(lv_obj_t *parent, lv_obj_t **value, int field, int step)
{
    lv_obj_t *r = row(parent);
    char t[8];
    snprintf(t, sizeof(t), "-%d", step);
    button(r, t, 72, 56, on_step, STEP(field, -step));
    *value = label(r, &lv_font_montserrat_28, "");
    lv_obj_set_width(*value, 290);
    lv_obj_set_style_text_align(*value, LV_TEXT_ALIGN_CENTER, 0);
    snprintf(t, sizeof(t), "+%d", step);
    button(r, t, 72, 56, on_step, STEP(field, step));
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
    stepper(scr_settings, &set_ftp, F_FTP, 5);
    stepper(scr_settings, &set_erg, F_ERG, 50);
    stepper(scr_settings, &set_weight, F_WEIGHT, 1);
    stepper(scr_settings, &set_hr, F_HR, 1);
    button(row(scr_settings), "Forget paired devices", 300, 56, on_forget, NULL);
    label(scr_settings, &lv_font_montserrat_16, esp_app_get_description()->version);
}

// ---- Wi-Fi --------------------------------------------------------------------

static void update_wifi(void)
{
    char buf[160];
    web_status(buf, sizeof(buf));
    lv_label_set_text(wifi_status, buf);
    lv_label_set_text(wifi_btn_label, web_running() ? "Turn off" : "Turn on");
    show(wifi_ota_btn, web_running());
    lv_label_set_text(wifi_ota_label, web_ota_allowed() ? "Update allowed (5 min)" : "Allow firmware update");
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
    wifi_status = label(scr_wifi, &lv_font_montserrat_20, "");
    lv_obj_t *b = button(row(scr_wifi), "", 220, 80, on_wifi_toggle, NULL);
    wifi_btn_label = lv_obj_get_child(b, 0);
    wifi_ota_btn = button(row(scr_wifi), "", 320, 70, on_allow_ota, NULL);
    wifi_ota_label = lv_obj_get_child(wifi_ota_btn, 0);
    label(scr_wifi, &lv_font_montserrat_16, "Wi-Fi turns off automatically when a ride starts.");
}

// ---- Ride ---------------------------------------------------------------------

static void build_ride(void)
{
    scr_ride = screen(NULL, false);
    lv_obj_t *top = row(scr_ride);
    r_time = label(top, &lv_font_montserrat_28, "");
    r_info = label(top, &lv_font_montserrat_20, "");

    r_power = label(scr_ride, &lv_font_montserrat_48, "");
    lv_obj_set_style_text_align(r_power, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(r_power, LV_PCT(100));
    r_target = label(scr_ride, &lv_font_montserrat_20, "");
    lv_obj_set_style_text_align(r_target, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(r_target, LV_PCT(100));

    lv_obj_t *mid = row(scr_ride);
    r_cad = label(mid, &lv_font_montserrat_28, "");
    r_hr = label(mid, &lv_font_montserrat_28, "");
    r_speed = label(mid, &lv_font_montserrat_28, "");
    r_stats = label(scr_ride, &lv_font_montserrat_16, "");

    lv_obj_t *b1 = row(scr_ride);
    button(b1, "-", 100, 70, on_action, (void *)ACT_MINUS);
    lv_obj_t *pause = button(b1, "", 110, 70, on_action, (void *)ACT_PAUSE_TOGGLE);
    r_pause_label = lv_obj_get_child(pause, 0);
    button(b1, "Lap", 100, 70, on_action, (void *)ACT_LAP);
    button(b1, "+", 100, 70, on_action, (void *)ACT_PLUS);

    lv_obj_t *b2 = row(scr_ride);
    r_grade_minus = button(b2, "Grade -", 120, 60, on_action, (void *)ACT_GRADE_MINUS);
    r_skip = button(b2, "Skip", 120, 60, on_action, (void *)ACT_SKIP);
    // Stop needs a long press so it can't be hit by accident mid-interval
    lv_obj_t *stop = button(b2, "Hold to stop", 160, 60, on_action, (void *)ACT_STOP);
    lv_obj_remove_event_cb(stop, on_action);
    lv_obj_add_event_cb(stop, on_action, LV_EVENT_LONG_PRESSED, (void *)ACT_STOP);
    lv_obj_set_style_bg_color(stop, lv_palette_main(LV_PALETTE_RED), 0);
    r_grade_plus = button(b2, "Grade +", 120, 60, on_action, (void *)ACT_GRADE_PLUS);

    // Banner in the free strip below the buttons, so it hides no numbers
    r_paused = label(scr_ride, &lv_font_montserrat_28, "PAUSED");
    lv_obj_add_flag(r_paused, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_width(r_paused, LV_PCT(100));
    lv_obj_set_style_text_align(r_paused, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_ver(r_paused, 10, 0);
    lv_obj_align(r_paused, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(r_paused, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r_paused, lv_palette_main(LV_PALETTE_ORANGE), 0);
}

static void update_ride(const ride_view_t *v)
{
    char t[16];
    fmt_time(t, sizeof(t), v->timer_s);
    lv_label_set_text(r_time, t);

    bool erg = v->mode == MODE_ERG;
    if (v->has_workout && !v->wo_done) {
        char left[16];
        fmt_time(left, sizeof(left), v->seg_left_s);
        lv_label_set_text_fmt(r_info, "Interval %d/%d  %s", v->seg_idx + 1, v->seg_count, left);
    } else {
        lv_label_set_text_fmt(r_info, "Gear %u/%u", v->gear, v->gears);
    }

    if (v->power_w >= 0) {
        lv_label_set_text_fmt(r_power, "%d W", v->power_w);
    } else {
        lv_label_set_text(r_power, "- W");
    }
    if (erg) {
        if (v->cadence_target) {
            lv_label_set_text_fmt(r_target, "Target %d W  (%u %%)  %u rpm", v->target_w, v->intensity,
                                  v->cadence_target);
        } else {
            lv_label_set_text_fmt(r_target, "Target %d W  (%u %%)", v->target_w, v->intensity);
        }
    } else {
        lv_label_set_text_fmt(r_target, "Road %+d.%d %%   Sent %+d.%02d %%", v->base_grade / 100,
                              abs(v->base_grade % 100) / 10, v->grade / 100, abs(v->grade % 100));
    }

    if (v->cadence_rpm10 >= 0) {
        lv_label_set_text_fmt(r_cad, "%d rpm", (v->cadence_rpm10 + 5) / 10);
    } else {
        lv_label_set_text(r_cad, "- rpm");
    }
    if (v->hr_bpm > 0) {
        lv_label_set_text_fmt(r_hr, "%d bpm", v->hr_bpm);
    } else {
        lv_label_set_text(r_hr, "- bpm");
    }
    if (v->speed_kmh100 >= 0) {
        lv_label_set_text_fmt(r_speed, "%ld.%ld km/h", (long)(v->speed_kmh100 / 100), (long)(v->speed_kmh100 % 100 / 10));
    } else {
        lv_label_set_text(r_speed, "- km/h");
    }
    lv_label_set_text_fmt(r_stats, "Avg %d W   NP %d W   %lu kJ   Lap %u", v->sum.avg_w, v->sum.np_w,
                          (unsigned long)v->sum.kj, v->lap + 1);

    lv_label_set_text(r_pause_label, v->state == RIDE_PAUSED ? "Resume" : "Pause");
    show(r_paused, v->state == RIDE_PAUSED);
    show(r_grade_minus, !erg);
    show(r_grade_plus, !erg);
    show(r_skip, v->has_workout && !v->wo_done);
}

// ---- Summary ------------------------------------------------------------------

static void on_accept_ftp(lv_event_t *e) { app_accept_ftp(); }

static void build_summary(void)
{
    scr_summary = screen("Ride finished", false);
    sum_text = label(scr_summary, &lv_font_montserrat_28, "");
    sum_ftp_btn = button(row(scr_summary), "", 300, 70, on_accept_ftp, NULL);
    sum_ftp_label = lv_obj_get_child(sum_ftp_btn, 0);
    button(row(scr_summary), "Done", 200, 80, on_action, (void *)ACT_DISMISS);
}

static void update_summary(const ride_view_t *v, const app_status_t *st)
{
    char t[16];
    fmt_time(t, sizeof(t), v->sum.duration_s);
    lv_label_set_text_fmt(sum_text,
                          "Time %s\nAvg %d W   NP %d W\nTSS %u.%u   IF %u.%02u\n%lu kJ   %lu.%lu km\nAvg HR %d%s",
                          t, v->sum.avg_w, v->sum.np_w, v->sum.tss10 / 10, v->sum.tss10 % 10, v->sum.if_1000 / 1000,
                          v->sum.if_1000 % 1000 / 10, (unsigned long)v->sum.kj,
                          (unsigned long)(v->sum.distance_m / 1000), (unsigned long)(v->sum.distance_m % 1000 / 100),
                          v->sum.avg_hr,
                          st->saving ? "\nSaving..."
                          : st->saved ? "\nSaved"
                                      : "\nNOT saved (shorter than 1 min, or SD card problem)");
    // 95 % of this ride's best 20 min beat the FTP: offer it, never apply it silently
    show(sum_ftp_btn, st->ftp_suggest_w > 0);
    lv_label_set_text_fmt(sum_ftp_label, "Set FTP to %u W", st->ftp_suggest_w);
}

// ---- Screens + refresh --------------------------------------------------------

static lv_obj_t *screen(const char *title, bool back)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s, PAD, 0);
    lv_obj_set_style_pad_row(s, PAD, 0);
    if (title || back) {
        lv_obj_t *r = row(s);
        if (back) {
            button(r, LV_SYMBOL_LEFT, 70, 50, on_back, NULL);
        }
        if (title) {
            label(r, &lv_font_montserrat_28, title);
        }
    }
    return s;
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

void ui_start(void)
{
    lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
    port.task_affinity = 1;
    port.task_stack = 8192;
    ESP_ERROR_CHECK(lvgl_port_init(&port));

    lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = bsp_panel(),
        .buffer_size = BSP_LCD_W * BSP_LCD_H,
        .hres = BSP_LCD_W,
        .vres = BSP_LCD_H,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {.buff_spiram = true, .direct_mode = true},
    };
    // Draw straight into the panel's two frame buffers, swap on vsync
    lvgl_port_display_rgb_cfg_t rgb_cfg = {.flags = {.bb_mode = true, .avoid_tearing = true}};
    lv_display_t *disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    if (bsp_touch()) {
        lvgl_port_touch_cfg_t touch = {.disp = disp, .handle = bsp_touch()};
        lvgl_port_add_touch(&touch);
    } else {
        ESP_LOGW(TAG, "no touch controller");
    }

    lvgl_port_lock(0);
    lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_ORANGE), true,
                          &lv_font_montserrat_20);
    build_home();
    build_users();
    build_name();
    build_workouts();
    build_settings();
    build_wifi();
    build_ride();
    build_summary();
    go(scr_home);
    lv_timer_create(refresh, 250, NULL);
    lvgl_port_unlock();
}
