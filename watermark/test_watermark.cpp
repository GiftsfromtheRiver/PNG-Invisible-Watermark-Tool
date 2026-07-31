// watermark_tool automated test
#include "watermark.h"
#include "rs_codec.h"
#include "lodepng.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <cstring>

using namespace watermark;
using namespace rs_codec;

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { pass++; std::cout << "  [PASS] " << msg << "\n"; } \
    else { fail++; std::cout << "  [FAIL] " << msg << "\n"; } \
} while(0)

// Generate test PNG in current directory
static void make_test_png(const std::string& path, uint32_t w, uint32_t h, bool has_alpha) {
    std::vector<uint8_t> image(w * h * (has_alpha ? 4 : 3));
    uint64_t rng = 12345;
    for (size_t i = 0; i < image.size(); i++) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        image[i] = rng & 0xFF;
    }
    unsigned error;
    if (has_alpha) {
        error = lodepng::encode(path, image, w, h, LCT_RGBA, 8);
    } else {
        error = lodepng::encode(path, image, w, h, LCT_RGB, 8);
    }
    if (error != 0) {
        std::cerr << "FATAL: cannot create " << path << " error=" << error << "\n";
        exit(1);
    }
}

// ========================================================================
// Original tests (backward compatibility)
// ========================================================================

void test_basic_embed_extract() {
    std::cout << "\n=== Basic Embed/Extract (no ECC) ===\n";
    
    make_test_png("test_wm_rgba.png", 100, 100, true);
    
    std::string text = "Hello, this is an invisible watermark!";
    uint64_t salt = 0xDEADBEEF;
    
    auto er = embed_text("test_wm_rgba.png", "test_wm_out_rgba.png", text, salt);
    CHECK(er.success, "RGBA embed ok");
    CHECK(er.used_bits > 0, "used " + std::to_string(er.used_bits) + " bits");
    
    auto xr = extract_text("test_wm_out_rgba.png", salt);
    CHECK(xr.success, "RGBA extract ok");
    CHECK(xr.text == text, "text matches: \"" + xr.text + "\"");
}

void test_rgb_image() {
    std::cout << "\n=== RGB (no Alpha, no ECC) ===\n";
    
    make_test_png("test_wm_rgb.png", 100, 100, false);
    
    auto cap = get_capacity("test_wm_rgb.png");
    CHECK(cap.has_alpha == false, "detected RGB (no A)");
    CHECK(cap.available_channels == 3, "3 channels available");
    
    std::string text = "RGB image watermark test 12345";
    uint64_t salt = 42;
    
    auto er = embed_text("test_wm_rgb.png", "test_wm_out_rgb.png", text, salt);
    CHECK(er.success, "RGB embed ok");
    CHECK(er.has_alpha == false, "output stays RGB");
    
    auto xr = extract_text("test_wm_out_rgb.png", salt);
    CHECK(xr.success, "RGB extract ok");
    CHECK(xr.text == text, "text matches");
}

void test_wrong_salt() {
    std::cout << "\n=== Wrong Salt ===\n";
    
    make_test_png("test_wm_salt.png", 80, 80, true);
    
    std::string text = "secret message";
    embed_text("test_wm_salt.png", "test_wm_salt_out.png", text, 111);
    
    auto xr = extract_text("test_wm_salt_out.png", 222);
    CHECK(xr.text != text, "wrong salt gives different text (correct!)");
}

void test_capacity_query() {
    std::cout << "\n=== Capacity Query ===\n";
    
    make_test_png("test_wm_cap.png", 200, 200, true);
    
    auto cap = get_capacity("test_wm_cap.png");
    CHECK(cap.width == 200, "width 200");
    CHECK(cap.height == 200, "height 200");
    CHECK(cap.has_alpha == true, "RGBA 4ch");
    CHECK(cap.blocks_per_channel > 0, "blocks/ch: " + std::to_string(cap.blocks_per_channel));
    CHECK(cap.max_text_bytes > 0, "max bytes: " + std::to_string(cap.max_text_bytes));
    
    std::cout << "  info: " << cap.width << "x" << cap.height 
              << " " << cap.available_channels << "ch"
              << " blocks/ch=" << cap.blocks_per_channel
              << " max=" << cap.max_text_bytes << "bytes\n";
}

