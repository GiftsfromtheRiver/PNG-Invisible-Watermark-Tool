// ================================================================
// PNG 不可见水印工具
// 功能: 嵌入/提取/查询容量 / 批量文件夹 / 快速示例 / ECC纠错
// 技术: ±1 LSB Matching + 3×3 块标记 + Reed-Solomon ECC
// ================================================================

#include "watermark.h"
#include "rs_codec.h"
#include "lodepng.h"
#include <iostream>
#include <string>
#include <sstream>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#endif

using namespace watermark;

// ====== 全局演示状态 ======
static int demo_step = 0;
static std::string demo_test_img;
static std::string demo_stego_img;
static uint64_t demo_salt = 114514;
static std::string demo_text = "© 2026 MyArtStudio · 本图片受不可见水印保护";

// ====== 延迟函数 ======
static void wait_seconds(int seconds) {
#ifdef _WIN32
    Sleep(seconds * 1000);
#else
    sleep(seconds);
#endif
}

// ====== 路径辅助函数 ======

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string strip_quotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

static std::string read_line(const std::string& prompt) {
    std::cout << prompt;
    std::cout.flush();
    std::string line;
    std::getline(std::cin, line);
    return trim(line);
}

static std::string read_path(const std::string& prompt) {
    return strip_quotes(read_line(prompt));
}

static uint64_t parse_salt(const std::string& s) {
    if (s.empty()) return 0;
    try {
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            return std::stoull(s.substr(2), nullptr, 16);
        }
        return std::stoull(s);
    } catch (...) {
        uint64_t h = 14695981039346656037ULL;
        for (char c : s) {
            h ^= (uint8_t)c;
            h *= 1099511628211ULL;
        }
        return h;
    }
}

// ====== ECC 等级辅助函数 ======

static int read_ecc_level() {
    std::cout << "\n  ECC 纠错等级:\n";
    std::cout << "    0 - 无纠错 (默认, 容量最大)\n";
    std::cout << "    1 - 低纠错 (修正4字节错误, 开销8字节)\n";
    std::cout << "    2 - 中低纠错 (修正8字节错误, 开销16字节)\n";
    std::cout << "    3 - 中纠错 (修正12字节错误, 开销24字节)\n";
    std::cout << "    4 - 中高纠错 (修正16字节错误, 开销32字节)\n";
    std::cout << "    5 - 高纠错 (修正24字节错误, 开销48字节)\n";
    std::cout << "    6 - 超高纠错 (修正32字节错误, 开销64字节)\n";
    std::string level_str = read_line("  选择 ECC 等级 (0-6, 留空=0): ");
    if (level_str.empty()) return 0;
    try {
        int level = std::stoi(level_str);
        if (level < 0 || level > 6) {
            std::cout << "  无效等级，使用默认 0\n";
            return 0;
        }
        return level;
    } catch (...) {
        std::cout << "  无效输入，使用默认 0\n";
        return 0;
    }
}

static void show_ecc_info(int ecc_level) {
    if (ecc_level == 0) return;
    int npar = rs_codec::ecc_level_to_npar(ecc_level);
    int k = 255 - npar;
    int max_errors = npar / 2;
    std::cout << "\n  📋 ECC 信息:\n";
    std::cout << "    等级: " << ecc_level << " (" << rs_codec::ecc_level_name(ecc_level) << ")\n";
    std::cout << "    RS(255," << k << ") - 每255字节块可修正 " << max_errors << " 字节错误\n";
    std::cout << "    适用于截图攻击等场景下的水印恢复\n";
}

// ====== 文件/路径辅助函数 ======

static bool is_directory(const std::string& path) {
#ifdef _WIN32
    int len = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (len <= 0) {
        len = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
        if (len <= 0) return false;
    }
    std::wstring wpath(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], len);
    while (!wpath.empty() && wpath.back() == L'\0') wpath.pop_back();
    
    DWORD attr = GetFileAttributesW(wpath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
#endif
}

