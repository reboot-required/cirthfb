#include "metrics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --- CPU percent --------------------------------------------------------- */

typedef struct {
    long long user, nice, system, idle, iowait, irq, softirq, steal;
} cpu_stat_t;

static int read_cpu_stat(cpu_stat_t *s)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;

    int ok = fscanf(f, "cpu %lld %lld %lld %lld %lld %lld %lld %lld",
                    &s->user, &s->nice, &s->system, &s->idle,
                    &s->iowait, &s->irq, &s->softirq, &s->steal) == 8;
    fclose(f);
    return ok ? 0 : -1;
}

float metrics_cpu_percent(void)
{
    cpu_stat_t a, b;

    if (read_cpu_stat(&a) < 0) return -1.0f;
    sleep(1);
    if (read_cpu_stat(&b) < 0) return -1.0f;

    long long idle_a  = a.idle + a.iowait;
    long long total_a = a.user + a.nice + a.system + idle_a +
                        a.irq + a.softirq + a.steal;

    long long idle_b  = b.idle + b.iowait;
    long long total_b = b.user + b.nice + b.system + idle_b +
                        b.irq + b.softirq + b.steal;

    long long dtotal = total_b - total_a;
    long long didle  = idle_b  - idle_a;

    if (dtotal <= 0) return 0.0f;
    return 100.0f * (float)(dtotal - didle) / (float)dtotal;
}

/* --- CPU temperature ----------------------------------------------------- */

float metrics_cpu_temp(void)
{
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!f) return -1.0f;

    int raw = 0;
    int n = fscanf(f, "%d", &raw);
    fclose(f);
    if (n != 1) return -1.0f;
    return (float)raw / 1000.0f;
}

/* --- RAM percent --------------------------------------------------------- */

int metrics_ram_percent(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;

    long long total = 0, available = 0;
    char line[128];

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemTotal:", 9) == 0)
            sscanf(line + 9, "%lld", &total);
        else if (strncmp(line, "MemAvailable:", 13) == 0)
            sscanf(line + 13, "%lld", &available);

        if (total && available) break;
    }
    fclose(f);

    if (total <= 0) return -1;
    return (int)(100LL * (total - available) / total);
}

/* --- Uptime string ------------------------------------------------------- */

void metrics_uptime_str(char *buf, size_t n)
{
    FILE *f = fopen("/proc/uptime", "r");
    if (!f) {
        snprintf(buf, n, "?");
        return;
    }

    double up_sec = 0.0;
    int rc = fscanf(f, "%lf", &up_sec);
    fclose(f);
    if (rc != 1) { snprintf(buf, n, "?"); return; }

    long long secs  = (long long)up_sec;
    int days  = (int)(secs / 86400);
    int hours = (int)((secs % 86400) / 3600);
    int mins  = (int)((secs % 3600)  / 60);

    snprintf(buf, n, "%dd %dh %dm", days, hours, mins);
}
