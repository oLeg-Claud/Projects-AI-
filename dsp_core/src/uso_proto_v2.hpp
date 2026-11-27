#ifndef DSP_USO_PROTO_V2_HPP
#define DSP_USO_PROTO_V2_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dsp {
namespace usov2 {

/* Wire constants mirrored 1:1 from uso_emb/include/uso_proto_v2.h */
static constexpr uint8_t  kProtoVersion     = 2u;
static constexpr uint8_t  kMagicHostToUso0  = 0xA5u;
static constexpr uint8_t  kMagicHostToUso1  = 0x5Au;
static constexpr uint8_t  kMagicUsoToHost0  = 0x5Au;
static constexpr uint8_t  kMagicUsoToHost1  = 0xA5u;

static constexpr uint8_t  kHdrSize          = 7u;
static constexpr uint8_t  kCrcSize          = 2u;
static constexpr uint8_t  kOverhead         = kHdrSize + kCrcSize; /* 9 */
static constexpr uint8_t  kMaxPayload       = 54u;
static constexpr uint8_t  kMaxFrame         = kOverhead + kMaxPayload; /* 63 < 64 USB FS MPS */
static constexpr uint8_t  kBatchHdrSize     = 12u;
static constexpr uint8_t  kMaxBatchSamples  = 10u;

enum MsgType : uint8_t {
    CMD_HELLO       = 0x01u,
    CMD_CONFIGURE   = 0x02u,
    CMD_START       = 0x03u,
    CMD_STOP        = 0x04u,
    CMD_SET_LIVE    = 0x05u,
    CMD_GET_DIAG    = 0x06u,
    CMD_RECALIBRATE = 0x07u,
    CMD_RESET_ADC   = 0x08u,

