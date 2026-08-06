#include "watermark.h"
#include "rs_codec.h"
#include "lodepng.h"

#include <random>
#include <algorithm>
#include <cstring>
#include <numeric>
#include <iostream>
#include <map>

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

inline void write_lsb_matching(uint8_t* image, size_t pixel_idx, int channel, 
                                uint8_t bit, uint64_t salt, uint32_t extra = 0) {
    size_t idx = pixel_idx * 4 + channel;
    uint8_t val = image[idx];
    uint8_t current_lsb = val & 1;
    
    if (current_lsb == bit) {
        return;
    }
    
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

struct BlockGrid {
    uint32_t grid_cols;
    uint32_t grid_rows;
    uint32_t img_width;
    uint32_t img_height;
    
    size_t block_center(size_t block_id) const {
        size_t gr = block_id / grid_cols;
        size_t gc = block_id % grid_cols;
        size_t cx = gc * 3 + 1;
        size_t cy = gr * 3 + 1;
        return cy * img_width + cx;
    }
    
    void block_pixels(size_t block_id, size_t out[9], int left_offset = 0, int top_offset = 0) const {
        size_t gr = block_id / grid_cols;
        size_t gc = block_id % grid_cols;
        int base_x = (int)(gc * 3) - left_offset;
        int base_y = (int)(gr * 3) - top_offset;
        int idx = 0;
        for (int dy = 0; dy < 3; dy++) {
            for (int dx = 0; dx < 3; dx++) {
                out[idx++] = (size_t)(base_y + dy) * img_width + (size_t)(base_x + dx);
            }
        }
    }
    
    size_t total_blocks() const { return (size_t)grid_cols * grid_rows; }
};

BlockGrid make_grid(uint32_t width, uint32_t height) {
    BlockGrid g;
    g.img_width = width;
    g.img_height = height;
    g.grid_cols = width / 3;
    g.grid_rows = height / 3;
    return g;
}

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

void embed_block(uint8_t* image,
                 const BlockGrid& grid,
                 int channel,
                 size_t block_id,
                 uint8_t data_bit,
                 uint64_t salt,
                 uint64_t block_seq) {
    size_t pixels[9];
    grid.block_pixels(block_id, pixels);
    
    write_lsb_matching(image, pixels[4], channel, data_bit, salt, 0xFFFFFFFF);
    
    for (int i = 0; i < 9; i++) {
        if (i == 4) continue;
        uint8_t marker = compute_marker_bit(salt, block_seq, i);
        write_lsb_matching(image, pixels[i], channel, marker, salt, (uint32_t)i);
    }
}

uint8_t extract_block(const uint8_t* image,
                      const BlockGrid& grid,
                      int channel,
                      size_t block_id,
                      int left_offset = 0, int top_offset = 0) {
    size_t pixels[9];
    grid.block_pixels(block_id, pixels, left_offset, top_offset);
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

// ---------- 大端编码/解码辅助 ----------

void write_u32_be(uint8_t* buf, uint32_t val) {
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >>  8) & 0xFF;
    buf[3] =  val        & 0xFF;
}

uint32_t read_u32_be(const uint8_t* buf) {
    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] <<  8) |
            (uint32_t)buf[3];
}

// ---------- PNG 加载/保存 ----------

struct PNGInfo {
    std::vector<uint8_t> data;
    uint32_t width = 0;
    uint32_t height = 0;
    bool has_alpha = false;
};

static bool read_file_wide(const std::wstring& wpath, std::vector<unsigned char>& buffer) {
#ifdef _WIN32
    FILE* file = _wfopen(wpath.c_str(), L"rb");
    if (!file) return false;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0) { fclose(file); return false; }
    buffer.resize(size);
    size_t read = fread(buffer.data(), 1, size, file);
    fclose(file);
    return read == (size_t)size;
#else
    return false;
#endif
}

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

