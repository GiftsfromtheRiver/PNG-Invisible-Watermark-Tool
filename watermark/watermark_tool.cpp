// ================================================================
// PNG 不可见水印工具
// 功能: 嵌入/提取/查询容量 / 批量文件夹 / 快速示例
// 技术: ±1 LSB Matching + 3×3 块标记 + 自适应通道 + 格式保持
// ================================================================

#include "watermark.h"
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
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace watermark;

// ====== 全局演示状态 ======
static int demo_step = 0;           // 0=未开始, 1=已生成图片, 2=已嵌入, 3=已完成
static std::string demo_test_img;   // 测试图片路径
static std::string demo_stego_img;  // 隐写图片路径
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
    if (attr == INVALID_FILE_ATTRIBUTES) {
        int len2 = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
        if (len2 > 0) {
            std::wstring wpath2(len2, L'\0');
            MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, &wpath2[0], len2);
            while (!wpath2.empty() && wpath2.back() == L'\0') wpath2.pop_back();
            attr = GetFileAttributesW(wpath2.c_str());
        }
    }
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
    if (len <= 0) {
        len = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
        if (len <= 0) return false;
    }
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

static std::vector<std::string> list_png_files(const std::string& dir) {
    std::vector<std::string> files;
#ifdef _WIN32
    int len = MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0);
    if (len <= 0) {
        len = MultiByteToWideChar(CP_ACP, 0, dir.c_str(), -1, nullptr, 0);
        if (len <= 0) return files;
    }
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
                if (has_ext(name, ".png")) {
                    files.push_back(dir + "/" + name);
                }
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
        if (has_ext(name, ".png")) {
            files.push_back(dir + "/" + name);
        }
    }
    closedir(d);
#endif
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
    if (userprofile) {
        return std::string(userprofile) + "\\Desktop";
    }
    return "";
#else
    return "";
#endif
}

// ====== 生成测试图片 ======
static std::string generate_test_image(const std::string& output_path) {
    const int W = 512, H = 384;
    std::vector<uint8_t> image(W * H * 4);
    
    // 创建漂亮的渐变背景（蓝→紫→粉）
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
    
    // 在中央绘制"水印演示"字样
    int cx = W / 2, cy = H / 2;
    int box_w = 280, box_h = 80;
    int bx = cx - box_w / 2, by = cy - box_h / 2;
    
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            size_t idx = (y * W + x) * 4;
            if (x >= bx && x < bx + box_w && y >= by && y < by + box_h) {
                if (y < by + 3 || y >= by + box_h - 3 || x < bx + 3 || x >= bx + box_w - 3) {
                    image[idx + 0] = 255;
                    image[idx + 1] = 255;
                    image[idx + 2] = 255;
                } else {
                    image[idx + 0] = (uint8_t)(image[idx + 0] * 0.4f + 200 * 0.6f);
                    image[idx + 1] = (uint8_t)(image[idx + 1] * 0.4f + 200 * 0.6f);
                    image[idx + 2] = (uint8_t)(image[idx + 2] * 0.4f + 200 * 0.6f);
                }
            }
        }
    }
    
    // 画几个方块模拟文字 "DEMO"
    int text_x = bx + 30, text_y = by + 18;
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            if ((i + j) % 2 == 0) continue;
            int px = text_x + i * 20 + j * 3;
            int py = text_y + j * 3;
            if (px < W && py < H) {
                size_t idx = (py * W + px) * 4;
                image[idx + 0] = 255;
                image[idx + 1] = 100 + i * 10;
                image[idx + 2] = 100 + j * 10;
            }
        }
    }
    
    unsigned error = lodepng::encode(output_path, image, W, H, LCT_RGBA, 8);
    if (error != 0) {
        return "";
    }
    return output_path;
}

// ====== 等待文件写入完成 ======
static bool wait_for_file(const std::string& path, int max_wait_ms = 3000) {
    int waited = 0;
    int step = 50; // 每50ms检查一次
    
    while (!file_exists(path) && waited < max_wait_ms) {
#ifdef _WIN32
        Sleep(step);
#else
        usleep(step * 1000);
#endif
        waited += step;
        if (waited % 500 == 0) {
            std::cout << ".";
            std::cout.flush();
        }
    }
    
    return file_exists(path);
}

