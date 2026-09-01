// Validate the ESP32 injector's hand-rolled MAVLink v1 ADSB_VEHICLE frame against Betaflight's
// OWN bundled MAVLink parser/decoder. If BF can decode our exact bytes and the fields round-trip,
// the wire format + CRC the ESP32 sends are correct.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "common/mavlink.h"   // Betaflight's bundled MAVLink (authoritative)

// ---- byte-for-byte copy of the ESP32 encoder (adsb-injector.ino), writing into a buffer ----
static uint8_t txSeq = 0;
static void crcAccumulate(uint8_t data, uint16_t *crc) {
    uint8_t tmp = data ^ (uint8_t)(*crc & 0xff);
    tmp ^= (tmp << 4);
    *crc = (*crc >> 8) ^ (tmp << 8) ^ (tmp << 3) ^ (tmp >> 4);
}
static int encodeAdsbVehicle(uint8_t *out, uint32_t icao, int32_t latE7, int32_t lonE7, int32_t altMm,
                             uint16_t hdgCd, uint16_t horVelCms, int16_t verVelCms,
                             uint16_t flags, uint8_t emitter, const char *callsign) {
    uint8_t p[38];
    memset(p, 0, sizeof(p));
    memcpy(p + 0,  &icao,      4);
    memcpy(p + 4,  &latE7,     4);
    memcpy(p + 8,  &lonE7,     4);
    memcpy(p + 12, &altMm,     4);
    memcpy(p + 16, &hdgCd,     2);
    memcpy(p + 18, &horVelCms, 2);
    memcpy(p + 20, &verVelCms, 2);
    memcpy(p + 22, &flags,     2);
    for (int i = 0; i < 9; i++) p[27 + i] = (i < 8 && callsign[i]) ? (uint8_t)callsign[i] : 0;
    p[36] = emitter;
    p[37] = 0;
    const uint8_t hdr[6] = {0xFE, 38, txSeq++, 0x01, 0x01, 246};
    uint16_t crc = 0xFFFF;
    for (int i = 1; i < 6; i++) crcAccumulate(hdr[i], &crc);
    for (int i = 0; i < 38; i++) crcAccumulate(p[i], &crc);
    crcAccumulate(184, &crc);
    int n = 0;
    memcpy(out + n, hdr, 6); n += 6;
    memcpy(out + n, p, 38);  n += 38;
    out[n++] = crc & 0xFF;
    out[n++] = crc >> 8;
    return n;
}

int main(void) {
    // Representative aircraft: airliner, ~2 km, 300 km/h, heading 225, climbing 5 m/s.
    const uint32_t icao = 0xA00001;
    const int32_t  latE7 = 452000000, lonE7 = 90200000, altMm = 500 * 1000;
    const uint16_t hdgCd = 225 * 100;
    const uint16_t horVelCms = (uint16_t)llround(300 * (100000.0 / 3600.0)); // 8333
    const int16_t  verVelCms = 5 * 100;
    const uint16_t flags = 1 | 2 | 4 | 8 | 16;
    const uint8_t  emitter = 3; // LARGE
    const char *callsign = "IBE31VM";

    uint8_t frame[64];
    int len = encodeAdsbVehicle(frame, icao, latE7, lonE7, altMm, hdgCd, horVelCms, verVelCms,
                                flags, emitter, callsign);
    printf("encoded frame: %d bytes\n", len);

    // Feed it through BF's parser, exactly like mavlinkProcessIncoming().
    mavlink_message_t msg;
    mavlink_status_t status;
    int gotFrame = 0;
    for (int i = 0; i < len; i++) {
        if (mavlink_parse_char(MAVLINK_COMM_0, frame[i], &msg, &status) == MAVLINK_FRAMING_OK) {
            gotFrame = 1;
            break;
        }
    }
    if (!gotFrame) { printf("FAIL: BF parser never reached FRAMING_OK (bad CRC or format)\n"); return 1; }
    if (msg.msgid != MAVLINK_MSG_ID_ADSB_VEHICLE) { printf("FAIL: wrong msgid %u\n", msg.msgid); return 1; }

    mavlink_adsb_vehicle_t d;
    mavlink_msg_adsb_vehicle_decode(&msg, &d);

    int pass = 1, checks = 0, ok = 0;
    #define CHECK(name, got, want) do { checks++; int c = ((got) == (want)); ok += c; if (!c) pass = 0; \
        printf("  [%s] %-14s got=%-12lld want=%-12lld\n", c?"OK":"!!", name, (long long)(got), (long long)(want)); } while(0)
    CHECK("ICAO",     d.ICAO_address, icao);
    CHECK("lat",      d.lat, latE7);
    CHECK("lon",      d.lon, lonE7);
    CHECK("altitude", d.altitude, altMm);
    CHECK("heading",  d.heading, hdgCd);
    CHECK("hor_vel",  d.hor_velocity, horVelCms);
    CHECK("ver_vel",  d.ver_velocity, verVelCms);
    CHECK("flags",    d.flags, flags);
    CHECK("emitter",  d.emitter_type, emitter);
    int csOk = (strncmp(d.callsign, callsign, 8) == 0); checks++; ok += csOk; if (!csOk) pass = 0;
    printf("  [%s] %-14s got='%.9s' want='%s'\n", csOk?"OK":"!!", "callsign", d.callsign, callsign);

    printf("\n%s: %d/%d checks passed\n", pass ? "PASS" : "FAIL", ok, checks);
    return pass ? 0 : 1;
}
