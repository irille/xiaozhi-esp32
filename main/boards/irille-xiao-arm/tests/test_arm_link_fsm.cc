// arm_link_fsm 的脱框架自检：零 ESP-IDF 依赖，开发机直接编译运行。
//
//   cc  -std=c11   -c ../arm_link_fsm.c        -o /tmp/fsm.o
//   c++ -std=c++17 -I.. -c test_arm_link_fsm.cc -o /tmp/test.o
//   c++ /tmp/fsm.o /tmp/test.o -o /tmp/t && /tmp/t
//
// 覆盖的是**决策逻辑本身**——迟到应答冒领、跨世代错配、重发上限、非幂等探查、
// 急停通道、清场 fence、复位恢复。这些留在 .cc 里就只能上板测，而上板测的是 25kg 舵机。

#include "arm_link_fsm.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

static int g_checks = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                 \
        }                                                                 \
    } while (0)

// 测试用参数：把超时压到小值，避免测试里等真实毫秒。
static ArmFsmConfig TestCfg() {
    ArmFsmConfig c;
    c.ack_timeout_ms = 300;
    c.done_grace_ms = 1000;
    c.fallback_deadline_ms = 92000;
    c.boot_window_ms = 4000;
    c.stop_ack_wait_ms = 200;
    c.max_attempts = 3;
    return c;
}

// 收到即处理时，接收时刻的世代 == 当前世代。需要模拟"迟到的行"时用 OnLineGen
// 显式传入更早的世代快照。
static ArmDecision OnLine(ArmFsm* f, const char* line, uint32_t now) {
    return arm_fsm_on_line(f, line, f->operation_generation, now);
}
static ArmDecision OnLineGen(ArmFsm* f, const char* line, uint32_t gen, uint32_t now) {
    return arm_fsm_on_line(f, line, gen, now);
}

// 把 fsm 推到「就绪、位置已知」——多数用例的起点。
static void BringUpReady(ArmFsm* f, uint32_t* now) {
    ArmDecision d = OnLine(f, "READY:1.0:04", *now);
    CHECK(d.action == ARM_ACT_NONE);            // 收到 READY 不下行
    CHECK(f->phase == ARM_PHASE_WAIT_BOOT_DONE);
    *now += 100;
    d = OnLine(f, "DONE", *now);       // boot 归位完成
    CHECK(d.action == ARM_ACT_SEND);            // 转 VERIFY，内部发一次 STATUS
    CHECK(std::strcmp(d.line, "STATUS") == 0);
    CHECK(f->phase == ARM_PHASE_VERIFY_BOOT_HOME);
    *now += 20;
    d = OnLine(f, "POS:A=90,B=70,C=80,D=90,E=90,F=170;ST=IDLE;ATT=3F", *now);
    CHECK(f->phase == ARM_PHASE_READY);
    CHECK(f->position_known == 1);
}

// 上电窗口内什么证据都没等到 ⇒ 先问一次 STATUS、无回音再落锁。把 now 推到落锁时刻。
static void BringUpLocked(ArmFsm* f, uint32_t* now) {
    *now += 4001;                  // boot_window_ms 到期
    arm_fsm_on_tick(f, *now);      // 不直接落锁：先问 STATUS
    *now += 901;                   // + ack_timeout_ms * 3
    arm_fsm_on_tick(f, *now);
    CHECK(f->phase == ARM_PHASE_LOCKED_AFTER_RESET);
    CHECK(f->position_known == 0);
}

static ArmRequest JointReq(const char* j, int deg) {
    ArmRequest r;
    std::memset(&r, 0, sizeof(r));
    r.kind = ARM_REQ_JOINT;
    std::snprintf(r.joint, sizeof(r.joint), "%s", j);
    r.angle = deg;
    return r;
}

static ArmRequest SimpleReq(ArmReqKind k) {
    ArmRequest r;
    std::memset(&r, 0, sizeof(r));
    r.kind = k;
    return r;
}

static ArmRequest NamedReq(ArmReqKind k, const char* name) {
    ArmRequest r;
    std::memset(&r, 0, sizeof(r));
    r.kind = k;
    std::snprintf(r.name, sizeof(r.name), "%s", name);
    return r;
}

// ------------------------------------------------------------------ 行分类
static void TestClassify() {
    CHECK(arm_fsm_classify("OK:4200") == ARM_LINE_OK_MAXMS);
    CHECK(arm_fsm_classify("OK:STOPPED") == ARM_LINE_OK_STOPPED);
    CHECK(arm_fsm_classify("OK:RELAXED") == ARM_LINE_OK_RELAXED);
    CHECK(arm_fsm_classify("DONE") == ARM_LINE_DONE);
    CHECK(arm_fsm_classify("BUSY") == ARM_LINE_BUSY);
    CHECK(arm_fsm_classify("ERROR:LIMIT") == ARM_LINE_ERROR);
    CHECK(arm_fsm_classify("ERROR:COLLISION") == ARM_LINE_ERROR);
    CHECK(arm_fsm_classify("ERROR:TIMEOUT") == ARM_LINE_ERROR);
    CHECK(arm_fsm_classify("PONG:1.1") == ARM_LINE_PONG);
    CHECK(arm_fsm_classify("POS:A=1;ST=IDLE;ATT=3F") == ARM_LINE_POS);
    CHECK(arm_fsm_classify("ST=MOVING") == ARM_LINE_ST);
    CHECK(arm_fsm_classify("READY:1.0:04") == ARM_LINE_READY);
    CHECK(arm_fsm_classify("") == ARM_LINE_MALFORMED);
    CHECK(arm_fsm_classify("OK") == ARM_LINE_MALFORMED);   // 残缺
    CHECK(arm_fsm_classify("garbage") == ARM_LINE_MALFORMED);

    // 带载荷的行必须真的带载荷：裸前缀是误码帧，不得混成合法应答。
    // "ERROR:" 若被放行会一路走到 error_code_of 落进 INVALID_COMMAND——
    // 一个残帧就变成了一次业务拒绝。
    CHECK(arm_fsm_classify("ERROR:") == ARM_LINE_MALFORMED);
    CHECK(arm_fsm_classify("PONG:") == ARM_LINE_MALFORMED);
    CHECK(arm_fsm_classify("POS:") == ARM_LINE_MALFORMED);
    CHECK(arm_fsm_classify("ST=") == ARM_LINE_MALFORMED);
    CHECK(arm_fsm_classify("READY:") == ARM_LINE_MALFORMED);
    CHECK(arm_fsm_classify("DONE:CONTACT") == ARM_LINE_MALFORMED);  // 未实现的变体
}

// ---------------------------------------------------- 状态回报的 fail-open 防线
// 下面三条都是"畸形回报骗过 boot 验证、让 position_known 变真"这一类，
// PR review 逐条抓出来的。它们的共同点是：单看某个片段像合法数据。