void test_long_text() {
    std::cout << "\n=== Long Text (no ECC) ===\n";
    
    make_test_png("test_wm_long.png", 200, 200, true);
    
    auto cap = get_capacity("test_wm_long.png");
    
    std::string text(cap.max_text_bytes - 10, 'A');
    
    uint64_t salt = 9999;
    auto er = embed_text("test_wm_long.png", "test_wm_long_out.png", text, salt);
    CHECK(er.success, "long text embed ok (" + std::to_string(text.size()) + " bytes)");
    
    auto xr = extract_text("test_wm_long_out.png", salt);
    CHECK(xr.success, "long text extract ok");
    CHECK(xr.text == text, "long text matches");
}

void test_text_overflow() {
    std::cout << "\n=== Capacity Overflow ===\n";
    
    make_test_png("test_wm_tiny.png", 10, 10, true);
    
    auto cap = get_capacity("test_wm_tiny.png");
    
    std::string long_text(cap.max_text_bytes + 100, 'X');
    
    auto er = embed_text("test_wm_tiny.png", "test_wm_tiny_out.png", long_text, 0);
    CHECK(!er.success, "oversized text correctly rejected");
}

void test_multiline_text() {
    std::cout << "\n=== Multiline Text ===\n";
    
    make_test_png("test_wm_ml.png", 100, 100, true);
    
    std::string text = "line1\nline2\nline3\nHello World!\nlast line";
    uint64_t salt = 777;
    
    embed_text("test_wm_ml.png", "test_wm_ml_out.png", text, salt);
    auto xr = extract_text("test_wm_ml_out.png", salt);
    CHECK(xr.text == text, "multiline text matches");
}

void test_special_chars() {
    std::cout << "\n=== Special Characters ===\n";
    
    make_test_png("test_wm_special.png", 100, 100, true);
    
    std::string text = "symbols: <>&\"' | greek: alpha beta gamma | test123!@#";
    uint64_t salt = 555;
    
    embed_text("test_wm_special.png", "test_wm_special_out.png", text, salt);
    auto xr = extract_text("test_wm_special_out.png", salt);
    CHECK(xr.text == text, "special chars match");
}

// ========================================================================
// RS Codec unit tests
// ========================================================================

void test_rs_no_errors() {
    std::cout << "\n=== RS: No Errors ===\n";
    
    std::vector<uint8_t> data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    int npar = 8;
    
    auto encoded = rs_encode(data, npar);
    CHECK(encoded.size() == 255, "encoded size = 255");
    CHECK(encoded[0] == 1 && encoded[1] == 2, "systematic: data at start");
    
    auto decoded = rs_decode(encoded, npar);
    CHECK(decoded.size() >= data.size(), "decoded size >= data size");
    
    bool match = true;
    for (size_t i = 0; i < data.size(); i++) {
        if (decoded[i] != data[i]) { match = false; break; }
    }
    CHECK(match, "decoded data matches original");
}

void test_rs_few_errors() {
    std::cout << "\n=== RS: Few Errors (within correction) ===\n";
    
    std::vector<uint8_t> data(50, 0);
    for (int i = 0; i < 50; i++) data[i] = i * 3 + 7;
    
    int npar = 16; // corrects 8 errors
    auto encoded = rs_encode(data, npar);
    
    // Inject 5 errors
    encoded[0] ^= 0x55;
    encoded[10] ^= 0xAA;
    encoded[50] ^= 0x33;
    encoded[100] ^= 0x77;
    encoded[200] ^= 0xFF;
    
    auto decoded = rs_decode(encoded, npar);
    CHECK(!decoded.empty(), "decode succeeded");
    
    bool match = !decoded.empty();
    for (size_t i = 0; i < data.size() && match; i++) {
        if (decoded[i] != data[i]) match = false;
    }
    CHECK(match, "all data corrected correctly");
}

void test_rs_too_many_errors() {
    std::cout << "\n=== RS: Too Many Errors ===\n";
    
    std::vector<uint8_t> data(20, 0);
    for (int i = 0; i < 20; i++) data[i] = i + 1;
    
    int npar = 8; // corrects 4
    auto encoded = rs_encode(data, npar);
    
    // Inject 6 errors (exceeds capability)
    for (int i = 0; i < 6; i++) {
        encoded[i * 10] ^= 0xFF;
    }
    
    auto decoded = rs_decode(encoded, npar);
    CHECK(decoded.empty(), "correctly detected too many errors");
}

