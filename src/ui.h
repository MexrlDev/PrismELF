#ifndef UI_H
#define UI_H
void ui_load_images(void);
int  ui_boot_settings(void);            /* load settings during the splash, before music starts; 1 = USB present */
void ui_splash(void (*work)(void));
void ui_run(void);
#endif
