#include "uso_v2_receiver.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace dsp::usov2;
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

static std::vector<uint8_t> batch(uint32_t index, uint8_t flags = 0, uint16_t fs = 2000, uint8_t gain = 0) {
    DataBatch data{};
    data.first_sample_idx = index;
    data.sample_count = 3;
    data.effective_fs_dhz = fs;
    data.pga_gain = gain;
    data.flags = flags;
    data.samples[0] = 11; data.samples[1] = -22; data.samples[2] = 33;
    uint8_t wire[kMaxFrame];
    auto count = encode_batch_for_test(1, data, wire);
    return {wire, wire + count};
}

static void feed(Receiver& receiver, const std::vector<uint8_t>& bytes) {
    receiver.feed(bytes.data(), bytes.size());
}

int main() {
    Receiver receiver;
    auto wire = batch(0);
    uint8_t reply[kMaxFrame];
    const uint8_t ack[] = {CMD_SET_LIVE, ST_OK};
    auto n = encode_frame(kMagicUsoToHost0, kMagicUsoToHost1, RSP_ACK, 0, ack, 2, reply);
    wire.insert(wire.end(), reply, reply + n);
    auto tail = batch(3);
    wire.insert(wire.end(), tail.begin(), tail.end());
    // Feed arbitrarily fragmented reads: data + response + data must all survive.
    receiver.feed(wire.data(), 5);
    receiver.feed(wire.data() + 5, wire.size() - 5);
    Frame frame{};
    REQUIRE(!receiver.take_response(RSP_ACK, 1, frame));
    REQUIRE(receiver.take_response(RSP_ACK, 0, frame)); // seq=0 is exact, not wildcard
    double values[16], fs = 0;
    int flags = 0;
    REQUIRE(receiver.read(values, 16, &flags, &fs) == 3 && flags == 0 && fs == 200);
    REQUIRE(values[0] == 11 && values[1] == -22 && values[2] == 33);
    REQUIRE(receiver.read(values, 16, &flags, &fs) == 3 && flags == 0);

    feed(receiver, batch(9)); // missing 6..8
    REQUIRE(receiver.read(values, 1, &flags, &fs) == 1 && flags == Receiver::Discontinuity);
    REQUIRE(receiver.read(values, 16, &flags, &fs) == 2 && flags == 0);
    REQUIRE(receiver.dropped_samples() == 3);
    feed(receiver, batch(12, kBatchPgaOvf));
    REQUIRE(receiver.read(values, 1, &flags, &fs) == 1 && flags == Receiver::Overflow);
    REQUIRE(receiver.read(values, 16, &flags, &fs) == 2 && flags == Receiver::Overflow);
    feed(receiver, batch(15, kBatchGapDropped));
    REQUIRE(receiver.read(values, 16, &flags, &fs) == 3 && flags == Receiver::Discontinuity);
    feed(receiver, batch(18, 0, 4000, 2));
    REQUIRE(receiver.read(values, 16, &flags, &fs) == 3 && flags == Receiver::Discontinuity && fs == 400);

    receiver.reset();
    feed(receiver, batch(0xFFFFFFFDu));
    REQUIRE(receiver.read(values, 16, &flags, &fs) == 3 && flags == 0);
    feed(receiver, batch(0));
    REQUIRE(receiver.read(values, 16, &flags, &fs) == 3 && flags == 0);
    REQUIRE(receiver.dropped_samples() == 0);
    auto corrupt = batch(3); corrupt.back() ^= 1;
    feed(receiver, corrupt);
    feed(receiver, batch(6));
    REQUIRE(receiver.read(values, 16, &flags, &fs) == 3 && flags == Receiver::Discontinuity);
    REQUIRE(receiver.crc_errors() == 1 && receiver.dropped_samples() == 3);
    feed(receiver, batch(0)); // reset/backwards index must not inflate lost count
    REQUIRE(receiver.read(values, 16, &flags, &fs) == 3 && flags == Receiver::Discontinuity);
    REQUIRE(receiver.dropped_samples() == 3);

    receiver.reset();
    for (uint32_t i = 0; i < 3000; ++i) feed(receiver, batch(i * 3));
    REQUIRE(receiver.dropped_samples() > 0);
    REQUIRE(receiver.read(values, 16, &flags, &fs) == 3 && flags == Receiver::Discontinuity);
    std::puts("V2 receiver: interleaving, fragmentation, exact seq, gaps, CRC, wrap, overflow and bounded queue OK");
}