// ====== 嵌入水印 ======
static void do_embed() {
    std::cout << "\n===== 嵌入不可见水印 =====\n\n";
    
    std::string input = read_path("输入PNG路径 (文件或文件夹): ");
    if (input.empty()) { std::cout << "已取消\n"; return; }
    
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
        std::cout << "  提示：每行按 Enter 换行，输入完成后在【新的一行】输入 :END 并回车结束。\n";
        std::cout << "  注意：是英文冒号 + 字母 END，不是键盘上的 End 键哦！\n";
        std::cout << "\n开始输入（输入 :END 结束）:\n";
        
        std::string text;
        std::string line;
        while (std::getline(std::cin, line)) {
            if (trim(line) == ":END") break;
            if (!text.empty()) text += "\n";
            text += line;
        }
        
        if (text.empty()) {
            std::cout << "水印文本为空, 已取消\n";
            return;
        }
        
        std::cout << "\n--- 批量嵌入中 ---\n";
        int ok = 0, fail = 0;
        for (size_t i = 0; i < files.size(); i++) {
            const std::string& f = files[i];
            std::string out = auto_output(f);
            
            std::cout << "  [" << (i+1) << "/" << files.size() << "] " << f << " ... ";
            std::cout.flush();
            
            auto result = embed_text(f, out, text, salt);
            if (result.success) {
                std::cout << "OK (" << result.used_bits << " bits)\n";
                ok++;
            } else {
                std::cout << "FAIL: " << result.error_msg << "\n";
                fail++;
            }
        }
        std::cout << "\n--- 批量完成 ---\n";
        std::cout << "  成功: " << ok << "  失败: " << fail << "\n";
        return;
    }
    
    // 单文件模式
    std::string output = read_path("输出PNG路径 (留空=自动命名): ");
    if (output.empty()) {
        output = auto_output(input);
        std::cout << "  -> " << output << "\n";
    }
    
    std::string salt_str = read_line("种子密钥 (数字/字符串, 留空=0): ");
    uint64_t salt = parse_salt(salt_str);
    
    std::cout << "\n请输入要隐藏的水印文本（支持多行）:\n";
    std::cout << "  提示：每行按 Enter 换行，输入完成后在【新的一行】输入 :END 并回车结束。\n";
    std::cout << "  注意：是英文冒号 + 字母 END，不是键盘上的 End 键哦！\n";
    std::cout << "\n开始输入（输入 :END 结束）:\n";
    
    std::string text;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (trim(line) == ":END") break;
        if (!text.empty()) text += "\n";
        text += line;
    }
    
    if (text.empty()) {
        std::cout << "水印文本为空, 已取消\n";
        return;
    }
    
    std::cout << "\n正在嵌入...\n";
    auto result = embed_text(input, output, text, salt);
    
    if (result.success) {
        std::cout << "\n[OK] 水印嵌入成功!\n";
        std::cout << "  图片尺寸: " << result.width << " x " << result.height << "\n";
        std::cout << "  通道模式: " << (result.has_alpha ? "RGBA (4通道)" : "RGB (3通道)") << "\n";
        std::cout << "  嵌入数据: " << result.used_bits << " bits (" 
                  << text.size() << " 字节)\n";
        std::cout << "  总容量:   " << result.capacity_bits << " bits (" 
                  << (result.capacity_bits / 8) << " 字节)\n";
        std::cout << "  使用率:   " << (100.0 * result.used_bits / result.capacity_bits) << "%\n";
        std::cout << "  输出文件: " << output << "\n";
        
        // ===== 等待文件真正写入磁盘 =====
        std::cout << "\n  ⏳ 等待文件写入完成";
        std::cout.flush();
        
        if (wait_for_file(output, 3000)) {
            std::cout << " ✅\n";
        } else {
            std::cout << " ⚠️ 超时，但文件可能已写入\n";
        }
        
        // ===== 检测是否是演示图片 =====
        // 使用文件名匹配而不是完整路径比较（更可靠）
        bool is_demo = false;
        if (!demo_test_img.empty()) {
            // 提取输出文件的文件名
            size_t pos1 = output.find_last_of("/\\");
            std::string out_name = (pos1 == std::string::npos) ? output : output.substr(pos1 + 1);
            // 检查是否是 demo_test_steg.png
            if (out_name == "demo_test_steg.png") {
                is_demo = true;
            }
        }
        
        if (is_demo && file_exists(output)) {
            demo_stego_img = output;
            demo_step = 2;
            std::cout << "\n";
            std::cout << "  ╔══════════════════════════════════════════════════════════╗\n";
            std::cout << "  │  🎯 检测到这是演示图片！                              ║\n";
            std::cout << "  │  水印已嵌入成功！                                     ║\n";
            std::cout << "  │  按 Enter 继续...                                     ║\n";
            std::cout << "  ╚══════════════════════════════════════════════════════════╝\n";
            
            // 等待用户按 Enter
            std::cin.get();
        }
    } else {
        std::cout << "\n[FAIL] " << result.error_msg << "\n";
    }
}

