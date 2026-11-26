/*
 * maya.h - the recompiled game as the hosts see it: boot, run a frame, the picture, the sound, input.
 * Portable C; the hosts (host/win32, later host/macos) are the only callers.
 */
#ifndef MAYA_H
#define MAYA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAYA_AUDIO_RATE 48000          /* stereo int16 frames a second */
#define MAYA_FRAME_HZ_NUM 7093790      /* PAL: 7,093,790 / 142,102 frames a second (49.92 Hz) */
#define MAYA_FRAME_HZ_DEN 142102

/* Boots the game from the data folder (the game's files: MAIN.CO ...); saves go to savedir (created when
 * needed).  0 on success; otherwise a message in err. */
int maya_boot(const char *datadir, const char *savedir, char *err, size_t errlen);

/* Runs the game to its next vertical blank.  1 while it runs; 0 when it ended (the player quit) or stopped on an
 * error (maya_error() then has the message). */
int maya_run_frame(void);

/* The last complete picture: w x h XRGB pixels (the display window, 320 x 200 for this game). */
const uint32_t *maya_picture(int *w, int *h);

/* Takes up to `frames` stereo frames of sound (interleaved int16, MAYA_AUDIO_RATE); returns how many. */
size_t maya_audio(int16_t *out, size_t frames);

/* Input: the pointer in game pixels (clamped), buttons bit 0 left, bit 1 right; Amiga raw key codes. */
void maya_mouse(int x, int y, int buttons);
void maya_key(int amiga_raw, int down);
/* The joystick in port 2: bit 0 up, 1 down, 2 left, 3 right, 4 fire. */
void maya_joystick(int bits);
void maya_keys_release(void);

const char *maya_error(void);
/* "Le Fetiche Maya" files check: 1 when dir holds the game (MAIN.CO). */
int maya_is_data_dir(const char *dir);

#ifdef __cplusplus
}
#endif

#endif
