#ifndef DSP_H
#define DSP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const double (*sos)[5];
    double dyn_sos[2][5];   /* Runtime-computed 4th-order Butterworth (2 biquads) for arbitrary Fs */
    double state[5][2];
    unsigned sos_count;     /* 0 = bypass, 2 = dynamic Butter4, 5 = legacy Butter10 */
    double fs_hz;
    double hp_x, hp_y, hp_b, hp_a;
    bool hp;
    /* Anti-alias integer decimator */
    uint8_t decim_factor;   /* 1..16 */
    uint8_t decim_count;
    double  decim_accum;
} uso_filter_t;

/* Legacy API (100% preserved) */
int uso_frequency_index(uint8_t code);
bool uso_filter_init(uso_filter_t *f, uint8_t code, bool hp);
void uso_filter_hp(uso_filter_t *f, bool enabled);
int32_t uso_filter_step(uso_filter_t *f, int32_t input);
unsigned uso_pack_sample(int32_t value, bool bits32, uint8_t out[6]);

/* Extended V2 API: arbitrary Fs, LPF bypass / dynamic Butter4 / legacy Butter10, dynamic HPF, decimation */
bool uso_filter_init_ex(
    uso_filter_t *f,
    double       fs_hz,
    uint8_t      lpf_mode,
    double       lpf_cutoff_hz,
    double       hpf_cutoff_hz,
    uint8_t      decim_factor
);
void uso_filter_set_hp_cutoff(uso_filter_t *f, double hpf_cutoff_hz);
bool uso_filter_step_ex(uso_filter_t *f, int32_t input, int32_t *output, bool *clipped);
unsigned uso_pack_sample_ex(int32_t value, bool bits32, bool overflow, uint8_t out[6]);

#endif
