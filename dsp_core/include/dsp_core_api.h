#ifndef DSP_CORE_API_H
#define DSP_CORE_API_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
  #ifdef DSPCORE_EXPORTS
    #define DSPCORE_API __declspec(dllexport)
  #elif defined(DSPCORE_STATIC)
    #define DSPCORE_API
  #else
    #define DSPCORE_API __declspec(dllimport)
  #endif
  #define DSPCORE_CALL __stdcall
#else
  #if __GNUC__ >= 4
    #define DSPCORE_API __attribute__((visibility("default")))
  #else
    #define DSPCORE_API
  #endif
  #define DSPCORE_CALL
#endif

/* ============================================================================
 * Constants matching legacy PROWODKA & TZ §3.2
 * ============================================================================ */
#define DSP_DLPOL_R        13
#define DSP_SYNC_CHIPS     128
#define DSP_VEC_CHIPS      32
#define DSP_SYNC_SAMPLES   (DSP_SYNC_CHIPS * DSP_DLPOL_R)  /* 1664 */
#define DSP_VEC_SAMPLES    (DSP_VEC_CHIPS  * DSP_DLPOL_R)  /* 416  */
#define DSP_MAX_VECTORS    128
#define DSP_RM14_CODES     16

/* ============================================================================
 * Enumerations (C ABI safe int32_t)
 * ============================================================================ */
typedef enum DspSourceType {
    DSP_SRC_LEGACY_SERIAL = 0,  /* Legacy USO: 57600 8N1, 5-byte (24-bit) / 6-byte (32-bit uco32) */
    DSP_SRC_NEW_ADC32     = 1,  /* New 32-bit ADC USO: 8 samples/half-period (256 samples/vector) */
    DSP_SRC_FILE_PLAYER   = 2   /* Binary .smp (sun32) emulator with variable playback speed */
} DspSourceType;

typedef enum DspFrameFormat {
    DSP_FMT_STD_GK        = 1,  /* №1: ZTS + GK, no pauses (39 vectors) */
    DSP_FMT_NDM_GK_RETR   = 2,  /* №2: GK + NDM + Repeater (104 vectors + 7 pauses) */
    DSP_FMT_GK_RETR       = 11, /* №11: GK + Repeater (43 vectors + 4 pauses) */
    DSP_FMT_STD           = 12, /* №12: Short without GK (33 vectors) */
    DSP_FMT_STD_RETR      = 13, /* №13: Without GK + Repeater (37 vectors + 4 pauses) */
    DSP_FMT_GEOMASH       = 30, /* №30: Geomash packet with CRC32 (34 vectors) */
    DSP_FMT_GEOMASH_RETR  = 31  /* №31: Geomash + Repeater with CRC32 (35 vectors + 1 pause) */
} DspFrameFormat;

typedef enum DspToolfaceChannel {
    DSP_TF_GRAV = 0, /* Gravity toolface (OTK1..7) */
    DSP_TF_MAG  = 1, /* Magnetic / Azimuthal toolface (AZ_OTK1..7) */
    DSP_TF_STAT = 2  /* High-resolution static toolface (OTKu) */
} DspToolfaceChannel;

typedef enum DspStatusCode {
    DSP_OK                  = 0,
    DSP_ERR_INVALID_ARG     = -1,
    DSP_ERR_PORT_OPEN       = -2,
    DSP_ERR_FILE_OPEN       = -3,
    DSP_ERR_ALREADY_RUNNING = -4,
    DSP_ERR_NOT_FOUND       = -5,
    DSP_ERR_OVERFLOW        = -6
} DspStatusCode;

/* ============================================================================
 * Configuration & POD Output Structures (P/Invoke compatible)
 * ============================================================================ */
