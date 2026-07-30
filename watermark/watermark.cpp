#include "watermark.h"
#include "lodepng.h"

#include <random>
#include <algorithm>
#include <cstring>
#include <numeric>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace watermark {

// ========================================================================
// 内部实现
// ========================================================================
namespace {

// ---------- ±1 LSB Matching ----------

inline uint8_t read_lsb(const uint8_t* image, size_t pixel_idx, int channel) {
    return image[pixel_idx * 4 + channel] & 1;
}

// ±1 嵌入：LSB 不匹配时随机 +1 或 -1，避免直方图阶梯分布
// 方向由 FNV-1a(salt, pixel_idx, channel, extra) 决定，保证可重现
inline void write_lsb_matching(uint8_t* image, size_t pixel_idx, int channel, 
                                uint8_t bit, uint64_t salt, uint32_t extra = 0) {
    size_t idx = pixel_idx * 4 + channel;
    uint8_t val = image[idx];
    uint8_t current_lsb = val & 1;
    
    if (current_lsb == bit) {
        return; // 已匹配，不需要修改
    }
    
    // 确定性 hash 决定 ±1 方向
    uint64_t hash = 14695981039346656037ULL;
    hash ^= salt;
    hash *= 1099511628211ULL;
    hash ^= pixel_idx;
    hash *= 1099511628211ULL;
    hash ^= (uint64_t)channel;
    hash *= 1099511628211ULL;
    hash ^= extra;
    hash *= 1099511628211ULL;
    
    bool go_up = (hash & 1);
    
    if (val == 0) {
        image[idx] = 1;
    } else if (val == 255) {
        image[idx] = 254;
    } else {
        image[idx] = go_up ? (val + 1) : (val - 1);
    }
}

// ---------- 3×3 块标记 ----------

inline uint8_t compute_marker_bit(uint64_t salt, size_t block_id, int neighbor_offset) {
    uint64_t hash = 14695981039346656037ULL;
    hash ^= salt;
    hash *= 1099511628211ULL;
    hash ^= block_id;
    hash *= 1099511628211ULL;
    hash ^= neighbor_offset;
    hash *= 1099511628211ULL;
    return hash & 1;
}

// ---------- 块级 PRNG 打乱 ----------
// 将图片分成不重叠的 3×3 网格块，用 PRNG 打乱块的顺序
// 容量精确 = grid_cols * grid_rows，无浪费

struct BlockGrid {
    uint32_t grid_cols;  // 可用列数
    uint32_t grid_rows;  // 可用行数
    uint32_t img_width;
    uint32_t img_height;
    
    // block_id → 中心像素在图片中的索引
    size_t block_center(size_t block_id) const {
        // block_id 映射到 grid 坐标
        size_t gr = block_id / grid_cols;
        size_t gc = block_id % grid_cols;
        // 中心像素坐标 (从第1行/列开始，留边界)
        size_t cx = gc * 3 + 1;
        size_t cy = gr * 3 + 1;
        return cy * img_width + cx;
    }
    
    // block_id → 3×3 块中所有 9 个像素的索引
    void block_pixels(size_t block_id, size_t out[9]) const {
        size_t gr = block_id / grid_cols;
        size_t gc = block_id % grid_cols;
        size_t base_x = gc * 3;  // 块左上角
        size_t base_y = gr * 3;
        int idx = 0;
        for (int dy = 0; dy < 3; dy++) {
            for (int dx = 0; dx < 3; dx++) {
                out[idx++] = (base_y + dy) * img_width + (base_x + dx);
            }
        }
    }
    
