#include "beacon_packet.h"

#include <string.h>

static void write_u32(uint8_t *out, uint32_t v)
{
    out[0] = (uint8_t)(v & 0xFF);
    out[1] = (uint8_t)((v >> 8) & 0xFF);
    out[2] = (uint8_t)((v >> 16) & 0xFF);
    out[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t read_u32(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static void write_i64(uint8_t *out, int64_t v)
{
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)(u >> (8 * i));
    }
}

static int64_t read_i64(const uint8_t *in)
{
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) {
        u |= (uint64_t)in[i] << (8 * i);
    }
    return (int64_t)u;
}

void sd_beacon_pack(const sd_beacon_t *in, uint8_t out[SD_BEACON_PACKET_LEN])
{
    out[0] = SD_MAGIC0;
    out[1] = SD_MAGIC1;
    out[2] = SD_VERSION;
    out[3] = SD_PKT_BEACON;
    out[4] = in->channel_id;
    out[5] = in->bpm;
    write_i64(&out[6], in->beat_timestamp_us);
    write_i64(&out[14], in->send_time_us);

    uint8_t *p = &out[22];
    for (int i = 0; i < SD_SCHEDULE_LOOKAHEAD; i++) {
        *p++ = (uint8_t)in->segments[i].type;
        write_u32(p, in->segments[i].start_beat);
        p += 4;
    }
}

bool sd_beacon_unpack(const uint8_t *data, int len, sd_beacon_t *out)
{
    if (len < SD_BEACON_PACKET_LEN) {
        return false;
    }
    if (data[0] != SD_MAGIC0 || data[1] != SD_MAGIC1 || data[2] != SD_VERSION) {
        return false;
    }
    if (data[3] != SD_PKT_BEACON) {
        return false;
    }

    out->channel_id = data[4];
    out->bpm = data[5];
    out->beat_timestamp_us = read_i64(&data[6]);
    out->send_time_us = read_i64(&data[14]);

    const uint8_t *p = &data[22];
    for (int i = 0; i < SD_SCHEDULE_LOOKAHEAD; i++) {
        out->segments[i].type = (sd_segment_type_t)*p++;
        out->segments[i].start_beat = read_u32(p);
        p += 4;
    }
    return true;
}