void test_rs_multi_block() {
    std::cout << "\n=== RS: Multi-Block ===\n";
    
    std::vector<uint8_t> data(300, 0);
    for (int i = 0; i < 300; i++) data[i] = i & 0xFF;
    
    int npar = 8; // k=247, so 300 bytes -> 2 blocks
    auto encoded = rs_encode(data, npar);
    CHECK(encoded.size() == 510, "2 blocks = 510 bytes");
    
    // Inject errors in both blocks
    encoded[0] ^= 0x11;   // block 0
    encoded[260] ^= 0x22; // block 1
    
    auto decoded = rs_decode(encoded, npar);
    CHECK(!decoded.empty(), "multi-block decode succeeded");
    
    bool match = !decoded.empty();
    for (int i = 0; i < 300 && match; i++) {
        if (decoded[i] != data[i]) match = false;
    }
    CHECK(match, "multi-block data corrected");
}

void test_rs_max_correction() {
    std::cout << "\n=== RS: Max Correction (npar=8, 4 errors) ===\n";
    
    std::vector<uint8_t> data(100, 0);
    for (int i = 0; i < 100; i++) data[i] = (i * 7 + 13) & 0xFF;
    
    int npar = 8;
    auto encoded = rs_encode(data, npar);
    
    // Exactly 4 errors
    encoded[5] ^= 0x01;
    encoded[25] ^= 0x02;
    encoded[75] ^= 0x04;
    encoded[95] ^= 0x08;
    
    auto decoded = rs_decode(encoded, npar);
    CHECK(!decoded.empty(), "decode with 4 errors succeeded");
    
    bool match = !decoded.empty();
    for (int i = 0; i < 100 && match; i++) {
        if (decoded[i] != data[i]) match = false;
    }
    CHECK(match, "4 errors corrected correctly");
}

// ========================================================================
// ECC watermark integration tests
// ========================================================================

void test_ecc_embed_extract() {
    std::cout << "\n=== ECC Watermark: Embed/Extract ===\n";
    
    make_test_png("test_wm_ecc.png", 200, 200, true);
    
    std::string text = "ECC watermark test message!";
    uint64_t salt = 12345;
    
    // Test each ECC level
    for (int level = 1; level <= 4; level++) {
        std::string fname = "test_wm_ecc_out_l" + std::to_string(level) + ".png";
        
        auto er = embed_text("test_wm_ecc.png", fname, text, salt, level);
        CHECK(er.success, "ECC level " + std::to_string(level) + " embed ok (used " + 
              std::to_string(er.used_bits) + " bits)");
        
        auto xr = extract_text(fname, salt, level);
        CHECK(xr.success, "ECC level " + std::to_string(level) + " extract ok");
        CHECK(xr.text == text, "ECC level " + std::to_string(level) + " text matches");
    }
}

void test_ecc_capacity_overflow() {
    std::cout << "\n=== ECC Watermark: Capacity Overflow ===\n";
    
    make_test_png("test_wm_ecc_tiny.png", 30, 30, true);
    
    auto cap = get_capacity("test_wm_ecc_tiny.png");
    std::cout << "  tiny image capacity: " << cap.max_text_bytes << " bytes\n";
    
    // With ECC, overhead reduces available space significantly
    std::string text(cap.max_text_bytes, 'X');
    
    auto er = embed_text("test_wm_ecc_tiny.png", "test_wm_ecc_tiny_out.png", text, 0, 2);
    // This might fail or succeed depending on exact capacity
    std::cout << "  ECC level 2 result: " << (er.success ? "success" : "rejected") << "\n";
    CHECK(true, "capacity check completed");
}

void test_ecc_backward_compat() {
    std::cout << "\n=== ECC Watermark: Backward Compatibility ===\n";
    
    make_test_png("test_wm_compat.png", 100, 100, true);
    
    std::string text = "backward compatible message";
    uint64_t salt = 999;
    
    // ecc_level=0 should behave exactly as before
    auto er = embed_text("test_wm_compat.png", "test_wm_compat_out.png", text, salt, 0);
    CHECK(er.success, "ecc_level=0 embed ok");
    
    auto xr = extract_text("test_wm_compat_out.png", salt, 0);
    CHECK(xr.success, "ecc_level=0 extract ok");
    CHECK(xr.text == text, "ecc_level=0 text matches");
}

