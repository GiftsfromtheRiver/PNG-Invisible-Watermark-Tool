// watermark_tool automated test
#include "watermark.h"
#include "lodepng.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <cstring>

using namespace watermark;

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

void test_basic_embed_extract() {
    std::cout << "\n=== Basic Embed/Extract ===\n";
    
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
    std::cout << "\n=== RGB (no Alpha) ===\n";
    
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
    std::cout << "\n=== Long Text ===\n";
    
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

int main() {
    std::cout << "========================================\n";
    std::cout << "  PNG Watermark Tool - Automated Test\n";
    std::cout << "========================================\n";
    
    test_basic_embed_extract();
    test_rgb_image();
    test_wrong_salt();
    test_capacity_query();
    test_long_text();
    test_text_overflow();
    test_multiline_text();
    test_special_chars();
    
    std::cout << "\n========================================\n";
    std::cout << "  Result: " << pass << " passed, " << fail << " failed\n";
    std::cout << "========================================\n";
    
    return fail > 0 ? 1 : 0;
}
