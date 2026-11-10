#define _POSIX_C_SOURCE 200809L

#include "mcs16.h"

void mcs16_to_float(const int16_t *source, float *target, size_t count)
{
    size_t index;

    if (source == NULL || target == NULL) {
        return;
    }
    for (index = 0u; index < count; ++index) {
        target[index] = (float)source[index] / 32768.0f;
    }
}

int mcs16_from_float(const float *source, int16_t *target, size_t count)
{
    size_t index;
    int clipped = 0;

    if (source == NULL || target == NULL) {
        return 0;
    }
    for (index = 0u; index < count; ++index) {
        float scaled = source[index] * 32768.0f;

        if (scaled >= 32767.0f) {
            target[index] = 32767;
            clipped = 1;
        } else if (scaled <= -32768.0f) {
            target[index] = -32768;
            clipped = 1;
        } else {
            target[index] = (int16_t)scaled;
        }
    }
    return clipped;
}
