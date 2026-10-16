#ifndef AUDIO_H
#define AUDIO_H
int  bgm_boot(int usb_already_checked);                    /* boot: USB BGM/bgm.wav|mp3 first (creates BGM/ + README if missing), else the embedded music */
int  bgm_start_embedded(void);          /* WAV baked into the ELF from repo music/, if any */
int  bgm_try_files(void);               /* mass:/ then mc0:/ then mc1:/ PrismELF/bgm.wav */
int  bgm_play_file(const char *path);   /* play a song from any device instead of the current music (blocking); 0 = ok */
int  bgm_play_file_async(const char *path, int loop);   /* same, loaded in the background; poll with bgm_load_poll. loop=1 repeats it */
int  bgm_restore_async(void);           /* back to the default music, loaded in the background */
int  bgm_load_poll(int *rc);            /* 0 idle, 1 loading, 2 finished (*rc = result, 0 = ok) */
int  bgm_looping(void);                 /* a user-chosen song that repeats */
int  bgm_restore_default(void);         /* back to the embedded / bgm.wav music */
int  bgm_active(void);
const char *bgm_title(void);            /* what is playing */
void bgm_poll(void);                    /* call every frame: when a user-played track ends, return to the default music */
void bgm_set_muted(int m);
void bgm_set_volume(int pct);           /* 0..100 */
void bgm_suspend(void);                 /* stop feeding audsrv (e.g. while a memory card is read) */
void bgm_resume(void);
void bgm_toggle_mute(void);
int  bgm_muted(void);
void bgm_toggle_pause(void);
int  bgm_paused(void);
void bgm_stop(void);
#endif