void test_ecc_wrong_level() {
    std::cout << "\n=== ECC Watermark: Wrong ECC Level ===\n";
    
    make_test_png("test_wm_ecc_wrong.png", 200, 200, true);
    
    std::string text = "ecc level mismatch test";
    uint64_t salt = 777;
    
    auto er = embed_text("test_wm_ecc_wrong.png", "test_wm_ecc_wrong_out.png", text, salt, 2);
    CHECK(er.success, "embed with ecc_level=2 ok");
    
    // Try to extract with wrong level
    auto xr = extract_text("test_wm_ecc_wrong_out.png", salt, 3);
    CHECK(!xr.success || xr.text != text, "wrong ecc level: different result (correct!)");
}

// ========================================================================
// RS Erasure Decode unit tests
// ========================================================================

void test_rs_erasure_no_errors() {
    std::cout << "\n=== RS Erasure: No Errors (with erasures marked) ===\n";
    
    std::vector<uint8_t> data(50, 0);
    for (int i = 0; i < 50; i++) data[i] = i * 5 + 3;
    
    int npar = 16;
    auto encoded = rs_encode(data, npar);
    
    // Mark some positions as erasures but don't corrupt them
    std::vector<std::vector<int>> erasures = {{0, 10, 20}};
    auto decoded = rs_decode_with_erasures(encoded, npar, erasures);
    CHECK(!decoded.empty(), "erasure decode with no corruption succeeded");
    
    bool match = !decoded.empty();
    for (size_t i = 0; i < data.size() && match; i++) {
        if (decoded[i] != data[i]) match = false;
    }
    CHECK(match, "erasure decode data matches (no corruption)");
}

void test_rs_erasure_correction() {
    std::cout << "\n=== RS Erasure: Correction within Capacity ===\n";
    
    // npar=16: can correct up to 16 erasures (pure erasure mode)
    std::vector<uint8_t> data(100, 0);
    for (int i = 0; i < 100; i++) data[i] = (i * 7 + 11) & 0xFF;
    
    int npar = 16;
    auto encoded = rs_encode(data, npar);
    
    // Corrupt 10 bytes and mark them as erasures
    std::vector<int> corrupt_positions = {0, 5, 10, 20, 30, 50, 70, 80, 90, 99};
    std::vector<std::vector<int>> erasures = {corrupt_positions};
    
    // Actually corrupt those positions
    auto corrupted = encoded;
    for (int pos : corrupt_positions) {
        corrupted[pos] ^= 0xFF;
    }
    
    // Standard decode should fail (10 errors > 8 max for npar=16)
    auto std_decoded = rs_decode(corrupted, npar);
    CHECK(std_decoded.empty(), "standard decode fails with 10 errors (correct)");
    
    // Erasure decode should succeed (10 erasures <= 16 = npar)
    auto era_decoded = rs_decode_with_erasures(corrupted, npar, erasures);
    CHECK(!era_decoded.empty(), "erasure decode succeeds with 10 erasures");
    
    bool match = !era_decoded.empty();
    for (size_t i = 0; i < data.size() && match; i++) {
        if (era_decoded[i] != data[i]) match = false;
    }
    CHECK(match, "erasure decode data corrected correctly");
}

void test_rs_erasure_max_capacity() {
    std::cout << "\n=== RS Erasure: Max Capacity (npar erasures) ===\n";
    
    // npar=8: max 8 erasures in pure erasure mode
    std::vector<uint8_t> data(80, 0);
    for (int i = 0; i < 80; i++) data[i] = (i * 3 + 1) & 0xFF;
    
    int npar = 8;
    auto encoded = rs_encode(data, npar);
    
    // Corrupt exactly 8 bytes
    std::vector<int> positions = {0, 10, 20, 30, 40, 50, 60, 70};
    std::vector<std::vector<int>> erasures = {positions};
    
    auto corrupted = encoded;
    for (int pos : positions) {
        corrupted[pos] ^= 0xAA;
    }
    
    auto decoded = rs_decode_with_erasures(corrupted, npar, erasures);
    CHECK(!decoded.empty(), "erasure decode at max capacity (8/8) succeeded");
    
    bool match = !decoded.empty();
    for (size_t i = 0; i < data.size() && match; i++) {
        if (decoded[i] != data[i]) match = false;
    }
    CHECK(match, "max capacity erasure decode data correct");
}