// ====== 提取水印 ======
static void do_extract() {
    std::cout << "\n===== 提取不可见水印 =====\n\n";
    
    std::string input = read_path("隐写PNG路径: ");
    if (input.empty()) { std::cout << "已取消\n"; return; }
    
    std::string salt_str = read_line("种子密钥 (需与嵌入时一致): ");
    uint64_t salt = parse_salt(salt_str);
    
    std::cout << "\n正在提取...\n";
    auto result = extract_text(input, salt);
    
    if (result.success) {
        std::cout << "\n[OK] 水印提取成功！\n";
        std::cout << "以下就是从图片中提取到的隐藏水印内容：\n";
        std::cout << "\n┌─────────────────────────────────────────────────────┐\n";
        std::cout << "│  " << result.text << "\n";
        std::cout << "└─────────────────────────────────────────────────────┘\n";
        std::cout << "\n如果看到的是乱码或无意义内容，请检查密钥是否正确。\n";
        
        // ===== 检测是否是演示图片 =====
        bool is_demo = false;
        if (!demo_stego_img.empty()) {
            size_t pos1 = input.find_last_of("/\\");
            std::string in_name = (pos1 == std::string::npos) ? input : input.substr(pos1 + 1);
            if (in_name == "demo_test_steg.png") {
                is_demo = true;
            }
        }
        
        if (is_demo && result.text == demo_text) {
            demo_step = 3;
            std::cout << "\n";
            std::cout << "  ╔══════════════════════════════════════════════════════════╗\n";
            std::cout << "  │  🎯 检测到这是演示图片！                              ║\n";
            std::cout << "  │  水印提取验证成功！                                   ║\n";
            std::cout << "  │  按 Enter 继续...                                     ║\n";
            std::cout << "  ╚══════════════════════════════════════════════════════════╝\n";
            
            // 等待用户按 Enter
            std::cin.get();
        }
    } else {
        std::cout << "\n[FAIL] " << result.error_msg << "\n";
    }
}

// ====== 查询容量 ======
static void do_capacity() {
    std::cout << "\n===== 查询水印容量 =====\n\n";
    
    std::string input = read_path("PNG图片路径: ");
    if (input.empty()) { std::cout << "已取消\n"; return; }
    
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
    std::cout << "  最大水印: " << info.max_text_bytes << " 字节\n";
    std::cout << "  约可嵌入: " << info.max_text_chars << " 个中文字符\n";
    std::cout << "\n  注: 采用 3×3 块标记模式, 每个数据bit需9个像素\n";
}

// ====== 快速示例 - 步骤1：生成测试图片 ======
static void do_demo_step1() {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    🧪 快速示例                          ║\n";
    std::cout << "║           跟着我一步步操作，5 分钟学会！               ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n";
    std::cout << "\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "  📌 步骤 1/4：生成测试图片\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "\n";
    std::cout << "  程序将自动生成一张测试图片，用于演示水印功能。\n";
    std::cout << "\n";
    
    std::string desktop = get_desktop_path();
    if (desktop.empty()) {
        desktop = ".";
    }
    demo_test_img = desktop + "/demo_test.png";
    demo_stego_img = desktop + "/demo_test_steg.png";
    
    std::cout << "  正在生成...\n";
    std::string result = generate_test_image(demo_test_img);
    if (result.empty()) {
        std::cout << "  ❌ 生成失败！\n";
        demo_step = 0;
        return;
    }
    std::cout << "  ✅ 已生成: " << demo_test_img << "\n";
    std::cout << "\n";
    std::cout << "  💡 提示：你可以在桌面看到这张图片，它是一张彩虹渐变图。\n";
    std::cout << "\n";
    std::cout << "  👉 按 Enter 继续下一步...";
    std::cin.get();
    
    demo_step = 1;
}