// POS 的每个关节角必须是**完整的十进制整数**。strtol 对 "oops" 返回 0、
// 对 "90x" 返回 90 都不报错——只要还给计数，污染的坐标就会被当成有效位置。
static void TestPosRejectsMalformedValues() {
    int j[6] = {0};
    CHECK(arm_fsm_parse_pos("POS:A=90,B=70,C=80,D=90,E=90,F=170;ST=IDLE;ATT=3F", j) == 1);
    CHECK(arm_fsm_parse_pos("POS:A=oops,B=70,C=80,D=90,E=90,F=170;ST=IDLE", j) == 0);
    CHECK(arm_fsm_parse_pos("POS:A=90x,B=70,C=80,D=90,E=90,F=170;ST=IDLE", j) == 0);
    CHECK(arm_fsm_parse_pos("POS:A=,B=70,C=80,D=90,E=90,F=170;ST=IDLE", j) == 0);
    // 协议角度限定 0–180（arm-serial.md §2）：越界值是误码，不是"另一个位置"。
    // strtol 能解析负号 ≠ 协议允许负角度——这条断言原先写反了。
    CHECK(arm_fsm_parse_pos("POS:A=-5,B=70,C=80,D=90,E=90,F=170;ST=IDLE", j) == 0);
    CHECK(arm_fsm_parse_pos("POS:A=181,B=70,C=80,D=90,E=90,F=170;ST=IDLE", j) == 0);
    CHECK(arm_fsm_parse_pos("POS:A=0,B=180,C=80,D=90,E=90,F=170;ST=IDLE", j) == 1);  // 两端合法

    // 走完整路径：污染的 POS 不得让 boot 验证解锁
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    OnLine(&f, "READY:1.0:04", now);
    now += 100;
    OnLine(&f, "DONE", now);
    now += 20;
    OnLine(&f, "POS:A=oops,B=70,C=80,D=90,E=90,F=170;ST=IDLE;ATT=3F", now);
    CHECK(f.has_pos == 0);          // 没缓存污染坐标
    // ST/ATT 本身合法，boot 验证放行是可以的；关键是位置数据没被污染
}

// 字段名必须在**字段边界**上。strstr 不验前驱，";XST=IDLE;XATT=3F" 里的
// "ST=" / "ATT=" 会被当成真字段，一行畸形回报就能骗开 boot 验证。
static void TestStatusKeysNeedFieldBoundary() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    OnLine(&f, "READY:1.0:04", now);
    now += 100;
    OnLine(&f, "DONE", now);
    CHECK(f.phase == ARM_PHASE_VERIFY_BOOT_HOME);
    now += 20;
    OnLine(&f, "POS:A=90,B=70,C=80,D=90,E=90,F=170;XST=IDLE;XATT=3F", now);
    CHECK(f.phase == ARM_PHASE_LOCKED_AFTER_RESET);   // ★ 不认伪字段
    CHECK(f.position_known == 0);
}

// 关节名必须**恰好一个字符**。上游若先取首字符，"Afoo" 就被静默截断成合法的 "A"，
// 白名单形同虚设——而 FR-010a 明写 MUST NOT 截断。
static void TestJointRejectsMultiChar() {
    CHECK(arm_fsm_joint_is_valid("A") == 1);
    CHECK(arm_fsm_joint_is_valid("F") == 1);
    CHECK(arm_fsm_joint_is_valid("Afoo") == 0);
    CHECK(arm_fsm_joint_is_valid("A:B") == 0);
    CHECK(arm_fsm_joint_is_valid("") == 0);
    CHECK(arm_fsm_joint_is_valid("G") == 0);
    CHECK(arm_fsm_joint_is_valid(nullptr) == 0);

    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);
    ArmRequest r = JointReq("Afoo", 95);
    ArmDecision d = arm_fsm_on_request(&f, &r, now);
    CHECK(d.action == ARM_ACT_REPLY);
    CHECK(d.reply.code == ARM_CODE_BAD_ARGUMENT);   // 拒绝，不是截断成 JOINT:A:95
}

// READY / PONG 的载荷必须是合协议的版本号。带尾巴的 "1.1junk" 若被接受，
// 一个误码帧就能点亮 collision_guard——board 于是对外宣称有自碰撞防护而板上没有；
// "READY:garbage" 若被接受，它会置上 ready_seen，后面一个 DONE 加一次 idle 就解锁。
static void TestVersionAndCollisionGuard() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;

    OnLine(&f, "READY:garbage", now);
    CHECK(f.ready_seen == 0);          // ★ 不认
    CHECK(f.link_epoch == 0);
    OnLine(&f, "READY:1", now);        // 缺 minor
    CHECK(f.ready_seen == 0);
    OnLine(&f, "READY:1.junk", now);
    CHECK(f.ready_seen == 0);

    OnLine(&f, "READY:1.0:04", now);   // 合法
    CHECK(f.ready_seen == 1);
    CHECK(f.link_epoch == 1);
    CHECK(f.fw_major == 1 && f.fw_minor == 0);
    CHECK(f.collision_guard == 0);

    // PONG 带尾巴不得点亮 collision_guard
    ArmFsm g; arm_fsm_init(&g, &c);
    OnLine(&g, "PONG:1.1junk", now);
    CHECK(g.collision_guard == 0);
    CHECK(g.fw_major == 0);
    OnLine(&g, "PONG:1.1", now);
    CHECK(g.collision_guard == 1);
}

// boot 窗口内没等到 DONE：不直接落锁，而是发一次 STATUS 问清楚——
// 那条 DONE 可能只是丢了，而真正的判据一直是 STATUS 而不是 DONE。
static void TestBootWindowFallsBackToStatus() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    OnLine(&f, "READY:1.0:04", now);
    CHECK(f.phase == ARM_PHASE_WAIT_BOOT_DONE);

    now += 4001;                       // 窗口到期，一个 DONE 也没见到
    ArmDecision d = arm_fsm_on_tick(&f, now);
    CHECK(d.action == ARM_ACT_SEND);   // ★ 先问一次，不是直接落锁
    CHECK(std::strcmp(d.line, "STATUS") == 0);
    CHECK(f.phase == ARM_PHASE_VERIFY_BOOT_HOME);

    // 下位机其实早已归位完：确认后照样解锁
    now += 20;
    OnLine(&f, "POS:A=90,B=70,C=80,D=90,E=90,F=170;ST=IDLE;ATT=3F", now);
    CHECK(f.phase == ARM_PHASE_READY);
    CHECK(f.position_known == 1);

}

// 动作超时 ⇒ 下位机锁存急停（归位超时还会 detach）⇒ 位置不再可信。
// 这条路径原先漏了降级：board 侧继续报 position_known，下一条动作照发。
static void TestTimeoutForfeitsPosition() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    ArmRequest r = JointReq("A", 95);
    arm_fsm_on_request(&f, &r, now);
    now += 30;
    OnLine(&f, "OK:2000", now);
    CHECK(f.position_known == 1);

    now += 500;
    OnLine(&f, "ERROR:TIMEOUT", now);
    CHECK(f.op_state == ARM_OP_TIMED_OUT);
    CHECK(f.op_code == ARM_CODE_TIMEOUT);
    CHECK(f.position_known == 0);                      // ★ 必须降级
    CHECK(f.phase == ARM_PHASE_LOCKED_AFTER_RESET);    // ★ 必须落锁

    // 此后普通运动被挡，只放行 home
    ArmRequest j = JointReq("B", 70);
    ArmDecision d = arm_fsm_on_request(&f, &j, now);
    CHECK(d.action == ARM_ACT_REPLY);
    CHECK(d.reply.ok == 0);
    ArmRequest home = SimpleReq(ARM_REQ_HOME);
    d = arm_fsm_on_request(&f, &home, now);
    CHECK(d.action == ARM_ACT_SEND);
}

