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

// 把 fsm 推到「就绪、位置已知」——多数用例的起点。
static void BringUpReady(ArmFsm* f, uint32_t* now) {
    ArmDecision d = arm_fsm_on_line(f, "READY:1.0:04", *now);
    CHECK(d.action == ARM_ACT_NONE);            // 收到 READY 不下行
    CHECK(f->phase == ARM_PHASE_WAIT_BOOT_DONE);
    *now += 100;
    d = arm_fsm_on_line(f, "DONE", *now);       // boot 归位完成
    CHECK(d.action == ARM_ACT_SEND);            // 转 VERIFY，内部发一次 STATUS
    CHECK(std::strcmp(d.line, "STATUS") == 0);
    CHECK(f->phase == ARM_PHASE_VERIFY_BOOT_HOME);
    *now += 20;
    d = arm_fsm_on_line(f, "POS:A=90,B=70,C=80,D=90,E=90,F=170;ST=IDLE;ATT=3F", *now);
    CHECK(f->phase == ARM_PHASE_READY);
    CHECK(f->position_known == 1);
}

static ArmRequest JointReq(char j, int deg) {
    ArmRequest r;
    std::memset(&r, 0, sizeof(r));
    r.kind = ARM_REQ_JOINT;
    r.joint = j;
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
}

// ------------------------------------------------------------------ 正常受理与终态
static void TestAcceptAndDone() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    ArmRequest r = JointReq('A', 95);
    ArmDecision d = arm_fsm_on_request(&f, &r, now);
    CHECK(d.action == ARM_ACT_SEND);
    CHECK(std::strcmp(d.line, "JOINT:A:95") == 0);
    CHECK(f.op_state == ARM_OP_PENDING_ACCEPTANCE);

    now += 50;
    d = arm_fsm_on_line(&f, "OK:4200", now);
    CHECK(d.action == ARM_ACT_REPLY);
    CHECK(d.reply.ok == 1);
    CHECK(d.reply.max_ms == 4200);
    CHECK(d.reply.operation_id == f.op_id);
    CHECK(f.op_state == ARM_OP_MOVING);

    now += 4000;
    d = arm_fsm_on_line(&f, "DONE", now);
    CHECK(f.op_state == ARM_OP_DONE);
    CHECK(f.position_known == 1);
}

// 即时应答**之前**到达的终态属于上一条，必须丢弃（协议 §4.3.2）
static void TestTerminalBeforeAckDiscarded() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    ArmRequest r = JointReq('B', 75);
    arm_fsm_on_request(&f, &r, now);
    CHECK(f.op_state == ARM_OP_PENDING_ACCEPTANCE);

    now += 10;
    ArmDecision d = arm_fsm_on_line(&f, "DONE", now);   // 迟到的上一条应答
    CHECK(d.action == ARM_ACT_NONE);
    CHECK(f.op_state == ARM_OP_PENDING_ACCEPTANCE);     // 没被误认领
}

// 同一纪元内，旧操作放弃后到达的 DONE 不得被新操作冒领
static void TestStaleTerminalAcrossGeneration() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    // 操作 1 受理后一直不给终态，直到 deadline 到期
    ArmRequest r1 = JointReq('A', 100);
    arm_fsm_on_request(&f, &r1, now);
    now += 50;
    arm_fsm_on_line(&f, "OK:1000", now);
    CHECK(f.op_state == ARM_OP_MOVING);
    uint32_t gen1 = f.operation_generation;

    now += 1000 + 1000 + 1;            // max_ms + grace 到期
    ArmDecision d = arm_fsm_on_tick(&f, now);
    CHECK(d.action == ARM_ACT_SEND);   // deadline 核对：单发一次 STATUS
    CHECK(std::strcmp(d.line, "STATUS") == 0);
    now += 20;
    d = arm_fsm_on_line(&f, "POS:A=100;ST=IDLE;ATT=3F", now);
    // 工具在受理时就已返回，此刻没有等待者——只更新状态，不产生 REPLY
    CHECK(d.action == ARM_ACT_NONE);
    CHECK(f.op_state == ARM_OP_LINK_FAILED);
    CHECK(f.op_code == ARM_CODE_LINK);      // 下位机已空闲却没见终态 ⇒ 链路故障
    CHECK(f.operation_generation != gen1);         // 清场后世代已推进

    // 操作 2
    ArmRequest r2 = JointReq('B', 80);
    d = arm_fsm_on_request(&f, &r2, now);
    CHECK(d.action == ARM_ACT_SEND);
    now += 30;
    arm_fsm_on_line(&f, "OK:800", now);
    CHECK(f.op_state == ARM_OP_MOVING);

    // 操作 1 的迟到 DONE 现在才到 —— 世代不匹配，必须丢弃
    now += 10;
    d = arm_fsm_on_line(&f, "DONE", now);
    CHECK(d.action == ARM_ACT_NONE);
    CHECK(f.op_state == ARM_OP_MOVING);     // 操作 2 没被冒领
}