    size_t total_blocks() const { return (size_t)grid_cols * grid_rows; }
};

BlockGrid make_grid(uint32_t width, uint32_t height) {
    BlockGrid g;
    g.img_width = width;
    g.img_height = height;
    g.grid_cols = width / 3;   // 从第 0 列开始，每 3 列一个块
    g.grid_rows = height / 3;
    return g;
}

// 打乱块顺序
std::vector<size_t> shuffle_blocks(size_t num_blocks, uint64_t salt) {
    std::vector<size_t> order(num_blocks);
    std::iota(order.begin(), order.end(), 0);
    std::mt19937_64 rng(salt);
    for (size_t i = num_blocks - 1; i > 0; i--) {
        std::uniform_int_distribution<size_t> dist(0, i);
        size_t j = dist(rng);
        std::swap(order[i], order[j]);
    }
    return order;
}

// 嵌入一个 3×3 块
void embed_block(uint8_t* image,
                 const BlockGrid& grid,
                 int channel,
                 size_t block_id,       // 原始块ID（用于计算像素位置）
                 uint8_t data_bit,
                 uint64_t salt,
                 uint64_t block_seq) {  // 序列号（用于标记的hash种子）
    size_t pixels[9];
    grid.block_pixels(block_id, pixels);
    
    // 中心像素(索引4)写数据 bit
    write_lsb_matching(image, pixels[4], channel, data_bit, salt, 0xFFFFFFFF);
    
    // 周边 8 像素写标记
    for (int i = 0; i < 9; i++) {
        if (i == 4) continue;
        uint8_t marker = compute_marker_bit(salt, block_seq, i);
        write_lsb_matching(image, pixels[i], channel, marker, salt, (uint32_t)i);
    }
}

// 提取一个 3×3 块（只读中心像素 LSB）
uint8_t extract_block(const uint8_t* image,
                      const BlockGrid& grid,
                      int channel,
                      size_t block_id) {
    size_t pixels[9];
    grid.block_pixels(block_id, pixels);
    return read_lsb(image, pixels[4], channel);
}

// ---------- 编解码 ----------

std::vector<uint8_t> bytes_to_bits(const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> bits;
    bits.reserve(bytes.size() * 8);
    for (uint8_t byte : bytes) {
        for (int i = 7; i >= 0; i--) {
            bits.push_back((byte >> i) & 1);
        }
    }
    return bits;
}

std::vector<uint8_t> bits_to_bytes(const std::vector<uint8_t>& bits) {
    size_t num_bytes = bits.size() / 8;
    std::vector<uint8_t> bytes(num_bytes);
    for (size_t i = 0; i < num_bytes; i++) {
        uint8_t byte = 0;
        for (int j = 0; j < 8; j++) {
            byte = (byte << 1) | bits[i * 8 + j];
        }
        bytes[i] = byte;
    }
    return bytes;
}

// 文本负载: [LENGTH:32bit大端][UTF-8 DATA]
std::vector<uint8_t> encode_text_payload(const std::string& text) {
    std::vector<uint8_t> payload;
    uint32_t len = (uint32_t)text.size();
    payload.push_back((len >> 24) & 0xFF);
    payload.push_back((len >> 16) & 0xFF);
    payload.push_back((len >>  8) & 0xFF);
    payload.push_back( len        & 0xFF);
    payload.insert(payload.end(), text.begin(), text.end());
    return payload;
}

bool decode_text_payload(const uint8_t* data, size_t data_len, std::string& out_text) {
    if (data_len < 4) return false;
    uint32_t len = ((uint32_t)data[0] << 24) |
                   ((uint32_t)data[1] << 16) |
                   ((uint32_t)data[2] <<  8) |
                    (uint32_t)data[3];
    if (4 + (size_t)len > data_len) return false;
    out_text.assign((const char*)(data + 4), len);
    return true;
}

// ---------- PNG 加载/保存（支持 Windows 中文路径） ----------

struct PNGInfo {
    std::vector<uint8_t> data;  // RGBA 像素数据
    uint32_t width = 0;
    uint32_t height = 0;
    bool has_alpha = false;
};

// Windows 下用宽字符读取文件
static bool read_file_wide(const std::wstring& wpath, std::vector<unsigned char>& buffer) {
#ifdef _WIN32
    FILE* file = _wfopen(wpath.c_str(), L"rb");
    if (!file) return false;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        fclose(file);
        return false;
    }
    buffer.resize(size);
    size_t read = fread(buffer.data(), 1, size, file);
    fclose(file);
    return read == (size_t)size;
#else
    return false;
#endif
}

// Windows 下用宽字符写入文件
static bool write_file_wide(const std::wstring& wpath, const std::vector<unsigned char>& data) {
#ifdef _WIN32
    FILE* file = _wfopen(wpath.c_str(), L"wb");
    if (!file) return false;
    size_t written = fwrite(data.data(), 1, data.size(), file);
    fclose(file);
    return written == data.size();
#else
    return false;
#endif
}

// 将 UTF-8 字符串转为宽字符
static std::wstring utf8_to_wstring(const std::string& str) {
#ifdef _WIN32
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (len <= 0) {
        // 降级到 ANSI (GBK)
        len = MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, nullptr, 0);
        if (len <= 0) return L"";
    }
    std::wstring wstr(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], len);
    while (!wstr.empty() && wstr.back() == L'\0') wstr.pop_back();
    return wstr;
