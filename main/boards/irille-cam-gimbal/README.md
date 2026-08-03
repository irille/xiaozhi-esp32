# irille-cam-gimbal

Waveshare ESP32-S3-CAM 主板（OV5640 + ES8311/ES7210 + NS4150B + CH32V003 IO 扩展 + 240×320 ST7789）
外挂 PCA9685 驱动的双 MG90S 云台。设计依据与全部裁定：
`docs/superpowers/plans/2026-08-03-m4-cam-gimbal-phase1.md`。

## 构建

正式产物必须用 CLAUDE.md 固件节的**完整**命令——少了「解析版本 + 删同名 release ZIP」
两步，上游脚本会把已有 ZIP 当缓存、跳过 fullclean 和 build，你拿到的是上一次的产物：

```bash
source ~/esp/esp-idf-v6.0.2/export.sh && cd firmware \
  && IRILLE_FIRMWARE_PROJECT_VER="$(python -c 'from scripts.release import get_project_version; print(get_project_version())')" \
  && test -n "$IRILLE_FIRMWARE_PROJECT_VER" \
  && rm -f "releases/v${IRILLE_FIRMWARE_PROJECT_VER}_irille-cam-gimbal.zip" \
  && python scripts/release.py irille-cam-gimbal
```

迭代期用 `idf.py set-target esp32s3` + 把 `config.json` 的 `sdkconfig_append` 与
`CONFIG_BOARD_TYPE_IRILLE_CAM_GIMBAL=y` 追加进 `sdkconfig`，再 `idf.py build` 即可。

## 纯逻辑自检（无框架，无 IDF 依赖）

```bash
c++ -std=c++17 -Wall -Wextra -I firmware/main/boards/irille-cam-gimbal \
    firmware/main/boards/irille-cam-gimbal/tests/test_gimbal_math.cc \
    -o /tmp/gimbal_math_test && /tmp/gimbal_math_test
```

`tests/` 必须是子目录：`main/CMakeLists.txt` 的板目录 glob 是**非递归**的，
放板目录根下会被编进固件。

## 云台标定（换舵机 / 换机械件后必做）

标定值权威在 NVS（namespace `gimbal`，全整数键），`config.h` 的宏只是 NVS 为空时的兜底。
**`merged-binary.bin` 全刷会清 NVS，标定随之丢失**——只用 `idf.py flash`。

三个 user-only 工具（AI 在 `tools/list` 里看不到）：

| 工具 | 用途 |
|---|---|
| `self.gimbal.set_pulse(ch, us)` | 绕过角度换算直接下发标称 µs，用来定中位、试探机械端点 |
| `self.gimbal.calibrate(axis, key, value)` | 写一项标定量并立即生效。key ∈ {center,min,max,udeg100}（按轴）∪ {hold_ms,osc_hz}（全局） |
| `self.gimbal.get_calibration()` | 回读两轴标定表 + hold_ms/osc_hz/prescale |

流程：① `calibrate hold_ms 0` 关掉自动松弛 → ② `set_pulse` 找正前方，写进 `center`
→ ③ 找两端机械极限，各留余量后写 `min`/`max` → ④ 量「转 45° 实际用了多少 µs」算出
`udeg100`（µs/度×100，**带符号**，负号即整轴方向翻转）→ ⑤ `calibrate hold_ms 2000` 恢复。

⚠️ `min`/`max` 改完后，**公示给 agent 的角度范围要下次重启才刷新**（上游 `AddTool` 按名去重且
不能运行时重注册），中间靠运行期钳位兜底。

## 两条必须知道的行为

- **松弛**：下发脉宽后 `hold_ms`（默认 2000 ms）自动断力，俯仰轴可能因重力垂落，
  工具返回的角度是**最后下发值**而非真实位姿。不想松弛就把 NVS `hold_ms` 设为 0（代码零改动）。
- **残余风险**：ESP32 若在「已下发脉宽、未到松弛时刻」的窗口里崩溃，舵机会保持该脉宽直到断电，
  窗口 ≤ `hold_ms`。根治手段是 INA219 真堵转检测，不在本期。

## 相机方向

送 LLM 的图不得镜像（CLAUDE.md 红线）。方向只能用 `config.h` 的 `CAMERA_HMIRROR` /
`CAMERA_VFLIP` 调，**永远不许**用 `DISPLAY_MIRROR_X/Y` 去「修」——那是 #27 踩过的坑
（屏上看着正，上传给 VLLM 的字全是反的）。判别动作见设计文档 §3-6.2 的「F-卡」。