// ------------------------------------------------------------------ 正常受理与终态
static void TestAcceptAndDone() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    ArmRequest r = JointReq("A", 95);
    ArmDecision d = arm_fsm_on_request(&f, &r, now);
    CHECK(d.action == ARM_ACT_SEND);
    CHECK(std::strcmp(d.line, "JOINT:A:95") == 0);
    CHECK(f.op_state == ARM_OP_PENDING_ACCEPTANCE);

    now += 50;
    d = OnLine(&f, "OK:4200", now);
    CHECK(d.action == ARM_ACT_REPLY);
    CHECK(d.reply.ok == 1);
    CHECK(d.reply.max_ms == 4200);
    CHECK(d.reply.operation_id == f.op_id);
    CHECK(f.op_state == ARM_OP_MOVING);

    now += 4000;
    d = OnLine(&f, "DONE", now);
    CHECK(f.op_state == ARM_OP_DONE);
    CHECK(f.position_known == 1);
}

// 即时应答**之前**到达的终态属于上一条，必须丢弃（协议 §4.3.2）
static void TestTerminalBeforeAckDiscarded() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    ArmRequest r = JointReq("B", 75);
    arm_fsm_on_request(&f, &r, now);
    CHECK(f.op_state == ARM_OP_PENDING_ACCEPTANCE);

    now += 10;
    ArmDecision d = OnLine(&f, "DONE", now);   // 迟到的上一条应答
    CHECK(d.action == ARM_ACT_NONE);
    CHECK(f.op_state == ARM_OP_PENDING_ACCEPTANCE);     // 没被误认领
}

// 同一纪元内，旧操作放弃后到达的 DONE 不得被新操作冒领
static void TestStaleTerminalAcrossGeneration() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    // 操作 1 受理后一直不给终态，直到 deadline 到期
    ArmRequest r1 = JointReq("A", 100);
    arm_fsm_on_request(&f, &r1, now);
    now += 50;
    OnLine(&f, "OK:1000", now);
    CHECK(f.op_state == ARM_OP_MOVING);
    uint32_t gen1 = f.operation_generation;

    now += 1000 + 1000 + 1;            // max_ms + grace 到期
    ArmDecision d = arm_fsm_on_tick(&f, now);
    CHECK(d.action == ARM_ACT_DRAIN_AND_SEND);  // 清场：先排空再单发 STATUS
    CHECK(std::strcmp(d.line, "STATUS") == 0);
    now += 20;
    d = OnLine(&f, "POS:A=100;ST=IDLE;ATT=3F", now);
    // 工具在受理时就已返回，此刻没有等待者——只更新状态，不产生 REPLY
    CHECK(d.action == ARM_ACT_NONE);
    CHECK(f.op_state == ARM_OP_LINK_FAILED);
    CHECK(f.op_code == ARM_CODE_LINK);      // 下位机已空闲却没见终态 ⇒ 链路故障
    CHECK(f.operation_generation != gen1);         // 清场后世代已推进

    // 操作 2
    ArmRequest r2 = JointReq("B", 80);
    d = arm_fsm_on_request(&f, &r2, now);
    CHECK(d.action == ARM_ACT_SEND);
    now += 30;
    OnLine(&f, "OK:800", now);
    CHECK(f.op_state == ARM_OP_MOVING);

    // 操作 1 的迟到 DONE 现在才到。它是在**旧世代**期间进入串口的，
    // 用那时的世代快照参与认领——五条条件第五条就是为这一刻存在的。
    now += 10;
    d = OnLineGen(&f, "DONE", gen1, now);
    CHECK(d.action == ARM_ACT_NONE);
    CHECK(f.op_state == ARM_OP_MOVING);     // 操作 2 没被冒领

    // 同一行若带着当前世代到达（即它真是操作 2 的终态），则应被正常认领
    d = OnLine(&f, "DONE", now);
    CHECK(f.op_state == ARM_OP_DONE);
}

// 跨纪元（下位机复位过）的终态同样丢弃
static void TestStaleTerminalAcrossEpoch() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    ArmRequest r = JointReq("C", 60);
    arm_fsm_on_request(&f, &r, now);
    now += 50;
    OnLine(&f, "OK:2000", now);
    CHECK(f.op_state == ARM_OP_MOVING);

    now += 100;
    OnLine(&f, "READY:1.0:02", now);   // 对端复位
    CHECK(f.op_state == ARM_OP_RESET);
    CHECK(f.position_known == 0);
    CHECK(f.phase == ARM_PHASE_WAIT_BOOT_DONE);
    uint32_t epoch = f.link_epoch;

    // 复位前那条动作的 DONE 现在才到
    now += 10;
    ArmDecision d = OnLine(&f, "DONE", now);
    // 它会被当作 boot 归位的候选（本纪元首个 DONE），但绝不能恢复旧操作
    CHECK(f.op_state == ARM_OP_RESET);
    CHECK(f.link_epoch == epoch);
    (void)d;
}

// ------------------------------------------------------------------ 重发
static void TestRetryExhausted() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    ArmRequest r = JointReq("D", 100);
    ArmDecision d = arm_fsm_on_request(&f, &r, now);
    CHECK(d.action == ARM_ACT_SEND);
    CHECK(f.op_attempts == 1);

    now += 301;
    d = arm_fsm_on_tick(&f, now);
    CHECK(d.action == ARM_ACT_SEND);          // 第 2 次
    CHECK(f.op_attempts == 2);

    now += 301;
    d = arm_fsm_on_tick(&f, now);
    CHECK(d.action == ARM_ACT_SEND);          // 第 3 次
    CHECK(f.op_attempts == 3);

    now += 301;
    d = arm_fsm_on_tick(&f, now);             // 用尽 ⇒ LINK
    CHECK(d.action == ARM_ACT_REPLY);
    CHECK(d.reply.ok == 0);
    CHECK(d.reply.code == ARM_CODE_LINK);
    CHECK(f.op_attempts == 3);                // 总尝试 3 次，不是 4 次
}

// 重发后收到 BUSY ⇒ 视为已受理，但拿不到 max_ms
static void TestBusyInferredAcceptance() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    ArmRequest r = JointReq("A", 120);
    arm_fsm_on_request(&f, &r, now);
    now += 301;
    arm_fsm_on_tick(&f, now);                 // 重发
    now += 20;
    ArmDecision d = OnLine(&f, "BUSY", now);
    CHECK(d.action == ARM_ACT_REPLY);
    CHECK(d.reply.ok == 1);
    CHECK(d.reply.max_ms == -1);              // 未知，如实上报
    CHECK(f.op_state == ARM_OP_MOVING);
    CHECK(f.op_deadline_ms == now + c.fallback_deadline_ms);  // 用兜底期限
}

