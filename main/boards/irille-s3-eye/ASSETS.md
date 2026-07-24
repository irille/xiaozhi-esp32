# irille-s3-eye 唤醒与表情资产

本目录保存 ESP32-S3-EYE 专用的人格资产。业务改动不得扩散到 common/core。

## 固定来源

- Assets generator：`78/xiaozhi-assets-generator@55517b40d724014faff00f941ca700cbf9d14b51`
- Otto GIF：`txp666/otto-emoji-gif-component@970cf66906d7c30059faa2704e7002f06b8c3619`（v1.3.0，MIT）
- 本地字幕字体：ESP Component Registry `78/xiaozhi-fonts@2.0.0`，component hash
  `3f4f9fa5dcb703208bf22e46a185bb2b4cc9d002b9474b90f7c77e55e0ac541a`
  与 repository commit `d45dbc64052d57048f20ab1770074172ce9eb53b` 中的
  `font_noto_sans_common_16_4.bin`（Apache-2.0）
- Stage 1 WakeNet9：Espressif Component Registry `espressif/esp-sr@2.4.7`，component
  hash `809d0041cdddd98a278f0d5afef7bb60a451290577b98cf718dfffc91bdcbd9b`，repository
  commit `2f8c4b0459db5bbb39abd77adae27962d6d94bcb` 中的 `wn9_heyily_tts2`
  （模型内部标识 `wakenet9l_tts2h12`，ESPRESSIF-MIT）
- 目标 assets 分区：`0x200000` bytes

输入文件和最终二进制哈希记录在 `assets-manifest.json`。不得用移动的 `main`、未固定的 release 页面下载物或本地改图替换这些来源。

## 生成

生成脚本直接复用固定 generator 提供的 `WakenetModelPacker` 与
`SpiffsGenerator`，不安装它的 Web UI 依赖，也不重写资产格式：

```bash
work_dir="$(mktemp -d)"
git clone https://github.com/78/xiaozhi-assets-generator.git "$work_dir/generator"
git -C "$work_dir/generator" checkout --detach 55517b40d724014faff00f941ca700cbf9d14b51
git clone https://github.com/txp666/otto-emoji-gif-component.git "$work_dir/otto"
git -C "$work_dir/otto" checkout --detach 970cf66906d7c30059faa2704e7002f06b8c3619
node main/boards/irille-s3-eye/tools/build_assets.mjs \
  "$work_dir/generator" "$work_dir/otto" \
  managed_components/78__xiaozhi-fonts \
  main/boards/irille-s3-eye/assets.bin
```

脚本拒绝 commit 不匹配的输入，固定 ESP32-S3、240×240、`wn9_heyily_tts2`
和 21 个 Otto GIF。先运行一次 ESP-IDF reconfigure/release build，确保固定版本的
`78/xiaozhi-fonts` 已位于 `managed_components/`。GIF 由 FFmpeg `8.1.2` 保持
240×240、帧数和时长重编码，以给本地 common 字体释放空间；manifest 同时记录原始
与打包后哈希。生成环境为 Node.js `v26.4.0`；最终输出：

- size：`1,840,675` bytes（距 `0x200000` 上限尚余 `256,477` bytes）
- SHA-256：`535b58f07ef478e716f6adeb703d4fb24087fe087f7706a6195e1034feee76ea`

`assets.bin` 是 release 的 board-local 输入，必须随 firmware commit 入库。上游全局
`.gitignore` 忽略 `*.bin`，因此生成或升级后需用 `git add -f
main/boards/irille-s3-eye/assets.bin`，并在提交前再次核对 manifest SHA；不能只把文件留在
维护者本机。

生成范围只允许：

- `index.json`
- `srmodels.bin`（仅 `wn9_heyily_tts2`）
- `font_noto_sans_common_16_4.bin`（`noto-v1/common/16/4`）
- 三份第三方许可正文：Otto MIT、xiaozhi-fonts Apache-2.0、ESP-SR ESPRESSIF-MIT
- 21 个与 xiaozhi emotion name 对齐的 GIF

不得加入背景、MultiNet、第二个 WakeNet 模型或其他字体。

## 可复现性审计

维护者必须在新的临时目录中重新 clone 两个固定来源，按上节命令生成到临时输出文件，
不得覆盖 board 目录中的已验收 `assets.bin`。比较文件大小、SHA-256 和 validator 结果后，
把命令、工具版本、输入 commit、输出哈希及差异结论记录到主仓库
`specs/003-wakeword-otto-persona/evidence/stage1.md`。只有内容一致才能声明可复现；临时目录
可在证据记录完成后删除，不得把 clone 或生成中间物提交到仓库。

