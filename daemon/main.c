#define _POSIX_C_SOURCE 200809L

#include "render.h"
#include "metrics.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FB_DEV           "/dev/fb1"
#define DEFAULT_INTERVAL  30

static volatile sig_atomic_t g_running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    const char *env_interval = getenv("UPDATE_INTERVAL");
    int interval = env_interval ? atoi(env_interval) : DEFAULT_INTERVAL;
    if (interval <= 0) interval = DEFAULT_INTERVAL;

    const char *fb_dev = getenv("FB_DEV");
    if (!fb_dev) fb_dev = FB_DEV;

    if (render_open(fb_dev) < 0) {
        fprintf(stderr, "cirthfbd: cannot open %s\n", fb_dev);
        return 1;
    }

    uint8_t *fb = render_fb();

    while (g_running) {
        char upt_line[64];
        char cpu_line[32];
        char ram_line[32];
        char temp_line[32];

        float cpu  = metrics_cpu_percent();   /* blocks ~1s internally */
        int   ram  = metrics_ram_percent();
        float temp = metrics_cpu_temp();
        metrics_uptime_str(upt_line, sizeof(upt_line));

        snprintf(cpu_line,  sizeof(cpu_line),  "CPU:  %.1f%%", cpu);
        snprintf(ram_line,  sizeof(ram_line),  "RAM:  %d%%",   ram);
        snprintf(temp_line, sizeof(temp_line), "TEMP: %.1fC",  temp);

        render_clear();
        draw_string(fb, 4,  4,  cpu_line);
        draw_string(fb, 4, 20,  ram_line);
        draw_string(fb, 4, 36,  temp_line);
        draw_string(fb, 4, 52,  upt_line);
        render_flush();

        /* sleep in 1s increments so SIGTERM wakes us promptly */
        for (int i = 1; i < interval && g_running; i++)
            sleep(1);
    }

    render_close();
    return 0;
}