// ------------------------------------------------------------------ 非幂等
// PICK/PLACE 在任何路径下都不得产生第二次发送
static void TestNonIdempotentNeverResent() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    ArmRequest r = NamedReq(ARM_REQ_PICK, "READY");
    ArmDecision d = arm_fsm_on_request(&f, &r, now);
    CHECK(d.action == ARM_ACT_SEND);
    CHECK(std::strcmp(d.line, "PICK:READY") == 0);
    CHECK(f.op_idempotent == 0);

    now += 301;
    d = arm_fsm_on_tick(&f, now);
    // 不重发 PICK，改为单发一次 STATUS 探查
    CHECK(d.action == ARM_ACT_SEND);
    CHECK(std::strcmp(d.line, "STATUS") == 0);
    CHECK(f.op_attempts == 1);                // 始终只发过一次 PICK
}

// 探查回 IDLE **不能**判「未受理」——动作可能已整条走完只是应答全丢
static void TestNonIdempotentProbeIdleIsUnknown() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    ArmRequest r = NamedReq(ARM_REQ_PLACE, "READY");
    arm_fsm_on_request(&f, &r, now);
    now += 301;
    arm_fsm_on_tick(&f, now);                 // 发 STATUS 探查
    now += 20;
    ArmDecision d = OnLine(&f, "POS:A=90;ST=IDLE;ATT=3F", now);
    CHECK(d.action == ARM_ACT_REPLY);
    CHECK(d.reply.ok == 0);
    CHECK(d.reply.code == ARM_CODE_ACCEPTANCE_UNKNOWN);   // 不是「未受理」
    CHECK(f.op_state == ARM_OP_ACCEPTANCE_UNKNOWN);
}

// 探查回 ST=MOVING ⇒ 可判已受理
static void TestNonIdempotentProbeMovingIsAccepted() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    ArmRequest r = NamedReq(ARM_REQ_PICK, "READY");
    arm_fsm_on_request(&f, &r, now);
    now += 301;
    arm_fsm_on_tick(&f, now);
    now += 20;
    ArmDecision d = OnLine(&f, "ST=MOVING", now);
    CHECK(d.action == ARM_ACT_REPLY);
    CHECK(d.reply.ok == 1);
    CHECK(d.reply.max_ms == -1);
    CHECK(f.op_state == ARM_OP_MOVING);
}

// ------------------------------------------------------------------ 即时拒绝
static void TestImmediateRejections() {
    struct { const char* line; ArmCode code; } cases[] = {
        {"ERROR:LIMIT", ARM_CODE_LIMIT},
        {"ERROR:COLLISION", ARM_CODE_COLLISION},
        {"ERROR:ESTOP", ARM_CODE_ESTOP},
        {"ERROR:RELAXED", ARM_CODE_RELAXED},
        {"ERROR:UNKNOWN_PRESET", ARM_CODE_UNKNOWN_PRESET},
        {"ERROR:INVALID_COMMAND", ARM_CODE_INVALID_COMMAND},
    };
    for (auto& tc : cases) {
        ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
        uint32_t now = 1000;
        BringUpReady(&f, &now);
        ArmRequest r = JointReq("A", 95);
        arm_fsm_on_request(&f, &r, now);
        now += 30;
        ArmDecision d = OnLine(&f, tc.line, now);
        CHECK(d.action == ARM_ACT_REPLY);
        CHECK(d.reply.ok == 0);
        CHECK(d.reply.code == tc.code);
        CHECK(f.op_state == ARM_OP_REJECTED);
        CHECK(arm_fsm_recovery_text(tc.code) != nullptr);
        CHECK(arm_fsm_recovery_text(tc.code)[0] != '\0');
    }
}

// ------------------------------------------------------------------ 参数白名单
// 不合规**直接拒绝、不做转义**。转义只会把问题藏起来：一个带换行的预设名
// 若被拼进命令行，就是第二条 UART 指令，足以绕过「RELAX 不向 agent 暴露」。
static void TestArgumentWhitelist() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    // ★ 命令注入：换行会把 RELAX 拼成第二条指令
    ArmRequest inject = NamedReq(ARM_REQ_MOVE_PRESET, "X\nRELAX");
    ArmDecision d = arm_fsm_on_request(&f, &inject, now);
    CHECK(d.action == ARM_ACT_REPLY);
    CHECK(d.reply.ok == 0);
    CHECK(d.reply.code == ARM_CODE_BAD_ARGUMENT);

    // 冒号会改变命令的参数切分
    ArmRequest colon = NamedReq(ARM_REQ_PICK, "A:B");
    d = arm_fsm_on_request(&f, &colon, now);
    CHECK(d.reply.code == ARM_CODE_BAD_ARGUMENT);

    // 回车同样是行分隔符
    ArmRequest cr = NamedReq(ARM_REQ_PLACE, "A\rB");
    d = arm_fsm_on_request(&f, &cr, now);
    CHECK(d.reply.code == ARM_CODE_BAD_ARGUMENT);

    // 空名与超长名（协议 §4.1：含 _ABOVE 不得越过 14）
    ArmRequest empty = NamedReq(ARM_REQ_MOVE_PRESET, "");
    d = arm_fsm_on_request(&f, &empty, now);
    CHECK(d.reply.code == ARM_CODE_BAD_ARGUMENT);
    ArmRequest longname = NamedReq(ARM_REQ_MOVE_PRESET, "ABCDEFGHIJKLMNOP");
    d = arm_fsm_on_request(&f, &longname, now);
    CHECK(d.reply.code == ARM_CODE_BAD_ARGUMENT);

    // 小写与空格也不放行（协议预设名是大写标识符）
    ArmRequest lower = NamedReq(ARM_REQ_MOVE_PRESET, "ready");
    d = arm_fsm_on_request(&f, &lower, now);
    CHECK(d.reply.code == ARM_CODE_BAD_ARGUMENT);

    // 关节字母越界
    ArmRequest badjoint = JointReq("Z", 90);
    d = arm_fsm_on_request(&f, &badjoint, now);
    CHECK(d.reply.code == ARM_CODE_BAD_ARGUMENT);

    // 角度越界（协议 §5.8 的解析层格式校验，不是关节限位）
    ArmRequest badangle = JointReq("A", 200);
    d = arm_fsm_on_request(&f, &badangle, now);
    CHECK(d.reply.code == ARM_CODE_BAD_ARGUMENT);

    // 合法值放行
    CHECK(arm_fsm_name_is_valid("READY") == 1);
    CHECK(arm_fsm_name_is_valid("READY_ABOVE") == 1);
    CHECK(arm_fsm_name_is_valid("BIN_1") == 1);
    CHECK(arm_fsm_joint_is_valid("A") == 1);
    CHECK(arm_fsm_joint_is_valid("F") == 1);
    CHECK(arm_fsm_joint_is_valid("G") == 0);

    ArmRequest good = NamedReq(ARM_REQ_MOVE_PRESET, "READY");
    d = arm_fsm_on_request(&f, &good, now);
    CHECK(d.action == ARM_ACT_SEND);
    CHECK(std::strcmp(d.line, "MOVE_PRESET:READY") == 0);
}