// 跨纪元（下位机复位过）的终态同样丢弃
static void TestStaleTerminalAcrossEpoch() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    ArmRequest r = JointReq('C', 60);
    arm_fsm_on_request(&f, &r, now);
    now += 50;
    arm_fsm_on_line(&f, "OK:2000", now);
    CHECK(f.op_state == ARM_OP_MOVING);

    now += 100;
    arm_fsm_on_line(&f, "READY:1.0:02", now);   // 对端复位
    CHECK(f.op_state == ARM_OP_RESET);
    CHECK(f.position_known == 0);
    CHECK(f.phase == ARM_PHASE_WAIT_BOOT_DONE);
    uint32_t epoch = f.link_epoch;

    // 复位前那条动作的 DONE 现在才到
    now += 10;
    ArmDecision d = arm_fsm_on_line(&f, "DONE", now);
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

    ArmRequest r = JointReq('D', 100);
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

    ArmRequest r = JointReq('A', 120);
    arm_fsm_on_request(&f, &r, now);
    now += 301;
    arm_fsm_on_tick(&f, now);                 // 重发
    now += 20;
    ArmDecision d = arm_fsm_on_line(&f, "BUSY", now);
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
    ArmDecision d = arm_fsm_on_line(&f, "POS:A=90;ST=IDLE;ATT=3F", now);
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
    ArmDecision d = arm_fsm_on_line(&f, "ST=MOVING", now);
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
        ArmRequest r = JointReq('A', 200);
        arm_fsm_on_request(&f, &r, now);
        now += 30;
        ArmDecision d = arm_fsm_on_line(&f, tc.line, now);
        CHECK(d.action == ARM_ACT_REPLY);
        CHECK(d.reply.ok == 0);
        CHECK(d.reply.code == tc.code);
        CHECK(f.op_state == ARM_OP_REJECTED);
        CHECK(arm_fsm_recovery_text(tc.code) != nullptr);
        CHECK(arm_fsm_recovery_text(tc.code)[0] != '\0');
    }
}

// 本地互斥：动作在途时第二条动作立即 BUSY，不下发
static void TestLocalBusy() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    ArmRequest r1 = JointReq('A', 95);
    arm_fsm_on_request(&f, &r1, now);
    now += 30;
    arm_fsm_on_line(&f, "OK:2000", now);

    ArmRequest r2 = JointReq('B', 70);
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

    ArmRequest r = JointReq('A', 150);
    arm_fsm_on_request(&f, &r, now);
    now += 30;
    arm_fsm_on_line(&f, "OK:5000", now);
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

    ArmRequest r = JointReq('A', 150);
    arm_fsm_on_request(&f, &r, now);
    now += 30;
    arm_fsm_on_line(&f, "OK:5000", now);

    ArmRequest s = SimpleReq(ARM_REQ_STOP);
    arm_fsm_on_request(&f, &s, now);
    now += 20;
    ArmDecision d = arm_fsm_on_line(&f, "OK:STOPPED", now);
    CHECK(d.action == ARM_ACT_REPLY);
    CHECK(d.reply.ok == 1);
    CHECK(f.op_state == ARM_OP_STOPPED);      // 在途动作同时收到终态

    // 三连发的另外两条回声
    now += 5;
    d = arm_fsm_on_line(&f, "OK:STOPPED", now);
    CHECK(d.action == ARM_ACT_NONE);          // 被吃掉，不再产生 REPLY
    now += 5;
    d = arm_fsm_on_line(&f, "OK:STOPPED", now);
    CHECK(d.action == ARM_ACT_NONE);

    // 后续新动作不受残留影响
    ArmRequest r2 = JointReq('B', 70);
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

    ArmRequest r = JointReq('A', 100);
    arm_fsm_on_request(&f, &r, now);
    now += 30;
    arm_fsm_on_line(&f, "OK:500", now);
    uint32_t gen = f.operation_generation;

    now += 500 + 1000 + 1;
    ArmDecision d = arm_fsm_on_tick(&f, now);
    CHECK(d.action == ARM_ACT_SEND);
    CHECK(std::strcmp(d.line, "STATUS") == 0);
    CHECK(f.op_state == ARM_OP_QUIESCING);
    CHECK(f.operation_generation == gen);            // ★ 此刻还不能递增

    now += 20;
    d = arm_fsm_on_line(&f, "POS:A=100;ST=IDLE;ATT=3F", now);
    CHECK(d.action == ARM_ACT_NONE);          // 无等待者
    CHECK(f.operation_generation != gen);            // 确认空闲后才递增
    ArmRequest r2 = JointReq('B', 70);
    d = arm_fsm_on_request(&f, &r2, now);
    CHECK(d.action == ARM_ACT_SEND);          // 槽已释放
}