static bool file_exists(const std::string& path) {
#ifdef _WIN32
    int len = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (len <= 0) return false;
    std::wstring wpath(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], len);
    while (!wpath.empty() && wpath.back() == L'\0') wpath.pop_back();
    DWORD attr = GetFileAttributesW(wpath.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

static bool has_ext(const std::string& fname, const std::string& ext) {
    if (fname.size() < ext.size()) return false;
    size_t pos = fname.size() - ext.size();
    if (ext[0] != '.') return false;
    for (size_t i = 0; i < ext.size(); i++) {
        if (std::tolower(fname[pos + i]) != std::tolower(ext[i])) return false;
    }
    return true;
}

static std::string detect_file_signature(const std::string& path) {
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return "";
    std::wstring wpath(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], wlen);
    while (!wpath.empty() && wpath.back() == L'\0') wpath.pop_back();
    FILE* f = _wfopen(wpath.c_str(), L"rb");
#else
    FILE* f = fopen(path.c_str(), "rb");
#endif
    if (!f) return "";
    uint8_t header[8] = {0};
    size_t n = fread(header, 1, 8, f);
    fclose(f);
    if (n == 0) return "";

    std::string hex;
    for (size_t i = 0; i < n; i++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02X ", header[i]);
        hex += buf;
    }
    if (!hex.empty() && hex.back() == ' ') hex.pop_back();

    if (n >= 8 && header[0]==0x89 && header[1]==0x50 && header[2]==0x4E && header[3]==0x47
        && header[4]==0x0D && header[5]==0x0A && header[6]==0x1A && header[7]==0x0A)
        return "PNG (文件头: 89 50 4E 47 0D 0A 1A 0A)";
    if (n >= 2 && header[0]==0xFF && header[1]==0xD8)
        return "JPEG (文件头: FF D8)";
    if (n >= 4 && header[0]==0x47 && header[1]==0x49 && header[2]==0x46 && header[3]==0x38)
        return "GIF (文件头: 47 49 46 38)";
    if (n >= 4 && header[0]==0x52 && header[1]==0x49 && header[2]==0x46 && header[3]==0x46)
        return "RIFF/WebP (文件头: 52 49 46 46)";
    if (n >= 2 && header[0]==0x42 && header[1]==0x4D)
        return "BMP (文件头: 42 4D)";
    if (n >= 4 && header[0]==0x25 && header[1]==0x50 && header[2]==0x44 && header[3]==0x46)
        return "PDF (文件头: 25 50 44 46)";

    return "未知格式 (文件头: " + hex + ")";
}

static bool is_png_file(const std::string& path) {
    std::string sig = detect_file_signature(path);
    return sig.find("PNG") != std::string::npos;
}

static std::vector<std::string> list_png_files(const std::string& dir) {
    std::vector<std::string> files;
#ifdef _WIN32
    int len = MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0);
    if (len <= 0) return files;
    std::wstring wdir(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, &wdir[0], len);
    while (!wdir.empty() && wdir.back() == L'\0') wdir.pop_back();
    
    std::wstring wpattern = wdir + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return files;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            std::wstring wname(fd.cFileName);
            int len2 = WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (len2 > 0) {
                std::string name(len2 - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), -1, &name[0], len2, nullptr, nullptr);
                if (has_ext(name, ".png")) files.push_back(dir + "/" + name);
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return files;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (has_ext(name, ".png")) files.push_back(dir + "/" + name);
    }
    closedir(d);
#endif

    // 通过文件头魔数验证 PNG 格式 (89 50 4E 47 0D 0A 1A 0A)
    static const uint8_t png_sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::string> validated;
    std::vector<std::string> skipped;
    for (const auto& full_path : files) {
#ifdef _WIN32
        int wlen = MultiByteToWideChar(CP_UTF8, 0, full_path.c_str(), -1, nullptr, 0);
        if (wlen <= 0) { skipped.push_back(full_path); continue; }
        std::wstring wpath(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, full_path.c_str(), -1, &wpath[0], wlen);
        while (!wpath.empty() && wpath.back() == L'\0') wpath.pop_back();
        FILE* f = _wfopen(wpath.c_str(), L"rb");
#else
        FILE* f = fopen(full_path.c_str(), "rb");
#endif
        if (!f) continue;  // 无法打开则跳过，不计入警告

        uint8_t header[8] = {0};
        size_t nread = fread(header, 1, 8, f);
        fclose(f);

        if (nread == 8 && memcmp(header, png_sig, 8) == 0) {
            validated.push_back(full_path);
        } else {
            skipped.push_back(full_path);
        }
    }

    if (!skipped.empty()) {
        std::cout << "\n[警告] 以下 " << skipped.size() << " 个文件扩展名为 .png 但不是有效的 PNG 格式，已跳过：\n";
        for (const auto& sf : skipped) {
            std::string sig = detect_file_signature(sf);
            std::cout << "  - " << sf << " (实际: " << (sig.empty() ? "无法读取" : sig) << ")\n";
        }
    }

    files = std::move(validated);
    std::sort(files.begin(), files.end());
    return files;
}

static std::string auto_output(const std::string& input) {
    size_t dot = input.rfind('.');
    if (dot == std::string::npos) return input + "_steg.png";
    return input.substr(0, dot) + "_steg.png";
}

static std::string get_desktop_path() {
#ifdef _WIN32
    char* userprofile = getenv("USERPROFILE");
    if (userprofile) return std::string(userprofile) + "\\Desktop";
    return "";
#else
    return "";
#endif
}

