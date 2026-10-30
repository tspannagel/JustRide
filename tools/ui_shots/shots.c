// Screenshot tool: compiles the firmware's UI (main/ui.c) against host LVGL,
// feeds it fixed example states and writes one BMP per screen into argv[1].
// The app/web functions ui.c calls are faked here; the screens are the real ones.

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../../main/ui.c"

// ---- Fake app state -------------------------------------------------------------

static ride_view_t g_view;
static app_status_t g_status;
static settings_t g_settings;
static bool g_web_on;

void app_get_view(ride_view_t *v, app_status_t *st)
{
    *v = g_view;
    *st = g_status;
}
void app_get_settings(settings_t *s) { *s = g_settings; }
void app_post_action(action_t a) {}
bool app_start_workout(const char *path) { return true; }

bool app_read_workout(const char *path, workout_t *w)
{
    static char xml[ZWO_MAX_BYTES];
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(xml, 1, sizeof(xml), f) : 0;
    if (f) {
        fclose(f);
    }
    return n > 0 && zwo_parse(xml, n, w);
}

static workout_t g_wo;
const workout_t *app_workout(void) { return &g_wo; }
void app_web_start(void) {}
bool app_edit_user(const user_t *u) { return true; }
void app_accept_ftp(void) {}
bool app_select_user(int idx) { return true; }
bool app_add_user(const char *name) { return true; }
bool app_delete_user(void) { return true; }
bool app_set_tz(const char *posix) { return true; }
bool settings_valid_name(const char *name) { return true; }

void web_stop(void) {}
bool web_running(void) { return g_web_on; }
bool web_ota_allowed(void) { return false; }
void web_allow_ota(void) {}
void web_status(char *buf, size_t size)
{
    snprintf(buf, size, g_web_on ? "http://trainer.local/\nhttp://192.168.1.42/" : "Wi-Fi off");
}

static void add_user(const char *id, const char *name, uint16_t ftp, uint8_t kg, uint8_t hr)
{
    user_t *u = &g_settings.users[g_settings.n_users++];
    snprintf(u->id, sizeof(u->id), "%s", id);
    snprintf(u->name, sizeof(u->name), "%s", name);
    u->ftp_w = ftp;
    u->max_erg_w = 600;
    u->weight_kg = kg;
    u->max_hr = hr;
}

// ---- Rendering --------------------------------------------------------------------

static uint16_t s_fb[BSP_LCD_W * BSP_LCD_H];
static uint8_t s_draw[BSP_LCD_W * BSP_LCD_H * 2];
static const char *s_out;

static uint32_t tick(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    const uint16_t *p = (const uint16_t *)px;
    for (int y = a->y1; y <= a->y2; y++) {
        for (int x = a->x1; x <= a->x2; x++) {
            s_fb[y * BSP_LCD_W + x] = *p++;
        }
    }
    lv_display_flush_ready(d);
}

// 24-bit BMP (shots.sh converts to PNG)
static void save(const char *name)
{
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.bmp", s_out, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    const int w = BSP_LCD_W, h = BSP_LCD_H, row = w * 3, size = 54 + row * h;
    uint8_t hdr[54] = {'B', 'M'};
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint16_t c = s_fb[y * w + x];
            uint8_t bgr[3] = {(uint8_t)((c & 0x1F) << 3), (uint8_t)((c >> 5 & 0x3F) << 2), (uint8_t)((c >> 11) << 3)};
            fwrite(bgr, 1, 3, f);
        }
    }
    fclose(f);
    printf("%s\n", path);
}

// ---- Example states ---------------------------------------------------------------

static void ride_common(void)
{
    memset(&g_view, 0, sizeof(g_view));
    g_view.state = RIDE_RUNNING;
    g_view.timer_s = 1534;
    g_view.cadence_rpm10 = 912;
    g_view.speed_kmh100 = 3240;
    g_view.hr_bpm = 148;
    g_view.gears = 24;
    g_view.intensity = 100;
    g_view.sum = (metrics_summary_t){.avg_w = 212, .np_w = 224, .kj = 325};
}

int main(int argc, char **argv)
{
    s_out = argc > 1 ? argv[1] : ".";
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *disp = lv_display_create(BSP_LCD_W, BSP_LCD_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, s_draw, NULL, sizeof(s_draw), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush);
    build_all(disp);  // same theme and screens as ui_start()

    add_user("u1", "Sam", 250, 75, 185);
    add_user("u3", "Anna", 190, 62, 178);
    snprintf(g_settings.tz, sizeof(g_settings.tz), "%s", "CET-1CEST,M3.5.0,M10.5.0/3");
    g_status = (app_status_t){.trainer = true, .hr = true, .controller = true, .clock_ok = true, .sd_ok = true};

    go(scr_home);
    refresh(NULL);
    save("home");

    g_status = (app_status_t){.sd_ok = true};  // first start: nothing paired, clock not set
    refresh(NULL);
    save("home-first-start");
    g_status = (app_status_t){.trainer = true, .hr = true, .controller = true, .clock_ok = true, .sd_ok = true};

    open_users(NULL);
    save("riders");

    open_name(true);
    lv_textarea_set_text(name_ta, "Anna");
    save("rider-name");

    open_workouts(NULL);
    save("workouts");

    open_preview(BSP_SD_MOUNT "/workouts/sweet_spot_3x10.zwo");
    save("workout-preview");

    open_settings(NULL);
    save("settings");

    g_web_on = true;
    open_wifi(NULL);
    save("wifi");

    ride_common();
    g_view.mode = MODE_FREE;
    g_view.power_w = 231;
    g_view.gear = 14;
    g_view.base_grade = 350;
    g_view.grade = 412;
    refresh(NULL);
    save("ride-free");

    ride_common();
    g_view.mode = MODE_ERG;
    g_view.power_w = 247;
    g_view.target_w = 250;
    g_view.cadence_target = 90;
    app_read_workout(BSP_SD_MOUNT "/workouts/sweet_spot_3x10.zwo", &g_wo);
    g_view.has_workout = true;
    g_view.seg_idx = 3;
    g_view.seg_count = g_wo.n;
    g_view.seg_left_s = 412;
    refresh(NULL);
    save("ride-workout");

    g_view.state = RIDE_PAUSED;
    refresh(NULL);
    save("ride-paused");

    memset(&g_view, 0, sizeof(g_view));
    g_view.state = RIDE_DONE;
    g_view.sum = (metrics_summary_t){.duration_s = 3725, .avg_w = 214, .np_w = 236, .max_w = 612, .tss10 = 905, .if_1000 = 944,
                                     .kj = 797, .avg_hr = 151, .distance_m = 33420};
    g_status.saved = true;
    g_status.ftp_suggest_w = 262;
    refresh(NULL);
    save("summary");
    return 0;
}