## 验证

```bash
python -m unittest main/boards/irille-s3-eye/tests/test_assets_contract.py -v
python main/boards/irille-s3-eye/tools/validate_assets.py \
  main/boards/irille-s3-eye/assets-manifest.json
```

发布前必须验证：

- assets header、checksum、文件表和 `0x5A5A` 数据前缀；
- 21 个 emotion name 恰好一次；
- srmodels 仅含 `wn9_heyily_tts2`；
- 本地字体及 metadata 恰好为 `noto-v1/common/16/4`；
- 输入与输出哈希匹配 manifest；
- WakeNet `_MODEL_INFO_`、`wn9_data` 与 `wn9_index` 内容哈希匹配锁定来源；
- Otto MIT LICENSE 存在；
- ESP-SR ESPRESSIF-MIT LICENSE 存在；
- `assets.bin <= 0x200000`。

## 许可

- Otto GIF 使用 MIT；必须保留
  `LICENSES/otto-emoji-gif-component.LICENSE`，manifest 同时记录固定 source commit。
- `78/xiaozhi-fonts` 使用 Apache-2.0；manifest 固定组件版本、component hash、仓库 commit
  和实际字体文件 SHA-256。
- 三份完整许可正文以 `LICENSE.otto-emoji-gif.txt`、`LICENSE.xiaozhi-fonts.txt` 与
  `LICENSE.esp-sr.txt` 嵌入
  `assets.bin`，因此会随 `merged-binary.bin` 和 release ZIP 一并交付；validator 固定其
  SHA-256，缺失、截断或替换均会使发布验证失败。
- Stage 1 `wn9_heyily_tts2` 来自上述锁定的 Espressif ESP-SR 组件，其 ESPRESSIF-MIT
  许可证正文随资产交付。专属模型只能在供应方明确给出
  来源、兼容版本和非独占商业使用许可后替换，不能用“文件可下载”代替许可证据。

## 升级

升级 generator、Otto、字体或 WakeNet 模型时，先在独立变更中冻结新的完整 commit/hash，
再同步 `build_assets.mjs`、manifest、合同测试和本文。必须重新执行干净目录复现、validator、
ESP-IDF v6.0.2 release build 与完整实机门禁；不能只更新下载地址、tag 或版本文字，也不能
复用旧 ZIP。旧 commit、哈希和 release 证据保留用于回退与审计。

## 构建与烧录

使用仓库根 `CLAUDE.md` 规定的 ESP-IDF v6.0.2 release fullclean 流程。不得复用已存在的同版本 release ZIP。烧录前先只读确认串口。

## 回退

把 `config.json` 的 Stage 1 六项恢复为：

- 删除 `CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=n`
- 删除 `CONFIG_SR_WN_WN9_HIESP=n`
- 删除 `CONFIG_SR_WN_WN9_SOPHIA_TTS=n`
- 删除 `CONFIG_SR_WN_WN9_HEYILY_TTS2=y`
- 删除 `CONFIG_FLASH_CUSTOM_ASSETS=y`
- 删除 `CONFIG_CUSTOM_ASSETS_FILE="boards/irille-s3-eye/assets.bin"`

随后按标准 release 流程 fullclean/build/flash。默认
`sdkconfig.defaults.esp32s3` 会重新选择 `wn9_nihaoxiaozhi_tts` 与默认 assets。
回退必须重刷 assets 分区，不能依赖旧分区残留。烧录时从对应 `build/` 目录执行分区级命令：

```bash
python -m esptool --chip esp32s3 -p /dev/cu.usbmodem101 -b 460800 \
  --before default-reset --after hard-reset write-flash "@flash_args"
```

`flash_args` 只写 bootloader、partition table、OTA data、application 和 assets，保留保存
Wi-Fi 与自定义 OTA URL 的 NVS (`0x9000`–`0xcfff`)。不得把 `merged-binary.bin` 从 `0x0`
写入已配置设备；raw merged image 会用空洞数据覆盖 NVS，使设备丢失网络/自部署服务设置并
回落到编译时默认 OTA。恢复 feature 产物时同样使用其 `build/flash_args` 分区级写入。

## 其他设备复用边界

可复用的是固定来源、manifest、validator 和干净复现的方法，不是这个 `assets.bin`。
脚本与产物明确绑定 ESP32-S3、240×240、`irille-s3-eye` 的 2 MiB assets 分区和当前字体/
显示栈。新设备必须在自己的 `main/boards/irille-*/` 目录建立 manifest、配置、容量合同和
实机证据；不得把本板二进制直接复制过去，也不得为复用而修改 common/core。