// 受理应答之前到达的三种终态**全部**丢弃，不只 DONE
static void TestAllTerminalsBeforeAckDiscarded() {
    const char* terminals[] = {"DONE", "OK:STOPPED", "ERROR:TIMEOUT"};
    for (auto* t : terminals) {
        ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
        uint32_t now = 1000;
        BringUpReady(&f, &now);
        ArmRequest r = JointReq("B", 75);
        arm_fsm_on_request(&f, &r, now);
        now += 10;
        ArmDecision d = OnLine(&f, t, now);
        CHECK(d.action == ARM_ACT_NONE);
        CHECK(f.op_state == ARM_OP_PENDING_ACCEPTANCE);   // 没被当成本条的结果
    }
}

// 清场时下位机报的不是干净的 IDLE（ESTOP / RELAXED / 畸形）⇒ fail-closed
static void TestQuiesceNonIdleFailsClosed() {
    const char* replies[] = {"ST=ESTOP", "ST=RELAXED", "POS:garbage"};
    for (auto* rep : replies) {
        ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
        uint32_t now = 1000;
        BringUpReady(&f, &now);
        ArmRequest r = JointReq("A", 100);
        arm_fsm_on_request(&f, &r, now);
        now += 30;
        OnLine(&f, "OK:500", now);
        now += 500 + 1000 + 1;
        arm_fsm_on_tick(&f, now);              // 转清场并发 STATUS
        now += 20;
        OnLine(&f, rep, now);
        CHECK(f.position_known == 0);          // 不当作干净收场
        CHECK(f.phase == ARM_PHASE_LOCKED_AFTER_RESET);
    }
}

// 没见过 READY 就收到 DONE：不得当作 boot 归位（证据链不完整）
static void TestDoneWithoutReadyStaysLocked() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    CHECK(f.phase == ARM_PHASE_WAIT_BOOT_DONE);
    CHECK(f.ready_seen == 0);

    ArmDecision d = OnLine(&f, "DONE", now);   // 孤儿 DONE，不是 boot 归位
    CHECK(d.action == ARM_ACT_NONE);
    CHECK(f.phase == ARM_PHASE_WAIT_BOOT_DONE);   // 没进验证
    CHECK(f.position_known == 0);

    BringUpLocked(&f, &now);
}

// ST / ATT 必须整字段比对：子串匹配会让 "ST=IDLE_BOGUS" fail-open
static void TestStrictStatusFields() {
    // boot 验证：欺骗性子串不得解锁
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    OnLine(&f, "READY:1.0:04", now);
    now += 100;
    OnLine(&f, "DONE", now);
    CHECK(f.phase == ARM_PHASE_VERIFY_BOOT_HOME);
    now += 20;
    OnLine(&f, "POS:A=90;ST=IDLE_BOGUS;ATT=3F", now);
    CHECK(f.phase == ARM_PHASE_LOCKED_AFTER_RESET);   // 不认
    CHECK(f.position_known == 0);

    // ATT 同理
    ArmFsm g; arm_fsm_init(&g, &c);
    uint32_t t = 1000;
    OnLine(&g, "READY:1.0:04", t);
    t += 100;
    OnLine(&g, "DONE", t);
    t += 20;
    OnLine(&g, "POS:A=90;ST=IDLE;ATT=3F0", t);
    CHECK(g.phase == ARM_PHASE_LOCKED_AFTER_RESET);
}

// 动作受理等待中收到 READY：等待者必须**立刻**拿到 RESET，
// 不能拖到 IO 层的兜底超时——那会突破主循环占用上界（宪法 III.2）
static void TestReadyWakesWaiters() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    ArmRequest r = JointReq("A", 95);
    arm_fsm_on_request(&f, &r, now);
    CHECK(f.op_state == ARM_OP_PENDING_ACCEPTANCE);
    now += 20;
    ArmDecision d = OnLine(&f, "READY:1.0:02", now);
    CHECK(d.action == ARM_ACT_REPLY);
    CHECK(d.reply.ok == 0);
    CHECK(d.reply.code == ARM_CODE_RESET);

    // 急停等待中同理
    ArmFsm g; arm_fsm_init(&g, &c);
    uint32_t t = 1000;
    BringUpReady(&g, &t);
    ArmRequest s = SimpleReq(ARM_REQ_STOP);
    arm_fsm_on_request(&g, &s, t);
    CHECK(g.stop_pending == 1);
    t += 20;
    d = OnLine(&g, "READY:1.0:02", t);
    CHECK(d.action == ARM_ACT_REPLY);
    CHECK(d.reply.code == ARM_CODE_RESET);
}

// 非幂等探查也没回音 ⇒ fail-closed 落锁，不得回到可运动状态（FR-021a）
static void TestNonIdempotentProbeSilenceFailsClosed() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    ArmRequest r = NamedReq(ARM_REQ_PICK, "READY");
    arm_fsm_on_request(&f, &r, now);
    now += 301;
    arm_fsm_on_tick(&f, now);          // 发 STATUS 探查
    now += 301;
    ArmDecision d = arm_fsm_on_tick(&f, now);   // 探查也没回音
    CHECK(d.action == ARM_ACT_REPLY);
    CHECK(d.reply.code == ARM_CODE_ACCEPTANCE_UNKNOWN);
    CHECK(f.position_known == 0);
    CHECK(f.phase == ARM_PHASE_LOCKED_AFTER_RESET);

    // 此后普通运动被挡，只放行 home
    ArmRequest j = JointReq("A", 95);
    d = arm_fsm_on_request(&f, &j, now);
    CHECK(d.action == ARM_ACT_REPLY);
    CHECK(d.reply.ok == 0);
}

// arm_moving 与操作生命周期一致：受理后为真，终态后为假
static void TestArmMovingTracksOperation() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);
    CHECK(f.arm_moving == 0);

    ArmRequest r = JointReq("A", 95);
    arm_fsm_on_request(&f, &r, now);
    now += 30;
    OnLine(&f, "OK:2000", now);
    CHECK(f.arm_moving == 1);          // 受理即在动

    now += 2000;
    OnLine(&f, "DONE", now);
    CHECK(f.arm_moving == 0);          // 终态后不再报在动
}

// 非法 speed 由决策器的白名单拒掉（工具层把未知档翻成 -1）
static void TestInvalidSpeedRejected() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    ArmRequest r = JointReq("A", 95);
    r.ramp_ms = -1;                    // 工具层对未知档返回 -1
    ArmDecision d = arm_fsm_on_request(&f, &r, now);
    CHECK(d.action == ARM_ACT_REPLY);
    CHECK(d.reply.code == ARM_CODE_BAD_ARGUMENT);

    r.ramp_ms = 200;                   // 超出协议允许的 [FAST, 100]
    d = arm_fsm_on_request(&f, &r, now);
    CHECK(d.reply.code == ARM_CODE_BAD_ARGUMENT);
}

