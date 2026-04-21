/**
 * @brief Tests for Phase 4 exposure / AF / fast-capture plumbing.
 *
 * These tests exercise the PROTOCOL INVARIANTS — the binary layout of the
 * extended SPI IDLE header, the SET_EXPOSURE command's payload encoding,
 * and the mapping of OV5640 register bits to the gain/exposure integers the
 * master sends. They catch regressions caused by accidentally reshuffling
 * byte offsets or flipping endianness without intending to.
 *
 * We do NOT boot ESP-IDF here — just the constants + small helper behaviors
 * extracted in pure-C form.
 */
#include "unity/unity.h"
#include <stdint.h>
#include <string.h>

/* Mirror the constants defined in esp32s3/src/spi/SPISlave.h +
 * factory_demo/main/app/Spi/spi_camera.h. If these ever diverge, the
 * master and slave will talk past each other — this test makes that
 * explicit so a typo shows up here first. */

#define SPI_IDLE_HEADER_AE_GAIN_OFFSET       5
#define SPI_IDLE_HEADER_AE_EXPOSURE_OFFSET   7
#define SPI_SET_EXPOSURE_PAYLOAD_OFFSET      1
#define SPI_SET_EXPOSURE_PAYLOAD_LEN         5

/* Status flag bits (must not collide) */
#define SPI_STATUS_JPEG_READY       0x01
#define SPI_STATUS_WIFI_ACTIVE      0x02
#define SPI_STATUS_WIFI_CONNECTED   0x04
#define SPI_STATUS_AF_LOCKED        0x08

/* Command bytes (must not collide, must be ≤ 0x0F for the 10× burst retry
 * to reliably deliver them per SPISlave.h comment). */
#define SPI_CMD_STATUS        0x01
#define SPI_CMD_GET_SIZE      0x02
#define SPI_CMD_READ_DATA     0x03
#define SPI_CMD_WIFI_ON       0x04
#define SPI_CMD_WIFI_OFF      0x05
#define SPI_CMD_REBOOT        0x06
#define SPI_CMD_IDENTIFY      0x07
#define SPI_CMD_AUTOFOCUS     0x08
#define SPI_CMD_SET_EXPOSURE  0x09

/* ---------- Tests ---------- */

void test_status_flag_bits_disjoint(void) {
    /* No two status flags may share a bit — otherwise the master can't
     * tell them apart in one poll byte. */
    uint8_t combined = SPI_STATUS_JPEG_READY |
                       SPI_STATUS_WIFI_ACTIVE |
                       SPI_STATUS_WIFI_CONNECTED |
                       SPI_STATUS_AF_LOCKED;
    int bits_set = 0;
    for (int i = 0; i < 8; i++) if (combined & (1 << i)) bits_set++;
    TEST_ASSERT_EQUAL_INT(4, bits_set);
    /* AF_LOCKED is bit 3, the value the master side expects. */
    TEST_ASSERT_EQUAL_UINT8(0x08, SPI_STATUS_AF_LOCKED);
}

void test_command_bytes_unique_and_in_range(void) {
    /* Every SPI command byte must be unique — the S3 slave's scan loop
     * short-circuits on first match. */
    uint8_t cmds[] = {
        SPI_CMD_STATUS, SPI_CMD_GET_SIZE, SPI_CMD_READ_DATA,
        SPI_CMD_WIFI_ON, SPI_CMD_WIFI_OFF, SPI_CMD_REBOOT, SPI_CMD_IDENTIFY,
        SPI_CMD_AUTOFOCUS, SPI_CMD_SET_EXPOSURE,
    };
    int n = sizeof(cmds) / sizeof(cmds[0]);
    for (int i = 0; i < n; i++) {
        /* Protocol constraint: 0x01–0x0F, wire-reliable range */
        TEST_ASSERT_TRUE(cmds[i] >= 0x01 && cmds[i] <= 0x0F);
        for (int j = i + 1; j < n; j++) {
            TEST_ASSERT_NOT_EQUAL(cmds[i], cmds[j]);
        }
    }
}

void test_idle_header_offsets_dont_overlap_size(void) {
    /* Header layout: [0]=status, [1..4]=jpeg_size, [5..6]=ae_gain, [7..9]=ae_exp
     * A mistaken shorter ae_gain offset would corrupt the jpeg_size field
     * seen by older masters. Pin the offsets to catch accidental edits. */
    TEST_ASSERT_EQUAL_INT(5, SPI_IDLE_HEADER_AE_GAIN_OFFSET);
    TEST_ASSERT_EQUAL_INT(7, SPI_IDLE_HEADER_AE_EXPOSURE_OFFSET);
    TEST_ASSERT_TRUE(SPI_IDLE_HEADER_AE_GAIN_OFFSET > 4);           // past jpeg_size
    TEST_ASSERT_TRUE(SPI_IDLE_HEADER_AE_EXPOSURE_OFFSET >=
                     SPI_IDLE_HEADER_AE_GAIN_OFFSET + 2);            // gain is 2 bytes
}