// ====== 快速示例 - 步骤2：引导嵌入 ======
static void do_demo_step2() {
    std::cout << "\n\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "  📌 步骤 2/4：嵌入水印（亲手操作！）\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "\n";
    std::cout << "  现在请按以下步骤操作：\n";
    std::cout << "\n";
    std::cout << "  ┌─────────────────────────────────────────────────────────┐\n";
    std::cout << "  │  ① 在菜单中选择「1. 嵌入水印」                       │\n";
    std::cout << "  │  ② 输入图片路径：                                    │\n";
    std::cout << "  │     " << demo_test_img << "\n";
    std::cout << "  │  ③ 输出路径：直接按 Enter（自动命名）               │\n";
    std::cout << "  │  ④ 密钥输入：114514                                 │\n";
    std::cout << "  │  ⑤ 水印文本输入：                                   │\n";
    std::cout << "  │     " << demo_text << "\n";
    std::cout << "  │  ⑥ 输入完成后，在新的一行输入 :END 并回车           │\n";
    std::cout << "  └─────────────────────────────────────────────────────────┘\n";
    std::cout << "\n";
    std::cout << "  ⚠️  注意：是英文冒号 + END，不是键盘上的 End 键！\n";
    std::cout << "\n";
    std::cout << "  ✅ 嵌入完成后，菜单会自动检测到并更新状态。\n";
    std::cout << "\n";
    std::cout << "  👉 按 Enter 开始操作...";
    std::cin.get();
}

// ====== 快速示例 - 步骤3：引导提取 ======
static void do_demo_step3() {
    // 检查隐写图是否存在
    if (!file_exists(demo_stego_img)) {
        std::cout << "\n\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        std::cout << "  ⚠️ 还没有完成嵌入步骤！\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        std::cout << "\n";
        std::cout << "  请先回到菜单，选择「1. 嵌入水印」，按照提示操作：\n";
        std::cout << "\n";
        std::cout << "  ┌─────────────────────────────────────────────────────────┐\n";
        std::cout << "  │  ① 输入图片路径：" << demo_test_img << "\n";
        std::cout << "  │  ② 输出路径：直接按 Enter                          │\n";
        std::cout << "  │  ③ 密钥输入：114514                               │\n";
        std::cout << "  │  ④ 水印文本：" << demo_text << "\n";
        std::cout << "  │  ⑤ 输入 :END 结束                                │\n";
        std::cout << "  └─────────────────────────────────────────────────────────┘\n";
        std::cout << "\n";
        std::cout << "  ✅ 嵌入完成后，再回来选择「4」继续。\n";
        std::cout << "\n";
        std::cout << "  👉 按 Enter 返回...";
        std::cin.get();
        demo_step = 1;
        return;
    }
    
    std::cout << "\n\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "  📌 步骤 3/4：验证水印（亲手操作！）\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "\n";
    std::cout << "  现在我们来验证水印是否真的嵌入进去了！\n";
    std::cout << "\n";
    std::cout << "  ┌─────────────────────────────────────────────────────────┐\n";
    std::cout << "  │  ① 在菜单中选择「2. 提取水印」                       │\n";
    std::cout << "  │  ② 输入隐写图片路径：                                │\n";
    std::cout << "  │     " << demo_stego_img << "\n";
    std::cout << "  │  ③ 密钥输入：114514                                 │\n";
    std::cout << "  └─────────────────────────────────────────────────────────┘\n";
    std::cout << "\n";
    std::cout << "  ✅ 提取完成后，回到这个菜单，选择「4」查看结果。\n";
    std::cout << "\n";
    std::cout << "  👉 按 Enter 开始操作...";
    std::cin.get();
}