// ====== 生成测试图片 ======
static std::string generate_test_image(const std::string& output_path) {
    const int W = 512, H = 384;
    std::vector<uint8_t> image(W * H * 4);
    
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            size_t idx = (y * W + x) * 4;
            float fx = (float)x / W;
            float fy = (float)y / H;
            float r = 0.5f + 0.5f * sin(fx * 3.0f + fy * 2.0f + 0.0f);
            float g = 0.5f + 0.5f * sin(fx * 2.5f + fy * 3.0f + 2.0f);
            float b = 0.5f + 0.5f * sin(fx * 3.5f + fy * 1.5f + 4.0f);
            image[idx + 0] = (uint8_t)(r * 230 + 25);
            image[idx + 1] = (uint8_t)(g * 230 + 25);
            image[idx + 2] = (uint8_t)(b * 230 + 25);
            image[idx + 3] = 255;
        }
    }
    
    unsigned error = lodepng::encode(output_path, image, W, H, LCT_RGBA, 8);
    if (error != 0) return "";
    return output_path;
}

static bool wait_for_file(const std::string& path, int max_wait_ms = 3000) {
    int waited = 0, step = 50;
    while (!file_exists(path) && waited < max_wait_ms) {
#ifdef _WIN32
        Sleep(step);
#else
        usleep(step * 1000);
#endif
        waited += step;
    }
    return file_exists(path);
}

// ====== 嵌入水印 ======
static void do_embed() {
    std::cout << "\n===== 嵌入不可见水印 =====\n\n";
    
    std::string input = read_path("输入PNG路径 (文件或文件夹): ");
    if (input.empty()) { std::cout << "已取消\n"; return; }
    
    // 选择 ECC 等级
    int ecc_level = read_ecc_level();
    show_ecc_info(ecc_level);
    
    // 多簇冗余副本数
    std::string nc_str = read_line("\n  多簇冗余副本数 (1-20, 留空=1, 建议配合ECC使用): ");
    int num_clusters = 1;
    if (!nc_str.empty()) {
        try { num_clusters = std::stoi(nc_str); } catch (...) {}
        if (num_clusters < 1) num_clusters = 1;
        if (num_clusters > 20) num_clusters = 20;
    }
    if (num_clusters > 1) {
        if (ecc_level < 1) ecc_level = 1; // multicluster requires ECC
        std::cout << "  多簇模式: " << num_clusters << " 个冗余副本 (ECC已自动调整为level " << ecc_level << ")\n";
    }
    
    if (is_directory(input)) {
        std::vector<std::string> files = list_png_files(input);
        if (files.empty()) {
            std::cout << "\n[WARN] 在 " << input << " 中未找到 PNG 文件\n";
            return;
        }
        std::cout << "\n  找到 " << files.size() << " 个 PNG 文件\n";
        
        std::string salt_str = read_line("种子密钥 (数字/字符串, 留空=0): ");
        uint64_t salt = parse_salt(salt_str);
        
        std::cout << "\n请输入要隐藏的水印文本（所有图片使用相同文本）:\n";
        std::cout << "  输入完成后在新行输入 :END 结束\n\n";
        
        std::string text;
        std::string line;
        while (std::getline(std::cin, line)) {
            if (trim(line) == ":END") break;
            if (!text.empty()) text += "\n";
            text += line;
        }
        
        if (text.empty()) { std::cout << "水印文本为空, 已取消\n"; return; }
        
        std::cout << "\n--- 批量嵌入中 (ECC level=" << ecc_level;
        if (num_clusters > 1) std::cout << ", clusters=" << num_clusters;
        std::cout << ") ---\n";
        int ok = 0, fail_count = 0;
        for (size_t i = 0; i < files.size(); i++) {
            const std::string& f = files[i];
            std::string out = auto_output(f);
            
            std::cout << "  [" << (i+1) << "/" << files.size() << "] " << f << " ... ";
            std::cout.flush();
            
            EmbedResult result;
            if (num_clusters > 1) {
                result = embed_text_multicluster(f, out, text, salt, ecc_level, num_clusters);
            } else {
                result = embed_text(f, out, text, salt, ecc_level);
            }
            if (result.success) {
                std::cout << "OK (" << result.used_bits << " bits)\n";
                ok++;
            } else {
                std::cout << "FAIL: " << result.error_msg << "\n";
                fail_count++;
            }
        }
        std::cout << "\n--- 批量完成 ---\n";
        std::cout << "  成功: " << ok << "  失败: " << fail_count << "\n";
        return;
    }
    
    // 单文件模式
    if (!is_png_file(input)) {
        std::string sig = detect_file_signature(input);
        std::cout << "\n[错误] 该文件不是有效的 PNG 格式" << (sig.empty() ? "" : "，实际为: " + sig) << "\n";
        return;
    }
    
    std::string output = read_path("输出PNG路径 (留空=自动命名): ");
    if (output.empty()) {
        output = auto_output(input);
        std::cout << "  -> " << output << "\n";
    }
    
    std::string salt_str = read_line("种子密钥 (数字/字符串, 留空=0): ");
    uint64_t salt = parse_salt(salt_str);
    
    std::cout << "\n请输入要隐藏的水印文本（支持多行）:\n";
    std::cout << "  输入完成后在新行输入 :END 结束\n\n";
    
    std::string text;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (trim(line) == ":END") break;
        if (!text.empty()) text += "\n";
        text += line;
    }
    
    if (text.empty()) { std::cout << "水印文本为空, 已取消\n"; return; }
    
    std::cout << "\n正在嵌入 (ECC level=" << ecc_level;
    if (num_clusters > 1) std::cout << ", clusters=" << num_clusters;
    std::cout << ")...\n";
    
    EmbedResult result;
    if (num_clusters > 1) {
        result = embed_text_multicluster(input, output, text, salt, ecc_level, num_clusters);
    } else {
        result = embed_text(input, output, text, salt, ecc_level);
    }
    
    if (result.success) {
        std::cout << "\n[OK] 水印嵌入成功!\n";
        std::cout << "  图片尺寸: " << result.width << " x " << result.height << "\n";
        std::cout << "  通道模式: " << (result.has_alpha ? "RGBA (4通道)" : "RGB (3通道)") << "\n";
        std::cout << "  ECC等级:  " << ecc_level;
        if (ecc_level > 0) {
            std::cout << " (" << rs_codec::ecc_level_name(ecc_level) << ")";
        } else {
            std::cout << " (无纠错)";
        }
        std::cout << "\n";
        if (num_clusters > 1) {
            std::cout << "  多簇冗余: " << num_clusters << " 个副本\n";
        }
        std::cout << "  嵌入数据: " << result.used_bits << " bits (" 
                  << text.size() << " 字节文本)\n";
        std::cout << "  每簇容量: " << result.capacity_bits << " bits\n";
        std::cout << "  输出文件: " << output << "\n";
    } else {
        std::cout << "\n[FAIL] " << result.error_msg << "\n";
    }
}