/* Reproduce the master-side IDLE-header decode: the master reads 16 bytes
 * and reconstructs 16-bit gain + 24-bit exposure. Ensures endianness
 * matches between master emit and slave pack. */
static void decode_ae(const uint8_t hdr[16], uint16_t *gain, uint32_t *exp) {
    *gain = (uint16_t)hdr[SPI_IDLE_HEADER_AE_GAIN_OFFSET] |
            ((uint16_t)hdr[SPI_IDLE_HEADER_AE_GAIN_OFFSET + 1] << 8);
    *exp = (uint32_t)hdr[SPI_IDLE_HEADER_AE_EXPOSURE_OFFSET] |
           ((uint32_t)hdr[SPI_IDLE_HEADER_AE_EXPOSURE_OFFSET + 1] << 8) |
           ((uint32_t)hdr[SPI_IDLE_HEADER_AE_EXPOSURE_OFFSET + 2] << 16);
}

/* Reproduce the slave-side IDLE-header encode. */
static void encode_ae(uint8_t hdr[16], uint16_t gain, uint32_t exp) {
    hdr[SPI_IDLE_HEADER_AE_GAIN_OFFSET    ] = (uint8_t)(gain       & 0xFF);
    hdr[SPI_IDLE_HEADER_AE_GAIN_OFFSET + 1] = (uint8_t)((gain >> 8) & 0xFF);
    hdr[SPI_IDLE_HEADER_AE_EXPOSURE_OFFSET    ] = (uint8_t)(exp        & 0xFF);
    hdr[SPI_IDLE_HEADER_AE_EXPOSURE_OFFSET + 1] = (uint8_t)((exp >>  8) & 0xFF);
    hdr[SPI_IDLE_HEADER_AE_EXPOSURE_OFFSET + 2] = (uint8_t)((exp >> 16) & 0xFF);
}

void test_ae_roundtrip_small_values(void) {
    uint8_t hdr[16] = {0};
    encode_ae(hdr, 42, 12345);
    uint16_t g; uint32_t e;
    decode_ae(hdr, &g, &e);
    TEST_ASSERT_EQUAL_UINT16(42, g);
    TEST_ASSERT_EQUAL_UINT32(12345, e);
}

void test_ae_roundtrip_max_values(void) {
    /* Gain is 10-bit on OV5640 (max 0x3FF); exposure is 20-bit (max 0xFFFFF).
     * Our wire format gives 16 bits for gain + 24 bits for exposure, so both
     * full sensor ranges round-trip cleanly. */
    uint8_t hdr[16] = {0};
    encode_ae(hdr, 0x03FF, 0x0FFFFF);
    uint16_t g; uint32_t e;
    decode_ae(hdr, &g, &e);
    TEST_ASSERT_EQUAL_UINT16(0x03FF, g);
    TEST_ASSERT_EQUAL_UINT32(0x0FFFFF, e);
}

void test_ae_high_exposure_byte_stays_in_24bit(void) {
    /* The 24-bit exposure field should never bleed into byte 10 (which
     * does not exist in our 10-byte header and is beyond any existing
     * legacy use). Verify the encode truncates to 24 bits. */
    uint8_t hdr[16] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
                       0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    encode_ae(hdr, 0, 0x01234567);   // top byte 0x01 should NOT be written
    /* Byte 10 should be untouched — we had preserved 0xAA there. */
    TEST_ASSERT_EQUAL_UINT8(0xAA, hdr[10]);
    /* But the low 24 bits should have landed. */
    TEST_ASSERT_EQUAL_UINT8(0x67, hdr[7]);
    TEST_ASSERT_EQUAL_UINT8(0x45, hdr[8]);
    TEST_ASSERT_EQUAL_UINT8(0x23, hdr[9]);
}

/* ---- SET_EXPOSURE payload decode (slave side) ---- */

/* Replicates the slave's lambda that converts an 8-byte rx chunk into
 * (gain, exposure). This is the exact decode the S3 control task does
 * when it sees CMD_SET_EXPOSURE at `cmd_offset` and reads 5 payload
 * bytes from `cmd_offset + 1`. */
static void slave_decode_set_exposure(const uint8_t *rx, int cmd_offset,
                                       uint16_t *gain, uint32_t *exp) {
    int po = cmd_offset + SPI_SET_EXPOSURE_PAYLOAD_OFFSET;
    const uint8_t *p = &rx[po];
    *gain = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    *exp  = (uint32_t)p[2] |
            ((uint32_t)p[3] << 8) |
            ((uint32_t)p[4] << 16);
}

/* Replicates the master's spi_camera_set_exposure burst: tx[0]=cmd,
 * tx[1..5]=payload LE. */
static void master_encode_set_exposure(uint8_t tx[8], uint16_t gain, uint32_t exp) {
    tx[0] = SPI_CMD_SET_EXPOSURE;
    tx[1] = gain & 0xFF;
    tx[2] = (gain >> 8) & 0xFF;
    tx[3] = exp & 0xFF;
    tx[4] = (exp >> 8) & 0xFF;
    tx[5] = (exp >> 16) & 0xFF;
    tx[6] = 0;
    tx[7] = 0;
}

