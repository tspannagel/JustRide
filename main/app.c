#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "ble_mgr.h"
#include "ble_srv.h"
#include "bsp.h"
#include "drv_ftms.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "evbus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "input_map.h"
#include "recorder.h"
#include "tz.h"
#include "ui.h"
#include "web.h"

static const char *TAG = "app";

#define TICK_MS 250
#define BEEP_MS 150
#define BEEP_GAP_MS 350
#define CLOCK_VALID_AFTER 1735689600  // 2025-01-01
#define REBROADCAST_MS 1000

static SemaphoreHandle_t s_lock;
static ride_t s_ride;
static ride_view_t s_view;
static app_status_t s_status;
static settings_t s_settings;
EXT_RAM_BSS_ATTR static workout_t s_wo;  // PSRAM: internal RAM is tight
static input_map_t s_input;
static bool s_finish_pending;
static uint8_t s_beeps_left;
static uint32_t s_next_beep_ms;
static uint8_t s_last_state = RIDE_IDLE;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint32_t utc_now(void) { return (uint32_t)time(NULL); }

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static ride_config_t ride_cfg(void)
{
    const user_t *u = settings_user(&s_settings);
    return (ride_config_t){
        .ftp_w = u->ftp_w,
        .max_erg_w = u->max_erg_w,
        .crr = s_settings.crr,
        .cw = s_settings.cw,
        .gears = s_settings.gears,
    };
}

static void start_ride(const workout_t *wo)
{
    if (s_ride.state != RIDE_IDLE) {
        return;
    }
    web_stop();  // Wi-Fi off while riding (RGB drift, and no OTA mid-ride)
    s_ride.cfg = ride_cfg();  // ride_start re-validates it and keeps live sensor values
    ride_start(&s_ride, wo, now_ms(), utc_now());
    const user_t *u = settings_user(&s_settings);
    rec_info_t info = {.ftp_w = u->ftp_w, .weight_kg = u->weight_kg, .max_hr = u->max_hr,
                       .utc_offset_s = tz_offset(s_settings.tz, utc_now())};
    snprintf(info.user_id, sizeof(info.user_id), "%s", u->id);
    snprintf(info.workout, sizeof(info.workout), "%s", wo ? wo->name : "");
    if (!recorder_start(utc_now(), &info)) {
        ESP_LOGE(TAG, "recorder not started: this ride is not saved");
    }
    s_status.ftp_suggest_w = 0;
    ESP_LOGI(TAG, "ride started (%s)", wo ? wo->name : "free ride");
}

static void do_action(uint8_t a)
{
    if (a == ACT_START_FREE) {
        start_ride(NULL);
    } else if (a == ACT_START_WORKOUT) {
        start_ride(&s_wo);
    } else {
        uint8_t before = s_ride.state;
        ride_action(&s_ride, a);
        if (before != RIDE_DONE && s_ride.state == RIDE_DONE) {
            // Written by the task loop after the trainer got its stop command
            s_finish_pending = true;
            s_status.saving = true;
            s_beeps_left = 1;
        }
    }
}

static void handle(const evt_t *e)
{
    switch (e->type) {
    case EVT_METRICS:
        ride_on_metrics(&s_ride, &e->metrics, now_ms());
        break;
    case EVT_DEVICE:
        if (e->device.role == SRC_TRAINER) {
            s_status.trainer = e->device.kind == DEV_CONNECTED;
        } else if (e->device.role == SRC_HR) {
            s_status.hr = e->device.kind == DEV_CONNECTED;
        } else if (e->device.role == SRC_CONTROLLER) {
            s_status.controller = e->device.kind == DEV_CONNECTED;
        } else if (e->device.role == SRC_RECEIVER) {
            s_status.receivers = e->device.value;
        }
        break;
    case EVT_ACTION:
        do_action(e->action);
        break;
    case EVT_INPUT: {
        int k = input_nav(&s_input, &e->input, now_ms(), s_ride.state);  // before input_map
        if (k >= 0) {
            ui_nav(k);
        }
        int a = input_map(&s_input, &e->input, now_ms(), s_ride.state);
        if (a >= 0) {
            do_action((uint8_t)a);
        }
        break;
    }
    default:
        break;
    }
}

