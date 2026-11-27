#ifndef DSP_H
#define DSP_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    const double (*sos)[5];
    double state[5][2];
    double hp_x, hp_y, hp_b, hp_a;
    bool hp;
} uso_filter_t;
int uso_frequency_index(uint8_t code);
bool uso_filter_init(uso_filter_t *f, uint8_t code, bool hp);
void uso_filter_hp(uso_filter_t *f, bool enabled);
int32_t uso_filter_step(uso_filter_t *f, int32_t input);
unsigned uso_pack_sample(int32_t value, bool bits32, uint8_t out[6]);
#endif