#else
    return L"";
#endif
}

PNGInfo load_png_info(const std::string& path) {
    PNGInfo info;
    unsigned error = 0;
    
#ifdef _WIN32
    std::wstring wpath = utf8_to_wstring(path);
    if (wpath.empty()) {
        info.width = info.height = 0;
        return info;
    }
    
    // 用宽字符读取文件
    std::vector<unsigned char> buffer;
    if (!read_file_wide(wpath, buffer)) {
        info.width = info.height = 0;
        return info;
    }
    
    // 用 lodepng 解码内存数据
    error = lodepng::decode(info.data, info.width, info.height, buffer, LCT_RGBA, 8);
    if (error != 0) {
        info.width = info.height = 0;
        return info;
    }
    
    // 检测是否有 Alpha 通道
    info.has_alpha = false;
    for (size_t i = 3; i < info.data.size(); i += 4) {
        if (info.data[i] != 255) {
            info.has_alpha = true;
            break;
        }
    }
    return info;
#else
    // Linux / macOS：直接用
    error = lodepng::decode(info.data, info.width, info.height, path, LCT_RGBA, 8);
    if (error != 0) {
        info.width = info.height = 0;
        return info;
    }
    info.has_alpha = false;
    for (size_t i = 3; i < info.data.size(); i += 4) {
        if (info.data[i] != 255) {
            info.has_alpha = true;
            break;
        }
    }
    return info;
#endif
}

bool save_png_adaptive(const std::string& path,
                       const std::vector<uint8_t>& rgba_data,
                       uint32_t width,
                       uint32_t height,
                       bool save_as_rgb) {
    unsigned error;
    std::vector<unsigned char> out;
    
    if (save_as_rgb) {
        std::vector<uint8_t> rgb(width * height * 3);
        for (size_t i = 0; i < (size_t)width * height; i++) {
            rgb[i * 3 + 0] = rgba_data[i * 4 + 0];
            rgb[i * 3 + 1] = rgba_data[i * 4 + 1];
            rgb[i * 3 + 2] = rgba_data[i * 4 + 2];
        }
        error = lodepng::encode(out, rgb, width, height, LCT_RGB, 8);
    } else {
        error = lodepng::encode(out, rgba_data, width, height, LCT_RGBA, 8);
    }
    
    if (error != 0) return false;
    
#ifdef _WIN32
    std::wstring wpath = utf8_to_wstring(path);
    if (wpath.empty()) return false;
    return write_file_wide(wpath, out);
#else
    // Linux / macOS：直接用 lodepng 保存
    if (save_as_rgb) {
        std::vector<uint8_t> rgb(width * height * 3);
        for (size_t i = 0; i < (size_t)width * height; i++) {
            rgb[i * 3 + 0] = rgba_data[i * 4 + 0];
            rgb[i * 3 + 1] = rgba_data[i * 4 + 1];
            rgb[i * 3 + 2] = rgba_data[i * 4 + 2];
        }
        error = lodepng::encode(path, rgb, width, height, LCT_RGB, 8);
        return error == 0;
    } else {
        error = lodepng::encode(path, rgba_data, width, height, LCT_RGBA, 8);
        return error == 0;
    }
#endif
}

// ---------- 通道分配 ----------
// 始终只使用 RGB 三通道，不使用 Alpha
// 原因：社交平台(QQ/微信等)传输 PNG 时可能剥离 Alpha 通道
// ch0=B(2), ch1=G(1), ch2=R(0) → 3通道

int channel_for_index(int idx, bool has_alpha) {
    static const int ch[] = {2, 1, 0, -1};  // B, G, R
    (void)has_alpha;
    return ch[idx];
}

} // anonymous namespace

// ========================================================================
// 公共 API
// ========================================================================