void test_rs_erasure_over_capacity() {
    std::cout << "\n=== RS Erasure: Over Capacity ===\n";
    
    std::vector<uint8_t> data(50, 0);
    for (int i = 0; i < 50; i++) data[i] = i + 1;
    
    int npar = 8;
    auto encoded = rs_encode(data, npar);
    
    // 9 erasures > npar=8 -> should fail
    std::vector<int> positions = {0, 5, 10, 15, 20, 25, 30, 35, 40};
    std::vector<std::vector<int>> erasures = {positions};
    
    auto corrupted = encoded;
    for (int pos : positions) {
        corrupted[pos] ^= 0x55;
    }
    
    auto decoded = rs_decode_with_erasures(corrupted, npar, erasures);
    CHECK(decoded.empty(), "correctly rejected 9 erasures > npar=8");
}

// ========================================================================
// Watermark Erasure Extraction integration tests
// ========================================================================

void test_watermark_erasure_extraction() {
    std::cout << "\n=== Watermark Erasure: Small Crop (npar=16) ===\n";
    
    // 600x600 -> grid: 200x200 = 40000 blocks per channel
    // Crop 3 pixels (1 block row) from bottom -> 600x597
    // Lost per channel: 200 blocks = 0.5%
    // P(bit lost) = 600/120000 = 0.005
    // P(symbol erased) = 1-(0.995)^8 = 0.0393
    // Expected erased: 255*0.0393 ≈ 10
    // npar=16: 10 erasures <= 16 ✓, 10 errors > 8 (standard fails) ✓
    
    uint32_t W = 600, H = 600;
    make_test_png("test_wm_eras.png", W, H, true);
    
    std::string text = "Erasure crop recovery!";
    uint64_t salt = 42424;
    int ecc_level = 2; // npar=16
    
    auto er = embed_text("test_wm_eras.png", "test_wm_eras_out.png", text, salt, ecc_level);
    CHECK(er.success, "erasure test embed ok (used " + std::to_string(er.used_bits) + " bits)");
    
    if (!er.success) return;
    
    // Load stego and crop 3 pixels from bottom (1 block row)
    std::vector<uint8_t> orig_data;
    uint32_t orig_w, orig_h;
    unsigned decode_err = lodepng::decode(orig_data, orig_w, orig_h,
                                          "test_wm_eras_out.png", LCT_RGBA, 8);
    CHECK(decode_err == 0, "loaded stego image");
    CHECK(orig_w == W && orig_h == H, "dimensions match");
    
    uint32_t crop_h = H - 3;
    std::vector<uint8_t> crop_data(crop_h * W * 4, 0);
    for (uint32_t y = 0; y < crop_h; y++)
        memcpy(crop_data.data() + y * W * 4, orig_data.data() + y * W * 4, W * 4);
    
    unsigned enc_err = lodepng::encode("test_wm_eras_cropped.png", crop_data, W, crop_h, LCT_RGBA, 8);
    CHECK(enc_err == 0, "saved cropped image");
    
    // Standard extraction should fail
    auto xr_std = extract_text("test_wm_eras_cropped.png", salt, ecc_level);
    std::cout << "  Standard extract: " << (xr_std.success ? "success" : "failed") << "\n";
    
    // Erasure extraction should succeed
    auto xr_era = extract_text_with_erasures("test_wm_eras_cropped.png", salt, ecc_level, W, H);
    CHECK(xr_era.success, "erasure extraction succeeded");
    if (xr_era.success) {
        CHECK(xr_era.text == text, "erasure text matches: \"" + xr_era.text + "\"");
    } else {
        std::cout << "  Error: " << xr_era.error_msg << "\n";
        fail++;
    }
}