// 本地互斥：动作在途时第二条动作立即 BUSY，不下发
static void TestLocalBusy() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    ArmRequest r1 = JointReq("A", 95);
    arm_fsm_on_request(&f, &r1, now);
    now += 30;
    OnLine(&f, "OK:2000", now);

    ArmRequest r2 = JointReq("B", 70);
    ArmDecision d = arm_fsm_on_request(&f, &r2, now);
    CHECK(d.action == ARM_ACT_REPLY);         // 不下发
    CHECK(d.reply.ok == 0);
    CHECK(d.reply.code == ARM_CODE_BUSY);
}

// ------------------------------------------------------------------ 急停
static void TestStopBypassesMutex() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    ArmRequest r = JointReq("A", 150);
    arm_fsm_on_request(&f, &r, now);
    now += 30;
    OnLine(&f, "OK:5000", now);
    uint32_t op_id = f.op_id;

    ArmRequest s = SimpleReq(ARM_REQ_STOP);
    ArmDecision d = arm_fsm_on_request(&f, &s, now);
    CHECK(d.action == ARM_ACT_SEND_STOP);     // 不被 BUSY 挡住
    CHECK(f.op_id == op_id);                  // 不占用操作槽
    CHECK(f.stop_pending == 1);
}

// 一个 OK:STOPPED ⇒ 报告已停止；三个 ⇒ 多余的被吃掉不污染后续
static void TestStopAckCounting() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    ArmRequest r = JointReq("A", 150);
    arm_fsm_on_request(&f, &r, now);
    now += 30;
    OnLine(&f, "OK:5000", now);

    ArmRequest s = SimpleReq(ARM_REQ_STOP);
    arm_fsm_on_request(&f, &s, now);
    now += 20;
    ArmDecision d = OnLine(&f, "OK:STOPPED", now);
    CHECK(d.action == ARM_ACT_REPLY);
    CHECK(d.reply.ok == 1);
    CHECK(f.op_state == ARM_OP_STOPPED);      // 在途动作同时收到终态

    // 三连发的另外两条回声
    now += 5;
    d = OnLine(&f, "OK:STOPPED", now);
    CHECK(d.action == ARM_ACT_NONE);          // 被吃掉，不再产生 REPLY
    now += 5;
    d = OnLine(&f, "OK:STOPPED", now);
    CHECK(d.action == ARM_ACT_NONE);

    // 后续新动作不受残留影响
    ArmRequest r2 = JointReq("B", 70);
    d = arm_fsm_on_request(&f, &r2, now);
    CHECK(d.action == ARM_ACT_SEND);
}

// 零 ACK ⇒ 必须报 LINK，不得声称已停止
static void TestStopNoAckIsLink() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    ArmRequest s = SimpleReq(ARM_REQ_STOP);
    ArmDecision d = arm_fsm_on_request(&f, &s, now);
    CHECK(d.action == ARM_ACT_SEND_STOP);

    now += 201;                                // 超过 stop_ack_wait_ms
    d = arm_fsm_on_tick(&f, now);
    CHECK(d.action == ARM_ACT_REPLY);
    CHECK(d.reply.ok == 0);
    CHECK(d.reply.code == ARM_CODE_LINK);
    CHECK(f.stop_pending == 0);
}

// ------------------------------------------------------------------ 清场 fence
// STATUS 回 IDLE ⇒ 递增世代、释放槽
static void TestQuiesceIdleReleases() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    ArmRequest r = JointReq("A", 100);
    arm_fsm_on_request(&f, &r, now);
    now += 30;
    OnLine(&f, "OK:500", now);
    uint32_t gen = f.operation_generation;

    now += 500 + 1000 + 1;
    ArmDecision d = arm_fsm_on_tick(&f, now);
    CHECK(d.action == ARM_ACT_DRAIN_AND_SEND);
    CHECK(std::strcmp(d.line, "STATUS") == 0);
    CHECK(f.op_state == ARM_OP_QUIESCING);
    CHECK(f.operation_generation == gen);            // ★ 此刻还不能递增

    now += 20;
    d = OnLine(&f, "POS:A=100;ST=IDLE;ATT=3F", now);
    CHECK(d.action == ARM_ACT_NONE);          // 无等待者
    CHECK(f.operation_generation != gen);            // 确认空闲后才递增
    ArmRequest r2 = JointReq("B", 70);
    d = arm_fsm_on_request(&f, &r2, now);
    CHECK(d.action == ARM_ACT_SEND);          // 槽已释放
}

// STATUS 回 ST=MOVING ⇒ 保留旧世代继续等终态，**不得死锁**
static void TestQuiesceMovingKeepsWaiting() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    ArmRequest r = JointReq("A", 100);
    arm_fsm_on_request(&f, &r, now);
    now += 30;
    OnLine(&f, "OK:500", now);
    uint32_t gen = f.operation_generation;

    now += 500 + 1000 + 1;
    arm_fsm_on_tick(&f, now);                 // 发 STATUS
    now += 20;
    ArmDecision d = OnLine(&f, "ST=MOVING", now);
    CHECK(d.action == ARM_ACT_NONE);          // 还在动，继续等
    CHECK(f.operation_generation == gen);            // ★ 世代未变，终态仍可被认领
    CHECK(f.op_state == ARM_OP_QUIESCING);

    now += 200;
    d = OnLine(&f, "DONE", now);     // 终态终于到了
    CHECK(d.action == ARM_ACT_NONE);          // 工具早已返回，这里只收尾
    CHECK(f.op_state == ARM_OP_DONE);
    ArmRequest r2 = JointReq("B", 70);
    d = arm_fsm_on_request(&f, &r2, now);
    CHECK(d.action == ARM_ACT_SEND);          // 槽已释放，没有死锁
}

// ------------------------------------------------------------------ 上电与复位
// WAIT_BOOT_DONE 期间零下行：连 PING/STATUS 都不发
static void TestBootWindowIsSilent() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    CHECK(f.phase == ARM_PHASE_WAIT_BOOT_DONE);

    ArmRequest st = SimpleReq(ARM_REQ_STATUS);
    ArmDecision d = arm_fsm_on_request(&f, &st, now);
    CHECK(d.action != ARM_ACT_SEND);          // 不下行

    ArmRequest stop = SimpleReq(ARM_REQ_STOP);
    d = arm_fsm_on_request(&f, &stop, now);
    CHECK(d.action != ARM_ACT_SEND_STOP);     // 急停也不发——此时靠切 12V
    CHECK(d.action == ARM_ACT_REPLY);
    CHECK(d.reply.ok == 0);

    ArmRequest home = SimpleReq(ARM_REQ_HOME);
    d = arm_fsm_on_request(&f, &home, now);
    CHECK(d.action != ARM_ACT_SEND);

    d = arm_fsm_on_tick(&f, now + 100);
    CHECK(d.action == ARM_ACT_NONE);
}

