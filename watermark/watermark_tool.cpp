// ================================================================
// PNG 不可见水印工具
// 功能: 嵌入/提取/查询容量 / 批量文件夹
// 技术: ±1 LSB Matching + 3×3 块标记 + 自适应通道 + 格式保持
// ================================================================

#include "watermark.h"
#include <iostream>
#include <string>
#include <sstream>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace watermark;

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

// 修复：正确处理中文路径
static bool is_directory(const std::string& path) {
#ifdef _WIN32
    // 使用 MultiByteToWideChar 正确处理中文
    int len = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (len <= 0) {
        // 如果 UTF-8 转换失败，尝试 ANSI
        len = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
        if (len <= 0) return false;
    }
    std::wstring wpath(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], len);
    // 如果有两个 null 终止符，去掉多余的
    while (!wpath.empty() && wpath.back() == L'\0') wpath.pop_back();
    
    DWORD attr = GetFileAttributesW(wpath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        // 尝试 ANSI
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

static bool has_ext(const std::string& fname, const std::string& ext) {
    if (fname.size() < ext.size()) return false;
    size_t pos = fname.size() - ext.size();
    if (ext[0] != '.') return false;
    for (size_t i = 0; i < ext.size(); i++) {
        if (std::tolower(fname[pos + i]) != std::tolower(ext[i])) return false;
    }
    return true;
}

// 修复：正确处理中文路径
static std::vector<std::string> list_png_files(const std::string& dir) {
    std::vector<std::string> files;
#ifdef _WIN32
    // 转换目录路径
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
            // 转回 UTF-8
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

// 从字符串解析 salt
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

// ---- 嵌入水印（支持单文件和文件夹） ----
static void do_embed() {
    std::cout << "\n===== 嵌入不可见水印 =====\n\n";
    
    std::string input = read_path("输入PNG路径 (文件或文件夹): ");
    if (input.empty()) { std::cout << "已取消\n"; return; }
    
    // 批量模式：输入是文件夹
    if (is_directory(input)) {
        std::vector<std::string> files = list_png_files(input);
        if (files.empty()) {
            std::cout << "\n[WARN] 在 " << input << " 中未找到 PNG 文件\n";
            return;
        }
        std::cout << "\n  找到 " << files.size() << " 个 PNG 文件\n";
        
        std::string salt_str = read_line("种子密钥 (数字/字符串, 留空=0): ");
        uint64_t salt = parse_salt(salt_str);
        
        std::cout << "\n输入水印文本 (所有图片使用相同文本):\n";
        std::cout << "(单独一行输入 :END 结束)\n";
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
    
    std::cout << "\n输入水印文本 (输入结束后按 Enter, 支持多行):\n";
    std::cout << "(单独一行输入 :END 结束)\n";
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
    } else {
        std::cout << "\n[FAIL] " << result.error_msg << "\n";
    }
}

// ---- 提取水印 ----
static void do_extract() {
    std::cout << "\n===== 提取不可见水印 =====\n\n";
    
    std::string input = read_path("隐写PNG路径: ");
    if (input.empty()) { std::cout << "已取消\n"; return; }
    
    std::string salt_str = read_line("种子密钥 (需与嵌入时一致): ");
    uint64_t salt = parse_salt(salt_str);
    
    std::cout << "\n正在提取...\n";
    auto result = extract_text(input, salt);
    
    if (result.success) {
        std::cout << "\n[OK] 水印提取成功!\n";
        std::cout << "  文本长度: " << result.text.size() << " 字节\n";
        std::cout << "\n--- 水印内容 ---\n";
        std::cout << result.text << "\n";
        std::cout << "--- 结束 ---\n";
    } else {
        std::cout << "\n[FAIL] " << result.error_msg << "\n";
    }
}

// ---- 查询容量 ----
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

// ---- 主菜单 ----
int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════╗\n";
    std::cout << "║    PNG 不可见水印工具 v1.2          ║\n";
    std::cout << "║    ±1 LSB Matching + 3x3 Block      ║\n";
    std::cout << "║    支持单文件 / 文件夹批量           ║\n";
    std::cout << "╚══════════════════════════════════════╝\n";
    std::cout << "\n";
    
    // 支持命令行直接执行
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
        }
    }
    
    // 交互模式
    while (true) {
        std::cout << "\n请选择功能:\n";
        std::cout << "  1. 嵌入水印 (Embed) [支持文件夹]\n";
        std::cout << "  2. 提取水印 (Extract)\n";
        std::cout << "  3. 查询容量 (Capacity)\n";
        std::cout << "  0. 退出\n";
        
        std::string choice = read_line("\n> ");
        
        if (choice == "1") do_embed();
        else if (choice == "2") do_extract();
        else if (choice == "3") do_capacity();
        else if (choice == "0") break;
        else std::cout << "无效选择\n";
    }
    
    std::cout << "\n再见!\n";
    return 0;
}