#ifndef DSP_USO_V2_RECEIVER_HPP
#define DSP_USO_V2_RECEIVER_HPP

#include "uso_proto_v2.hpp"
#include <algorithm>
#include <deque>

namespace dsp::usov2 {

// One receiver owns every byte, including data interleaved with command replies.
// Blocks retain their boundaries so a gap never joins two signal segments.
class Receiver {
public:
    static constexpr int Overflow = 1;
    static constexpr int Discontinuity = 2;
    static constexpr size_t MaxQueuedSamples = 8192;

    void reset() { *this = Receiver{}; }

    void feed(const uint8_t* bytes, size_t size) {
        Frame frame{};
        for (size_t i = 0; i < size; ++i) {
            const int result = parser_.feed(bytes[i], frame);
            if (result < 0) gap_pending_ = true;
            if (result != 1) continue;
            if (frame.msg_type != MSG_DATA_BATCH) {
                if (replies_.size() == 16) replies_.pop_front();
                replies_.push_back(frame);
                continue;
            }
            DataBatch batch{};
            if (!unpack_batch(frame.payload, frame.payload_len, batch) ||
                !batch.effective_fs_dhz || batch.pga_gain > 5) {
                gap_pending_ = true;
                continue;
            }
            ++packets_;
            bool gap = gap_pending_ || (batch.flags & (kBatchGapDropped | kBatchAdcAlarm));
            if (have_index_ && batch.first_sample_idx != expected_index_) {
                const uint32_t delta = batch.first_sample_idx - expected_index_;
                // Modulo arithmetic handles UINT32_MAX -> 0. A backwards jump
                // is a new acquisition/duplicate, not billions of lost samples.
                if (delta < 0x80000000u) dropped_ += delta;
                gap = true;
            }
            if (have_index_ && (last_fs_ != batch.effective_fs_dhz || last_gain_ != batch.pga_gain))
                gap = true;
            have_index_ = true;
            expected_index_ = batch.first_sample_idx + batch.sample_count;
            last_fs_ = batch.effective_fs_dhz;
            last_gain_ = batch.pga_gain;
            gap_pending_ = false;
            if (queued_samples_ + batch.sample_count > MaxQueuedSamples) {
                dropped_ += static_cast<uint32_t>(queued_samples_);
                blocks_.clear();
                queued_samples_ = 0;
                gap = true;
            }
            blocks_.push_back({batch, 0, gap});
            queued_samples_ += batch.sample_count;
        }
    }

    bool take_response(uint8_t type, uint16_t seq, Frame& out) {
        for (auto it = replies_.begin(); it != replies_.end(); ++it) {
            if (it->msg_type == type && it->seq == seq) {
                out = *it;
                replies_.erase(it);
                return true;
            }
        }
        return false;
    }

    int read(double* out, int capacity, int* flags, double* fs_hz) {
        if (flags) *flags = 0;
        if (!out || capacity <= 0 || blocks_.empty()) return 0;
        auto& block = blocks_.front();
        const int count = std::min(capacity, static_cast<int>(block.data.sample_count - block.offset));
        if (flags) {
            if (block.data.flags & (kBatchPgaOvf | kBatchAdcAlarm)) *flags |= Overflow;
            if (block.gap && block.offset == 0) *flags |= Discontinuity;
        }
        if (fs_hz) *fs_hz = block.data.effective_fs_dhz * 0.1;
        for (int i = 0; i < count; ++i) out[i] = block.data.samples[block.offset++];
        queued_samples_ -= count;
        if (block.offset == block.data.sample_count) blocks_.pop_front();
        return count;
    }

    uint32_t crc_errors() const { return parser_.crc_errors(); }
    uint32_t dropped_samples() const { return dropped_; }
    uint32_t packets() const { return packets_; }

private:
    struct Block { DataBatch data; uint8_t offset; bool gap; };
    StreamParser parser_;
    std::deque<Frame> replies_;
    std::deque<Block> blocks_;
    size_t queued_samples_{0};
    bool have_index_{false}, gap_pending_{false};
    uint32_t expected_index_{0}, dropped_{0}, packets_{0};
    uint16_t last_fs_{0};
    uint8_t last_gain_{0};
};
} // namespace dsp::usov2
#endif
