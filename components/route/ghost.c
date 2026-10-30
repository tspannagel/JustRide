#include "ghost.h"

#include <stdlib.h>
#include <string.h>

void ghost_begin(ghost_t *g)
{
    g->n = 0;
    g->mm = 0;
}

bool ghost_add(ghost_t *g, const ride_sample_t *s)
{
    if (g->n >= GHOST_MAX_S) {
        return false;
    }
    if (s->speed_kmh100 > 0) {
        g->mm += (uint64_t)s->speed_kmh100 * 100 / 36;  // as metrics_add: km/h x100 -> mm per s
    }
    g->dist_dm[g->n++] = (uint32_t)(g->mm / 100);
    return true;
}

uint32_t ghost_dist_m(const ghost_t *g, uint32_t t_s)
{
    if (t_s == 0 || g->n == 0) {
        return 0;
    }
    return g->dist_dm[(t_s < g->n ? t_s : g->n) - 1] / 10;
}

int32_t ghost_time_at(const ghost_t *g, uint32_t dist_m)
{
    if (dist_m == 0) {
        return 0;
    }
    uint32_t want = dist_m * 10;
    if (g->n == 0 || g->dist_dm[g->n - 1] < want) {
        return -1;
    }
    // First second at which the distance reached want (it never decreases)
    uint32_t lo = 0, hi = g->n - 1;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g->dist_dm[mid] >= want) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return (int32_t)lo + 1;
}

// Next CSV field into out (quotes removed); returns the rest after the comma, NULL at the end
static const char *field(const char *p, char *out, size_t size)
{
    size_t k = 0;
    bool q = false;
    for (; *p && (q || *p != ','); p++) {
        if (*p == '"') {
            q = !q;
        } else if (*p != '\r' && *p != '\n' && k + 1 < size) {
            out[k++] = *p;
        }
    }
    out[k] = 0;
    return *p == ',' ? p + 1 : NULL;
}

bool ghost_summary_row(const char *line, ghost_summary_t *out)
{
    // ride_id,start_utc,duration_s,moving_s,workout,ftp_w,avg_w,np_w,if,tss,kj,avg_hr,max_hr,avg_cad,distance_km,...
    char f[15][64];
    const char *p = line;
    int n = 0;
    while (p && n < 15) {
        p = field(p, f[n++], sizeof(f[0]));
    }
    if (n < 15 || f[0][0] < '0' || f[0][0] > '9') {
        return false;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->ride_id, f[0], strnlen(f[0], sizeof(out->ride_id) - 1));
    memcpy(out->workout, f[4], strnlen(f[4], sizeof(out->workout) - 1));
    out->duration_s = (uint32_t)strtoul(f[2], NULL, 10);
    char *dot;
    unsigned long km = strtoul(f[14], &dot, 10), m = 0;
    if (*dot == '.') {
        // three decimals: metres
        char frac[4] = "000";
        for (int i = 0; i < 3 && dot[1 + i] >= '0' && dot[1 + i] <= '9'; i++) {
            frac[i] = dot[1 + i];
        }
        m = strtoul(frac, NULL, 10);
    }
    out->distance_m = (uint32_t)(km * 1000 + m);
    return true;
}