static std::wstring utf8_to_wstring(const std::string& str) {
#ifdef _WIN32
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (len <= 0) {
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
    if (wpath.empty()) { info.width = info.height = 0; return info; }
    std::vector<unsigned char> buffer;
    if (!read_file_wide(wpath, buffer)) { info.width = info.height = 0; return info; }
    error = lodepng::decode(info.data, info.width, info.height, buffer, LCT_RGBA, 8);
    if (error != 0) { info.width = info.height = 0; return info; }
#else
    error = lodepng::decode(info.data, info.width, info.height, path, LCT_RGBA, 8);
    if (error != 0) { info.width = info.height = 0; return info; }
#endif
    
    info.has_alpha = false;
    for (size_t i = 3; i < info.data.size(); i += 4) {
        if (info.data[i] != 255) { info.has_alpha = true; break; }
    }
    return info;
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

int channel_for_index(int idx, bool has_alpha) {
    static const int ch[] = {2, 1, 0, -1};
    (void)has_alpha;
    return ch[idx];
}

// ---------- 嵌入/提取 bits 的通用流程 ----------

// 将所有 bits 嵌入图片
void embed_bits_to_image(uint8_t* image_data,
                         const BlockGrid& grid,
                         const std::vector<uint8_t>& bits,
                         uint64_t salt,
                         bool has_alpha) {
    int num_channels = 3;
    size_t blocks_per_channel = grid.total_blocks();
    size_t bits_written = 0;
    
    for (int ci = 0; ci < num_channels && bits_written < bits.size(); ci++) {
        int channel = channel_for_index(ci, has_alpha);
        if (channel < 0) break;
        
        auto block_order = shuffle_blocks(blocks_per_channel, salt * 1000 + ci);
        
        for (size_t bi = 0; bi < block_order.size() && bits_written < bits.size(); bi++) {
            size_t orig_block_id = block_order[bi];
            embed_block(image_data, grid, channel, orig_block_id,
                       bits[bits_written], salt, bi);
            bits_written++;
        }
    }
}

// 从图片中提取所有 bits (最多 max_bits 个)
std::vector<uint8_t> extract_bits_from_image(const uint8_t* image_data,
                                              const BlockGrid& grid,
                                              size_t max_bits,
                                              uint64_t salt,
                                              bool has_alpha) {
    int num_channels = 3;
    size_t blocks_per_channel = grid.total_blocks();
    std::vector<uint8_t> all_bits;
    all_bits.reserve(max_bits);
    
    for (int ci = 0; ci < num_channels; ci++) {
        int channel = channel_for_index(ci, has_alpha);
        if (channel < 0) break;
        
        auto block_order = shuffle_blocks(blocks_per_channel, salt * 1000 + ci);
        
        for (size_t bi = 0; bi < block_order.size(); bi++) {
            size_t orig_block_id = block_order[bi];
            uint8_t bit = extract_block(image_data, grid, channel, orig_block_id);
            all_bits.push_back(bit);
            if (all_bits.size() >= max_bits) return all_bits;
        }
    }
    
    return all_bits;
}

// ---------- 擦除感知提取 ----------

// Check if all 9 pixels of a block are within image bounds
bool block_in_bounds(const BlockGrid& grid, size_t block_id,
                     uint32_t img_w, uint32_t img_h,
                     int left_offset = 0, int top_offset = 0) {
    size_t gr = block_id / grid.grid_cols;
    size_t gc = block_id % grid.grid_cols;
    int base_x = (int)(gc * 3) - left_offset;
    int base_y = (int)(gr * 3) - top_offset;
    return (base_x >= 0) && (base_x + 2 < (int)img_w) && 
           (base_y >= 0) && (base_y + 2 < (int)img_h);
}

// Extract bits using original grid, marking out-of-bounds blocks as erasures
std::vector<uint8_t> extract_bits_with_erasures(
    const uint8_t* stego_data,
    const BlockGrid& orig_grid,
    uint32_t stego_w, uint32_t stego_h,
    size_t max_bits,
    uint64_t salt,
    bool has_alpha,
    std::vector<size_t>* out_erasure_bit_positions,
    int left_offset, int top_offset) {
    
    int num_channels = 3;
    size_t blocks_per_channel = orig_grid.total_blocks();
    std::vector<uint8_t> all_bits;
    all_bits.reserve(max_bits);
    out_erasure_bit_positions->clear();
    
    // stego_grid uses screenshot dimensions for pixel addressing
    BlockGrid stego_grid = orig_grid;
    stego_grid.img_width = stego_w;
    stego_grid.img_height = stego_h;
    
    for (int ci = 0; ci < num_channels; ci++) {
        int channel = channel_for_index(ci, has_alpha);
        if (channel < 0) break;
        
        auto block_order = shuffle_blocks(blocks_per_channel, salt * 1000 + ci);
        
        for (size_t bi = 0; bi < block_order.size(); bi++) {
            size_t orig_block_id = block_order[bi];
            size_t bit_pos = all_bits.size();
            
            if (block_in_bounds(orig_grid, orig_block_id, stego_w, stego_h, left_offset, top_offset)) {
                uint8_t bit = extract_block(stego_data, stego_grid, channel, orig_block_id, left_offset, top_offset);
                all_bits.push_back(bit);
            } else {
                all_bits.push_back(0);
                out_erasure_bit_positions->push_back(bit_pos);
            }
            
            if (all_bits.size() >= max_bits) return all_bits;
        }
    }
    
    return all_bits;
}

// ---------- 字节级单元 + 空间局部性 (ECC 专用) ----------
// 核心思想：每个 RS 字节的 8 bit 来自同一"单元"（3 个相邻块 × 3 通道 = 9 位，用 8 位）
// 单元通过 salt 打乱分散到全图，保证：
//   1) 裁剪删除的块集中在少数单元内 → 每个被删单元 = 1 个 RS 符号擦除
//   2) 相比旧方案（1 块 = 1 bit 散布到多个符号），擦除数降为 ~1/8
//
// 单元定义：同行连续 3 个 block，编号 (row, cell_col)
//   block 0 的通道 0,1,2 → bit 0,1,2
//   block 1 的通道 0,1,2 → bit 3,4,5
//   block 2 的通道 0,1   → bit 6,7   (通道 2 跳过，每单元 8 bit)

void embed_bits_cell_major(uint8_t* image_data,
                            const BlockGrid& grid,
                            const std::vector<uint8_t>& bits,
                            uint64_t salt,
                            bool has_alpha) {
    size_t cell_cols = grid.grid_cols / 3;
    size_t total_cells = cell_cols * grid.grid_rows;
    if (total_cells == 0) return;
    
    auto cell_order = shuffle_blocks(total_cells, salt);
    
    size_t bits_written = 0;
    for (size_t ci = 0; ci < cell_order.size() && bits_written < bits.size(); ci++) {
        size_t cell_id = cell_order[ci];
        size_t cell_row = cell_id / cell_cols;
        size_t cell_col = cell_id % cell_cols;
        
        // 遍历单元内 3 个 block，每个 block 3 通道，共 9 位，只用前 8 位
        for (int k = 0; k < 3 && bits_written < bits.size(); k++) {
            size_t block_col = cell_col * 3 + k;
            if (block_col >= grid.grid_cols) break;
            size_t block_id = cell_row * grid.grid_cols + block_col;
            
            for (int ch = 0; ch < 3 && bits_written < bits.size(); ch++) {
                if (k == 2 && ch == 2) break; // 跳过第 9 位
                int channel = channel_for_index(ch, has_alpha);
                embed_block(image_data, grid, channel, block_id,
                           bits[bits_written], salt, bits_written);
                bits_written++;
            }
        }
    }
}

// 单元级提取（支持擦除标记）
std::vector<uint8_t> extract_bits_cell_major_with_erasures(
    const uint8_t* stego_data,
    const BlockGrid& orig_grid,
    uint32_t stego_w, uint32_t stego_h,
    size_t max_bits,
    uint64_t salt,
    bool has_alpha,
    std::vector<size_t>* out_erasure_bit_positions,
    int left_offset, int top_offset) {
    
    size_t cell_cols = orig_grid.grid_cols / 3;
    size_t total_cells = cell_cols * orig_grid.grid_rows;
    std::vector<uint8_t> all_bits;
    all_bits.reserve(max_bits);
    out_erasure_bit_positions->clear();
    
    BlockGrid stego_grid = orig_grid;
    stego_grid.img_width = stego_w;
    stego_grid.img_height = stego_h;
    
    if (total_cells == 0) return all_bits;
    
    auto cell_order = shuffle_blocks(total_cells, salt);
    
    for (size_t ci = 0; ci < cell_order.size() && all_bits.size() < max_bits; ci++) {
        size_t cell_id = cell_order[ci];
        size_t cell_row = cell_id / cell_cols;
        size_t cell_col = cell_id % cell_cols;
        
        for (int k = 0; k < 3 && all_bits.size() < max_bits; k++) {
            size_t block_col = cell_col * 3 + k;
            if (block_col >= orig_grid.grid_cols) break;
            size_t block_id = cell_row * orig_grid.grid_cols + block_col;
            
            for (int ch = 0; ch < 3 && all_bits.size() < max_bits; ch++) {
                if (k == 2 && ch == 2) break;
                int channel = channel_for_index(ch, has_alpha);
                size_t bit_pos = all_bits.size();
                
                if (block_in_bounds(orig_grid, block_id, stego_w, stego_h, left_offset, top_offset)) {
                    uint8_t bit = extract_block(stego_data, stego_grid, channel, block_id, left_offset, top_offset);
                    all_bits.push_back(bit);
                } else {
                    all_bits.push_back(0);
                    out_erasure_bit_positions->push_back(bit_pos);
                }
            }
        }
    }
    
    return all_bits;
}

// ---------- 多簇辅助函数 ----------

// 获取条带行范围
void get_strip_range(int grid_rows, int num_clusters, int cluster_idx,
                     int& start_row, int& end_row) {
    int rows_per_strip = grid_rows / num_clusters;
    start_row = cluster_idx * rows_per_strip;
    if (cluster_idx == num_clusters - 1) {
        end_row = grid_rows;
    } else {
        end_row = start_row + rows_per_strip;
    }
}

// 在指定条带内嵌入 bits
void embed_bits_to_strip(uint8_t* image_data,
                          const BlockGrid& grid,
                          const std::vector<uint8_t>& bits,
                          uint64_t cluster_seed,
                          bool has_alpha,
                          int strip_start_row,
                          int strip_end_row) {
    int num_channels = 3;
    size_t bits_written = 0;
    
    for (int ci = 0; ci < num_channels && bits_written < bits.size(); ci++) {
        int channel = channel_for_index(ci, has_alpha);
        if (channel < 0) break;
        
        // Collect blocks in strip [start_row, end_row)
        size_t first_block = (size_t)strip_start_row * grid.grid_cols;
        size_t last_block = (size_t)strip_end_row * grid.grid_cols;
        size_t strip_block_count = last_block - first_block;
        
        if (strip_block_count == 0) continue;
        
        // PRNG shuffle within strip
        auto block_order = shuffle_blocks(strip_block_count, cluster_seed * 1000 + ci);
        
        for (size_t bi = 0; bi < block_order.size() && bits_written < bits.size(); bi++) {
            size_t orig_block_id = first_block + block_order[bi];
            embed_block(image_data, grid, channel, orig_block_id,
                       bits[bits_written], cluster_seed, bi);
            bits_written++;
        }
    }
}

// 从指定条带提取 bits + 擦除信息
std::vector<uint8_t> extract_bits_from_strip_with_erasures(
    const uint8_t* stego_data,
    const BlockGrid& grid,
    uint32_t stego_w, uint32_t stego_h,
    size_t max_bits,
    uint64_t cluster_seed,
    bool has_alpha,
    int strip_start_row,
    int strip_end_row,
    std::vector<size_t>* out_erasure_positions,
    int left_offset, int top_offset) {
    
    int num_channels = 3;
    std::vector<uint8_t> all_bits;
    all_bits.reserve(max_bits);
    out_erasure_positions->clear();
    
    // stego_grid uses screenshot dimensions for pixel addressing
    BlockGrid stego_grid = grid;
    stego_grid.img_width = stego_w;
    stego_grid.img_height = stego_h;
    
    for (int ci = 0; ci < num_channels; ci++) {
        int channel = channel_for_index(ci, has_alpha);
        if (channel < 0) break;
        
        size_t first_block = (size_t)strip_start_row * grid.grid_cols;
        size_t last_block = (size_t)strip_end_row * grid.grid_cols;
        size_t strip_block_count = last_block - first_block;
        
        if (strip_block_count == 0) continue;
        
        auto block_order = shuffle_blocks(strip_block_count, cluster_seed * 1000 + ci);
        
        for (size_t bi = 0; bi < block_order.size(); bi++) {
            size_t bit_pos = all_bits.size();
            size_t orig_block_id = first_block + block_order[bi];
            
            if (block_in_bounds(grid, orig_block_id, stego_w, stego_h, left_offset, top_offset)) {
                uint8_t bit = extract_block(stego_data, stego_grid, channel, orig_block_id, left_offset, top_offset);
                all_bits.push_back(bit);
            } else {
                all_bits.push_back(0);
                out_erasure_positions->push_back(bit_pos);
            }
            
            if (all_bits.size() >= max_bits) return all_bits;
        }
    }
    
    return all_bits;
}

// 字符串多数投票
std::string majority_vote_text(const std::vector<std::string>& payloads) {
    if (payloads.empty()) return "";
    std::map<std::string, int> counts;
    for (const auto& p : payloads) counts[p]++;
    std::string best;
    int best_count = 0;
    for (const auto& kv : counts) {
        if (kv.second > best_count) {
            best = kv.first;
            best_count = kv.second;
        }
    }
    return best;
}

} // anonymous namespace

// ========================================================================
// 公共 API
// ========================================================================

EmbedResult embed_text(const std::string& input_png,
                       const std::string& output_png,
                       const std::string& text,
                       uint64_t salt,
                       int ecc_level) {
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
    
    int num_channels = 3;
    auto grid = make_grid(png.width, png.height);
    size_t blocks_per_channel = grid.total_blocks();
    size_t total_blocks = blocks_per_channel * num_channels;
    result.capacity_bits = total_blocks;
    
    if (blocks_per_channel == 0) {
        result.error_msg = "image too small for watermark (need at least 3x3)";
        return result;
    }
    
    std::vector<uint8_t> bits;
    
    if (ecc_level == 0) {
        // 原始模式: 直接嵌入 [4字节长度][文本]
        auto payload = encode_text_payload(text);
        bits = bytes_to_bits(payload);
    } else {
        // ECC 模式: RS 编码
        int npar = rs_codec::ecc_level_to_npar(ecc_level);
        if (npar <= 0) {
            result.error_msg = "invalid ecc_level";
            return result;
        }
        
        // 1. 构建 payload: [4字节text_len大端][text_bytes]
        std::vector<uint8_t> payload;
        uint32_t text_len = (uint32_t)text.size();
        payload.resize(4);
        write_u32_be(payload.data(), text_len);
        payload.insert(payload.end(), text.begin(), text.end());
        
        // 2. RS 编码
        auto ecc_data = rs_codec::rs_encode(payload, npar);
        
        // 3. 构建 header: ecc_data_length 的4字节大端，重复3次 = 12字节
        uint32_t ecc_data_len = (uint32_t)ecc_data.size();
        std::vector<uint8_t> header(12);
        write_u32_be(&header[0], ecc_data_len);
        write_u32_be(&header[4], ecc_data_len);
        write_u32_be(&header[8], ecc_data_len);
        
        // 4. total_bytes = header + ecc_data
        std::vector<uint8_t> total_bytes;
        total_bytes.reserve(header.size() + ecc_data.size());
        total_bytes.insert(total_bytes.end(), header.begin(), header.end());
        total_bytes.insert(total_bytes.end(), ecc_data.begin(), ecc_data.end());
        
        // 5. 转为 bits
        bits = bytes_to_bits(total_bytes);
    }
    
    if (ecc_level > 0) {
        // Cell-major capacity: each cell = 3 blocks × 3 channels = 8 bits used
        size_t cell_cols = grid.grid_cols / 3;
        size_t total_cells = cell_cols * grid.grid_rows;
        size_t cell_capacity = total_cells * 8;
        if (bits.size() > cell_capacity) {
            result.error_msg = "text too large (" + std::to_string(bits.size()) + 
                              " bits needed, " + std::to_string(cell_capacity) + " available)";
            return result;
        }
        embed_bits_cell_major(png.data.data(), grid, bits, salt, png.has_alpha);
    } else {
        if (bits.size() > total_blocks) {
            result.error_msg = "text too large (" + std::to_string(bits.size()) + 
                              " bits needed, " + std::to_string(total_blocks) + " available)";
            return result;
        }
        embed_bits_to_image(png.data.data(), grid, bits, salt, png.has_alpha);
    }
    
    if (!save_png_adaptive(output_png, png.data, png.width, png.height, !png.has_alpha)) {
        result.error_msg = "failed to save PNG: " + output_png;
        return result;
    }
    
    result.success = true;
    result.used_bits = bits.size();
    return result;
}

ExtractResult extract_text(const std::string& stego_png,
                           uint64_t salt,
                           int ecc_level) {
    ExtractResult result;
    
    auto png = load_png_info(stego_png);
    if (png.width == 0) {
        result.error_msg = "failed to load PNG: " + stego_png;
        return result;
    }
    
    auto grid = make_grid(png.width, png.height);
    int num_channels = 3;
    size_t blocks_per_channel = grid.total_blocks();
    size_t total_blocks = blocks_per_channel * num_channels;
    
    if (ecc_level == 0) {
        // 原始模式: 提取 [4字节长度][文本]
        // 先提取前32位获取长度
        auto header_bits = extract_bits_from_image(png.data.data(), grid, 32, salt, png.has_alpha);
        if (header_bits.size() < 32) {
            result.error_msg = "not enough data for length header";
            return result;
        }
        
        auto header_bytes = bits_to_bytes(header_bits);
        uint32_t text_len = read_u32_be(header_bytes.data());
        size_t total_bits_needed = 32 + (size_t)text_len * 8;
        
        if (total_bits_needed > total_blocks) {
            result.error_msg = "claimed text length exceeds image capacity";
            return result;
        }
        
        auto all_bits = extract_bits_from_image(png.data.data(), grid, total_bits_needed, salt, png.has_alpha);
        auto all_bytes = bits_to_bytes(all_bits);
        
        if (!decode_text_payload(all_bytes.data(), all_bytes.size(), result.text)) {
            result.error_msg = "failed to decode watermark (wrong salt?)";
            return result;
        }
        
        result.success = true;
        return result;
    }
    
    // ECC 模式
    int npar = rs_codec::ecc_level_to_npar(ecc_level);
    if (npar <= 0) {
        result.error_msg = "invalid ecc_level";
        return result;
    }
    
    // 1. 先提取 12 字节 header (96 bits) - 使用列优先顺序
    std::vector<size_t> dummy_erasures;
    auto header_bits = extract_bits_cell_major_with_erasures(
        png.data.data(), grid, png.width, png.height,
        96, salt, png.has_alpha, &dummy_erasures, 0, 0);
    if (header_bits.size() < 96) {
        result.error_msg = "not enough data for ECC header";
        return result;
    }
    
    auto header_bytes = bits_to_bytes(header_bits);
    
    // 2. 对3份4字节做多数投票得到 ecc_data_len
    uint32_t len1 = read_u32_be(&header_bytes[0]);
    uint32_t len2 = read_u32_be(&header_bytes[4]);
    uint32_t len3 = read_u32_be(&header_bytes[8]);
    
    uint32_t ecc_data_len;
    if (len1 == len2 || len1 == len3) {
        ecc_data_len = len1;
    } else if (len2 == len3) {
        ecc_data_len = len2;
    } else {
        // 三者都不同，取第一个（无法投票）
        ecc_data_len = len1;
    }
    
    // 合理性检查
    if (ecc_data_len == 0 || ecc_data_len % 255 != 0) {
        result.error_msg = "invalid ECC data length (not multiple of 255)";
        return result;
    }
    
    // 3. 提取完整数据: 12字节 header + ecc_data_len 字节
    size_t total_bytes_needed = 12 + ecc_data_len;
    size_t total_bits_needed = total_bytes_needed * 8;
    
    if (total_bits_needed > total_blocks) {
        result.error_msg = "ECC data exceeds image capacity";
        return result;
    }
    
    std::vector<size_t> dummy_erasures2;
    auto all_bits = extract_bits_cell_major_with_erasures(
        png.data.data(), grid, png.width, png.height,
        total_bits_needed, salt, png.has_alpha, &dummy_erasures2, 0, 0);
    auto all_bytes = bits_to_bytes(all_bits);
    
    // 4. 提取 ecc_data (跳过12字节 header)
    if (all_bytes.size() < total_bytes_needed) {
        result.error_msg = "not enough data extracted";
        return result;
    }
    
    std::vector<uint8_t> ecc_data(all_bytes.begin() + 12, all_bytes.begin() + 12 + ecc_data_len);
    
    // 5. RS 解码
    auto payload = rs_codec::rs_decode(ecc_data, npar);
    if (payload.empty()) {
        result.error_msg = "RS decode failed (too many errors)";
        return result;
    }
    
    // 6. 解析 payload: [4字节text_len][text_bytes]
    if (payload.size() < 4) {
        result.error_msg = "decoded payload too short";
        return result;
    }
    
    uint32_t text_len = read_u32_be(payload.data());
    if (4 + (size_t)text_len > payload.size()) {
        result.error_msg = "text length mismatch in decoded payload";
        return result;
    }
    
    result.text.assign((const char*)(payload.data() + 4), text_len);
    result.success = true;
    return result;
}

ExtractResult extract_text_with_erasures(const std::string& stego_png,
                                          uint64_t salt,
                                          int ecc_level,
                                          uint32_t orig_width,
                                          uint32_t orig_height) {
    ExtractResult result;
    
    auto png = load_png_info(stego_png);
    if (png.width == 0) {
        result.error_msg = "failed to load PNG: " + stego_png;
        return result;
    }
    
    auto orig_grid = make_grid(orig_width, orig_height);
    int num_channels = 3;
    size_t blocks_per_channel = orig_grid.total_blocks();
    size_t total_blocks = blocks_per_channel * num_channels;
    
    int npar = rs_codec::ecc_level_to_npar(ecc_level);
    if (npar <= 0) {
        result.error_msg = "erasure extraction requires ECC level >= 1";
        return result;
    }
    
    // Calculate possible crop offset ranges
    int max_left_offset = (orig_width > png.width) ? (int)(orig_width - png.width) : 0;
    int max_top_offset = (orig_height > png.height) ? (int)(orig_height - png.height) : 0;
    
    // Lambda: try extraction at a specific offset
    auto try_extract = [&](int lo, int to) -> ExtractResult {
        ExtractResult r;
        
        // 1. Extract 96 bits header with erasure info (column-major)
        std::vector<size_t> header_erasure_positions;
        auto header_bits = extract_bits_cell_major_with_erasures(
            png.data.data(), orig_grid, png.width, png.height,
            96, salt, png.has_alpha, &header_erasure_positions, lo, to);
        
        if (header_bits.size() < 96) {
            r.error_msg = "not enough data for ECC header";
            return r;
        }
        
        std::vector<size_t> era_set(header_erasure_positions.begin(), header_erasure_positions.end());
        std::sort(era_set.begin(), era_set.end());
        
        auto is_erased = [&era_set](size_t pos) -> bool {
            return std::binary_search(era_set.begin(), era_set.end(), pos);
        };
        
        std::vector<uint8_t> voted_header_bytes(12, 0);
        for (int bit_offset = 0; bit_offset < 32; bit_offset++) {
            int votes[2] = {0, 0};
            for (int copy = 0; copy < 3; copy++) {
                size_t pos = (size_t)(copy * 32 + bit_offset);
                if (!is_erased(pos)) {
                    votes[header_bits[pos]]++;
                }
            }
            uint8_t voted_bit = (votes[1] > votes[0]) ? 1 : 0;
            int total_votes = votes[0] + votes[1];
            if (total_votes > 0) {
                for (int copy = 0; copy < 3; copy++) {
                    int byte_idx = copy * 4 + bit_offset / 8;
                    int bit_in_byte = 7 - (bit_offset % 8);
                    voted_header_bytes[byte_idx] |= (voted_bit << bit_in_byte);
                }
            }
        }
        
        auto header_bytes_raw = bits_to_bytes(header_bits);
        auto header_bytes = voted_header_bytes;
        
        uint32_t len1 = read_u32_be(&header_bytes[0]);
        uint32_t len2 = read_u32_be(&header_bytes[4]);
        uint32_t len3 = read_u32_be(&header_bytes[8]);
        
        uint32_t ecc_data_len;
        if (len1 == len2 || len1 == len3) ecc_data_len = len1;
        else if (len2 == len3) ecc_data_len = len2;
        else {
            len1 = read_u32_be(&header_bytes_raw[0]);
            len2 = read_u32_be(&header_bytes_raw[4]);
            len3 = read_u32_be(&header_bytes_raw[8]);
            if (len1 == len2 || len1 == len3) ecc_data_len = len1;
            else if (len2 == len3) ecc_data_len = len2;
            else ecc_data_len = len1;
        }
        
        if (ecc_data_len == 0 || ecc_data_len % 255 != 0) {
            size_t max_possible_bytes = total_blocks * 3 / 8;
            uint32_t best_len = 0;
            uint32_t min_diff = 0xFFFFFFFF;
            for (uint32_t candidate = 255; candidate <= max_possible_bytes && candidate < 100000; candidate += 255) {
                uint32_t diff = (candidate > ecc_data_len) ? (candidate - ecc_data_len) : (ecc_data_len - candidate);
                if (diff < min_diff) { min_diff = diff; best_len = candidate; }
            }
            if (best_len > 0 && min_diff < 255) ecc_data_len = best_len;
            else { r.error_msg = "invalid header"; return r; }
        }
        
        // 3. Extract full data
        size_t total_bytes_needed = 12 + ecc_data_len;
        size_t total_bits_needed = total_bytes_needed * 8;
        
        if (total_bits_needed > total_blocks) {
            r.error_msg = "ECC data exceeds capacity";
            return r;
        }
        
        std::vector<size_t> all_erasure_bit_positions;
        auto all_bits = extract_bits_cell_major_with_erasures(
            png.data.data(), orig_grid, png.width, png.height,
            total_bits_needed, salt, png.has_alpha, &all_erasure_bit_positions, lo, to);
        
        auto all_bytes = bits_to_bytes(all_bits);
        if (all_bytes.size() < total_bytes_needed) {
            r.error_msg = "not enough data";
            return r;
        }
        
        std::vector<uint8_t> ecc_data(all_bytes.begin() + 12, all_bytes.begin() + 12 + ecc_data_len);
        
        // 4. Bit-level erasures -> RS symbol-level erasures
        size_t num_rs_blocks = ecc_data_len / 255;
        std::vector<std::vector<int>> erasures_per_block(num_rs_blocks);
        
        for (size_t bit_pos : all_erasure_bit_positions) {
            if (bit_pos < 96) continue;
            size_t byte_offset = (bit_pos - 96) / 8;
            size_t rs_block_idx = byte_offset / 255;
            int symbol_idx = (int)(byte_offset % 255);
            if (rs_block_idx < num_rs_blocks) {
                auto& evec = erasures_per_block[rs_block_idx];
                if (std::find(evec.begin(), evec.end(), symbol_idx) == evec.end())
                    evec.push_back(symbol_idx);
            }
        }
        
        // 5. RS decode
        auto payload = rs_codec::rs_decode_with_erasures(ecc_data, npar, erasures_per_block);
        if (payload.empty()) {
            r.error_msg = "RS decode failed";
            return r;
        }
        
        // 6. Parse payload
        if (payload.size() < 4) { r.error_msg = "payload too short"; return r; }
        uint32_t text_len = read_u32_be(payload.data());
        if (4 + (size_t)text_len > payload.size()) { r.error_msg = "text len mismatch"; return r; }
        
        r.text.assign((const char*)(payload.data() + 4), text_len);
        r.success = true;
        return r;
    };
    
    // Scan all possible crop offsets
    std::string last_error;
    for (int to = 0; to <= max_top_offset; to++) {
        for (int lo = 0; lo <= max_left_offset; lo++) {
            auto r = try_extract(lo, to);
            if (r.success) {
                if (max_top_offset > 0 || max_left_offset > 0) {
                    std::cout << "  [INFO] Found at offset (left=" << lo << ", top=" << to << ")\n";
                }
                return r;
            }
            last_error = r.error_msg;
        }
    }
    
    int total_offsets = (max_top_offset + 1) * (max_left_offset + 1);
    result.error_msg = "failed across " + std::to_string(total_offsets) + 
                      " crop offsets (top: 0-" + std::to_string(max_top_offset) + 
                      ", left: 0-" + std::to_string(max_left_offset) + 
                      "). Try multicluster mode for better recovery.";
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

// ========================================================================
// 多簇冗余 API (v2.0)
// ========================================================================

EmbedResult embed_text_multicluster(const std::string& input_png,
                                      const std::string& output_png,
                                      const std::string& text,
                                      uint64_t salt,
                                      int ecc_level,
                                      int num_clusters) {
    EmbedResult result;
    
    if (text.empty()) {
        result.error_msg = "watermark text is empty";
        return result;
    }
    if (num_clusters < 1) num_clusters = 1;
    if (ecc_level < 1) ecc_level = 1; // multicluster requires ECC
    
    auto png = load_png_info(input_png);
    if (png.width == 0) {
        result.error_msg = "failed to load PNG: " + input_png;
        return result;
    }
    
    result.width = png.width;
    result.height = png.height;
    result.has_alpha = png.has_alpha;
    
    auto grid = make_grid(png.width, png.height);
    int num_channels = 3;
    
    if (grid.total_blocks() == 0 || grid.grid_rows < (uint32_t)num_clusters) {
        result.error_msg = "image too small for " + std::to_string(num_clusters) + " clusters";
        return result;
    }
    
    // Build RS-encoded data
    int npar = rs_codec::ecc_level_to_npar(ecc_level);
    
    std::vector<uint8_t> payload;
    uint32_t text_len = (uint32_t)text.size();
    payload.resize(4);
    write_u32_be(payload.data(), text_len);
    payload.insert(payload.end(), text.begin(), text.end());
    
    auto ecc_data = rs_codec::rs_encode(payload, npar);
    uint32_t ecc_data_len = (uint32_t)ecc_data.size();
    
    // Header: 3 copies of ecc_data_len = 12 bytes
    std::vector<uint8_t> header(12);
    write_u32_be(&header[0], ecc_data_len);
    write_u32_be(&header[4], ecc_data_len);
    write_u32_be(&header[8], ecc_data_len);
    
    // Cluster data = header + ecc_data
    std::vector<uint8_t> cluster_data;
    cluster_data.reserve(header.size() + ecc_data.size());
    cluster_data.insert(cluster_data.end(), header.begin(), header.end());
    cluster_data.insert(cluster_data.end(), ecc_data.begin(), ecc_data.end());
    
    auto bits = bytes_to_bits(cluster_data);
    
    // Check capacity per cluster (minimum strip determines limit)
    int strip_start, strip_end;
    get_strip_range(grid.grid_rows, num_clusters, 0, strip_start, strip_end);
    size_t min_strip_blocks = ((size_t)(strip_end - strip_start)) * grid.grid_cols;
    size_t min_strip_capacity = min_strip_blocks * num_channels;
    
    if (bits.size() > min_strip_capacity) {
        result.error_msg = "text too large for multicluster (" + 
                          std::to_string(bits.size()) + " bits needed per cluster, " +
                          std::to_string(min_strip_capacity) + " bits available in smallest strip)";
        return result;
    }
    
    result.capacity_bits = min_strip_capacity;
    
    // Embed into each cluster strip
    if (num_clusters == 1) {
        // Single cluster: use cell-major ordering for better left/right crop recovery
        embed_bits_cell_major(png.data.data(), grid, bits, salt, png.has_alpha);
    } else {
        for (int c = 0; c < num_clusters; c++) {
            get_strip_range(grid.grid_rows, num_clusters, c, strip_start, strip_end);
            uint64_t cluster_seed = salt * 10000 + c;
            
            embed_bits_to_strip(png.data.data(), grid, bits, cluster_seed,
                               png.has_alpha, strip_start, strip_end);
        }
    }
    
    result.used_bits = bits.size() * num_clusters;
    
    if (!save_png_adaptive(output_png, png.data, png.width, png.height, !png.has_alpha)) {
        result.error_msg = "failed to save PNG: " + output_png;
        return result;
    }
    
    result.success = true;
    return result;
}

ExtractResult extract_text_multicluster(const std::string& stego_png,
                                          uint64_t salt,
                                          int ecc_level,
                                          int num_clusters,
                                          uint32_t orig_width,
                                          uint32_t orig_height) {
    ExtractResult result;
    
    if (num_clusters < 1) num_clusters = 1;
    if (ecc_level < 1) {
        result.error_msg = "multicluster extraction requires ECC level >= 1";
        return result;
    }
    
    auto png = load_png_info(stego_png);
    if (png.width == 0) {
        result.error_msg = "failed to load PNG: " + stego_png;
        return result;
    }
    
    auto orig_grid = make_grid(orig_width, orig_height);
    int npar = rs_codec::ecc_level_to_npar(ecc_level);
    if (npar <= 0) {
        result.error_msg = "invalid ecc_level";
        return result;
    }
    
    // Single cluster: delegate to cell-major extraction (same as option 4)
    if (num_clusters == 1) {
        return extract_text_with_erasures(stego_png, salt, ecc_level, orig_width, orig_height);
    }
    
    // Calculate possible crop offset ranges
    // The screenshot is assumed to be a crop of the original image
    // We need to try all possible (left_offset, top_offset) values
    int max_left_offset = (orig_width > png.width) ? (int)(orig_width - png.width) : 0;
    int max_top_offset = (orig_height > png.height) ? (int)(orig_height - png.height) : 0;
    
    // Lambda: try decoding one cluster at a given offset
    // Returns decoded text, or empty string on failure
    auto try_cluster = [&](int c, int lo, int to) -> std::string {
        int strip_start, strip_end;
        get_strip_range(orig_grid.grid_rows, num_clusters, c, strip_start, strip_end);
        if (strip_start >= strip_end) return "";
        
        uint64_t cluster_seed = salt * 10000 + c;
        
        // 1. Extract 96-bit header with erasures
        std::vector<size_t> header_erasures;
        auto header_bits = extract_bits_from_strip_with_erasures(
            png.data.data(), orig_grid, png.width, png.height,
            96, cluster_seed, png.has_alpha,
            strip_start, strip_end, &header_erasures, lo, to);
        
        if (header_bits.size() < 96) return "";
        
        // 2. Parse header with bit-level majority voting across 3 copies
        std::vector<size_t> era_set = header_erasures;
        std::sort(era_set.begin(), era_set.end());
        
        auto is_erased = [&era_set](size_t pos) -> bool {
            return std::binary_search(era_set.begin(), era_set.end(), pos);
        };
        
        std::vector<uint8_t> voted_bytes(12, 0);
        for (int bit_offset = 0; bit_offset < 32; bit_offset++) {
            int votes[2] = {0, 0};
            for (int copy = 0; copy < 3; copy++) {
                size_t pos = (size_t)(copy * 32 + bit_offset);
                if (!is_erased(pos)) {
                    votes[header_bits[pos]]++;
                }
            }
            uint8_t voted_bit = (votes[1] > votes[0]) ? 1 : 0;
            int total_votes = votes[0] + votes[1];
            if (total_votes > 0) {
                for (int copy = 0; copy < 3; copy++) {
                    int byte_idx = copy * 4 + bit_offset / 8;
                    int bit_in_byte = 7 - (bit_offset % 8);
                    voted_bytes[byte_idx] |= (voted_bit << bit_in_byte);
                }
            }
        }
        
        uint32_t len1 = read_u32_be(&voted_bytes[0]);
        uint32_t len2 = read_u32_be(&voted_bytes[4]);
        uint32_t len3 = read_u32_be(&voted_bytes[8]);
        
        uint32_t ecc_data_len;
        if (len1 == len2 || len1 == len3) ecc_data_len = len1;
        else if (len2 == len3) ecc_data_len = len2;
        else ecc_data_len = len1;
        
        if (ecc_data_len == 0 || ecc_data_len % 255 != 0) {
            uint32_t best = 255;
            uint32_t min_diff = 0xFFFFFFFF;
            for (uint32_t cand = 255; cand <= 65535; cand += 255) {
                uint32_t d = (cand > ecc_data_len) ? (cand - ecc_data_len) : (ecc_data_len - cand);
                if (d < min_diff) { min_diff = d; best = cand; }
            }
            if (min_diff < 255) ecc_data_len = best;
            else return "";
        }
        
        // 3. Extract full data with erasures
        size_t total_bytes_needed = 12 + ecc_data_len;
        size_t total_bits_needed = total_bytes_needed * 8;
        
        std::vector<size_t> all_erasures;
        auto all_bits = extract_bits_from_strip_with_erasures(
            png.data.data(), orig_grid, png.width, png.height,
            total_bits_needed, cluster_seed, png.has_alpha,
            strip_start, strip_end, &all_erasures, lo, to);
        
        if (all_bits.size() < total_bits_needed) return "";
        
        auto all_bytes = bits_to_bytes(all_bits);
        if (all_bytes.size() < total_bytes_needed) return "";
        
        std::vector<uint8_t> ecc_data(all_bytes.begin() + 12,
                                       all_bytes.begin() + 12 + ecc_data_len);
        
        // 4. Convert bit-level erasures to RS symbol-level erasures
        size_t num_rs_blocks = ecc_data_len / 255;
        std::vector<std::vector<int>> erasures_per_block(num_rs_blocks);
        
        for (size_t bit_pos : all_erasures) {
            if (bit_pos < 96) continue;
            size_t byte_offset = (bit_pos - 96) / 8;
            size_t rs_block_idx = byte_offset / 255;
            int symbol_idx = (int)(byte_offset % 255);
            if (rs_block_idx < num_rs_blocks) {
                auto& evec = erasures_per_block[rs_block_idx];
                if (std::find(evec.begin(), evec.end(), symbol_idx) == evec.end()) {
                    evec.push_back(symbol_idx);
                }
            }
        }
        
        // 5. RS decode
        auto payload = rs_codec::rs_decode_with_erasures(ecc_data, npar, erasures_per_block);
        if (payload.empty()) {
            payload = rs_codec::rs_decode(ecc_data, npar);
        }
        if (payload.empty()) return "";
        
        // 6. Parse payload
        if (payload.size() < 4) return "";
        uint32_t decoded_text_len = read_u32_be(payload.data());
        if (4 + (size_t)decoded_text_len > payload.size()) return "";
        
        return std::string((const char*)(payload.data() + 4), decoded_text_len);
    };
    
    // Scan all possible crop offsets
    std::vector<std::string> success_payloads;
    int best_lo = 0, best_to = 0;
    
    for (int to = 0; to <= max_top_offset; to++) {
        for (int lo = 0; lo <= max_left_offset; lo++) {
            // Quick scan: try header-only check on first cluster to filter offsets fast
            // If the dimensions match (no crop), just try normally
            bool any_offset = (max_top_offset > 0 || max_left_offset > 0);
            
            if (any_offset) {
                // Fast header check: extract 96 bits from cluster 0 with this offset
                int strip_start, strip_end;
                get_strip_range(orig_grid.grid_rows, num_clusters, 0, strip_start, strip_end);
                if (strip_start >= strip_end) continue;
                
                uint64_t cluster_seed = salt * 10000 + 0;
                std::vector<size_t> quick_erasures;
                auto quick_bits = extract_bits_from_strip_with_erasures(
                    png.data.data(), orig_grid, png.width, png.height,
                    96, cluster_seed, png.has_alpha,
                    strip_start, strip_end, &quick_erasures, lo, to);
                
                if (quick_bits.size() < 96) continue;
                
                // Quick header validation
                int erased_count = 0;
                for (size_t pos : quick_erasures) {
                    if (pos < 96) erased_count++;
                }
                // If more than 60 header bits are erased, this offset is unlikely
                if (erased_count > 60) continue;
                
                // Check if header parses to a valid length
                std::vector<size_t> era_set(quick_erasures.begin(), quick_erasures.end());
                std::sort(era_set.begin(), era_set.end());
                auto is_era = [&era_set](size_t pos) -> bool {
                    return std::binary_search(era_set.begin(), era_set.end(), pos);
                };
                
                std::vector<uint8_t> qb(12, 0);
                for (int bo = 0; bo < 32; bo++) {
                    int vt[2] = {0, 0};
                    for (int cp = 0; cp < 3; cp++) {
                        size_t pos = (size_t)(cp * 32 + bo);
                        if (!is_era(pos)) vt[quick_bits[pos]]++;
                    }
                    uint8_t vb = (vt[1] > vt[0]) ? 1 : 0;
                    if (vt[0] + vt[1] > 0) {
                        for (int cp = 0; cp < 3; cp++) {
                            qb[cp * 4 + bo / 8] |= (vb << (7 - (bo % 8)));
                        }
                    }
                }
                uint32_t ql = read_u32_be(&qb[0]);
                if (ql == 0 || ql % 255 != 0) {
                    // Check if it's close to a valid value
                    bool close = false;
                    for (uint32_t cand = 255; cand <= 65535; cand += 255) {
                        if (cand > ql && cand - ql < 255) { close = true; break; }
                        if (ql > cand && ql - cand < 255) { close = true; break; }
                        if (cand == ql) { close = true; break; }
                    }
                    if (!close) continue;
                }
            }
            
            // Full decode: try all clusters at this offset
            bool found_at_offset = false;
            for (int c = 0; c < num_clusters; c++) {
                auto text = try_cluster(c, lo, to);
                if (!text.empty()) {
                    success_payloads.push_back(text);
                    best_lo = lo;
                    best_to = to;
                    found_at_offset = true;
                }
            }
            
            // If we found results at this offset, no need to try more offsets
            if (found_at_offset) {
                goto done_scanning;
            }
        }
    }
    done_scanning:
    
    if (success_payloads.empty()) {
        int total_offsets = (max_top_offset + 1) * (max_left_offset + 1);
        result.error_msg = "all clusters failed across " + std::to_string(total_offsets) + 
                          " crop offsets (top: 0-" + std::to_string(max_top_offset) + 
                          ", left: 0-" + std::to_string(max_left_offset) + ")";
        return result;
    }
    
    if (max_top_offset > 0 || max_left_offset > 0) {
        std::cout << "  [INFO] Found at offset (left=" << best_lo << ", top=" << best_to << ")\n";
    }
    
    // Majority vote
    result.text = majority_vote_text(success_payloads);
    result.success = true;
    return result;
}

CapacityInfo get_capacity_multicluster(const std::string& png_path, int num_clusters) {
    CapacityInfo info;
    
    auto png = load_png_info(png_path);
    if (png.width == 0) return info;
    
    info.width = png.width;
    info.height = png.height;
    info.has_alpha = png.has_alpha;
    info.total_pixels = (size_t)png.width * png.height;
    info.available_channels = 3;
    
    auto grid = make_grid(png.width, png.height);
    
    if (num_clusters < 1) num_clusters = 1;
    
    // Per-cluster capacity = minimum strip size
    int strip_start, strip_end;
    get_strip_range(grid.grid_rows, num_clusters, 0, strip_start, strip_end);
    size_t min_strip_blocks = ((size_t)(strip_end - strip_start)) * grid.grid_cols;
    
    info.blocks_per_channel = min_strip_blocks;
    
    size_t total_bits = min_strip_blocks * info.available_channels;
    // Header takes 96 bits
    size_t data_bits = total_bits > 96 ? total_bits - 96 : 0;
    info.max_text_bytes = data_bits / 8;
    info.max_text_chars = info.max_text_bytes / 3;
    
    return info;
}

} // namespace watermark
