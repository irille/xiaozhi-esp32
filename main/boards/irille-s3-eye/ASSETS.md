# irille-s3-eye 唤醒与表情资产

本目录保存 ESP32-S3-EYE 专用的人格资产。业务改动不得扩散到 common/core。

## 固定来源

- Assets generator：`78/xiaozhi-assets-generator@55517b40d724014faff00f941ca700cbf9d14b51`
- Otto GIF：`txp666/otto-emoji-gif-component@970cf66906d7c30059faa2704e7002f06b8c3619`（v1.3.0，MIT）
- Stage 1 WakeNet9：`wn9_hiesp`
- 目标 assets 分区：`0x200000` bytes

输入文件和最终二进制哈希记录在 `assets-manifest.json`。不得用移动的 `main`、未固定的 release 页面下载物或本地改图替换这些来源。

## 生成

待 T010 完成：记录冻结 generator 的本地启动方式、ESP32-S3/240×240 配置、Hi ESP 模型选择、21 个 Otto GIF 导入顺序、生成环境和最终 SHA-256。

生成范围只允许：

- `index.json`
- `srmodels.bin`（仅 `wn9_hiesp`）
- 21 个与 xiaozhi emotion name 对齐的 GIF

不得加入字体、背景、MultiNet 或第二个 WakeNet 模型。

## 验证

待 T009/T013 完成：记录 board-local contract tests 与 validator 命令。发布前必须验证：

- assets header、checksum、文件表和 `0x5A5A` 数据前缀；
- 21 个 emotion name 恰好一次；
- srmodels 仅含 `wn9_hiesp`；
- 输入与输出哈希匹配 manifest；
- Otto MIT LICENSE 存在；
- `assets.bin <= 0x200000`。

## 构建与烧录

使用仓库根 `CLAUDE.md` 规定的 ESP-IDF v6.0.2 release fullclean 流程。不得复用已存在的同版本 release ZIP。烧录前先只读确认串口。

## 回退

待 T012/T028 完成：记录恢复默认 assets 和旧唤醒模型所需的精确 `config.json` 变更、fullclean/build/flash 和实机回归结果。回退必须重刷 assets 分区，不能依赖旧分区残留。
