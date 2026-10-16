#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"

Settings cfg = { 1, 1, 100, 0, 0, 1, 1, 1, 1, 0 };

void settings_defaults(Settings *s)
{
    s->show_hidden = 1; s->music_on = 1; s->volume = 100; s->sort_mode = 0; s->start_screen = 0;
    s->confirm_delete = 1; s->show_hints = 1; s->write_log = 0; s->pause_music_mc = 1; s->show_all_devices = 0; s->host_dev = 0; s->player[0] = 0;
}

int settings_to_text(const Settings *s, char *buf, int n)
{
    return snprintf(buf, n,
        "# PrismELF settings\nshow_hidden=%d\nmusic_on=%d\nvolume=%d\nsort_mode=%d\nstart_screen=%d\n"
        "confirm_delete=%d\nshow_hints=%d\nwrite_log=%d\npause_music_mc=%d\nshow_all_devices=%d\nhost_dev=%d\nplayer=%s\n",
        s->show_hidden, s->music_on, s->volume, s->sort_mode, s->start_screen,
        s->confirm_delete, s->show_hints, s->write_log, s->pause_music_mc, s->show_all_devices, s->host_dev, s->player);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void settings_from_text(Settings *s, const char *text)
{
    char line[192];
    const char *p = text;
    while (*p) {
        int n = 0;
        while (p[n] && p[n] != '\n' && n < (int)sizeof line - 1) { line[n] = p[n]; n++; }
        line[n] = 0;
        p += n; while (*p && *p != '\n') p++; if (*p == '\n') p++;
        if (line[0] == '#' || !strchr(line, '=')) continue;
        char *eq = strchr(line, '='); *eq = 0;
        int v = atoi(eq + 1);
        if (!strcmp(line, "player")) { char *e = eq + 1; size_t l = strlen(e); while (l && (e[l - 1] == '\r' || e[l - 1] == ' ')) e[--l] = 0; snprintf(s->player, sizeof s->player, "%s", e); continue; }
        if (!strcmp(line, "show_hidden")) s->show_hidden = clampi(v, 0, 1);
        else if (!strcmp(line, "music_on")) s->music_on = clampi(v, 0, 1);
        else if (!strcmp(line, "volume")) s->volume = clampi(v, 0, 100);
        else if (!strcmp(line, "sort_mode")) s->sort_mode = clampi(v, 0, 2);
        else if (!strcmp(line, "start_screen")) s->start_screen = clampi(v, 0, 1);
        else if (!strcmp(line, "confirm_delete")) s->confirm_delete = clampi(v, 0, 1);
        else if (!strcmp(line, "show_hints")) s->show_hints = clampi(v, 0, 1);
        else if (!strcmp(line, "write_log")) s->write_log = clampi(v, 0, 1);
        else if (!strcmp(line, "pause_music_mc")) s->pause_music_mc = clampi(v, 0, 1);
        else if (!strcmp(line, "host_dev")) s->host_dev = clampi(v, 0, 1);
        else if (!strcmp(line, "show_all_devices")) s->show_all_devices = clampi(v, 0, 1);
    }
}