// STATUS 回 ST=MOVING ⇒ 保留旧世代继续等终态，**不得死锁**
static void TestQuiesceMovingKeepsWaiting() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    ArmRequest r = JointReq('A', 100);
    arm_fsm_on_request(&f, &r, now);
    now += 30;
    arm_fsm_on_line(&f, "OK:500", now);
    uint32_t gen = f.operation_generation;

    now += 500 + 1000 + 1;
    arm_fsm_on_tick(&f, now);                 // 发 STATUS
    now += 20;
    ArmDecision d = arm_fsm_on_line(&f, "ST=MOVING", now);
    CHECK(d.action == ARM_ACT_NONE);          // 还在动，继续等
    CHECK(f.operation_generation == gen);            // ★ 世代未变，终态仍可被认领
    CHECK(f.op_state == ARM_OP_QUIESCING);

    now += 200;
    d = arm_fsm_on_line(&f, "DONE", now);     // 终态终于到了
    CHECK(d.action == ARM_ACT_NONE);          // 工具早已返回，这里只收尾
    CHECK(f.op_state == ARM_OP_DONE);
    ArmRequest r2 = JointReq('B', 70);
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
    arm_fsm_on_line(&f, "READY:1.1:04", now);
    now += 100;
    ArmDecision d = arm_fsm_on_line(&f, "DONE", now);
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
    ArmDecision d = arm_fsm_on_tick(&f, now + 4001);
    CHECK(f.phase == ARM_PHASE_LOCKED_AFTER_RESET);
    CHECK(f.position_known == 0);
    (void)d;
}

// boot 窗口内没等到 DONE ⇒ 落锁
static void TestBootWindowTimeoutFallsToLocked() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    arm_fsm_on_line(&f, "READY:1.0:04", now);
    arm_fsm_on_tick(&f, now + 4001);
    CHECK(f.phase == ARM_PHASE_LOCKED_AFTER_RESET);
    CHECK(f.position_known == 0);
}

// 锁定态放行 HOME，挡住其余运动命令
static void TestLockedAllowsHomeOnly() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    arm_fsm_on_line(&f, "READY:1.0:04", now);
    arm_fsm_on_tick(&f, now + 4001);
    now += 4001;
    CHECK(f.phase == ARM_PHASE_LOCKED_AFTER_RESET);

    ArmRequest j = JointReq('A', 95);
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
    arm_fsm_on_line(&f, "READY:1.0:04", now);
    arm_fsm_on_tick(&f, now + 4001);
    now += 4001;

    ArmRequest home = SimpleReq(ARM_REQ_HOME);
    arm_fsm_on_request(&f, &home, now);
    now += 30;
    arm_fsm_on_line(&f, "OK:2600", now);
    now += 2000;
    arm_fsm_on_line(&f, "DONE", now);
    CHECK(f.phase == ARM_PHASE_READY);
    CHECK(f.position_known == 1);
}