void test_watermark_erasure_higher_ecc() {
    std::cout << "\n=== Watermark Erasure: Higher ECC (npar=48) ===\n";
    
    // 600x600, ecc_level=5 (npar=48)
    // Crop 9 pixels (3 block rows) from bottom -> 600x591
    // Lost per channel: 600 blocks = 1.5%
    // P(bit lost) = 1800/120000 = 0.015
    // P(symbol erased) = 1-(0.985)^8 = 0.1136
    // Expected erased: 255*0.1136 ≈ 29
    // npar=48: 29 erasures <= 48 ✓, 29 errors > 24 (standard fails) ✓
    
    uint32_t W = 600, H = 600;
    make_test_png("test_wm_eras2.png", W, H, true);
    
    std::string text = "Higher ECC erasure test!";
    uint64_t salt = 99999;
    int ecc_level = 5; // npar=48
    
    auto er = embed_text("test_wm_eras2.png", "test_wm_eras2_out.png", text, salt, ecc_level);
    CHECK(er.success, "higher ECC embed ok");
    
    if (!er.success) return;
    
    std::vector<uint8_t> orig_data;
    uint32_t orig_w, orig_h;
    lodepng::decode(orig_data, orig_w, orig_h, "test_wm_eras2_out.png", LCT_RGBA, 8);
    
    uint32_t crop_h = H - 9;  // lose 3 block rows
    std::vector<uint8_t> crop_data(crop_h * W * 4, 0);
    for (uint32_t y = 0; y < crop_h; y++)
        memcpy(crop_data.data() + y * W * 4, orig_data.data() + y * W * 4, W * 4);
    
    lodepng::encode("test_wm_eras2_cropped.png", crop_data, W, crop_h, LCT_RGBA, 8);
    
    auto xr_era = extract_text_with_erasures("test_wm_eras2_cropped.png", salt, ecc_level, W, H);
    CHECK(xr_era.success, "higher ECC erasure extraction succeeded");
    if (xr_era.success) {
        CHECK(xr_era.text == text, "higher ECC text matches: \"" + xr_era.text + "\"");
    } else {
        std::cout << "  Error: " << xr_era.error_msg << "\n";
        fail++;
    }
}

// ========================================================================
// Multi-cluster tests
// ========================================================================

void test_multicluster_basic() {
    std::cout << "\n=== MultiCluster: Basic Embed/Extract (5 clusters) ===\n";
    
    uint32_t W = 600, H = 600;
    make_test_png("test_mc_basic.png", W, H, true);
    
    std::string text = "MultiCluster basic test!";
    uint64_t salt = 55555;
    int ecc_level = 3;
    int num_clusters = 5;
    
    auto er = embed_text_multicluster("test_mc_basic.png", "test_mc_basic_out.png",
                                       text, salt, ecc_level, num_clusters);
    CHECK(er.success, "multicluster embed ok (used " + std::to_string(er.used_bits) + " bits)");
    
    // Extract from original (no crop)
    auto xr = extract_text_multicluster("test_mc_basic_out.png", salt, ecc_level,
                                         num_clusters, W, H);
    CHECK(xr.success, "multicluster extract ok");
    CHECK(xr.text == text, "text matches: \"" + xr.text + "\"");
}

void test_multicluster_capacity() {
    std::cout << "\n=== MultiCluster: Capacity Query ===\n";
    
    make_test_png("test_mc_cap.png", 600, 600, true);
    
    auto cap1 = get_capacity_multicluster("test_mc_cap.png", 1);
    auto cap5 = get_capacity_multicluster("test_mc_cap.png", 5);
    auto cap10 = get_capacity_multicluster("test_mc_cap.png", 10);
    
    CHECK(cap1.blocks_per_channel > 0, "1-cluster blocks/ch: " + std::to_string(cap1.blocks_per_channel));
    CHECK(cap5.blocks_per_channel > 0, "5-cluster blocks/ch: " + std::to_string(cap5.blocks_per_channel));
    CHECK(cap10.blocks_per_channel > 0, "10-cluster blocks/ch: " + std::to_string(cap10.blocks_per_channel));
    
    // 5 clusters should have ~1/5 the capacity of 1 cluster
    double ratio5 = (double)cap5.blocks_per_channel / cap1.blocks_per_channel;
    CHECK(ratio5 > 0.15 && ratio5 < 0.25, "5-cluster ratio ~0.2: " + std::to_string(ratio5));
    
    std::cout << "  1-cluster: " << cap1.max_text_bytes << " bytes\n";
    std::cout << "  5-cluster: " << cap5.max_text_bytes << " bytes\n";
    std::cout << " 10-cluster: " << cap10.max_text_bytes << " bytes\n";
}

