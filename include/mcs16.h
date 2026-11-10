#ifndef MCS16_H
#define MCS16_H

#include <stddef.h>
#include <stdint.h>

/*
 * S16_LE <-> float boundary conversion.
 *
 * The shared memory ring carries exactly what a real device would carry:
 * interleaved 16-bit samples.  Everything above this boundary is float, and
 * this is the only place the conversion happens, so a scaling change can never
 * hide in two different layers.
 *
 * Scaling: int16 / 32768.0, the same convention the ALSA world uses, so
 * -32768 maps to -1.0 and 32767 maps to just under 1.0.
 */

void mcs16_to_float(const int16_t *source, float *target, size_t count);

/* Clamps to [-32768, 32767] before narrowing; returns non-zero when any sample
 * had to be clipped (the caller may want to report it). */
int mcs16_from_float(const float *source, int16_t *target, size_t count);

#endif /* MCS16_H */