// 恢复归位的四个失败出口都回到锁定
static void TestRecoveryHomeFailureExits() {
    const char* terminals[] = {"OK:STOPPED", "ERROR:TIMEOUT", "READY:1.0:02"};
    for (auto* t : terminals) {
        ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
        uint32_t now = 1000;
        arm_fsm_on_line(&f, "READY:1.0:04", now);
        arm_fsm_on_tick(&f, now + 4001);
        now += 4001;

        ArmRequest home = SimpleReq(ARM_REQ_HOME);
        arm_fsm_on_request(&f, &home, now);
        now += 30;
        arm_fsm_on_line(&f, "OK:2600", now);
        CHECK(f.phase == ARM_PHASE_RECOVERING_HOME);
        now += 100;
        arm_fsm_on_line(&f, t, now);
        CHECK(f.position_known == 0);
        CHECK(f.phase == ARM_PHASE_LOCKED_AFTER_RESET ||
              f.phase == ARM_PHASE_WAIT_BOOT_DONE);   // 再次 READY 走 boot 路径
    }

    // 第四个出口：链路故障
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    arm_fsm_on_line(&f, "READY:1.0:04", now);
    arm_fsm_on_tick(&f, now + 4001);
    now += 4001;
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
    arm_fsm_on_line(&f, "OK:2600", now);
    now += 100;
    arm_fsm_on_line(&f, "OK:STOPPED", now);   // 归位途中被停
    CHECK(f.position_known == 0);             // ★ 下位机已 detach，位置不再可信
    CHECK(f.phase == ARM_PHASE_LOCKED_AFTER_RESET);
}

// ------------------------------------------------------------------ 状态查询
static void TestStatusDoesNotPollWhileMoving() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);
    CHECK(arm_fsm_status_needs_uart(&f) == 1);   // 空闲时可查

    ArmRequest r = JointReq('A', 95);
    arm_fsm_on_request(&f, &r, now);
    now += 30;
    arm_fsm_on_line(&f, "OK:3000", now);
    CHECK(f.op_state == ARM_OP_MOVING);

    CHECK(arm_fsm_status_needs_uart(&f) == 0);   // ★ 动作中返回缓存
    ArmRequest st = SimpleReq(ARM_REQ_STATUS);
    ArmDecision d = arm_fsm_on_request(&f, &st, now);
    CHECK(d.action == ARM_ACT_REPLY);            // 不产生任何发送
}

// ------------------------------------------------------------------ 版本与映射
static void TestVersionAndCollisionGuard() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    arm_fsm_on_line(&f, "READY:1.0:04", now);
    CHECK(f.fw_major == 1 && f.fw_minor == 0);
    CHECK(f.collision_guard == 0);

    ArmFsm g; arm_fsm_init(&g, &c);
    arm_fsm_on_line(&g, "PONG:1.1", now);
    CHECK(g.fw_major == 1 && g.fw_minor == 1);
    CHECK(g.collision_guard == 1);
}

// COLLISION 在两个版本下都能识别并映射（v1.0 只是不会收到它）
static void TestCollisionMappedOnBothVersions() {
    for (const char* ready : {"READY:1.0:04", "READY:1.1:04"}) {
        ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
        uint32_t now = 1000;
        arm_fsm_on_line(&f, ready, now);
        now += 100;
        arm_fsm_on_line(&f, "DONE", now);
        now += 20;
        arm_fsm_on_line(&f, "POS:A=90;ST=IDLE;ATT=3F", now);
        CHECK(f.phase == ARM_PHASE_READY);

        ArmRequest r = JointReq('C', 40);
        arm_fsm_on_request(&f, &r, now);
        now += 30;
        ArmDecision d = arm_fsm_on_line(&f, "ERROR:COLLISION", now);
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
    }
}

// 命令行拼装
static void TestCommandRendering() {
    ArmFsm f; ArmFsmConfig c = TestCfg(); arm_fsm_init(&f, &c);
    uint32_t now = 1000;
    BringUpReady(&f, &now);

    struct { ArmRequest req; const char* expect; } cases[] = {
        {JointReq('A', 95), "JOINT:A:95"},
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
    ArmRequest r = JointReq('B', 70);
    r.ramp_ms = 50;
    ArmDecision d = arm_fsm_on_request(&g, &r, now);
    CHECK(std::strcmp(d.line, "JOINT:B:70:50") == 0);
}

int main() {
    TestClassify();
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
    TestLockedAllowsHomeOnly();
    TestRecoveryHomeSuccess();
    TestRecoveryHomeFailureExits();
    TestNormalHomeFailureDegrades();
    TestStatusDoesNotPollWhileMoving();
    TestVersionAndCollisionGuard();
    TestCollisionMappedOnBothVersions();
    TestRecoveryTextComplete();
    TestCommandRendering();

    std::printf("arm_link_fsm self-check: %d checks passed\n", g_checks);
    return 0;
}