EmbedResult embed_text(const std::string& input_png,
                       const std::string& output_png,
                       const std::string& text,
                       uint64_t salt) {
    EmbedResult result;
    
    if (text.empty()) {
        result.error_msg = "watermark text is empty";
        return result;
    }
    
    auto png = load_png_info(input_png);
    if (png.width == 0) {
        result.error_msg = "failed to load PNG: " + input_png;
        return result;
    }
    
    result.width = png.width;
    result.height = png.height;
    result.has_alpha = png.has_alpha;
    
    int num_channels = 3;  // 始终只用 RGB 三通道
    auto grid = make_grid(png.width, png.height);
    size_t blocks_per_channel = grid.total_blocks();
    size_t total_blocks = blocks_per_channel * num_channels;
    result.capacity_bits = total_blocks;
    
    if (blocks_per_channel == 0) {
        result.error_msg = "image too small for watermark (need at least 3x3)";
        return result;
    }
    
    auto payload = encode_text_payload(text);
    auto bits = bytes_to_bits(payload);
    
    if (bits.size() > total_blocks) {
        result.error_msg = "text too large (" + std::to_string(bits.size()) + 
                          " bits needed, " + std::to_string(total_blocks) + " available)";
        return result;
    }
    
    // 跨通道嵌入
    size_t bits_written = 0;
    for (int ci = 0; ci < num_channels && bits_written < bits.size(); ci++) {
        int channel = channel_for_index(ci, png.has_alpha);
        if (channel < 0) break;
        
        auto block_order = shuffle_blocks(blocks_per_channel, salt * 1000 + ci);
        
        for (size_t bi = 0; bi < block_order.size() && bits_written < bits.size(); bi++) {
            size_t orig_block_id = block_order[bi];
            embed_block(png.data.data(), grid, channel, orig_block_id,
                       bits[bits_written], salt, bi);
            bits_written++;
        }
    }
    
    if (bits_written < bits.size()) {
        result.error_msg = "failed to embed all bits";
        return result;
    }
    
    if (!save_png_adaptive(output_png, png.data, png.width, png.height, !png.has_alpha)) {
        result.error_msg = "failed to save PNG: " + output_png;
        return result;
    }
    
    result.success = true;
    result.used_bits = bits_written;
    return result;
}

ExtractResult extract_text(const std::string& stego_png,
                           uint64_t salt) {
    ExtractResult result;
    
    auto png = load_png_info(stego_png);
    if (png.width == 0) {
        result.error_msg = "failed to load PNG: " + stego_png;
        return result;
    }
    
    int num_channels = 3;
    auto grid = make_grid(png.width, png.height);
    size_t blocks_per_channel = grid.total_blocks();
    
    std::vector<uint8_t> all_bits;
    
    for (int ci = 0; ci < num_channels; ci++) {
        int channel = channel_for_index(ci, png.has_alpha);
        if (channel < 0) break;
        
        auto block_order = shuffle_blocks(blocks_per_channel, salt * 1000 + ci);
        
        for (size_t bi = 0; bi < block_order.size(); bi++) {
            size_t orig_block_id = block_order[bi];
            uint8_t bit = extract_block(png.data.data(), grid, channel, orig_block_id);
            all_bits.push_back(bit);
            
            // 读够 32 bit 后解析长度
            if (all_bits.size() >= 32 && (all_bits.size() % 8 == 0)) {
                auto header_bytes = bits_to_bytes(all_bits);
                uint32_t text_len = ((uint32_t)header_bytes[0] << 24) |
                                    ((uint32_t)header_bytes[1] << 16) |
                                    ((uint32_t)header_bytes[2] <<  8) |
                                     (uint32_t)header_bytes[3];
                size_t total_bits_needed = 32 + (size_t)text_len * 8;
                if (all_bits.size() >= total_bits_needed) goto done_reading;
            }
        }
    }
    
done_reading:
    if (all_bits.size() < 32) {
        result.error_msg = "not enough data for length header";
        return result;
    }
    
    auto all_bytes = bits_to_bytes(all_bits);
    if (!decode_text_payload(all_bytes.data(), all_bytes.size(), result.text)) {
        result.error_msg = "failed to decode watermark (wrong salt?)";
        return result;
    }
    
    result.success = true;
    return result;
}

CapacityInfo get_capacity(const std::string& png_path) {
    CapacityInfo info;
    
    auto png = load_png_info(png_path);
    if (png.width == 0) return info;
    
    info.width = png.width;
    info.height = png.height;
    info.has_alpha = png.has_alpha;
    info.total_pixels = (size_t)png.width * png.height;
    info.available_channels = 3;
    
    auto grid = make_grid(png.width, png.height);
    info.blocks_per_channel = grid.total_blocks();
    
    size_t total_blocks = info.blocks_per_channel * info.available_channels;
    size_t data_bits = total_blocks > 32 ? total_blocks - 32 : 0;
    info.max_text_bytes = data_bits / 8;
    info.max_text_chars = info.max_text_bytes / 3;
    
    return info;
}

} // namespace watermark