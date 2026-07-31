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
// ecc_level:   ECC等级 (0=无纠错, 1-6=RS纠错等级, 越高纠错越强但开销越大)
EmbedResult embed_text(const std::string& input_png,
                       const std::string& output_png,
                       const std::string& text,
                       uint64_t salt,
                       int ecc_level = 0);

// 从隐写PNG中提取文本水印
// stego_png:   隐写PNG文件路径
// salt:        种子密钥(必须与嵌入时一致)
// ecc_level:   ECC等级 (必须与嵌入时一致)
ExtractResult extract_text(const std::string& stego_png,
                           uint64_t salt,
                           int ecc_level = 0);

// 擦除感知提取：用于截图攻击后的恢复
// orig_width/orig_height: 原始图像尺寸（嵌入时的尺寸）
// 提取时按原图的3×3块结构遍历，超出截图范围的块标记为擦除
ExtractResult extract_text_with_erasures(const std::string& stego_png,
                                          uint64_t salt,
                                          int ecc_level,
                                          uint32_t orig_width,
                                          uint32_t orig_height);

// 查询PNG图片可容纳的水印容量
CapacityInfo get_capacity(const std::string& png_path);

// ======== 多簇冗余嵌入/提取 (v2.0) ========
// 将图像垂直分为 num_clusters 个条带，每个条带独立嵌入完整的水印数据副本。
// 提取时逐簇尝试 RS 擦除解码，多数投票选最终结果。
// 适用于截图裁剪攻击：只要有一个簇完好即可恢复。

// 多簇冗余嵌入
EmbedResult embed_text_multicluster(const std::string& input_png,
                                      const std::string& output_png,
                                      const std::string& text,
                                      uint64_t salt,
                                      int ecc_level,
                                      int num_clusters);

// 多簇提取 + 多数投票
ExtractResult extract_text_multicluster(const std::string& stego_png,
                                          uint64_t salt,
                                          int ecc_level,
                                          int num_clusters,
                                          uint32_t orig_width,
                                          uint32_t orig_height);

// 查询多簇模式下每簇的容量
CapacityInfo get_capacity_multicluster(const std::string& png_path, int num_clusters);

} // namespace watermark

#endif // WATERMARK_H