// ====== 提取水印 ======
static void do_extract() {
    std::cout << "\n===== 提取不可见水印 =====\n\n";
    
    std::string input = read_path("隐写PNG路径: ");
    if (input.empty()) { std::cout << "已取消\n"; return; }
    
    if (!is_png_file(input)) {
        std::string sig = detect_file_signature(input);
        std::cout << "\n[错误] 该文件不是有效的 PNG 格式" << (sig.empty() ? "" : "，实际为: " + sig) << "\n";
        return;
    }
    
    std::string salt_str = read_line("种子密钥 (需与嵌入时一致): ");
    uint64_t salt = parse_salt(salt_str);
    
    // 选择 ECC 等级
    int ecc_level = read_ecc_level();
    
    std::cout << "\n正在提取 (ECC level=" << ecc_level << ")...\n";
    auto result = extract_text(input, salt, ecc_level);
    
    if (result.success) {
        std::cout << "\n[OK] 水印提取成功！\n";
        std::cout << "以下就是从图片中提取到的隐藏水印内容：\n";
        std::cout << "\n┌─────────────────────────────────────────────────────┐\n";
        std::cout << "│  " << result.text << "\n";
        std::cout << "└─────────────────────────────────────────────────────┘\n";
        std::cout << "\n如果看到的是乱码或无意义内容，请检查密钥和ECC等级是否正确。\n";
    } else {
        std::cout << "\n[FAIL] " << result.error_msg << "\n";
    }
}

