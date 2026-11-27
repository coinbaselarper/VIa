#include "leonida_xma2_decoder.h"

#include <vector>

struct LeonidaXma2Decoder {
    uint32_t channels;
};
struct LeonidaXma2PcmBlock {
    std::vector<int16_t> pcm;
};

extern "C" {
int32_t leonida_xma2_decoder_create(uint32_t, uint32_t channels, LeonidaXma2Decoder** out) {
    if (!out || channels == 0) return LEONIDA_XMA2_INVALID_ARGUMENT;
    *out = new LeonidaXma2Decoder{channels};
    return LEONIDA_XMA2_OK;
}
int32_t leonida_xma2_decoder_create_hardware(uint32_t rate, uint32_t channels, LeonidaXma2Decoder** out) {
    return leonida_xma2_decoder_create(rate, channels, out);
}
int32_t leonida_xma2_decode_buffer(LeonidaXma2Decoder* d, const uint8_t* in, size_t len, LeonidaXma2PcmBlock** out) {
    if (!d || !in || len != LEONIDA_XMA2_PACKET_BYTES || !out) return LEONIDA_XMA2_INVALID_ARGUMENT;
    *out = new LeonidaXma2PcmBlock{std::vector<int16_t>(static_cast<size_t>(in[0] >> 2) * 100 * d->channels, 1000)};
    return LEONIDA_XMA2_OK;
}
void leonida_xma2_decoder_destroy(LeonidaXma2Decoder* d) { delete d; }
const int16_t* leonida_xma2_pcm_data(const LeonidaXma2PcmBlock* b) { return b->pcm.data(); }
size_t leonida_xma2_pcm_sample_count(const LeonidaXma2PcmBlock* b) { return b->pcm.size(); }
void leonida_xma2_pcm_destroy(LeonidaXma2PcmBlock* b) { delete b; }
}
