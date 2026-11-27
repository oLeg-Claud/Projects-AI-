#include "dsp.h"
#include "uso_config.h"
#include <limits.h>
#include <math.h>
#include <string.h>
#include "filter_coeffs.inc"

int uso_frequency_index(uint8_t code)
{
    if (code <= 6) return code;
    if (code >= 0x10 && code <= 0x16) return 7 + code - 0x10;
    return -1;
}
void uso_filter_hp(uso_filter_t *f, bool enabled)
{
    f->hp = enabled;
    f->hp_x = f->hp_y = 0;
}
bool uso_filter_init(uso_filter_t *f, uint8_t code, bool hp)
{
    int index = uso_frequency_index(code);
    if (index < 0) return false;
    memset(f, 0, sizeof(*f));
    f->sos = filter_sos[index];
    double k = tan(3.14159265358979323846 * USO_HP_CUTOFF_HZ / USO_SAMPLE_RATE_HZ);
    f->hp_b = 1.0 / (1.0 + k);
    f->hp_a = (k - 1.0) / (k + 1.0);
    uso_filter_hp(f, hp);
    return true;
}
int32_t uso_filter_step(uso_filter_t *f, int32_t input)
{
    double x = (double)input;
    if (f->hp) {
        double y = f->hp_b * (x - f->hp_x) - f->hp_a * f->hp_y;
        f->hp_x = x; f->hp_y = y; x = y;
    }
    for (unsigned i = 0; i < 5; ++i) {
        const double *c = f->sos[i];
        double y = c[0] * x + f->state[i][0];
        f->state[i][0] = c[1] * x - c[3] * y + f->state[i][1];
        f->state[i][1] = c[2] * x - c[4] * y;
        x = y;
    }
    if (x >= INT32_MAX) return INT32_MAX;
    if (x <= INT32_MIN) return INT32_MIN;
    return (int32_t)llround(x);
}
unsigned uso_pack_sample(int32_t value, bool bits32, uint8_t out[6])
{
    uint32_t raw = (uint32_t)value;
    out[0] = 0;
    out[1] = (uint8_t)(raw >> 24);
    out[2] = (uint8_t)(raw >> 16);
    out[3] = (uint8_t)(raw >> 8);
    if (bits32) { out[4] = (uint8_t)raw; out[5] = 0xff; return 6; }
    out[4] = 0xff;
    return 5;
}