// ====== 擦除提取（截图攻击恢复） ======
static void do_extract_with_erasures() {
    std::cout << "\n===== 提取水印（带擦除恢复 - 截图攻击） =====\n\n";
    std::cout << "  适用于截图裁剪后的图片，利用原图尺寸信息恢复水印。\n";
    std::cout << "  需要知道原始图片的尺寸（嵌入水印时的尺寸）。\n\n";
    
    std::string input = read_path("截图PNG路径: ");
    if (input.empty()) { std::cout << "已取消\n"; return; }
    
    if (!is_png_file(input)) {
        std::string sig = detect_file_signature(input);
        std::cout << "\n[错误] 该文件不是有效的 PNG 格式" << (sig.empty() ? "" : "，实际为: " + sig) << "\n";
        return;
    }
    
    std::string ow_str = read_line("原始图片宽度 (像素): ");
    std::string oh_str = read_line("原始图片高度 (像素): ");
    if (ow_str.empty() || oh_str.empty()) { std::cout << "已取消\n"; return; }
    
    uint32_t orig_w = 0, orig_h = 0;
    try {
        orig_w = std::stoul(ow_str);
        orig_h = std::stoul(oh_str);
    } catch (...) {
        std::cout << "无效的尺寸输入\n";
        return;
    }
    
    if (orig_w < 3 || orig_h < 3) {
        std::cout << "尺寸太小\n";
        return;
    }
    
    std::string salt_str = read_line("种子密钥 (需与嵌入时一致): ");
    uint64_t salt = parse_salt(salt_str);
    
    std::cout << "\n  ECC 纠错等级 (需与嵌入时一致):\n";
    std::string ecc_str = read_line("  ECC等级 (1-6): ");
    int ecc_level = 2;
    try { ecc_level = std::stoi(ecc_str); } catch (...) {}
    if (ecc_level < 1 || ecc_level > 6) {
        std::cout << "无效等级，使用默认 2\n";
        ecc_level = 2;
    }
    
    std::cout << "\n正在提取 (ECC level=" << ecc_level 
              << ", 原图=" << orig_w << "x" << orig_h << ")...\n";
    
    auto result = extract_text_with_erasures(input, salt, ecc_level, orig_w, orig_h);
    
    if (result.success) {
        std::cout << "\n[OK] 擦除恢复提取成功！\n";
        std::cout << "以下就是从截图中恢复的隐藏水印内容：\n";
        std::cout << "\n┌─────────────────────────────────────────────────────┐\n";
        std::cout << "│  " << result.text << "\n";
        std::cout << "└─────────────────────────────────────────────────────┘\n";
    } else {
        std::cout << "\n[FAIL] " << result.error_msg << "\n";
        std::cout << "  提示：擦除数可能超过纠错能力，尝试提高 ECC 等级重新嵌入。\n";
    }
}

// ====== 多簇提取（截图攻击恢复） ======
static void do_extract_multicluster() {
    std::cout << "\n===== 提取水印（多簇 + 擦除恢复） =====\n\n";
    std::cout << "  适用于多簇冗余嵌入后的截图裁剪恢复。\n";
    std::cout << "  每个簇独立尝试 RS 擦除解码，多数投票选最终结果。\n\n";
    
    std::string input = read_path("截图PNG路径: ");
    if (input.empty()) { std::cout << "已取消\n"; return; }
    
    if (!is_png_file(input)) {
        std::string sig = detect_file_signature(input);
        std::cout << "\n[错误] 该文件不是有效的 PNG 格式" << (sig.empty() ? "" : "，实际为: " + sig) << "\n";
        return;
    }
    
    std::string ow_str = read_line("原始图片宽度 (像素): ");
    std::string oh_str = read_line("原始图片高度 (像素): ");
    if (ow_str.empty() || oh_str.empty()) { std::cout << "已取消\n"; return; }
    
    uint32_t orig_w = 0, orig_h = 0;
    try {
        orig_w = std::stoul(ow_str);
        orig_h = std::stoul(oh_str);
    } catch (...) {
        std::cout << "无效的尺寸输入\n";
        return;
    }
    
    if (orig_w < 3 || orig_h < 3) {
        std::cout << "尺寸太小\n";
        return;
    }
    
    std::string salt_str = read_line("种子密钥 (需与嵌入时一致): ");
    uint64_t salt = parse_salt(salt_str);
    
    std::string ecc_str = read_line("ECC等级 (1-6): ");
    int ecc_level = 3;
    try { ecc_level = std::stoi(ecc_str); } catch (...) {}
    if (ecc_level < 1 || ecc_level > 6) {
        std::cout << "无效等级，使用默认 3\n";
        ecc_level = 3;
    }
    
    std::string nc_str = read_line("簇数 (1-20, 留空=10): ");
    int num_clusters = 10;
    if (!nc_str.empty()) {
        try { num_clusters = std::stoi(nc_str); } catch (...) {}
    }
    if (num_clusters < 1) num_clusters = 1;
    if (num_clusters > 20) num_clusters = 20;
    
    std::cout << "\n正在提取 (ECC=" << ecc_level 
              << ", clusters=" << num_clusters
              << ", 原图=" << orig_w << "x" << orig_h << ")...\n";
    
    auto result = extract_text_multicluster(input, salt, ecc_level, num_clusters, orig_w, orig_h);
    
    if (result.success) {
        std::cout << "\n[OK] 多簇恢复提取成功！\n";
        std::cout << "以下就是从截图中恢复的隐藏水印内容：\n";
        std::cout << "\n┌─────────────────────────────────────────────────────┐\n";
        std::cout << "│  " << result.text << "\n";
        std::cout << "└─────────────────────────────────────────────────────┘\n";
    } else {
        std::cout << "\n[FAIL] " << result.error_msg << "\n";
        std::cout << "  提示：尝试增加簇数或提高 ECC 等级重新嵌入。\n";
    }
}