static void task(void *arg)
{
    uint32_t last_tick = now_ms(), last_bcast = last_tick;
    for (;;) {
        evt_t e;
        bool got = evbus_get(&e, pdMS_TO_TICKS(50));
        lock();
        if (got) {
            handle(&e);
        }
        if (s_ride.state == RIDE_IDLE && s_last_state != RIDE_IDLE) {
            web_start(&s_settings);  // back on after the ride (start_ride turned it off)
        }
        s_last_state = s_ride.state;
        uint32_t now = now_ms();
        if (now - last_tick >= TICK_MS) {
            last_tick = now;
            ride_sample_t sample;
            uint8_t beeps;
            if (ride_tick(&s_ride, now, utc_now(), &sample, &beeps)) {
                recorder_add(&sample);
            }
            if (beeps) {
                s_beeps_left = beeps;
            }
        }
        if (s_beeps_left && (int32_t)(now - s_next_beep_ms) >= 0) {
            bsp_beep(BEEP_MS);
            s_beeps_left--;
            s_next_beep_ms = now + BEEP_GAP_MS;
        }
        control_cmd_t cmd;
        ride_command(&s_ride, &cmd);
        drv_ftms_set(&cmd);
        ride_view(&s_ride, &s_view);
        if (now - last_bcast >= REBROADCAST_MS) {
            // Power and cadence as shown (3 s power while riding; -1 when stale) to a watch
            last_bcast = now;
            ble_srv_update(s_view.power_w, s_view.cadence_rpm10, now);
        }
        s_status.clock_ok = utc_now() > CLOCK_VALID_AFTER;
        bool finish = s_finish_pending;
        s_finish_pending = false;
        unlock();

        // Writing the FIT file can take seconds. Do it after the lowest-resistance
        // command went out and without the lock, so the UI keeps running.
        // Only this task touches the recorder and handles ride actions.
        if (finish) {
            uint16_t ftp_est;
            bool saved = recorder_finish(&ftp_est);
            lock();
            s_status.saving = false;
            s_status.saved = saved;
            // Only suggested, never applied: one bad power reading must not raise every ERG target
            uint16_t ftp = settings_user(&s_settings)->ftp_w;
            s_status.ftp_suggest_w = ftp_est > ftp && ftp_est <= RIDE_FTP_MAX ? ftp_est : 0;
            unlock();
        }
    }
}

void app_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_status.sd_ok = bsp_sd_mounted();
    settings_load(&s_settings);
    ride_config_t cfg = ride_cfg();
    ride_init(&s_ride, &cfg);
    ble_mgr_set_user(settings_user(&s_settings)->id);  // before ble_mgr_start
    recorder_init();
    recorder_recover();
    xTaskCreatePinnedToCore(task, "control", 6144, NULL, 5, NULL, 1);
}

void app_get_view(ride_view_t *v, app_status_t *st)
{
    lock();
    *v = s_view;
    *st = s_status;
    unlock();
}

void app_post_action(action_t a)
{
    evt_t e = {.type = EVT_ACTION, .action = (uint8_t)a};
    evbus_post(&e);
}

void app_web_start(void)
{
    lock();
    if (s_ride.state == RIDE_IDLE) {
        web_start(&s_settings);
    }
    unlock();
}

bool app_read_workout(const char *path, workout_t *w)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    char *xml = malloc(ZWO_MAX_BYTES);
    size_t n = xml ? fread(xml, 1, ZWO_MAX_BYTES, f) : 0;
    fclose(f);
    bool ok = xml && zwo_parse(xml, n, w);
    free(xml);
    if (ok && w->name[0] == 0) {
        const char *base = strrchr(path, '/');
        snprintf(w->name, sizeof(w->name), "%s", base ? base + 1 : path);
    }
    return ok;
}

