#ifndef WATERMARK_H
#define WATERMARK_H

#include <cstdint>
#include <string>
#include <vector>

namespace watermark {

// 嵌入结果
struct EmbedResult {
    bool success = false;
    std::string error_msg;
    size_t used_bits = 0;       // 实际使用的bit数
    size_t capacity_bits = 0;   // 图片总可用bit数
    uint32_t width = 0;
    uint32_t height = 0;
    bool has_alpha = false;
};

// 提取结果
struct ExtractResult {
    bool success = false;
    std::string error_msg;
    std::string text;           // 提取到的水印文本
};

// 容量信息
struct CapacityInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    bool has_alpha = false;
    size_t total_pixels = 0;
    size_t available_channels = 0;  // 可用通道数 (3 or 4)
    size_t blocks_per_channel = 0;  // 每通道可用块数
    size_t max_text_bytes = 0;      // 最大可嵌入字节数
    size_t max_text_chars = 0;      // 大致可嵌入字符数(UTF-8)
};

// 嵌入文本水印到PNG图片
// input_png:   输入PNG文件路径
// output_png:  输出隐写PNG文件路径
// text:        要隐藏的水印文本(UTF-8)
// salt:        种子密钥(PRNG种子, 嵌入和提取必须一致)
EmbedResult embed_text(const std::string& input_png,
                       const std::string& output_png,
                       const std::string& text,
                       uint64_t salt);

// 从隐写PNG中提取文本水印
// stego_png:   隐写PNG文件路径
// salt:        种子密钥(必须与嵌入时一致)
ExtractResult extract_text(const std::string& stego_png,
                           uint64_t salt);

// 查询PNG图片可容纳的水印容量
CapacityInfo get_capacity(const std::string& png_path);

} // namespace watermark

#endif // WATERMARK_H