// ====== 查询容量 ======
static void do_capacity() {
    std::cout << "\n===== 查询水印容量 =====\n\n";
    
    std::string input = read_path("PNG图片路径: ");
    if (input.empty()) { std::cout << "已取消\n"; return; }
    
    if (!is_png_file(input)) {
        std::string sig = detect_file_signature(input);
        std::cout << "\n[错误] 该文件不是有效的 PNG 格式" << (sig.empty() ? "" : "，实际为: " + sig) << "\n";
        return;
    }
    
    auto info = get_capacity(input);
    
    if (info.width == 0) {
        std::cout << "\n[FAIL] 无法读取图片: " << input << "\n";
        return;
    }
    
    std::cout << "\n  图片尺寸: " << info.width << " x " << info.height << "\n";
    std::cout << "  总像素数: " << info.total_pixels << "\n";
    std::cout << "  通道模式: " << (info.has_alpha ? "RGBA (4通道)" : "RGB (3通道)") << "\n";
    std::cout << "  可用通道: " << info.available_channels << "\n";
    std::cout << "  每通道块数: " << info.blocks_per_channel << "\n";
    std::cout << "  总可用块数: " << (info.blocks_per_channel * info.available_channels) << "\n";
    std::cout << "  最大水印: " << info.max_text_bytes << " 字节 (无ECC)\n";
    std::cout << "  约可嵌入: " << info.max_text_chars << " 个中文字符\n";
    
    // 显示各 ECC 等级的有效容量
    std::cout << "\n  📊 各 ECC 等级有效容量:\n";
    size_t total_bits = info.blocks_per_channel * info.available_channels;
    for (int level = 0; level <= 6; level++) {
        int npar = rs_codec::ecc_level_to_npar(level);
        if (level == 0) {
            size_t text_bytes = (total_bits - 32) / 8;
            std::cout << "    Level 0 (无ECC): " << text_bytes << " bytes\n";
        } else {
            int k = 255 - npar;
            // header = 12 bytes (96 bits), remaining bits for RS data
            size_t rs_bits = total_bits - 96;
            size_t rs_bytes_total = rs_bits / 8;
            size_t num_blocks = rs_bytes_total / 255;
            size_t effective_data = num_blocks * k;
            size_t text_bytes = effective_data > 4 ? effective_data - 4 : 0;
            int max_errors = npar / 2;
            std::cout << "    Level " << level << " (修正" << max_errors << "字节/块): "
                      << text_bytes << " bytes\n";
        }
    }
}

// ====== 快速示例 ======
static void do_demo_step1() {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    🧪 快速示例                          ║\n";
    std::cout << "║           跟着我一步步操作，5 分钟学会！               ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n";
    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "  📌 步骤 1/4：生成测试图片\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
    
    std::string desktop = get_desktop_path();
    if (desktop.empty()) desktop = ".";
    demo_test_img = desktop + "/demo_test.png";
    demo_stego_img = desktop + "/demo_test_steg.png";
    
    std::cout << "  正在生成...\n";
    std::string result = generate_test_image(demo_test_img);
    if (result.empty()) {
        std::cout << "  ❌ 生成失败！\n";
        demo_step = 0;
        return;
    }
    std::cout << "  ✅ 已生成: " << demo_test_img << "\n\n";
    std::cout << "  👉 按 Enter 继续下一步...";
    std::cin.get();
    demo_step = 1;
}

static void do_demo_step2() {
    std::cout << "\n\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "  📌 步骤 2/4：嵌入水印\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
    std::cout << "  请在菜单中选择「1. 嵌入水印」操作:\n\n";
    std::cout << "  ┌─────────────────────────────────────────────────────────┐\n";
    std::cout << "  │  ① 输入图片路径: " << demo_test_img << "\n";
    std::cout << "  │  ② ECC等级: 0 (演示用无ECC)                         │\n";
    std::cout << "  │  ③ 输出路径: 直接 Enter (自动命名)                  │\n";
    std::cout << "  │  ④ 密钥: 114514                                     │\n";
    std::cout << "  │  ⑤ 水印: " << demo_text << "\n";
    std::cout << "  │  ⑥ 新行输入 :END 结束                               │\n";
    std::cout << "  └─────────────────────────────────────────────────────────┘\n\n";
    std::cout << "  👉 按 Enter 开始操作...";
    std::cin.get();
}