bool app_start_workout_mem(const workout_t *w)
{
    lock();
    bool ok = w->n > 0 && s_ride.state == RIDE_IDLE;
    if (ok) {
        s_wo = *w;
    }
    unlock();
    if (ok) {
        app_post_action(ACT_START_WORKOUT);
    }
    return ok;
}

bool app_start_workout(const char *path)
{
    workout_t *w = malloc(sizeof(*w));
    bool ok = w && app_read_workout(path, w) && app_start_workout_mem(w);
    free(w);
    return ok;
}

const workout_t *app_workout(void) { return &s_wo; }

void app_get_settings(settings_t *s)
{
    lock();
    *s = s_settings;
    unlock();
}

static void apply_settings(void)
{
    if (s_ride.state == RIDE_IDLE) {
        ride_config_t cfg = ride_cfg();
        ride_init(&s_ride, &cfg);
    }
    settings_save(&s_settings);
}

bool app_edit_user(const user_t *u)
{
    bool ok = u->ftp_w >= RIDE_FTP_MIN && u->ftp_w <= RIDE_FTP_MAX && u->max_erg_w >= 100 &&
              u->max_erg_w <= RIDE_ERG_CAP_W && u->weight_kg >= 30 && u->weight_kg <= 200 && u->max_hr >= 100 &&
              u->max_hr <= 230 && settings_valid_name(u->name);
    lock();
    int idx = settings_find_user(&s_settings, u->id);
    ok = ok && s_ride.state == RIDE_IDLE && idx >= 0;
    if (ok) {
        user_t *cur = &s_settings.users[idx];
        if (idx == s_settings.user && u->ftp_w != cur->ftp_w) {
            s_status.ftp_suggest_w = 0;
        }
        memcpy(cur->name, u->name, sizeof(cur->name));
        cur->ftp_w = u->ftp_w;
        cur->max_erg_w = u->max_erg_w;
        cur->weight_kg = u->weight_kg;
        cur->max_hr = u->max_hr;
        apply_settings();
    }
    unlock();
    return ok;
}

void app_accept_ftp(void)
{
    lock();
    if (s_ride.state != RIDE_RUNNING && s_ride.state != RIDE_PAUSED && s_status.ftp_suggest_w) {
        settings_user(&s_settings)->ftp_w = s_status.ftp_suggest_w;
        s_status.ftp_suggest_w = 0;
        apply_settings();
    }
    unlock();
}

bool app_set_tz(const char *posix)
{
    lock();
    bool ok = s_ride.state == RIDE_IDLE && tz_valid(posix);
    if (ok && strcmp(posix, s_settings.tz) != 0) {
        snprintf(s_settings.tz, sizeof(s_settings.tz), "%s", posix);
        settings_save(&s_settings);
    }
    unlock();
    return ok;
}

// Called with the lock held, after s_settings.user changed
static void user_changed(void)
{
    s_status.ftp_suggest_w = 0;
    apply_settings();
    ble_mgr_set_user(settings_user(&s_settings)->id);  // reconnects to this user's HR strap
}

bool app_select_user(const char *id)
{
    lock();
    int idx = settings_find_user(&s_settings, id);
    bool ok = s_ride.state == RIDE_IDLE && idx >= 0;
    if (ok && idx != s_settings.user) {
        s_settings.user = (uint8_t)idx;
        user_changed();
    }
    unlock();
    return ok;
}

bool app_add_user(const char *name)
{
    lock();
    bool ok = s_ride.state == RIDE_IDLE && settings_add_user(&s_settings, name);
    if (ok) {
        user_changed();
    }
    unlock();
    return ok;
}

bool app_delete_user(const char *id)
{
    lock();
    char active[sizeof(s_settings.users[0].id)];
    memcpy(active, settings_user(&s_settings)->id, sizeof(active));
    bool ok = s_ride.state == RIDE_IDLE && settings_delete_user(&s_settings, settings_find_user(&s_settings, id));
    if (ok && strcmp(active, settings_user(&s_settings)->id) != 0) {
        user_changed();
    } else if (ok) {
        apply_settings();
    }
    unlock();
    return ok;
}
