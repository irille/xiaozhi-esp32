// gimbal_math.h 的 host 侧自检。无框架、无 fixture，一个 main() + assert。
//
// 跑法（开发机，不需要 ESP-IDF）：
//   c++ -std=c++17 -Wall -Wextra \
//       -I firmware/main/boards/irille-cam-gimbal \
//       firmware/main/boards/irille-cam-gimbal/tests/test_gimbal_math.cc \
//       -o /tmp/gimbal_math_test && /tmp/gimbal_math_test
//
// 本文件必须留在 tests/ 子目录：main/CMakeLists.txt 的
// file(GLOB ... boards/${BOARD_TYPE}/*.cc) 是非递归的，放板目录根下会被编进固件。

#ifdef NDEBUG
#undef NDEBUG  // 自检靠 assert，绝不允许被优化掉
#endif

#include <cassert>
#include <cstdio>
#include <initializer_list>

#include "gimbal_math.h"

namespace {

// 出厂默认表：水平轴实测零偏置，正前方 = 1500；MG90S 标称 10 µs/°。
constexpr AxisCal kPanDefault{1500, 1050, 1950, 1000};   // ±45° → 1050..1950
constexpr AxisCal kTiltDefault{1500, 1250, 1750, 1000};  // ±25° → 1250..1750

// 方向翻转的那台：udeg100 为负 → min_us 数值上反而大于 max_us。
constexpr AxisCal kPanFlipped{1500, 1950, 1050, -1000};

// 由默认角度界换算软限位，跟 Gimbal 载入 NVS 时该做的事一致。
constexpr AxisCal MakeCal(int center_us, int udeg100, int min_deg, int max_deg) {
  AxisCal cal{center_us, 0, 0, udeg100};
  cal.min_us = DegToUs(cal, min_deg);
  cal.max_us = DegToUs(cal, max_deg);
  return cal;
}

// ① 0° → center_us
void TestCenter() {
  assert(DegToUs(kPanDefault, 0) == 1500);
  assert(DegToUs(kTiltDefault, 0) == 1500);
  assert(DegToUs(kPanFlipped, 0) == 1500);
  assert(DegToUs(AxisCal{1480, 1000, 2000, 1000}, 0) == 1480);  // 标定过的台子
  assert(UsToDeg(kPanDefault, 1500) == 0);
}

// ② 正负角落在 center 两侧，符号正确；udeg100 取负则整轴翻转
void TestSign() {
  assert(DegToUs(kPanDefault, 45) == 1950);
  assert(DegToUs(kPanDefault, -45) == 1050);
  assert(DegToUs(kPanDefault, 45) > kPanDefault.center_us);
  assert(DegToUs(kPanDefault, -45) < kPanDefault.center_us);

  // 方向翻转：同样是 +45°，脉宽必须落到另一侧
  assert(DegToUs(kPanFlipped, 45) == 1050);
  assert(DegToUs(kPanFlipped, -45) == 1950);

  // 反向换算也要跟着翻
  assert(UsToDeg(kPanDefault, 1950) == 45);
  assert(UsToDeg(kPanFlipped, 1950) == -45);

  // 非整 10 的刻度：7.33 µs/° 也要四舍五入而非截断（45×7.33 = 329.85 → 330）
  assert(DegToUs(AxisCal{1500, 0, 0, 733}, 45) == 1830);
}

// ③ 超软限位 → 钳到边界值，clamped 置位
void TestSoftClamp() {
  bool clamped = false;

  assert(ClampUs(kPanDefault, 1500, &clamped) == 1500 && !clamped);
  assert(ClampUs(kPanDefault, 2000, &clamped) == 1950 && clamped);
  assert(ClampUs(kPanDefault, 1000, &clamped) == 1050 && clamped);
  assert(ClampUs(kTiltDefault, 1900, &clamped) == 1750 && clamped);

  // 方向翻转表（min_us > max_us）必须钳到同样的物理端点，不能把区间搞反
  assert(ClampUs(kPanFlipped, 2000, &clamped) == 1950 && clamped);
  assert(ClampUs(kPanFlipped, 1000, &clamped) == 1050 && clamped);
  assert(ClampUs(kPanFlipped, 1500, &clamped) == 1500 && !clamped);

  // 端点本身不算钳位
  assert(ClampUs(kPanDefault, 1950, &clamped) == 1950 && !clamped);
  assert(ClampUs(kPanDefault, 1050, &clamped) == 1050 && !clamped);

  // nullptr 允许
  assert(ClampUs(kPanDefault, 9999, nullptr) == 1950);

  // 由角度界换算出来的软限位，与手写的默认表一致（Gimbal 载 NVS 的那条路径）
  constexpr AxisCal built = MakeCal(1500, 1000, -45, 45);
  static_assert(built.min_us == 1050 && built.max_us == 1950, "角度→软限位换算");
  constexpr AxisCal built_flipped = MakeCal(1500, -1000, -45, 45);
  static_assert(built_flipped.min_us == 1950 && built_flipped.max_us == 1050,
                "负 udeg100 下 min/max 数值上互换");
}

// ④ 软限位内、但超物理 µs 界 → 被物理界钳住（标定表被刷坏 / NVS 被清空的场景）
void TestPhysicalClamp() {
  bool clamped = false;
  // 一张离谱的表：软限位 400..2600，整段超出 MG90S 的 600..2400
  constexpr AxisCal bogus{1500, 400, 2600, 1000};

  assert(ClampUs(bogus, 450, &clamped) == kPhysMinUs && clamped);   // 450 在软限位内
  assert(ClampUs(bogus, 2550, &clamped) == kPhysMaxUs && clamped);  // 2550 也在软限位内
  assert(ClampUs(bogus, 1500, &clamped) == 1500 && !clamped);

  // 退化配置：软限位整段落在物理界外，宁可停在物理端点也不下发顶死端止的脉宽
  constexpr AxisCal degenerate{1500, 400, 500, 1000};
  assert(ClampUs(degenerate, 1500, &clamped) == kPhysMinUs && clamped);
}

// ⑤ osc_hz ±8% → prescale 与 count 随之变化，且 count 不溢出 12-bit
void TestPcaCounts() {
  // 回归锚点：实机跑通水平轴时用的就是 121 / 307
  assert(PcaPrescale(25000000, 50) == 121);
  assert(UsToCount(1500, 25000000, 50) == 307);

  const int p_hi = PcaPrescale(27000000, 50);  // +8%
  const int p_lo = PcaPrescale(23000000, 50);  // −8%
  assert(p_hi == 131);
  assert(p_lo == 111);
  assert(p_hi != 121 && p_lo != 121);

  const int c_nom = UsToCount(1500, 25000000, 50);
  const int c_hi = UsToCount(1500, 27000000, 50);
  const int c_lo = UsToCount(1500, 23000000, 50);
  assert(c_hi != c_nom && c_lo != c_nom && c_hi != c_lo);  // 标定量确实到达了 count

  // 整个物理包络在 ±8% 下都不许溢出 12-bit，也不许撞上 FULL_OFF 位
  for (int osc = 23000000; osc <= 27000000; osc += 100000) {
    for (int us = kPhysMinUs; us <= kPhysMaxUs; ++us) {
      const int c = UsToCount(us, osc, 50);
      assert(c > 0 && c <= kPcaMaxCount);
      assert((static_cast<uint16_t>(c) & kPcaFullOff) == 0);
    }
  }

  // 单调：脉宽越长 count 越大
  assert(UsToCount(600, 25000000, 50) < UsToCount(1500, 25000000, 50));
  assert(UsToCount(1500, 25000000, 50) < UsToCount(2400, 25000000, 50));

  // prescale 钳位与非法入参
  assert(PcaPrescale(25000000, 1) == kPcaMaxPrescale);      // 频率太低 → 钳 255
  assert(PcaPrescale(25000000, 10000) == kPcaMinPrescale);  // 频率太高 → 钳 3
  assert(PcaPrescale(0, 50) == kPcaMinPrescale);
  assert(UsToCount(0, 25000000, 50) == 0);
  assert(UsToCount(1500, 0, 50) == 0);
  // prescale 被钳住时仍按实际帧长算，count 不会溢出
  assert(UsToCount(2400, 25000000, 1) <= kPcaMaxCount);
  // ⚠️ 上面那个物理包络的循环里 count 最大只到 ~490，两条断言其实恒真、
  // 12-bit 钳位分支零覆盖。补两条真正逼近上界的：50Hz 下 count 超 4095 需
  // us > 19985（一整帧 20ms），钳位必须生效，且钳完不许撞进 FULL_OFF 位。
  assert(UsToCount(20000, 25000000, 50) == kPcaMaxCount);
  assert((static_cast<uint16_t>(UsToCount(20000, 25000000, 50)) & kPcaFullOff) == 0);
}

// 物理界单层钳位（set_pulse 用：绕过软限位但物理包络仍强制）
void TestClampPhys() {
  assert(ClampPhysUs(500) == kPhysMinUs);
  assert(ClampPhysUs(3000) == kPhysMaxUs);
  assert(ClampPhysUs(1500) == 1500);
}

// 公示角度界向内取整：区间里每个角度都必须真的到得了（不被 ClampUs 改动）
void TestDegBoundInward() {
  // 典型非整刻度标定台：UsToDeg(1428) 四舍五入是 -10，但 DegToUs(-10)=1427 < 1428
  const AxisCal cal{1500, 1428, 1572, 733};
  const int lo = DegBoundInward(cal, cal.min_us, true);
  const int hi = DegBoundInward(cal, cal.max_us, false);
  for (int d = lo; d <= hi; ++d) {
    bool clamped = true;
    ClampUs(cal, DegToUs(cal, d), &clamped);
    assert(!clamped);  // 公示区间内不许出现「合法但到不了」的角度
  }
  // 方向翻转的台子同样成立
  const AxisCal flip{1500, 1572, 1428, -733};
  const int flo = DegBoundInward(flip, flip.max_us, true);
  const int fhi = DegBoundInward(flip, flip.min_us, false);
  for (int d = flo; d <= fhi; ++d) {
    bool clamped = true;
    ClampUs(flip, DegToUs(flip, d), &clamped);
    assert(!clamped);
  }
}

// ⑥ 往返：角度 → µs → 角度
void TestRoundTrip() {
  for (int d = -45; d <= 45; ++d) {
    assert(UsToDeg(kPanDefault, DegToUs(kPanDefault, d)) == d);
    assert(UsToDeg(kPanFlipped, DegToUs(kPanFlipped, d)) == d);
  }
  for (int d = -25; d <= 25; ++d) {
    assert(UsToDeg(kTiltDefault, DegToUs(kTiltDefault, d)) == d);
  }
  // 非整刻度的标定台子：容差 ±1°（µs 是整数，量化误差不可避免）
  for (int udeg100 : {733, -733, 1050, 1234, -911}) {
    const AxisCal cal{1480, 0, 0, udeg100};
    for (int d = -45; d <= 45; ++d) {
      const int back = UsToDeg(cal, DegToUs(cal, d));
      assert(back - d <= 1 && d - back <= 1);
    }
  }
  // udeg100 == 0（未标定表）不许除零崩掉
  assert(UsToDeg(AxisCal{1500, 1050, 1950, 0}, 1800) == 0);
  assert(DegToUs(AxisCal{1500, 1050, 1950, 0}, 30) == 1500);
}

// 时间盒：0 = 永不松弛，边界闭区间
void TestTimeBox() {
  assert(!TimeBoxExpired(0, 0));
  assert(!TimeBoxExpired(0, 999999999LL));  // 永不松弛特例
  assert(TimeBoxExpired(1000, 1000));       // 边界闭区间
  assert(TimeBoxExpired(1000, 1001));
  assert(!TimeBoxExpired(1000, 999));

  assert(DeadlineUs(5000000, 2000) == 7000000);
  assert(DeadlineUs(5000000, 0) == 0);   // hold_ms=0 → 编码成「永不」
  assert(DeadlineUs(5000000, -1) == 0);  // NVS 被写脏也不炸
  assert(!TimeBoxExpired(DeadlineUs(5000000, 0), 999999999LL));
  assert(TimeBoxExpired(DeadlineUs(5000000, 2000), 7000000));
}

}  // namespace

int main() {
  TestCenter();
  TestSign();
  TestSoftClamp();
  TestPhysicalClamp();
  TestPcaCounts();
  TestClampPhys();
  TestDegBoundInward();
  TestRoundTrip();
  TestTimeBox();

  std::printf("gimbal_math self-check OK\n");
  std::printf("  prescale(25MHz,50Hz)=%d  count(1500us)=%d\n",
              PcaPrescale(25000000, 50), UsToCount(1500, 25000000, 50));
  std::printf("  osc +8%%: prescale=%d count=%d | osc -8%%: prescale=%d count=%d\n",
              PcaPrescale(27000000, 50), UsToCount(1500, 27000000, 50),
              PcaPrescale(23000000, 50), UsToCount(1500, 23000000, 50));
  std::printf("  pan span: %d..%d us (deg -45..+45), phys envelope %d..%d us\n",
              DegToUs(kPanDefault, -45), DegToUs(kPanDefault, 45), kPhysMinUs, kPhysMaxUs);
  return 0;
}