// VERIFY_BOOT_HOME 只发一次 STATUS
static void TestVerifyBootHomeSendsOneStatus() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    OnLine(&f, "READY:1.1:04", now);
    now += 100;
    ArmDecision d = OnLine(&f, "DONE", now);
    CHECK(d.action == ARM_ACT_SEND);
    CHECK(std::strcmp(d.line, "STATUS") == 0);

    d = arm_fsm_on_tick(&f, now + 50);
    CHECK(d.action == ARM_ACT_NONE);          // 不重复发
    CHECK(f.collision_guard == 1);            // 1.1 ⇒ 有自碰撞防护
}

// 始终未见 READY（board 启动晚于下位机）⇒ 超时落锁
static void TestNoReadyFallsToLocked() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpLocked(&f, &now);
}

// 没见过 READY ⇒ 窗口到期直接落锁，连 STATUS 都不问。
// board 启动晚于下位机时，下位机闲在**任意位置**也会回 IDLE;ATT=3F——那不是归位
// 证据。回退问 STATUS 的机制只对"见过 READY、DONE 丢了"成立，别越界。
static void TestNoReadyNeverUnlocksFromStatus() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000 + 4001;
    ArmDecision d = arm_fsm_on_tick(&f, now);
    CHECK(d.action == ARM_ACT_NONE);                 // ★ 不问
    CHECK(f.phase == ARM_PHASE_LOCKED_AFTER_RESET);

    now += 20;                                       // 就算状态自己送上门
    OnLine(&f, "POS:A=90,B=70,C=80,D=90,E=90,F=170;ST=IDLE;ATT=3F", now);
    CHECK(f.phase == ARM_PHASE_LOCKED_AFTER_RESET);  // 仍然锁定
    CHECK(f.position_known == 0);
}

// boot 窗口内没等到 DONE，随后连 STATUS 也没回音 ⇒ 落锁
static void TestBootWindowTimeoutFallsToLocked() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    OnLine(&f, "READY:1.0:04", now);
    BringUpLocked(&f, &now);
}

// 锁定态放行 HOME，挡住其余运动命令
static void TestLockedAllowsHomeOnly() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    OnLine(&f, "READY:1.0:04", now);
    BringUpLocked(&f, &now);

    ArmRequest j = JointReq("A", 95);
    ArmDecision d = arm_fsm_on_request(&f, &j, now);
    CHECK(d.action == ARM_ACT_REPLY);
    CHECK(d.reply.ok == 0);
    CHECK(d.reply.code == ARM_CODE_RESET);

    ArmRequest p = NamedReq(ARM_REQ_PICK, "READY");
    d = arm_fsm_on_request(&f, &p, now);
    CHECK(d.action == ARM_ACT_REPLY);
    CHECK(d.reply.ok == 0);

    ArmRequest home = SimpleReq(ARM_REQ_HOME);
    d = arm_fsm_on_request(&f, &home, now);
    CHECK(d.action == ARM_ACT_SEND);          // ★ HOME 必须放行
    CHECK(std::strcmp(d.line, "HOME") == 0);
    CHECK(f.phase == ARM_PHASE_RECOVERING_HOME);

    // STOP / PING / STATUS 也放行
    ArmRequest stop = SimpleReq(ARM_REQ_STOP);
    d = arm_fsm_on_request(&f, &stop, now);
    CHECK(d.action == ARM_ACT_SEND_STOP);
}

// 恢复归位成功 ⇒ 就绪
static void TestRecoveryHomeSuccess() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    OnLine(&f, "READY:1.0:04", now);
    BringUpLocked(&f, &now);

    ArmRequest home = SimpleReq(ARM_REQ_HOME);
    arm_fsm_on_request(&f, &home, now);
    now += 30;
    OnLine(&f, "OK:2600", now);
    now += 2000;
    OnLine(&f, "DONE", now);
    CHECK(f.phase == ARM_PHASE_READY);
    CHECK(f.position_known == 1);
}

// 恢复归位的四个失败出口都回到锁定
static void TestRecoveryHomeFailureExits() {
    const char* terminals[] = {"OK:STOPPED", "ERROR:TIMEOUT", "READY:1.0:02"};
    for (auto* t : terminals) {
        ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
        uint32_t now = 1000;
        OnLine(&f, "READY:1.0:04", now);
        BringUpLocked(&f, &now);

        ArmRequest home = SimpleReq(ARM_REQ_HOME);
        arm_fsm_on_request(&f, &home, now);
        now += 30;
        OnLine(&f, "OK:2600", now);
        CHECK(f.phase == ARM_PHASE_RECOVERING_HOME);
        now += 100;
        OnLine(&f, t, now);
        CHECK(f.position_known == 0);
        CHECK(f.phase == ARM_PHASE_LOCKED_AFTER_RESET ||
              f.phase == ARM_PHASE_WAIT_BOOT_DONE);   // 再次 READY 走 boot 路径
    }

    // 第四个出口：链路故障
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    OnLine(&f, "READY:1.0:04", now);
    BringUpLocked(&f, &now);
    ArmRequest home = SimpleReq(ARM_REQ_HOME);
    arm_fsm_on_request(&f, &home, now);
    for (int i = 0; i < 3; ++i) { now += 301; arm_fsm_on_tick(&f, now); }
    CHECK(f.position_known == 0);
    CHECK(f.phase == ARM_PHASE_LOCKED_AFTER_RESET);
}

// 常态（就绪）下的 HOME 被停止/超时，同样使位置不可信
static void TestNormalHomeFailureDegrades() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);
    CHECK(f.position_known == 1);

    ArmRequest home = SimpleReq(ARM_REQ_HOME);
    arm_fsm_on_request(&f, &home, now);
    now += 30;
    OnLine(&f, "OK:2600", now);
    now += 100;
    OnLine(&f, "OK:STOPPED", now);   // 归位途中被停
    CHECK(f.position_known == 0);             // ★ 下位机已 detach，位置不再可信
    CHECK(f.phase == ARM_PHASE_LOCKED_AFTER_RESET);
}

// ------------------------------------------------------------------ 状态查询
static void TestStatusDoesNotPollWhileMoving() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);
    CHECK(arm_fsm_status_needs_uart(&f) == 1);   // 空闲时可查

    ArmRequest r = JointReq("A", 95);
    arm_fsm_on_request(&f, &r, now);
    now += 30;
    OnLine(&f, "OK:3000", now);
    CHECK(f.op_state == ARM_OP_MOVING);

    CHECK(arm_fsm_status_needs_uart(&f) == 0);   // ★ 动作中返回缓存
    ArmRequest st = SimpleReq(ARM_REQ_STATUS);
    ArmDecision d = arm_fsm_on_request(&f, &st, now);
    CHECK(d.action == ARM_ACT_REPLY);            // 不产生任何发送
}