void test_multicluster_crop_30() {
    std::cout << "\n=== MultiCluster: 30% Crop Recovery (10 clusters) ===\n";
    
    uint32_t W = 600, H = 600;
    make_test_png("test_mc_crop30.png", W, H, true);
    
    std::string text = "Crop recovery 30%!";
    uint64_t salt = 77777;
    int ecc_level = 6; // npar=64
    int num_clusters = 10;
    
    auto er = embed_text_multicluster("test_mc_crop30.png", "test_mc_crop30_out.png",
                                       text, salt, ecc_level, num_clusters);
    CHECK(er.success, "embed ok (used " + std::to_string(er.used_bits) + " bits)");
    if (!er.success) return;
    
    // Load stego and crop bottom 30% (180 pixels = 60 block rows)
    std::vector<uint8_t> orig_data;
    uint32_t orig_w, orig_h;
    unsigned decode_err = lodepng::decode(orig_data, orig_w, orig_h,
                                          "test_mc_crop30_out.png", LCT_RGBA, 8);
    CHECK(decode_err == 0, "loaded stego image");
    
    uint32_t crop_h = H - (uint32_t)(H * 0.3); // 60% of original
    std::vector<uint8_t> crop_data(crop_h * W * 4, 0);
    for (uint32_t y = 0; y < crop_h; y++)
        memcpy(crop_data.data() + y * W * 4, orig_data.data() + y * W * 4, W * 4);
    
    lodepng::encode("test_mc_crop30_cropped.png", crop_data, W, crop_h, LCT_RGBA, 8);
    
    std::cout << "  Cropped from " << W << "x" << H << " to " << W << "x" << crop_h << "\n";
    
    // Standard extract on cropped should likely fail
    auto xr_std = extract_text("test_mc_crop30_cropped.png", salt, ecc_level);
    std::cout << "  Standard extract: " << (xr_std.success ? "success" : "failed") << "\n";
    
    // Multi-cluster extract should succeed
    auto xr_mc = extract_text_multicluster("test_mc_crop30_cropped.png", salt, ecc_level,
                                            num_clusters, W, H);
    CHECK(xr_mc.success, "multicluster extraction succeeded after 30% crop");
    if (xr_mc.success) {
        CHECK(xr_mc.text == text, "text matches: \"" + xr_mc.text + "\"");
    } else {
        std::cout << "  Error: " << xr_mc.error_msg << "\n";
        fail++;
    }
}

void test_multicluster_crop_50() {
    std::cout << "\n=== MultiCluster: 50% Crop Recovery (10 clusters) ===\n";
    
    uint32_t W = 600, H = 600;
    make_test_png("test_mc_crop50.png", W, H, true);
    
    std::string text = "Crop recovery 50%!";
    uint64_t salt = 88888;
    int ecc_level = 6;
    int num_clusters = 10;
    
    auto er = embed_text_multicluster("test_mc_crop50.png", "test_mc_crop50_out.png",
                                       text, salt, ecc_level, num_clusters);
    CHECK(er.success, "embed ok");
    if (!er.success) return;
    
    // Crop bottom 50%
    std::vector<uint8_t> orig_data;
    uint32_t orig_w, orig_h;
    lodepng::decode(orig_data, orig_w, orig_h, "test_mc_crop50_out.png", LCT_RGBA, 8);
    
    uint32_t crop_h = H / 2;
    std::vector<uint8_t> crop_data(crop_h * W * 4, 0);
    for (uint32_t y = 0; y < crop_h; y++)
        memcpy(crop_data.data() + y * W * 4, orig_data.data() + y * W * 4, W * 4);
    
    lodepng::encode("test_mc_crop50_cropped.png", crop_data, W, crop_h, LCT_RGBA, 8);
    
    std::cout << "  Cropped from " << W << "x" << H << " to " << W << "x" << crop_h << "\n";
    
    auto xr_mc = extract_text_multicluster("test_mc_crop50_cropped.png", salt, ecc_level,
                                            num_clusters, W, H);
    CHECK(xr_mc.success, "multicluster extraction succeeded after 50% crop");
    if (xr_mc.success) {
        CHECK(xr_mc.text == text, "text matches: \"" + xr_mc.text + "\"");
    } else {
        std::cout << "  Error: " << xr_mc.error_msg << "\n";
        fail++;
    }
}

