#ifndef USO_BOOTLOADER

#include "dsp.h"
#include "uso_config.h"
#include "uso_proto_v2.h"
#include <limits.h>
#include <math.h>
#include <string.h>
#include "filter_coeffs.inc"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const double kLegacyCutoffsHz[14] = {
    20.0, 15.0, 12.0, 9.0, 6.5, 4.3, 3.0,
    11.0, 8.0,  6.0,  4.0, 3.0, 2.5, 2.0
};

int uso_frequency_index(uint8_t code)
{
    if (code <= 6) return code;
    if (code >= 0x10 && code <= 0x16) return 7 + code - 0x10;
    return -1;
}

static void compute_hp_coeffs(uso_filter_t *f, double hpf_cutoff_hz)
{
    if (!f || hpf_cutoff_hz <= 0.0 || f->fs_hz <= 1.0) {
        if (f) {
            f->hp = false;
            f->hp_x = f->hp_y = 0.0;
        }
        return;
    }
    double fc = hpf_cutoff_hz;
    if (fc > 0.4 * f->fs_hz) fc = 0.4 * f->fs_hz;
    double k = tan(M_PI * fc / f->fs_hz);
    f->hp_b = 1.0 / (1.0 + k);
    f->hp_a = (k - 1.0) / (k + 1.0);
    f->hp_x = 0.0;
    f->hp_y = 0.0;
    f->hp = true;
}

static void design_butter2_biquad(double fs_hz, double fc_hz, double q_factor, double out[5])
{
    /* Exact bilinear transform for 2nd-order lowpass section:
     * H(s) = 1 / (s^2 + s/Q + 1), s = (1/K)*(1 - z^-1)/(1 + z^-1), K = tan(pi * fc / fs) */
    double k = tan(M_PI * fc_hz / fs_hz);
    double k2 = k * k;
    double norm = 1.0 / (1.0 + k / q_factor + k2);
    out[0] = k2 * norm;                         /* b0 */
    out[1] = 2.0 * out[0];                      /* b1 */
    out[2] = out[0];                            /* b2 */
    out[3] = 2.0 * (k2 - 1.0) * norm;           /* a1 */
    out[4] = (1.0 - k / q_factor + k2) * norm;  /* a2 */
}

void uso_filter_hp(uso_filter_t *f, bool enabled)
{
    if (!f) return;
    f->hp = enabled;
    f->hp_x = f->hp_y = 0.0;
}

void uso_filter_set_hp_cutoff(uso_filter_t *f, double hpf_cutoff_hz)
{
    compute_hp_coeffs(f, hpf_cutoff_hz);
}

bool uso_filter_init(uso_filter_t *f, uint8_t code, bool hp)
{
    int index = uso_frequency_index(code);
    if (!f || index < 0) return false;
    memset(f, 0, sizeof(*f));
    f->fs_hz = (double)USO_SAMPLE_RATE_HZ;
    f->sos = filter_sos[index];
    f->sos_count = 5u;
    f->decim_factor = 1u;
    compute_hp_coeffs(f, USO_HP_CUTOFF_HZ);
    uso_filter_hp(f, hp);
    return true;
}