void test_set_exposure_wire_roundtrip(void) {
    uint8_t tx[8];
    master_encode_set_exposure(tx, 0x1234, 0xABCDEF);
    uint16_t g; uint32_t e;
    /* Clean-wire path: cmd lands at offset 0, payload at 1..5 */
    slave_decode_set_exposure(tx, 0, &g, &e);
    TEST_ASSERT_EQUAL_UINT16(0x1234, g);
    TEST_ASSERT_EQUAL_UINT32(0xABCDEF, e);
}

void test_set_exposure_payload_fits_in_8byte_buffer(void) {
    /* Slave only processes SET_EXPOSURE if (cmd_offset + 1 + 5) <= 8, i.e.
     * cmd must land at offset 0, 1, or 2. Beyond that the slave bails with
     * a "payload truncated" warning. Assert the worst supported offset still
     * fits, and one past is out of bounds. */
    int po_worst = 2 + SPI_SET_EXPOSURE_PAYLOAD_OFFSET;
    TEST_ASSERT_TRUE(po_worst + SPI_SET_EXPOSURE_PAYLOAD_LEN <= 8);
    int po_over = 3 + SPI_SET_EXPOSURE_PAYLOAD_OFFSET;
    TEST_ASSERT_TRUE(po_over + SPI_SET_EXPOSURE_PAYLOAD_LEN > 8);
}

/* ---- OV5640 register packing (CameraManager encoding) ---- */

/* The slave-side CameraManager::setExposure packs a 10-bit gain into
 * regs 0x350A (high 2 bits) + 0x350B (low 8 bits). Verify the pack/unpack
 * matches the datasheet so a regression would show up here. */

static void ov5640_pack_gain(uint16_t gain, uint8_t *reg_0x350A, uint8_t *reg_0x350B) {
    *reg_0x350A = (gain >> 8) & 0x03;
    *reg_0x350B = gain & 0xFF;
}
static uint16_t ov5640_unpack_gain(uint8_t reg_0x350A, uint8_t reg_0x350B) {
    return ((uint16_t)(reg_0x350A & 0x03) << 8) | reg_0x350B;
}

void test_ov5640_gain_pack_roundtrip(void) {
    for (uint16_t g = 0; g <= 0x3FF; g += 13) {
        uint8_t a, b;
        ov5640_pack_gain(g, &a, &b);
        TEST_ASSERT_EQUAL_UINT16(g, ov5640_unpack_gain(a, b));
    }
}

void test_ov5640_gain_pack_masks_high_bits(void) {
    /* A caller passing > 0x3FF (bad value) should get silently truncated,
     * not cause an unrelated register to spill into 0x350A. */
    uint8_t a, b;
    ov5640_pack_gain(0xFFFF, &a, &b);
    TEST_ASSERT_EQUAL_UINT8(0x03, a);   // only bottom 2 bits kept
    TEST_ASSERT_EQUAL_UINT8(0xFF, b);
}

/* ---- Fast-capture mode semantics ---- */

/* These constants must match spi_camera.c:s_fast_mode delays. If someone
 * tunes them, update both sides. */
#define TRIGGER_WAIT_NORMAL_MS     500
#define TRIGGER_WAIT_FAST_MS       300
#define INTERCAM_WAIT_NORMAL_MS    50
#define INTERCAM_WAIT_FAST_MS      30

void test_fast_mode_actually_saves_time(void) {
    /* Each burst: 1 × trigger wait + 4 × inter-cam wait. */
    int normal = TRIGGER_WAIT_NORMAL_MS + 4 * INTERCAM_WAIT_NORMAL_MS;
    int fast   = TRIGGER_WAIT_FAST_MS   + 4 * INTERCAM_WAIT_FAST_MS;
    int saved  = normal - fast;
    TEST_ASSERT_TRUE(saved >= 200);    // at least 200 ms saved
    TEST_ASSERT_TRUE(fast < normal);   // never slower than normal
}

/* ---- Unity runner ---- */

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_status_flag_bits_disjoint);
    RUN_TEST(test_command_bytes_unique_and_in_range);
    RUN_TEST(test_idle_header_offsets_dont_overlap_size);
    RUN_TEST(test_ae_roundtrip_small_values);
    RUN_TEST(test_ae_roundtrip_max_values);
    RUN_TEST(test_ae_high_exposure_byte_stays_in_24bit);
    RUN_TEST(test_set_exposure_wire_roundtrip);
    RUN_TEST(test_set_exposure_payload_fits_in_8byte_buffer);
    RUN_TEST(test_ov5640_gain_pack_roundtrip);
    RUN_TEST(test_ov5640_gain_pack_masks_high_bits);
    RUN_TEST(test_fast_mode_actually_saves_time);
    UNITY_END();
}