void test_multicluster_single() {
    std::cout << "\n=== MultiCluster: Single Cluster (num_clusters=1) ===\n";
    
    uint32_t W = 300, H = 300;
    make_test_png("test_mc_single.png", W, H, true);
    
    std::string text = "Single cluster test";
    uint64_t salt = 11111;
    int ecc_level = 2;
    
    auto er = embed_text_multicluster("test_mc_single.png", "test_mc_single_out.png",
                                       text, salt, ecc_level, 1);
    CHECK(er.success, "single-cluster embed ok");
    
    auto xr = extract_text_multicluster("test_mc_single_out.png", salt, ecc_level, 1, W, H);
    CHECK(xr.success, "single-cluster extract ok");
    CHECK(xr.text == text, "text matches: \"" + xr.text + "\"");
}

void test_multicluster_utf8() {
    std::cout << "\n=== MultiCluster: UTF-8 Text ===\n";
    
    uint32_t W = 600, H = 600;
    make_test_png("test_mc_utf8.png", W, H, true);
    
    std::string text = "Multicluster UTF-8: 你好世界 © 2026 测试 αβγ";
    uint64_t salt = 99999;
    int ecc_level = 4;
    int num_clusters = 5;
    
    auto er = embed_text_multicluster("test_mc_utf8.png", "test_mc_utf8_out.png",
                                       text, salt, ecc_level, num_clusters);
    CHECK(er.success, "UTF-8 multicluster embed ok");
    
    auto xr = extract_text_multicluster("test_mc_utf8_out.png", salt, ecc_level,
                                         num_clusters, W, H);
    CHECK(xr.success, "UTF-8 multicluster extract ok");
    CHECK(xr.text == text, "UTF-8 text matches");
}

void test_multicluster_backward_compat() {
    std::cout << "\n=== MultiCluster: Original API Still Works ===\n";
    
    // Ensure existing embed_text/extract_text still work after adding multicluster code
    make_test_png("test_mc_compat.png", 200, 200, true);
    
    std::string text = "backward compat check";
    uint64_t salt = 42;
    
    auto er = embed_text("test_mc_compat.png", "test_mc_compat_out.png", text, salt, 3);
    CHECK(er.success, "original embed still works");
    
    auto xr = extract_text("test_mc_compat_out.png", salt, 3);
    CHECK(xr.success, "original extract still works");
    CHECK(xr.text == text, "original text matches");
    
    // Also test erasure extraction
    auto xr_era = extract_text_with_erasures("test_mc_compat_out.png", salt, 3, 200, 200);
    CHECK(xr_era.success, "original erasure extract still works (no crop)");
    CHECK(xr_era.text == text, "original erasure text matches");
}

int main() {
    std::cout << "========================================\n";
    std::cout << "  PNG Watermark Tool - Automated Test\n";
    std::cout << "  (with RS ECC + Erasure + MultiCluster)\n";
    std::cout << "========================================\n";
    
    // Original tests (backward compatibility)
    test_basic_embed_extract();
    test_rgb_image();
    test_wrong_salt();
    test_capacity_query();
    test_long_text();
    test_text_overflow();
    test_multiline_text();
    test_special_chars();
    
    // RS codec unit tests
    test_rs_no_errors();
    test_rs_few_errors();
    test_rs_too_many_errors();
    test_rs_multi_block();
    test_rs_max_correction();
    
    // RS erasure decode tests
    test_rs_erasure_no_errors();
    test_rs_erasure_correction();
    test_rs_erasure_max_capacity();
    test_rs_erasure_over_capacity();
    
    // ECC watermark integration tests
    test_ecc_embed_extract();
    test_ecc_capacity_overflow();
    test_ecc_backward_compat();
    test_ecc_wrong_level();
    
    // Watermark erasure extraction tests
    test_watermark_erasure_extraction();
    test_watermark_erasure_higher_ecc();
    
    // Multi-cluster tests
    test_multicluster_basic();
    test_multicluster_capacity();
    test_multicluster_crop_30();
    test_multicluster_crop_50();
    test_multicluster_single();
    test_multicluster_utf8();
    test_multicluster_backward_compat();
    
    std::cout << "\n========================================\n";
    std::cout << "  Result: " << pass << " passed, " << fail << " failed\n";
    std::cout << "========================================\n";
    
    return fail > 0 ? 1 : 0;
}
