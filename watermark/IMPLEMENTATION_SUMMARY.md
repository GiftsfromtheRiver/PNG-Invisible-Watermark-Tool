# RS 擦除解码功能实现总结

## 完成的工作

### 1. rs_codec.h / rs_codec.cpp
- **新增函数**: `rs_decode_with_erasures()`
- **算法**: Forney 直接法纯擦除解码
- **核心逻辑**:
  - 构造擦除定位多项式 Λ(x) = ∏(1 - X_j·x)
  - 计算错误评估多项式 Ω(x) = S(x)·Λ(x) mod x^{npar}
  - 使用 Forney 公式计算每个擦除位置的错误值
  - 修正擦除位置的字节值
- **容量**: 每个擦除消耗 1 个校验符号（而错误消耗 2 个）

### 2. watermark.h / watermark.cpp
- **新增接口**: `extract_text_with_erasures()`
- **核心功能**:
  - 使用原图尺寸创建 BlockGrid
  - 提取时检测超出截图范围的块，标记为擦除
  - 位级擦除转换为 RS 符号级擦除
  - 调用擦除解码恢复数据
- **Header 处理**:
  - 实现位级多数投票，考虑擦除信息
  - 3 份 32-bit header 冗余，即使部分位被擦除也能正确恢复

### 3. watermark_tool.cpp
- **菜单选项 4**: 带擦除恢复的水印提取
  - 输入截图 PNG 路径
  - 输入原始图片尺寸（宽×高）
  - 输入 salt 和 ECC 等级
  - 调用擦除提取并显示结果
- **命令行模式**: `extract-erasure` 子命令
  - 格式: `watermark_tool extract-erasure stego.png orig_w orig_h salt ecc_level`
- **版本号**: 更新至 v1.5

### 4. test_watermark.cpp
- **新增 6 个测试用例**:
  1. RS 擦除解码（无错误）
  2. RS 擦除解码（10 个擦除，npar=16）
  3. RS 擦除解码（最大容量，npar=8）
  4. RS 擦除解码（超出容量，应失败）
  5. 水印擦除提取（小裁剪，npar=16）
  6. 水印擦除提取（高 ECC，npar=48）
- **测试结果**: 69 个测试全部通过，0 失败

### 5. build_watermark.bat
- 更新说明，包含擦除解码功能
- 添加 `extract-erasure` 命令行用法说明
- 列出各 ECC 等级的擦除容量

## 技术细节

### 擦除解码的数学原理
- **错误定位数**: X_j = α^(254-pos_j)
- **逆元**: X_j^{-1} = α^((1+pos_j) mod 255)
- **Forney 公式**: e_j = Ω(X_j^{-1}) / Λ'(X_j^{-1})
- **约束**: num_erasures + 2×num_errors ≤ npar

### 实际应用限制
由于每个 RS 符号（字节）由 8 个 bit 组成，每个 bit 存储在不同的块中：
- 即使很小的空间裁剪也会导致大量符号级擦除
- **可行性示例**:
  - 600×600 图片，裁剪 3 像素（1 块行），ECC Level 2：约 10 个擦除 ✓
  - 600×600 图片，裁剪 9 像素（3 块行），ECC Level 5：约 29 个擦除 ✓
  - 600×600 图片，裁剪 50%（300 像素）：约 237 个擦除 ✗（超过任何 ECC 等级）

### 最佳使用场景
- 轻微裁剪攻击（丢失 1-5% 像素）
- 配合高 ECC 等级（Level 4-6）可承受更大裁剪
- 需要知道原始图片尺寸

## 验证结果
```
========================================
  PNG Watermark Tool - Automated Test
  (with Reed-Solomon ECC + Erasure)
========================================
  Result: 69 passed, 0 failed
========================================
```

## 文件清单
所有文件已上传到项目空间 `/watermark_tool_rs/`:
- rs_codec.h
- rs_codec.cpp
- watermark.h
- watermark.cpp
- watermark_tool.cpp
- test_watermark.cpp
- build_watermark.bat

## 向后兼容性
- 所有原有测试通过
- ECC Level 0（无纠错）行为完全不变
- 标准 `extract_text()` 函数不受影响
- 新增功能仅在调用 `extract_text_with_erasures()` 时生效