static void do_demo_step3() {
    if (!file_exists(demo_stego_img)) {
        std::cout << "\n\n  ⚠️ 还没有完成嵌入步骤！请先选择「1. 嵌入水印」\n";
        std::cout << "  👉 按 Enter 返回...";
        std::cin.get();
        demo_step = 1;
        return;
    }
    
    std::cout << "\n\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "  📌 步骤 3/4：提取验证\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
    std::cout << "  请在菜单中选择「2. 提取水印」:\n\n";
    std::cout << "  ┌─────────────────────────────────────────────────────────┐\n";
    std::cout << "  │  ① 输入隐写图片路径: " << demo_stego_img << "\n";
    std::cout << "  │  ② 密钥: 114514                                     │\n";
    std::cout << "  │  ③ ECC等级: 0                                       │\n";
    std::cout << "  └─────────────────────────────────────────────────────────┘\n\n";
    std::cout << "  👉 按 Enter 开始操作...";
    std::cin.get();
}

static void do_demo_step4() {
    std::cout << "\n\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "  📌 步骤 4/4：🎉 验证结果\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
    
    if (!file_exists(demo_stego_img)) {
        std::cout << "  ❌ 没有找到隐写图片\n";
        std::cout << "  👉 按 Enter 返回...";
        std::cin.get();
        demo_step = 1;
        return;
    }
    
    std::cout << "  正在自动提取水印验证...\n";
    auto result = extract_text(demo_stego_img, demo_salt);
    
    if (result.success && result.text == demo_text) {
        std::cout << "\n  ┌─────────────────────────────────────────────────────┐\n";
        std::cout << "  │  ✅ 提取到的水印: " << result.text << "\n";
        std::cout << "  └─────────────────────────────────────────────────────┘\n";
        std::cout << "\n  🎉 恭喜！水印嵌入和提取成功！\n";
        demo_step = 4;
    } else {
        std::cout << "\n  ❌ 提取失败或内容不匹配\n";
        demo_step = 3;
    }
    
    std::cout << "\n  👉 按 Enter 继续...";
    std::cin.get();
}

static void do_demo() {
    bool has_test_img = file_exists(demo_test_img);
    bool has_stego_img = file_exists(demo_stego_img);
    
    if (has_test_img && has_stego_img && demo_step == 1) demo_step = 2;
    if (has_stego_img && !has_test_img) { demo_step = 0; }
    if (!has_test_img && !has_stego_img && demo_step > 0) demo_step = 0;
    
    if (demo_step == 0) do_demo_step1();
    else if (demo_step == 1) do_demo_step2();
    else if (demo_step == 2) {
        if (!file_exists(demo_stego_img)) {
            std::cout << "\n  ⚠️ 隐写图片未找到，请先嵌入\n";
            std::cout << "  👉 按 Enter 返回...";
            std::cin.get();
            demo_step = 1;
        } else {
            do_demo_step3();
        }
    } else if (demo_step == 3 || demo_step == 4) do_demo_step4();
    else { demo_step = 0; do_demo_step1(); }
}

static void reset_demo() {
    demo_step = 0;
    demo_test_img = "";
    demo_stego_img = "";
}

