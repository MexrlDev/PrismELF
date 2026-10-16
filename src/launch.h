#ifndef LAUNCH_H
#define LAUNCH_H
void iop_init(void);                    /* reset IOP + load embedded modules + fileXioInit */
int  iop_load_mc(void);                 /* load the memory-card driver on demand; 0 = ok */
int  launch_elf(const char *path);      /* does not return on success */
int  launch_elf_args(const char *path, int argc, char *argv[]);   /* same, passing arguments (argv[0] = the ELF, argv[1] = a file to open) */
void exit_to_osd(void);
#endif
