#ifndef SETTINGS_H
#define SETTINGS_H
typedef struct {
    int show_hidden;      /* show .files / .folders */
    int music_on;         /* background music audible */
    int volume;           /* 0..100 */
    int sort_mode;        /* 0 name, 1 size, 2 type */
    int start_screen;     /* 0 USB, 1 devices list */
    int confirm_delete;
    int show_hints;       /* button hints at the bottom */
    int write_log;        /* write FileExplorer-Logs to disk */
    int pause_music_mc;   /* silence music while a memory card is being read (it stalls the IOP) */
    int host_dev;         /* list host: (PCSX2 / emulator shared folder) in the devices list */
    int show_all_devices; /* also list hdd0:, cdfs:, host:, rom0:, rom1: in the devices list */
    char player[128];     /* ELF that opens video and the audio formats this app cannot decode (empty = none) */
} Settings;
extern Settings cfg;
void settings_defaults(Settings *s);
int  settings_to_text(const Settings *s, char *buf, int n);
void settings_from_text(Settings *s, const char *text);   /* unknown keys ignored, values clamped */
#endif
