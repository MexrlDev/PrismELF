#ifndef FLACDEC_H
#define FLACDEC_H
/* Small FLAC decoder (mono / stereo, 4-32 bit, any rate). Output is always 16-bit stereo. */
typedef struct FlacDec FlacDec;
FlacDec *flacdec_open(const unsigned char *buf, unsigned size, int *rate, int *channels);   /* NULL if not a usable FLAC */
int  flacdec_read(FlacDec *d, short *out, int max_frames);   /* stereo frames written; 0 = end of stream */
void flacdec_rewind(FlacDec *d);
void flacdec_close(FlacDec *d);
#endif