bool uso_filter_init_ex(
    uso_filter_t *f,
    double       fs_hz,
    uint8_t      lpf_mode,
    double       lpf_cutoff_hz,
    double       hpf_cutoff_hz,
    uint8_t      decim_factor
)
{
    if (!f || fs_hz < 10.0 || fs_hz > 40000.0) return false;
    if (decim_factor < 1u || decim_factor > 16u) return false;

    memset(f, 0, sizeof(*f));
    f->fs_hz = fs_hz;
    f->decim_factor = decim_factor;

    if (lpf_mode == USO_V2_LPF_BYPASS) {
        f->sos = NULL;
        f->sos_count = 0u;
    } else if (lpf_mode == USO_V2_LPF_BUTTER4) {
        /* 4th-order Butterworth = 2 cascaded 2nd-order sections with exact pole Qs */
        double fc = lpf_cutoff_hz;
        double max_fc = 0.42 * fs_hz;
        if (decim_factor > 1u) {
            double nyq_decim = 0.40 * (fs_hz / (double)decim_factor);
            if (fc <= 0.0 || fc > nyq_decim) fc = nyq_decim;
        }
        if (fc <= 0.5) fc = 0.25 * fs_hz;
        if (fc > max_fc) fc = max_fc;

        /* Q0 = 1 / (2 * cos(3*pi/8)) = 0.541196100146197, Q1 = 1 / (2 * cos(pi/8)) = 1.3065629648763766 */
        design_butter2_biquad(fs_hz, fc, 0.5411961001461970, f->dyn_sos[0]);
        design_butter2_biquad(fs_hz, fc, 1.3065629648763766, f->dyn_sos[1]);
        f->sos = f->dyn_sos;
        f->sos_count = 2u;
    } else if (lpf_mode == USO_V2_LPF_LEGACY10) {
        /* Pick closest cutoff from committed 14-entry Fs=400 Hz table */
        int best = 0;
        double best_diff = 1e9;
        double target = (lpf_cutoff_hz > 0.5) ? lpf_cutoff_hz : 20.0;
        for (int i = 0; i < 14; ++i) {
            double d = fabs(kLegacyCutoffsHz[i] - target);
            if (d < best_diff) {
                best_diff = d;
                best = i;
            }
        }
        f->sos = filter_sos[best];
        f->sos_count = 5u;
    } else {
        return false;
    }

    compute_hp_coeffs(f, hpf_cutoff_hz);
    return true;
}

static int32_t clamp_to_i32(double x, bool *clipped)
{
    if (x >= (double)INT32_MAX) {
        if (clipped) *clipped = true;
        return INT32_MAX;
    }
    if (x <= (double)INT32_MIN) {
        if (clipped) *clipped = true;
        return INT32_MIN;
    }
    return (int32_t)llround(x);
}

static double filter_core_double(uso_filter_t *f, int32_t input)
{
    double x = (double)input;
    if (f->hp) {
        double y = f->hp_b * (x - f->hp_x) - f->hp_a * f->hp_y;
        f->hp_x = x;
        f->hp_y = y;
        x = y;
    }
    if (f->sos && f->sos_count > 0u) {
        for (unsigned i = 0; i < f->sos_count; ++i) {
            const double *c = f->sos[i];
            double y = c[0] * x + f->state[i][0];
            f->state[i][0] = c[1] * x - c[3] * y + f->state[i][1];
            f->state[i][1] = c[2] * x - c[4] * y;
            x = y;
        }
    }
    return x;
}

int32_t uso_filter_step(uso_filter_t *f, int32_t input)
{
    return clamp_to_i32(filter_core_double(f, input), NULL);
}

bool uso_filter_step_ex(uso_filter_t *f, int32_t input, int32_t *output, bool *clipped)
{
    if (!f || !output) return false;
    if (clipped) *clipped = false;

    double y = filter_core_double(f, input);
    if (f->decim_factor <= 1u) {
        *output = clamp_to_i32(y, clipped);
        return true;
    }

    f->decim_accum += y;
    if (++f->decim_count >= f->decim_factor) {
        double avg = f->decim_accum / (double)f->decim_factor;
        f->decim_accum = 0.0;
        f->decim_count = 0u;
        *output = clamp_to_i32(avg, clipped);
        return true;
    }
    return false;
}

unsigned uso_pack_sample(int32_t value, bool bits32, uint8_t out[6])
{
    return uso_pack_sample_ex(value, bits32, false, out);
}

unsigned uso_pack_sample_ex(int32_t value, bool bits32, bool overflow, uint8_t out[6])
{
    uint32_t raw = (uint32_t)value;
    uint8_t trailer = overflow ? 0xFEu : 0xFFu;
    out[0] = 0;
    out[1] = (uint8_t)(raw >> 24);
    out[2] = (uint8_t)(raw >> 16);
    out[3] = (uint8_t)(raw >> 8);
    if (bits32) {
        out[4] = (uint8_t)raw;
        out[5] = trailer;
        return 6;
    }
    out[4] = trailer;
    return 5;
}

#endif /* !USO_BOOTLOADER */