typedef struct DspConfig {
    int32_t source_type;       /* DspSourceType */
    char    port_or_path[260]; /* COM port ("COM3", "/dev/ttyUSB0") or .smp path */
    int32_t uso_bit_mode;      /* 24 or 32 (uco32) */
    int32_t gain_index;        /* 0..5 -> 0x20..0x25 (24-bit) or 0x90..0x95 (32-bit) */
    double  carrier_freq_hz;   /* 16, 10, 7, 5, 3.6, 2.5, 1.8 */
    double  sample_rate_hz;    /* 250.0 for legacy USO; or 16.0 * carrier_freq_hz for new 8 sps/bit USO */
    int32_t corrector_enabled; /* 1 -> 0x71, 0 -> 0x72 */
    int32_t format_id;         /* DspFrameFormat (1, 2, 11, 12, 13, 30, 31) */
    int32_t ncod;              /* RM(1,4) codebook: 0 or 1 */
    int32_t search_window_ig;  /* Sliding window ±iG samples (default 13) */
    int32_t dcp_threshold;     /* Reliability threshold % (0..100, default 55) */
    int32_t enable_step_track; /* 1 = enable PIK5/ID dynamic H drift tracking */
    int32_t playback_speed;    /* File player speed multiplier: 1, 2, 4, 8 (0 = max speed no sleep) */
    char    fir_file_path[260];/* Optional path to KIH*.dat file (empty = passthrough / built-in) */

    /* Reference geophysical parameters for surface inclinometry */
    double  ref_hop_nt;        /* Reference magnetic field Hop, nT (e.g. 46500) */
    double  ref_dip_deg;       /* Reference magnetic dip, deg (e.g. 71.0) */
    double  ref_dec_deg;       /* Magnetic declination + convergence ums, deg */
    double  ref_az_op_deg;     /* Reference azimuth for NUBT Hz sign resolution */
    double  kgk_zts;           /* Gamma-ray scale factor (default 1.0) */
    int32_t use_nubt;          /* 1 = enable NUBT short-collar correction */
} DspConfig;

typedef struct DspToolfaceEvent {
    int64_t  timestamp_us;     /* Monotonic/epoch microseconds */
    int32_t  channel;          /* DspToolfaceChannel */
    int32_t  vector_index;     /* Index within frame where OTK completed */
    double   angle_deg;        /* 0.0 .. 360.0 */
    int32_t  reliability_pct;  /* 0 .. 100 */
    double   latency_ms;       /* Measured DSP decode latency */
} DspToolfaceEvent;

typedef struct DspVectorEvent {
    int32_t  frame_number;
    int32_t  vector_index;
    int32_t  total_vectors;
    uint8_t  code_5bit;        /* 0..31 */
    int32_t  reliability_pct;  /* 0..100 */
    int32_t  peak_offset_ig;   /* Best offset inside [-iG, +iG) */
    int32_t  phase_polarity;   /* +1 or -1 */
    int32_t  code_scores_pct[DSP_RM14_CODES]; /* Signed normalized correlation % (-100..+100) for all 16 RM(1,4) codes */
} DspVectorEvent;

typedef struct DspFrameData {
    int32_t  frame_number;
    int32_t  format_id;
    int32_t  is_static_mode;   /* ctat (1 = static, 0 = drilling) */
    int32_t  service_mode;     /* iot (0..10) */
    int32_t  phase_polarity;   /* +1 or -1 */
    int32_t  sync_corr_pct;    /* Sync preamble correlation % */
    double   step_h;           /* Current resampler step H */
    int32_t  last_pik_drift;   /* Last PIK5 drift correction ID */
    int32_t  qual_stat;        /* Static quality flag */
    int32_t  crc_valid;        /* -1 = N/A, 0 = bad CRC32, 1 = valid CRC32 (Geomash) */

    /* ZTS Inclinometry & Toolface */
    double   otk_grav_deg;
    double   otk_mag_deg;
    double   otk_stat_deg;
    double   zu_deg;
    double   az_raw_deg;
    double   az_korr_deg;      /* -9999.0 if invalid */
    double   az_true_deg;      /* -9999.0 if invalid */
    double   gk_zts;
    double   g_total;
    double   gx, gy, gz;
    int32_t  hx, hy, hz;       /* nT */
    double   h_total;          /* nT */
    double   dip_calc_deg;

    /* ZTS Service */
    int32_t  rpm_zts;
    double   temp_zts;
    double   volt_zts;
    int32_t  vibr_zts;
    int32_t  battery_pct;
    double   kks_signal_mv;

    /* NDM (Near-Bit Module, Format 2) */
    double   zu_ndm_deg;
    double   gk_up_ndm;
    double   gk_dn_ndm;
    double   temp_ndm;
    int32_t  rpm_ndm;
    double   wob_ndm;
    double   torque_ndm;
    double   press_ndm;
    int32_t  tok_ndm;
    double   peak_ndm;
    double   du_ndm;
    double   mah_ndm;

    /* Raw decoded vectors & reliabilities */
    int32_t  vector_count;
    uint8_t  vectors_5bit[DSP_MAX_VECTORS];
    uint8_t  reliabilities[DSP_MAX_VECTORS];
} DspFrameData;

typedef struct DspSignalBlock {
    const double* raw_samples;
    const double* filtered_samples;
    int32_t       sample_count;
    int32_t       overflow_flag; /* 1 if 0xFE marker observed */
} DspSignalBlock;

/* ============================================================================
 * Callback Types (Zero-Latency Architecture — TZ §3.4)
 * ============================================================================ */