// 空闲时的 status 查询会打串口——回报到达时**必须**产生 REPLY，
// 否则工具调用会一直干等到超时并误报 LINK。
static void TestIdleStatusQueryGetsReply() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);
    CHECK(arm_fsm_status_needs_uart(&f) == 1);

    ArmRequest st = SimpleReq(ARM_REQ_STATUS);
    ArmDecision d = arm_fsm_on_request(&f, &st, now);
    CHECK(d.action == ARM_ACT_SEND);
    CHECK(std::strcmp(d.line, "STATUS") == 0);

    now += 20;
    d = OnLine(&f, "POS:A=90,B=70,C=80,D=90,E=90,F=170;ST=IDLE;ATT=3F", now);
    CHECK(d.action == ARM_ACT_REPLY);            // ★ 有人在等，必须应答
    CHECK(d.reply.ok == 1);
    CHECK(f.has_pos == 1);
    CHECK(f.joints[0] == 90 && f.joints[5] == 170);

    // 回报消费掉后不应再重复产生 REPLY
    now += 10;
    d = OnLine(&f, "POS:A=90;ST=IDLE;ATT=3F", now);
    CHECK(d.action == ARM_ACT_NONE);
}

// POS 行解析
static void TestParsePos() {
    int j[6] = {0};
    CHECK(arm_fsm_parse_pos("POS:A=90,B=70,C=80,D=90,E=90,F=170;ST=IDLE;ATT=3F", j) == 1);
    CHECK(j[0] == 90 && j[1] == 70 && j[2] == 80 && j[3] == 90 && j[4] == 90 && j[5] == 170);
    CHECK(arm_fsm_parse_pos("ST=MOVING", j) == 0);
    CHECK(arm_fsm_parse_pos("POS:A=1;ST=IDLE", j) == 0);   // 不足六个
}

// ------------------------------------------------------------------ 版本与映射
// COLLISION 在两个版本下都能识别并映射（v1.0 只是不会收到它）
static void TestCollisionMappedOnBothVersions() {
    for (const char* ready : {"READY:1.0:04", "READY:1.1:04"}) {
        ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
        uint32_t now = 1000;
        OnLine(&f, ready, now);
        now += 100;
        OnLine(&f, "DONE", now);
        now += 20;
        OnLine(&f, "POS:A=90;ST=IDLE;ATT=3F", now);
        CHECK(f.phase == ARM_PHASE_READY);

        ArmRequest r = JointReq("C", 40);
        arm_fsm_on_request(&f, &r, now);
        now += 30;
        ArmDecision d = OnLine(&f, "ERROR:COLLISION", now);
        CHECK(d.reply.code == ARM_CODE_COLLISION);
        CHECK(std::strstr(arm_fsm_recovery_text(ARM_CODE_COLLISION), "extend") != nullptr ||
              std::strstr(arm_fsm_recovery_text(ARM_CODE_COLLISION), "Extend") != nullptr);
    }
}

static void TestRecoveryTextComplete() {
    ArmCode all[] = {ARM_CODE_LIMIT, ARM_CODE_COLLISION, ARM_CODE_BUSY, ARM_CODE_ESTOP,
                     ARM_CODE_RELAXED, ARM_CODE_UNKNOWN_PRESET, ARM_CODE_INVALID_COMMAND,
                     ARM_CODE_TIMEOUT, ARM_CODE_LINK, ARM_CODE_RESET,
                     ARM_CODE_ACCEPTANCE_UNKNOWN};
    for (ArmCode c : all) {
        const char* t = arm_fsm_recovery_text(c);
        CHECK(t != nullptr);
        CHECK(t[0] != '\0');
        // code 名与 recovery 是同一枚举上的两张平行表，一起断言才不会只补一半：
        // 漏一个分支时 agent 拿到的是空错误码，而空字符串不会让别处失败。
        const char* n = arm_fsm_code_name(c);
        CHECK(n != nullptr);
        CHECK(n[0] != '\0');
    }
    CHECK(std::strcmp(arm_fsm_code_name(ARM_CODE_BAD_ARGUMENT), "BAD_ARGUMENT") == 0);
    CHECK(std::strcmp(arm_fsm_code_name(ARM_CODE_TIMEOUT), "TIMEOUT") == 0);
}

// 命令行拼装
static void TestCommandRendering() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    struct { ArmRequest req; const char* expect; } cases[] = {
        {JointReq("A", 95), "JOINT:A:95"},
        {SimpleReq(ARM_REQ_GRIP_OPEN), "GRIP_OPEN"},
        {SimpleReq(ARM_REQ_GRIP_CLOSE), "GRIP_CLOSE"},
        {NamedReq(ARM_REQ_MOVE_PRESET, "READY"), "MOVE_PRESET:READY"},
        {NamedReq(ARM_REQ_PICK, "BIN"), "PICK:BIN"},
        {NamedReq(ARM_REQ_PLACE, "BIN"), "PLACE:BIN"},
    };
    for (auto& tc : cases) {
        ArmFsm g = f;   // 从同一就绪态出发
        ArmDecision d = arm_fsm_on_request(&g, &tc.req, now);
        CHECK(d.action == ARM_ACT_SEND);
        CHECK(std::strcmp(d.line, tc.expect) == 0);
    }

    // 带速度档的 JOINT
    ArmFsm g = f;
    ArmRequest r = JointReq("B", 70);
    r.ramp_ms = 50;
    ArmDecision d = arm_fsm_on_request(&g, &r, now);
    CHECK(std::strcmp(d.line, "JOINT:B:70:50") == 0);
}

int main() {
    TestClassify();
    TestPosRejectsMalformedValues();
    TestStatusKeysNeedFieldBoundary();
    TestJointRejectsMultiChar();
    TestTimeoutForfeitsPosition();
    TestAcceptAndDone();
    TestTerminalBeforeAckDiscarded();
    TestStaleTerminalAcrossGeneration();
    TestStaleTerminalAcrossEpoch();
    TestRetryExhausted();
    TestBusyInferredAcceptance();
    TestNonIdempotentNeverResent();
    TestNonIdempotentProbeIdleIsUnknown();
    TestNonIdempotentProbeMovingIsAccepted();
    TestImmediateRejections();
    TestArgumentWhitelist();
    TestDoneWithoutReadyStaysLocked();
    TestStrictStatusFields();
    TestReadyWakesWaiters();
    TestNonIdempotentProbeSilenceFailsClosed();
    TestArmMovingTracksOperation();
    TestInvalidSpeedRejected();
    TestAllTerminalsBeforeAckDiscarded();
    TestQuiesceNonIdleFailsClosed();
    TestLocalBusy();
    TestStopBypassesMutex();
    TestStopAckCounting();
    TestStopNoAckIsLink();
    TestQuiesceIdleReleases();
    TestQuiesceMovingKeepsWaiting();
    TestBootWindowIsSilent();
    TestVerifyBootHomeSendsOneStatus();
    TestNoReadyFallsToLocked();
    TestBootWindowTimeoutFallsToLocked();
    TestBootWindowFallsBackToStatus();
    TestNoReadyNeverUnlocksFromStatus();
    TestLockedAllowsHomeOnly();
    TestRecoveryHomeSuccess();
    TestRecoveryHomeFailureExits();
    TestNormalHomeFailureDegrades();
    TestStatusDoesNotPollWhileMoving();
    TestIdleStatusQueryGetsReply();
    TestParsePos();
    TestVersionAndCollisionGuard();
    TestCollisionMappedOnBothVersions();
    TestRecoveryTextComplete();
    TestCommandRendering();

    std::printf("arm_link_fsm self-check: %d checks passed\n", g_checks);
    return 0;
}