// ====== 快速示例 - 步骤4：完成 ======
static void do_demo_step4() {
    std::cout << "\n\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "  📌 步骤 4/4：🎉 验证结果\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "\n";
    
    // 检查隐写图片是否存在
    if (!file_exists(demo_stego_img)) {
        std::cout << "  ❌ 没有找到隐写图片，请先完成步骤 2（嵌入水印）。\n";
        std::cout << "\n";
        std::cout << "  👉 按 Enter 返回...";
        std::cin.get();
        demo_step = 1;
        return;
    }
    
    std::cout << "  正在自动提取水印进行验证...\n";
    auto result = extract_text(demo_stego_img, demo_salt);
    
    if (result.success && result.text == demo_text) {
        std::cout << "\n";
        std::cout << "  ┌─────────────────────────────────────────────────────┐\n";
        std::cout << "  │  ✅ 提取到的水印内容：                            │\n";
        std::cout << "  │  " << result.text << "\n";
        std::cout << "  └─────────────────────────────────────────────────────┘\n";
        std::cout << "\n";
        std::cout << "  🎉 恭喜！你成功完成了水印的嵌入和提取！\n";
        demo_step = 4;
    } else if (result.success) {
        std::cout << "\n";
        std::cout << "  ┌─────────────────────────────────────────────────────┐\n";
        std::cout << "  │  ⚠️ 提取到的水印内容：                            │\n";
        std::cout << "  │  " << result.text << "\n";
        std::cout << "  └─────────────────────────────────────────────────────┘\n";
        std::cout << "\n";
        std::cout << "  ❌ 水印内容与预期不符，请检查：\n";
        std::cout << "     1. 密钥是否输入正确（114514）？\n";
        std::cout << "     2. 是否输入了正确的水印文本？\n";
        std::cout << "     3. 是否选择了正确的隐写图片？\n";
        demo_step = 3;
    } else {
        std::cout << "\n  ❌ 提取失败： " << result.error_msg << "\n";
        std::cout << "\n     请检查：\n";
        std::cout << "     1. 是否按照步骤嵌入了水印？\n";
        std::cout << "     2. 密钥是否输入正确（114514）？\n";
        std::cout << "     3. 是否选择了正确的隐写图片？\n";
        demo_step = 3;
    }
    
    std::cout << "\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "  📌 恭喜完成！\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "\n";
    std::cout << "  你已经学会了使用这个工具！现在你可以：\n";
    std::cout << "\n";
    std::cout << "  📂 批量处理：选择「1. 嵌入水印」，输入文件夹路径\n";
    std::cout << "  🔑 自定义密钥：使用你自己的数字或字符串作为密钥\n";
    std::cout << "  📝 自定义水印：输入任何你想要隐藏的文字\n";
    std::cout << "\n";
    std::cout << "  测试图片已保存在桌面：\n";
    std::cout << "    📄 原图：" << demo_test_img << "\n";
    std::cout << "    🔒 水印图：" << demo_stego_img << "\n";
    std::cout << "\n";
    std::cout << "  💡 你可以把水印图发给朋友，只要密钥正确，他们也能提取出你的水印！\n";
    std::cout << "\n";
    
    std::cout << "  👉 按 Enter 继续...";
    std::cin.get();
}

// ====== 快速示例入口 ======
static void do_demo() {
    // 检查演示文件状态
    bool has_test_img = file_exists(demo_test_img);
    bool has_stego_img = file_exists(demo_stego_img);
    
    // 如果有隐写图但 demo_step 还是 1，说明用户已经嵌入了，更新状态
    if (has_test_img && has_stego_img && demo_step == 1) {
        demo_step = 2;
    }
    
    // 如果只有隐写图没有原图（异常情况），重置
    if (has_stego_img && !has_test_img) {
        std::cout << "\n  ⚠️ 检测到隐写图片但找不到原图，将重新开始。\n";
        demo_step = 0;
    }
    
    // 如果既没有原图也没有隐写图，但 demo_step > 0，重置
    if (!has_test_img && !has_stego_img && demo_step > 0) {
        demo_step = 0;
    }
    
    if (demo_step == 0) {
        do_demo_step1();
    } else if (demo_step == 1) {
        do_demo_step2();
    } else if (demo_step == 2) {
        if (!file_exists(demo_stego_img)) {
            std::cout << "\n  ⚠️ 还没有找到隐写图片，请先完成嵌入步骤！\n";
            std::cout << "  👉 按 Enter 返回...";
            std::cin.get();
            demo_step = 1;
            return;
        }
        do_demo_step3();
    } else if (demo_step == 3 || demo_step == 4) {
        do_demo_step4();
    } else {
        demo_step = 0;
        do_demo_step1();
    }
}