typedef void (DSPCORE_CALL *DspOnToolfaceCallback)(const DspToolfaceEvent* evt, void* user_data);
typedef void (DSPCORE_CALL *DspOnVectorCallback)(const DspVectorEvent* evt, void* user_data);
typedef void (DSPCORE_CALL *DspOnFrameCallback)(const DspFrameData* frame, void* user_data);
typedef void (DSPCORE_CALL *DspOnSignalCallback)(const DspSignalBlock* block, void* user_data);

typedef struct DspCallbacks {
    DspOnToolfaceCallback on_toolface;
    DspOnVectorCallback   on_vector;
    DspOnFrameCallback    on_frame;
    DspOnSignalCallback   on_signal;
    void*                 user_data;
} DspCallbacks;

/* Opaque engine handle */
typedef struct DspEngine DspEngine;

/* ============================================================================
 * Public C ABI Functions
 * ============================================================================ */

/** Populate a DspConfig structure with safe default values. */
DSPCORE_API void DSPCORE_CALL dsp_config_default(DspConfig* cfg);

/** Create a new DSP engine instance. Returns NULL on allocation failure. */
DSPCORE_API DspEngine* DSPCORE_CALL dsp_create(const DspConfig* cfg, const DspCallbacks* callbacks);

/** Destroy a DSP engine instance (stops worker thread if active). */
DSPCORE_API void DSPCORE_CALL dsp_destroy(DspEngine* engine);

/** Update runtime parameters (gain, corrector, threshold, format, passport refs). */
DSPCORE_API int32_t DSPCORE_CALL dsp_configure(DspEngine* engine, const DspConfig* cfg);

/** Load FIR filter coefficients from a legacy KIH*.dat text file. Returns tap count or < 0 on error. */
DSPCORE_API int32_t DSPCORE_CALL dsp_load_fir_file(DspEngine* engine, const char* path);

/** Start background acquisition thread for configured IUsoSource. */
DSPCORE_API int32_t DSPCORE_CALL dsp_start(DspEngine* engine);

/** Stop background acquisition thread cleanly. */
DSPCORE_API void DSPCORE_CALL dsp_stop(DspEngine* engine);

/**
 * Push raw ADC samples directly into the DSP pipeline (synchronous mode for
 * unit tests, custom drivers, or offline batch processing without background threads).
 */
DSPCORE_API int32_t DSPCORE_CALL dsp_push_samples(
    DspEngine* engine,
    const double* samples,
    int32_t count,
    int32_t overflow_flag
);

/** Copy latest decoded frame state safely. */
DSPCORE_API int32_t DSPCORE_CALL dsp_get_last_frame(DspEngine* engine, DspFrameData* out_frame);

/** Query current resampler step H and active SIMD acceleration string ("AVX2", "SSE2", "SCALAR"). */
DSPCORE_API double DSPCORE_CALL dsp_get_step_h(const DspEngine* engine);
DSPCORE_API const char* DSPCORE_CALL dsp_get_simd_isa(void);

/** Returns 1 if background acquisition worker is currently active, 0 otherwise. */
DSPCORE_API int32_t DSPCORE_CALL dsp_is_running(const DspEngine* engine);

/**
 * Probe a serial port using the legacy USO32 handshake (TRAEKT2k.cpp N_USO):
 *   TX 0x10 (stop stream) -> purge -> TX 0xAA -> TX 0x66 -> RX [0x01, <USO_ID>, _, _, 0xFF]
 * Returns DSP_OK and writes *out_uso_id if detected, or negative DspStatusCode.
 */
DSPCORE_API int32_t DSPCORE_CALL dsp_probe_legacy_uso(const char* port_name, int32_t* out_uso_id);

/**
 * Automatically enumerate all OS serial ports (Windows SERIALCOMM registry / Linux ttyUSB+ttyACM)
 * and probe each port until a legacy USO32 responds to 0x66.
 * Writes the discovered port name (e.g. "COM4" or "/dev/ttyUSB0") into out_port_buf and sets *out_uso_id.
 */
DSPCORE_API int32_t DSPCORE_CALL dsp_scan_legacy_uso(
    char* out_port_buf,
    int32_t buf_len,
    int32_t* out_uso_id
);

/**
 * Utility: Parse one legacy 5-byte (24-bit) or 6-byte (32-bit uco32) USO packet.
 * Verifies 0x00 header and 0xFF/0xFE trailer. Returns DSP_OK, DSP_ERR_OVERFLOW (0xFE), or DSP_ERR_INVALID_ARG.
 */
DSPCORE_API int32_t DSPCORE_CALL dsp_parse_legacy_packet(
    const uint8_t* packet,
    int32_t packet_len,
    int32_t bit_mode,
    int32_t* out_sample
);

#ifdef __cplusplus
}
#endif

#endif /* DSP_CORE_API_H */