    RSP_ACK         = 0x80u,
    RSP_HELLO       = 0x81u,
    RSP_START       = 0x83u,
    RSP_DIAG        = 0x86u,
    RSP_CAL         = 0x87u,
    MSG_DATA_BATCH  = 0x90u
};

enum Status : uint8_t {
    ST_OK              = 0u,
    ST_ERR_BAD_LEN     = 1u,
    ST_ERR_BAD_PARAM   = 2u,
    ST_ERR_ADC_FAULT   = 3u,
    ST_ERR_BUSY        = 4u,
    ST_ERR_CAL_FAIL    = 5u,
    ST_ERR_UNKNOWN_CMD = 6u
};

enum DrCode : uint8_t {
    DR_20    = 0x04u,
    DR_50    = 0x05u,
    DR_60    = 0x06u,
    DR_100   = 0x07u,
    DR_400   = 0x08u,
    DR_1200  = 0x09u,
    DR_2400  = 0x0Au,
    DR_4800  = 0x0Bu,
    DR_AUTO  = 0xFFu
};

enum AdcFilter : uint8_t {
    FLT_SINC1 = 0u,
    FLT_SINC2 = 1u,
    FLT_SINC3 = 2u,
    FLT_SINC4 = 3u,
    FLT_FIR   = 4u
};

enum LpfMode : uint8_t {
    LPF_BYPASS   = 0u,
    LPF_BUTTER4  = 1u,
    LPF_LEGACY10 = 2u
};

static constexpr uint8_t kAdcFlagChop       = (1u << 0);
static constexpr uint8_t kAdcFlagVbias      = (1u << 1);
static constexpr uint8_t kAdcFlagPgaBypass  = (1u << 2);
static constexpr uint8_t kStreamAutoRecover = (1u << 0);

static constexpr uint8_t kBatchPgaOvf       = (1u << 0);
static constexpr uint8_t kBatchAdcAlarm     = (1u << 1);
static constexpr uint8_t kBatchGapDropped   = (1u << 2);
static constexpr uint8_t kBatchHpfActive    = (1u << 3);
static constexpr uint8_t kBatchLpfActive    = (1u << 4);
static constexpr uint8_t kBatchChopActive   = (1u << 5);

static constexpr uint8_t kLiveMaskGain      = (1u << 0);
static constexpr uint8_t kLiveMaskHpf       = (1u << 1);
static constexpr uint8_t kLiveMaskLpf       = (1u << 2);
static constexpr uint8_t kLiveMaskBatch     = (1u << 3);

static constexpr uint8_t kCfgWireLen        = 16u;
static constexpr uint8_t kLiveWireLen       = 8u;
static constexpr uint8_t kHelloRspWireLen   = 28u;
static constexpr uint8_t kStartRspWireLen   = 12u;
static constexpr uint8_t kDiagRspWireLen    = 44u;

inline void put_u16_le(uint8_t* dst, uint16_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFFu);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

inline void put_u32_le(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFFu);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

inline uint16_t get_u16_le(const uint8_t* src) {
    return static_cast<uint16_t>(static_cast<uint16_t>(src[0]) | (static_cast<uint16_t>(src[1]) << 8));
}

inline uint32_t get_u32_le(const uint8_t* src) {
    return static_cast<uint32_t>(src[0]) |
           (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16) |
           (static_cast<uint32_t>(src[3]) << 24);
}

inline uint16_t crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (unsigned bit = 0; bit < 8u; ++bit) {
            if (crc & 0x8000u) crc = static_cast<uint16_t>((crc << 1) ^ 0x1021u);
            else crc = static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

struct WireConfig {
    uint16_t carrier_freq_mhz   = 5000u;
    uint8_t  dr_code            = DR_AUTO;
    uint8_t  decimation         = 0u;
    uint8_t  pga_gain           = 2u;
    uint8_t  adc_filter         = FLT_SINC4;
    uint8_t  adc_flags          = kAdcFlagVbias;
    uint8_t  inpmux             = 0x89u;
    uint8_t  dsp_lpf_mode       = LPF_BYPASS;
    uint8_t  batch_size         = 0u;
    uint16_t dsp_lpf_cutoff_dhz = 0u;
    uint16_t dsp_hpf_cutoff_mhz = 100u;
    uint8_t  stream_flags       = kStreamAutoRecover;
    uint8_t  reserved           = 0u;
};

struct HelloRsp {
    uint8_t  proto_version = 0;
    uint8_t  device_id     = 0;
    uint8_t  fw_major      = 0;
    uint8_t  fw_minor      = 0;
    uint8_t  fw_patch      = 0;
    uint8_t  adc_id        = 0;
    uint8_t  ready         = 0;
    uint8_t  fault         = 0;
    uint32_t capabilities  = 0;
    uint32_t adc_clock_hz  = 7372800u;
    uint8_t  mcu_uid[12]{};
};

struct StartRsp {
    uint8_t  status           = 0;
    uint8_t  dr_code          = 0;
    uint8_t  decimation       = 1;
    uint8_t  batch_size       = 1;
    uint8_t  pga_gain         = 0;
    uint8_t  adc_filter       = 0;
    uint8_t  adc_flags        = 0;
    uint8_t  inpmux           = 0x89u;
    uint16_t effective_fs_dhz = 4000u;
    uint16_t hw_fs_dhz        = 4000u;
};

struct DiagRsp {
    uint32_t uptime_ms        = 0;
    uint32_t samples          = 0;
    uint32_t dropped_samples  = 0;
    uint32_t missed_drdy      = 0;
    uint32_t checksum_errors  = 0;
    uint32_t adc_errors       = 0;
    uint32_t stale_count      = 0;
    uint32_t tx_overflows     = 0;
    uint32_t rx_overflows     = 0;
    uint32_t v2_crc_errors    = 0;
    uint16_t effective_fs_dhz = 0;
    uint8_t  adc_status       = 0;
    uint8_t  state_flags      = 0;
};

struct DataBatch {
    uint32_t timestamp_ms     = 0;
    uint32_t first_sample_idx = 0;
    uint16_t effective_fs_dhz = 0;
    uint8_t  flags            = 0;
    uint8_t  pga_gain         = 0;
    uint8_t  sample_count     = 0;
    int32_t  samples[kMaxBatchSamples]{};
};

struct Frame {
    uint8_t  version     = 0;
    uint8_t  flags       = 0;
    uint8_t  msg_type    = 0;
    uint16_t seq         = 0;
    uint8_t  payload_len = 0;
    uint8_t  payload[kMaxPayload]{};
};

inline unsigned encode_frame(
    uint8_t magic0,
    uint8_t magic1,
    uint8_t msg_type,
    uint16_t seq,
    const uint8_t* payload,
    uint8_t payload_len,
    uint8_t out[kMaxFrame]
) {
    if (!out || payload_len > kMaxPayload) return 0u;
    out[0] = magic0;
    out[1] = magic1;
    out[2] = static_cast<uint8_t>(kProtoVersion << 4);
    out[3] = msg_type;
    put_u16_le(out + 4, seq);
    out[6] = payload_len;
    if (payload_len > 0u && payload) {
        std::memcpy(out + kHdrSize, payload, payload_len);
    }
    uint16_t c = crc16(out, static_cast<size_t>(kHdrSize + payload_len));
    put_u16_le(out + kHdrSize + payload_len, c);
    return static_cast<unsigned>(kOverhead + payload_len);
}

inline unsigned encode_cmd(
    uint8_t msg_type,
    uint16_t seq,
    const uint8_t* payload,
    uint8_t payload_len,
    uint8_t out[kMaxFrame]
) {
    return encode_frame(kMagicHostToUso0, kMagicHostToUso1, msg_type, seq, payload, payload_len, out);
}

inline void pack_config(const WireConfig& cfg, uint8_t out[kCfgWireLen]) {
    put_u16_le(out + 0, cfg.carrier_freq_mhz);
    out[2]  = cfg.dr_code;
    out[3]  = cfg.decimation;
    out[4]  = cfg.pga_gain;
    out[5]  = cfg.adc_filter;
    out[6]  = cfg.adc_flags;
    out[7]  = cfg.inpmux;
    out[8]  = cfg.dsp_lpf_mode;
    out[9]  = cfg.batch_size;
    put_u16_le(out + 10, cfg.dsp_lpf_cutoff_dhz);
    put_u16_le(out + 12, cfg.dsp_hpf_cutoff_mhz);
    out[14] = cfg.stream_flags;
    out[15] = cfg.reserved;
}

inline void pack_live(
    uint8_t mask,
    uint8_t pga_gain,
    uint8_t lpf_mode,
    uint8_t batch_size,
    uint16_t lpf_cutoff_dhz,
    uint16_t hpf_cutoff_mhz,
    uint8_t out[kLiveWireLen]
) {
    out[0] = mask;
    out[1] = pga_gain;
    out[2] = lpf_mode;
    out[3] = batch_size;
    put_u16_le(out + 4, lpf_cutoff_dhz);
    put_u16_le(out + 6, hpf_cutoff_mhz);
}

inline bool unpack_hello_rsp(const uint8_t* in, uint8_t len, HelloRsp& rsp) {
    if (!in || len != kHelloRspWireLen) return false;
    rsp.proto_version = in[0];
    rsp.device_id     = in[1];
    rsp.fw_major      = in[2];
    rsp.fw_minor      = in[3];
    rsp.fw_patch      = in[4];
    rsp.adc_id        = in[5];
    rsp.ready         = in[6];
    rsp.fault         = in[7];
    rsp.capabilities  = get_u32_le(in + 8);
    rsp.adc_clock_hz  = get_u32_le(in + 12);
    std::memcpy(rsp.mcu_uid, in + 16, 12u);
    return true;
}

inline bool unpack_start_rsp(const uint8_t* in, uint8_t len, StartRsp& rsp) {
    if (!in || len != kStartRspWireLen) return false;
    rsp.status           = in[0];
    rsp.dr_code          = in[1];
    rsp.decimation       = in[2];
    rsp.batch_size       = in[3];
    rsp.pga_gain         = in[4];
    rsp.adc_filter       = in[5];
    rsp.adc_flags        = in[6];
    rsp.inpmux           = in[7];
    rsp.effective_fs_dhz = get_u16_le(in + 8);
    rsp.hw_fs_dhz        = get_u16_le(in + 10);
    return true;
}

inline bool unpack_diag_rsp(const uint8_t* in, uint8_t len, DiagRsp& rsp) {
    if (!in || len != kDiagRspWireLen) return false;
    rsp.uptime_ms        = get_u32_le(in + 0);
    rsp.samples          = get_u32_le(in + 4);
    rsp.dropped_samples  = get_u32_le(in + 8);
    rsp.missed_drdy      = get_u32_le(in + 12);
    rsp.checksum_errors  = get_u32_le(in + 16);
    rsp.adc_errors       = get_u32_le(in + 20);
    rsp.stale_count      = get_u32_le(in + 24);
    rsp.tx_overflows     = get_u32_le(in + 28);
    rsp.rx_overflows     = get_u32_le(in + 32);
    rsp.v2_crc_errors    = get_u32_le(in + 36);
    rsp.effective_fs_dhz = get_u16_le(in + 40);
    rsp.adc_status       = in[42];
    rsp.state_flags      = in[43];
    return true;
}

inline unsigned encode_batch_for_test(uint16_t seq, const DataBatch& batch, uint8_t out[kMaxFrame]) {
    if (batch.sample_count == 0u || batch.sample_count > kMaxBatchSamples) return 0u;
    uint8_t payload[kMaxPayload]{};
    put_u32_le(payload + 0, batch.timestamp_ms);
    put_u32_le(payload + 4, batch.first_sample_idx);
    put_u16_le(payload + 8, batch.effective_fs_dhz);
    payload[10] = batch.flags;
    payload[11] = static_cast<uint8_t>(((batch.pga_gain & 0x0Fu) << 4) | (batch.sample_count & 0x0Fu));
    for (uint8_t i = 0; i < batch.sample_count; ++i) {
        uint32_t raw = 0;
        std::memcpy(&raw, &batch.samples[i], sizeof(raw));
        put_u32_le(payload + kBatchHdrSize + static_cast<size_t>(i) * 4u, raw);
    }
    return encode_frame(
        kMagicUsoToHost0, kMagicUsoToHost1, MSG_DATA_BATCH, seq,
        payload, static_cast<uint8_t>(kBatchHdrSize + batch.sample_count * 4u), out
    );
}

inline bool unpack_batch(const uint8_t* payload, uint8_t payload_len, DataBatch& batch) {
    if (!payload || payload_len < kBatchHdrSize + 4u) return false;
    uint8_t count = payload[11] & 0x0Fu;
    if (count == 0u || count > kMaxBatchSamples) return false;
    if (payload_len != static_cast<uint8_t>(kBatchHdrSize + count * 4u)) return false;
    batch.timestamp_ms     = get_u32_le(payload + 0);
    batch.first_sample_idx = get_u32_le(payload + 4);
    batch.effective_fs_dhz = get_u16_le(payload + 8);
    batch.flags            = payload[10];
    batch.pga_gain         = static_cast<uint8_t>((payload[11] >> 4) & 0x0Fu);
    batch.sample_count     = count;
    for (uint8_t i = 0; i < count; ++i) {
        uint32_t raw = get_u32_le(payload + kBatchHdrSize + static_cast<size_t>(i) * 4u);
        std::memcpy(&batch.samples[i], &raw, sizeof(raw));
    }
    return true;
}

class StreamParser {
public:
    explicit StreamParser(uint8_t m0 = kMagicUsoToHost0, uint8_t m1 = kMagicUsoToHost1)
        : magic0_(m0), magic1_(m1) {}

    void reset() {
        state_ = 0u;
        pos_ = 0u;
        expected_total_ = 0u;
    }

    /* Returns: 0 = need more bytes, 1 = valid frame decoded, -1 = CRC/header error */
    int feed(uint8_t byte, Frame& out_frame) {
        switch (state_) {
            case 0u:
                if (byte == magic0_) {
                    buf_[0] = byte;
                    pos_ = 1u;
                    state_ = 1u;
                }
                return 0;
            case 1u:
                if (byte == magic1_) {
                    buf_[1] = byte;
                    pos_ = 2u;
                    state_ = 2u;
                } else if (byte == magic0_) {
                    state_ = 1u;
                } else {
                    reset();
                }
                return 0;
            case 2u:
                buf_[pos_++] = byte;
                if (pos_ == kHdrSize) {
                    uint8_t ver = static_cast<uint8_t>(buf_[2] >> 4);
                    uint8_t plen = buf_[6];
                    if (ver != kProtoVersion || plen > kMaxPayload) {
                        crc_errors_++;
                        reset();
                        return -1;
                    }
                    expected_total_ = static_cast<uint8_t>(kOverhead + plen);
                    state_ = 3u;
                }
                return 0;
            case 3u:
                buf_[pos_++] = byte;
                if (pos_ >= expected_total_) {
                    uint8_t plen = buf_[6];
                    uint16_t rx_crc = get_u16_le(buf_ + kHdrSize + plen);
                    uint16_t calc_crc = crc16(buf_, static_cast<size_t>(kHdrSize + plen));
                    if (rx_crc != calc_crc) {
                        crc_errors_++;
                        reset();
                        return -1;
                    }
                    out_frame.version     = static_cast<uint8_t>(buf_[2] >> 4);
                    out_frame.flags       = static_cast<uint8_t>(buf_[2] & 0x0Fu);
                    out_frame.msg_type    = buf_[3];
                    out_frame.seq         = get_u16_le(buf_ + 4);
                    out_frame.payload_len = plen;
                    if (plen > 0u) {
                        std::memcpy(out_frame.payload, buf_ + kHdrSize, plen);
                    }
                    reset();
                    return 1;
                }
                return 0;
            default:
                reset();
                return 0;
        }
    }

    uint32_t crc_errors() const { return crc_errors_; }

private:
    uint8_t  magic0_;
    uint8_t  magic1_;
    uint8_t  state_{0u};
    uint8_t  pos_{0u};
    uint8_t  expected_total_{0u};
    uint8_t  buf_[kMaxFrame]{};
    uint32_t crc_errors_{0u};
};

} // namespace usov2
} // namespace dsp

#endif /* DSP_USO_PROTO_V2_HPP */