// ====== 主菜单 ======
int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif
    
    std::string desktop = get_desktop_path();
    if (!desktop.empty()) {
        demo_test_img = desktop + "/demo_test.png";
        demo_stego_img = desktop + "/demo_test_steg.png";
        if (file_exists(demo_test_img) && file_exists(demo_stego_img)) demo_step = 2;
        else if (file_exists(demo_test_img)) demo_step = 1;
    }
    
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════╗\n";
    std::cout << "║    PNG 不可见水印工具 v2.3              ║\n";
    std::cout << "║    ±1 LSB Matching + 3x3 Block          ║\n";
    std::cout << "║    + Reed-Solomon ECC + 多簇冗余        ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n";
    
    if (demo_step == 1) {
        std::cout << "  📌 检测到演示文件，进度：已生成测试图\n";
        std::cout << "  💡 选择「4」继续\n\n";
    } else if (demo_step == 2) {
        std::cout << "  📌 检测到演示文件，进度：已嵌入水印\n";
        std::cout << "  💡 选择「4」继续\n\n";
    }
    
    // 命令行模式
    if (argc >= 5) {
        std::string mode = argv[1];
        if (mode == "embed" && argc >= 6) {
            std::string input = argv[2];
            std::string output = argv[3];
            uint64_t salt = parse_salt(argv[4]);
            std::string text = argv[5];
            int ecc_level = (argc >= 7) ? std::atoi(argv[6]) : 0;
            
            auto result = embed_text(input, output, text, salt, ecc_level);
            if (result.success) {
                std::cout << "OK: embedded " << result.used_bits << " bits into " << output;
                if (ecc_level > 0) std::cout << " (ECC level " << ecc_level << ")";
                std::cout << "\n";
                return 0;
            } else {
                std::cerr << "FAIL: " << result.error_msg << "\n";
                return 1;
            }
        } else if (mode == "extract" && argc >= 4) {
            std::string input = argv[2];
            uint64_t salt = parse_salt(argv[3]);
            int ecc_level = (argc >= 5) ? std::atoi(argv[4]) : 0;
            
            auto result = extract_text(input, salt, ecc_level);
            if (result.success) {
                std::cout << result.text << "\n";
                return 0;
            } else {
                std::cerr << "FAIL: " << result.error_msg << "\n";
                return 1;
            }
        } else if (mode == "extract-erasure" && argc >= 7) {
            // watermark_tool extract-erasure stego.png orig_w orig_h salt ecc_level
            std::string input = argv[2];
            uint32_t orig_w = std::stoul(argv[3]);
            uint32_t orig_h = std::stoul(argv[4]);
            uint64_t salt = parse_salt(argv[5]);
            int ecc_level = std::atoi(argv[6]);
            
            auto result = extract_text_with_erasures(input, salt, ecc_level, orig_w, orig_h);
            if (result.success) {
                std::cout << result.text << "\n";
                return 0;
            } else {
                std::cerr << "FAIL: " << result.error_msg << "\n";
                return 1;
            }
        } else if (mode == "embed-multicluster" && argc >= 8) {
            // watermark_tool embed-multicluster input.png output.png salt text ecc_level num_clusters
            std::string input = argv[2];
            std::string output = argv[3];
            uint64_t salt = parse_salt(argv[4]);
            std::string text = argv[5];
            int ecc_level = std::atoi(argv[6]);
            int num_clusters = std::atoi(argv[7]);
            
            auto result = embed_text_multicluster(input, output, text, salt, ecc_level, num_clusters);
            if (result.success) {
                std::cout << "OK: embedded " << result.used_bits << " bits (" 
                         << num_clusters << " clusters) into " << output << "\n";
                return 0;
            } else {
                std::cerr << "FAIL: " << result.error_msg << "\n";
                return 1;
            }
        } else if (mode == "extract-multicluster" && argc >= 8) {
            // watermark_tool extract-multicluster stego.png orig_w orig_h salt ecc_level num_clusters
            std::string input = argv[2];
            uint32_t orig_w = std::stoul(argv[3]);
            uint32_t orig_h = std::stoul(argv[4]);
            uint64_t salt = parse_salt(argv[5]);
            int ecc_level = std::atoi(argv[6]);
            int num_clusters = std::atoi(argv[7]);
            
            auto result = extract_text_multicluster(input, salt, ecc_level, num_clusters, orig_w, orig_h);
            if (result.success) {
                std::cout << result.text << "\n";
                return 0;
            } else {
                std::cerr << "FAIL: " << result.error_msg << "\n";
                return 1;
            }
        } else if (mode == "capacity" && argc >= 3) {
            auto info = get_capacity(argv[2]);
            if (info.width > 0) {
                std::cout << info.width << "x" << info.height 
                         << " " << info.available_channels << "ch"
                         << " max=" << info.max_text_bytes << "bytes"
                         << " ~" << info.max_text_chars << "chars\n";
                return 0;
            } else {
                std::cerr << "FAIL: cannot read " << argv[2] << "\n";
                return 1;
            }
        } else if (mode == "reset") {
            reset_demo();
            std::cout << "Demo state reset.\n";
            return 0;
        }
    }
    
    // 交互模式
    while (true) {
        if (demo_step == 1) {
            std::string desktop_path = get_desktop_path();
            if (!desktop_path.empty()) {
                std::string expected = desktop_path + "/demo_test_steg.png";
                if (file_exists(expected)) {
                    demo_stego_img = expected;
                    demo_step = 2;
                    std::cout << "\n  🎯 自动检测：水印图片已生成！\n";
                }
            }
        }
        
        std::cout << "\n请选择功能:\n";
        std::cout << "  1. 嵌入水印 (Embed) [支持文件夹+ECC+多簇冗余]\n";
        std::cout << "  2. 提取水印 (Extract) [支持ECC]\n";
        std::cout << "  3. 查询容量 (Capacity)\n";
        std::cout << "  4. 提取水印（单簇擦除恢复 - 截图攻击）\n";
        std::cout << "  5. 提取水印（多簇 + 擦除恢复）\n";
        std::cout << "  6. 🧪 快速示例 (Demo)\n";
        std::cout << "  0. 退出\n";
        
        std::string choice = read_line("\n> ");
        
        if (choice == "1") do_embed();
        else if (choice == "2") do_extract();
        else if (choice == "3") do_capacity();
        else if (choice == "4") do_extract_with_erasures();
        else if (choice == "5") do_extract_multicluster();
        else if (choice == "6") do_demo();
        else if (choice == "0") break;
        else std::cout << "无效选择\n";
    }
    
    std::cout << "\n再见!\n";
    return 0;
}