// ====== 重置演示状态 ======
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
    
    // 检查演示文件状态
    std::string desktop = get_desktop_path();
    if (!desktop.empty()) {
        demo_test_img = desktop + "/demo_test.png";
        demo_stego_img = desktop + "/demo_test_steg.png";
        
        if (file_exists(demo_test_img) && file_exists(demo_stego_img)) {
            demo_step = 2;
        } else if (file_exists(demo_test_img)) {
            demo_step = 1;
        }
    }
    
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════╗\n";
    std::cout << "║    PNG 不可见水印工具 v1.3          ║\n";
    std::cout << "║    ±1 LSB Matching + 3x3 Block      ║\n";
    std::cout << "║    支持单文件 / 文件夹批量           ║\n";
    std::cout << "╚══════════════════════════════════════╝\n";
    std::cout << "\n";
    
    if (demo_step == 1) {
        std::cout << "  📌 检测到演示文件，当前进度：已生成测试图\n";
        std::cout << "  💡 选择「4」继续学习嵌入水印\n\n";
    } else if (demo_step == 2) {
        std::cout << "  📌 检测到演示文件，当前进度：已嵌入水印\n";
        std::cout << "  💡 选择「4」继续学习提取验证\n\n";
    }
    
    // 命令行模式
    if (argc >= 5) {
        std::string mode = argv[1];
        if (mode == "embed" && argc >= 6) {
            std::string input = argv[2];
            std::string output = argv[3];
            uint64_t salt = parse_salt(argv[4]);
            std::string text = argv[5];
            
            auto result = embed_text(input, output, text, salt);
            if (result.success) {
                std::cout << "OK: embedded " << result.used_bits << " bits into " << output << "\n";
                return 0;
            } else {
                std::cerr << "FAIL: " << result.error_msg << "\n";
                return 1;
            }
        } else if (mode == "extract" && argc >= 4) {
            std::string input = argv[2];
            uint64_t salt = parse_salt(argv[3]);
            
            auto result = extract_text(input, salt);
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
        // ===== 每次循环检查演示状态 =====
        if (demo_step == 1) {
            std::string desktop_path = get_desktop_path();
            if (!desktop_path.empty()) {
                std::string expected = desktop_path + "/demo_test_steg.png";
                if (file_exists(expected)) {
                    demo_stego_img = expected;
                    demo_step = 2;
                    std::cout << "\n  🎯 自动检测：水印图片已生成！状态已更新。\n";
                }
            }
        }
        
        std::string demo_action;
        
        if (demo_step == 0) {
            demo_action = "🧪 快速示例 (Demo) - 新手推荐！";
        } else if (demo_step == 1) {
            demo_action = "▶️ 继续快速示例 - 嵌入水印 (步骤2/4)";
        } else if (demo_step == 2) {
            demo_action = "▶️ 继续快速示例 - 提取验证 (步骤3/4)";
        } else if (demo_step == 3 || demo_step == 4) {
            demo_action = "🎉 查看快速示例结果 (步骤4/4)";
        }
        
        std::cout << "\n请选择功能:\n";
        std::cout << "  1. 嵌入水印 (Embed) [支持文件夹]\n";
        std::cout << "  2. 提取水印 (Extract)\n";
        std::cout << "  3. 查询容量 (Capacity)\n";
        std::cout << "  4. " << demo_action << "\n";
        std::cout << "  0. 退出\n";
        
        std::string choice = read_line("\n> ");
        
        if (choice == "1") {
            do_embed();
        } else if (choice == "2") {
            do_extract();
        } else if (choice == "3") {
            do_capacity();
        } else if (choice == "4") {
            do_demo();
        } else if (choice == "0") {
            break;
        } else {
            std::cout << "无效选择\n";
        }
    }
    
    std::cout << "\n再见!\n";
    return 0;
}