#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "bsp.h"
#include "drv_ftms.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "evbus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "input_map.h"
#include "recorder.h"
#include "web.h"

static const char *TAG = "app";

#define TICK_MS 250
#define BEEP_MS 150
#define BEEP_GAP_MS 350
#define CLOCK_VALID_AFTER 1735689600  // 2025-01-01

static SemaphoreHandle_t s_lock;
static ride_t s_ride;
static ride_view_t s_view;
static app_status_t s_status;
static settings_t s_settings;
static workout_t s_wo;
static input_map_t s_input;
static bool s_finish_pending;
static uint8_t s_beeps_left;
static uint32_t s_next_beep_ms;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint32_t utc_now(void) { return (uint32_t)time(NULL); }

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static ride_config_t ride_cfg(void)
{
    return (ride_config_t){
        .ftp_w = s_settings.ftp_w,
        .max_erg_w = s_settings.max_erg_w,
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
    recorder_start(utc_now(), wo ? wo->name : "", s_settings.ftp_w);
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
        }
        break;
    case EVT_ACTION:
        do_action(e->action);
        break;
    case EVT_INPUT: {
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
    uint32_t last_tick = now_ms();
    for (;;) {
        evt_t e;
        bool got = evbus_get(&e, pdMS_TO_TICKS(50));
        lock();
        if (got) {
            handle(&e);
        }
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
        s_status.clock_ok = utc_now() > CLOCK_VALID_AFTER;
        bool finish = s_finish_pending;
        s_finish_pending = false;
        unlock();

        // Writing the FIT file can take seconds. Do it after the lowest-resistance
        // command went out and without the lock, so the UI keeps running.
        // Only this task touches the recorder and handles ride actions.
        if (finish) {
            bool saved = recorder_finish();
            lock();
            s_status.saving = false;
            s_status.saved = saved;
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

bool app_start_workout(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    char *xml = malloc(ZWO_MAX_BYTES);
    size_t n = xml ? fread(xml, 1, ZWO_MAX_BYTES, f) : 0;
    fclose(f);
    bool ok = false;
    lock();
    if (xml && s_ride.state == RIDE_IDLE) {
        ok = zwo_parse(xml, n, &s_wo);
        if (ok && s_wo.name[0] == 0) {
            const char *base = strrchr(path, '/');
            snprintf(s_wo.name, sizeof(s_wo.name), "%s", base ? base + 1 : path);
        }
    }
    unlock();
    free(xml);
    if (ok) {
        app_post_action(ACT_START_WORKOUT);
    }
    return ok;
}

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

void app_set_ftp(uint16_t ftp_w)
{
    lock();
    if (s_ride.state == RIDE_IDLE && ftp_w >= RIDE_FTP_MIN && ftp_w <= RIDE_FTP_MAX) {
        s_settings.ftp_w = ftp_w;
        apply_settings();
    }
    unlock();
}

void app_set_max_erg(uint16_t w)
{
    lock();
    if (s_ride.state == RIDE_IDLE && w >= 100 && w <= RIDE_ERG_CAP_W) {
        s_settings.max_erg_w = w;
        apply_settings();
    }
    unlock();
}
