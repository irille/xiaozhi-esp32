#include "arm_link_fsm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- 小工具

int arm_fsm_has_prefix(const char* s, const char* prefix) {
    if (!s || !prefix) return 0;
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

static int is_digits(const char* s) {
    if (!s || !*s) return 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return 0;
    }
    return 1;
}

// 时钟比较一律走差值：uint32_t 毫秒约 49.7 天回绕，绝对比较会在回绕点附近
// 提前或延后判定超时。
static int elapsed_past(uint32_t now, uint32_t since, uint32_t span) {
    return (uint32_t)(now - since) > span;
}
static int deadline_passed(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) > 0;
}

ArmLineKind arm_fsm_classify(const char* line) {
    if (!line || !line[0]) return ARM_LINE_MALFORMED;
    if (strcmp(line, "OK:STOPPED") == 0) return ARM_LINE_OK_STOPPED;
    if (strcmp(line, "OK:RELAXED") == 0) return ARM_LINE_OK_RELAXED;
    if (arm_fsm_has_prefix(line, "OK:") && is_digits(line + 3)) return ARM_LINE_OK_MAXMS;
    // 严格相等，不用前缀：截断出来的 "DONE..." 之类不得被当成完成。
    if (strcmp(line, "DONE") == 0) return ARM_LINE_DONE;
    if (strcmp(line, "BUSY") == 0) return ARM_LINE_BUSY;
    // 带载荷的行必须**真的带载荷**（治具 link_fsm.c 的判据，理由同样是"误码帧不得
    // 混成合法应答"）。裸前缀放行的后果很具体：截断出来的 "ERROR:" 会一路走到
    // error_code_of 落进 INVALID_COMMAND，一个误码帧就变成了一次业务拒绝。
    if (arm_fsm_has_prefix(line, "ERROR:")) return line[6] ? ARM_LINE_ERROR : ARM_LINE_MALFORMED;
    if (arm_fsm_has_prefix(line, "PONG:")) return line[5] ? ARM_LINE_PONG : ARM_LINE_MALFORMED;
    if (arm_fsm_has_prefix(line, "POS:")) return line[4] ? ARM_LINE_POS : ARM_LINE_MALFORMED;
    if (arm_fsm_has_prefix(line, "ST=")) return line[3] ? ARM_LINE_ST : ARM_LINE_MALFORMED;
    if (arm_fsm_has_prefix(line, "READY:")) return line[6] ? ARM_LINE_READY : ARM_LINE_MALFORMED;
    return ARM_LINE_MALFORMED;
}

static ArmCode error_code_of(const char* line) {
    const char* p = line + 6;  // 跳过 "ERROR:"
    if (strcmp(p, "LIMIT") == 0) return ARM_CODE_LIMIT;
    if (strcmp(p, "COLLISION") == 0) return ARM_CODE_COLLISION;
    if (strcmp(p, "BUSY") == 0) return ARM_CODE_BUSY;
    if (strcmp(p, "ESTOP") == 0) return ARM_CODE_ESTOP;
    if (strcmp(p, "RELAXED") == 0) return ARM_CODE_RELAXED;
    if (strcmp(p, "UNKNOWN_PRESET") == 0) return ARM_CODE_UNKNOWN_PRESET;
    if (strcmp(p, "TIMEOUT") == 0) return ARM_CODE_TIMEOUT;
    if (strcmp(p, "RESET") == 0) return ARM_CODE_RESET;
    return ARM_CODE_INVALID_COMMAND;
}

const char* arm_fsm_recovery_text(ArmCode code) {
    switch (code) {
        case ARM_CODE_LIMIT:
            return "Pick an angle inside that joint's range.";
        case ARM_CODE_COLLISION:
            return "That path would hit the arm itself. Extend the elbow or wrist first, "
                   "or approach from another pose.";
        case ARM_CODE_BUSY:
            return "Another motion is still running. Poll self.arm.status until it reaches "
                   "a terminal state.";
        case ARM_CODE_ESTOP:
            return "Emergency stop is latched. Call self.arm.home to clear it.";
        case ARM_CODE_RELAXED:
            return "The arm is relaxed and has lost its position. Call self.arm.home first.";
        case ARM_CODE_UNKNOWN_PRESET:
            return "No preset by that name. Check the available preset names.";
        case ARM_CODE_INVALID_COMMAND:
            return "The arguments were rejected. Check joint letter, angle and speed.";
        case ARM_CODE_BAD_ARGUMENT:
            return "Malformed argument. Joint must be A-F and preset names must be 1-14 "
                   "characters of A-Z, 0-9 or underscore.";
        case ARM_CODE_TIMEOUT:
            return "The motion ran past its time budget and the controller latched an "
                   "emergency stop. Call self.arm.home to clear it.";
        case ARM_CODE_LINK:
            return "Lost the serial link to the arm controller. The motion result is unknown.";
        case ARM_CODE_RESET:
            return "The arm controller reset and lost its position. Call self.arm.home to recover.";
        case ARM_CODE_ACCEPTANCE_UNKNOWN:
            return "Could not confirm whether the arm accepted this command. Check "
                   "self.arm.status before retrying - do not repeat a pick or place blindly.";
        default:
            return "";
    }
}

// 与 recovery_text 是同一个枚举上的两张平行表，放一起才不会只补一半——
// recovery 那半有逐码断言拦着，code 名这半原先在工具层、零覆盖，漏一个分支
// agent 拿到的就是空错误码。
const char* arm_fsm_code_name(ArmCode code) {
    switch (code) {
        case ARM_CODE_LIMIT: return "LIMIT";
        case ARM_CODE_COLLISION: return "COLLISION";
        case ARM_CODE_BUSY: return "BUSY";
        case ARM_CODE_ESTOP: return "ESTOP";
        case ARM_CODE_RELAXED: return "RELAXED";
        case ARM_CODE_UNKNOWN_PRESET: return "UNKNOWN_PRESET";
        case ARM_CODE_INVALID_COMMAND: return "INVALID_COMMAND";
        case ARM_CODE_BAD_ARGUMENT: return "BAD_ARGUMENT";
        case ARM_CODE_TIMEOUT: return "TIMEOUT";
        case ARM_CODE_LINK: return "LINK";
        case ARM_CODE_RESET: return "RESET";
        case ARM_CODE_ACCEPTANCE_UNKNOWN: return "ACCEPTANCE_UNKNOWN";
        default: return "";
    }
}

const char* arm_fsm_op_state_name(ArmOpState st) {
    switch (st) {
        case ARM_OP_IDLE: return "idle";
        case ARM_OP_PENDING_ACCEPTANCE: return "pending_acceptance";
        case ARM_OP_MOVING: return "moving";
        case ARM_OP_QUIESCING: return "moving";  // 对 agent 仍表现为在动
        case ARM_OP_DONE: return "done";
        case ARM_OP_STOPPED: return "stopped";
        case ARM_OP_TIMED_OUT: return "timed_out";
        case ARM_OP_RESET: return "reset";
        case ARM_OP_LINK_FAILED: return "link_failed";
        case ARM_OP_REJECTED: return "rejected";
        case ARM_OP_ACCEPTANCE_UNKNOWN: return "acceptance_unknown";
        default: return "idle";
    }
}

// ---------------------------------------------------------------- 参数白名单
// **不做转义**：不合规直接拒绝。转义只会把问题藏起来——一个带换行的预设名若被
// 拼进命令行，就是第二条 UART 指令，足以绕过"RELAX 不向 agent 暴露"这条边界。

int arm_fsm_joint_is_valid(char joint) {
    return joint >= 'A' && joint <= 'F';
}

int arm_fsm_name_is_valid(const char* name) {
    if (!name) return 0;
    size_t n = strlen(name);
    if (n < 1 || n > 14) return 0;   // 协议 §4.1：预设名（含 _ABOVE）≤ 14
    for (size_t i = 0; i < n; ++i) {
        char c = name[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return 0;
    }
    return 1;
}

// ---------------------------------------------------------------- 决策构造

static ArmDecision none_(void) {
    ArmDecision d;
    memset(&d, 0, sizeof d);
    d.action = ARM_ACT_NONE;
    d.reply.max_ms = -1;
    return d;
}

static ArmDecision send_(const char* line) {
    ArmDecision d = none_();
    d.action = ARM_ACT_SEND;
    snprintf(d.line, sizeof d.line, "%s", line);
    return d;
}

static ArmDecision drain_and_send_(const char* line) {
    ArmDecision d = none_();
    d.action = ARM_ACT_DRAIN_AND_SEND;
    snprintf(d.line, sizeof d.line, "%s", line);
    return d;
}

static ArmDecision send_stop_(void) {
    ArmDecision d = none_();
    d.action = ARM_ACT_SEND_STOP;
    return d;
}

static ArmDecision reply_ok_(ArmReplyKind kind, uint32_t op_id, int32_t max_ms) {
    ArmDecision d = none_();
    d.action = ARM_ACT_REPLY;
    d.reply.ok = 1;
    d.reply.kind = kind;
    d.reply.operation_id = op_id;
    d.reply.max_ms = max_ms;
    return d;
}

static ArmDecision reply_err_(ArmCode code) {
    ArmDecision d = none_();
    d.action = ARM_ACT_REPLY;
    d.reply.ok = 0;
    d.reply.code = code;
    d.reply.max_ms = -1;
    return d;
}

// ---------------------------------------------------------------- 状态小工具

static int op_active(const ArmFsm* f) {
    return f->op_state == ARM_OP_PENDING_ACCEPTANCE ||
           f->op_state == ARM_OP_MOVING ||
           f->op_state == ARM_OP_QUIESCING;
}

// 终态认领的五条条件。串口终态不带任何操作标识，缺一条就会被旧应答冒领。
// 第五条用的是**接收时刻**的世代快照——用处理时刻的当前值等于恒真。
static int can_claim_terminal(const ArmFsm* f) {
    return (f->op_state == ARM_OP_MOVING || f->op_state == ARM_OP_QUIESCING) &&
           f->op_accepted &&
           f->op_epoch == f->link_epoch &&
           f->rx_generation == f->op_generation;
}

// 位置不再可信时的唯一落锁点。曾经这两行被手抄在六处，其中两处顺序还是反的——
// 有一处更是干脆漏了（TIMED_OUT），于是下位机已锁存急停、board 侧却还报位置可信。
void lock_position_lost(ArmFsm* f) { f->position_known = 0; f->phase = ARM_PHASE_LOCKED_AFTER_RESET; }

// 信任降级是**终态的属性**，不是各调用点的动作。收在表里，漏一处在结构上就不可能。
//   RESET              下位机复位，位置全丢
//   TIMED_OUT          下位机锁存急停并 detach（recovery 文案自己写着要 home）
//   ACCEPTANCE_UNKNOWN 不知道动作有没有发生，也就不知道臂在哪
// DONE / STOPPED / REJECTED 不在其中：闭环动作停在当前角，下位机仍知道自己在哪。
// LINK_FAILED 也不在——清场确认空闲后位置由 POS 回报兜住；清场**没**确认空闲的那条
// 路径在调用点额外落锁（那里才是真的不确定）。
static int terminal_forfeits_position(ArmOpState st) {
    return st == ARM_OP_RESET || st == ARM_OP_TIMED_OUT ||
           st == ARM_OP_ACCEPTANCE_UNKNOWN;
}

// 「受理成功、转入等终态」的唯一入口。三处调用只差期限算法：拿到 OK:<max_ms>
// 时用实测耗时 + grace，由 BUSY / ST=MOVING 推断出来时 max_ms 未知、用保守兜底。
static ArmDecision accept_op(ArmFsm* f, int32_t max_ms, uint32_t deadline_ms) {
    f->op_accepted = 1;
    f->op_max_ms = max_ms;
    f->op_deadline_ms = deadline_ms;
    f->op_state = ARM_OP_MOVING;
    f->arm_moving = 1;
    return reply_ok_(ARM_REPLY_ACCEPTED, f->op_id, max_ms);
}

// 操作的唯一收口。归位的任何非成功终态也在这里统一降级——下位机在归位被中断时
// detach（protocol.cpp 的 motion_abort_clear），此后位置不再可信；常态归位与恢复
// 归位一视同仁，少走一条路径就会留下 phase 卡在 RECOVERING_HOME 的死角。
static void finish_op(ArmFsm* f, ArmOpState st, ArmCode code) {
    int was_home = f->op_is_home;
    f->op_state = st;
    f->op_code = code;
    f->probe_outstanding = 0;
    f->arm_moving = 0;          // 动作已收口，状态快照要跟上
    f->operation_generation++;  // 作废一切在途事件

    if (was_home && st == ARM_OP_DONE) {
        if (f->phase == ARM_PHASE_RECOVERING_HOME) f->phase = ARM_PHASE_READY;
        f->position_known = 1;
        return;
    }
    if (terminal_forfeits_position(st) || was_home) {
        lock_position_lost(f);
    }
}

static void enter_wait_boot(ArmFsm* f, uint32_t now_ms) {
    f->phase = ARM_PHASE_WAIT_BOOT_DONE;
    f->position_known = 0;
    f->boot_window_start_ms = now_ms;
    f->boot_window_armed = 1;
}

static void parse_version(ArmFsm* f, const char* p) {
    int major = 0, minor = 0;
    if (sscanf(p, "%d.%d", &major, &minor) == 2) {
        f->fw_major = major;
        f->fw_minor = minor;
        f->collision_guard = (major > 1 || (major == 1 && minor >= 1)) ? 1 : 0;
        f->version_probe_sent = 1;   // 已知版本，不必再探
    }
}

// 取 `key=` 字段的值并与 want 整体比较。**不能用 strstr**：那样 `ST=IDLE_BOGUS`
// 会被当成 IDLE，清场与 boot 验证双双 fail-open。字段以 ';' 分隔或行尾结束。
static int field_equals(const char* line, const char* key, const char* want) {
    const char* p = strstr(line, key);
    if (!p) return 0;
    p += strlen(key);
    size_t n = strlen(want);
    if (strncmp(p, want, n) != 0) return 0;
    char after = p[n];
    return after == '\0' || after == ';';
}

static int st_is_moving(const char* line) {
    return field_equals(line, "ST=", "MOVING");
}

// 严格只认 IDLE。ST=ESTOP / ST=RELAXED / 畸形行都**不算**空闲——把它们当空闲会
// 在下位机仍锁存急停时释放操作槽。
static int st_is_idle(const char* line) {
    return field_equals(line, "ST=", "IDLE");
}

static int st_is_idle_attached(const char* line) {
    return st_is_idle(line) && field_equals(line, "ATT=", "3F");
}

int arm_fsm_parse_pos(const char* line, int out_joints[6]) {
    if (!arm_fsm_has_prefix(line, "POS:")) return 0;
    int got = 0;
    for (int i = 0; i < 6; ++i) {
        char key[3] = {(char)('A' + i), '=', '\0'};
        const char* p = strstr(line, key);
        if (!p) continue;
        out_joints[i] = (int)strtol(p + 2, NULL, 10);
        ++got;
    }
    return got == 6;
}

static void cache_pos(ArmFsm* f, const char* line) {
    int j[6];
    if (arm_fsm_parse_pos(line, j)) {
        memcpy(f->joints, j, sizeof j);
        f->has_pos = 1;
    }
    f->arm_moving = st_is_moving(line);
}

// 状态查询在途时被打断（复位 / 超时）：必须给等待者一个明确答复。
static ArmDecision fail_status_query(ArmFsm* f, ArmCode code) {
    f->status_query_outstanding = 0;
    return reply_err_(code);
}

// ---------------------------------------------------------------- init

void arm_fsm_init(ArmFsm* fsm, const ArmFsmConfig* cfg) {
    memset(fsm, 0, sizeof(*fsm));
    fsm->phase = ARM_PHASE_WAIT_BOOT_DONE;
    fsm->op_state = ARM_OP_IDLE;
    fsm->op_max_ms = -1;
    fsm->boot_window_armed = 1;   // board 启动即计时：始终等不到 READY 也要落锁
    fsm->boot_window_start_ms = 0;
    fsm->ack_timeout_ms = cfg->ack_timeout_ms;
    fsm->done_grace_ms = cfg->done_grace_ms;
    fsm->fallback_deadline_ms = cfg->fallback_deadline_ms;
    fsm->boot_window_ms = cfg->boot_window_ms;
    fsm->stop_ack_wait_ms = cfg->stop_ack_wait_ms;
    fsm->max_attempts = cfg->max_attempts;
}

int arm_fsm_status_needs_uart(const ArmFsm* fsm) {
    if (fsm->op_state == ARM_OP_MOVING || fsm->op_state == ARM_OP_QUIESCING) return 0;
    if (fsm->phase == ARM_PHASE_WAIT_BOOT_DONE) return 0;  // 零下行
    return 1;
}

// ---------------------------------------------------------------- 命令拼装

static void render_cmd(const ArmRequest* r, char* out, size_t n) {
    switch (r->kind) {
        case ARM_REQ_HOME: snprintf(out, n, "HOME"); break;
        case ARM_REQ_GRIP_OPEN: snprintf(out, n, "GRIP_OPEN"); break;
        case ARM_REQ_GRIP_CLOSE: snprintf(out, n, "GRIP_CLOSE"); break;
        case ARM_REQ_JOINT:
            if (r->ramp_ms > 0) {
                snprintf(out, n, "JOINT:%c:%d:%d", r->joint, r->angle, r->ramp_ms);
            } else {
                snprintf(out, n, "JOINT:%c:%d", r->joint, r->angle);
            }
            break;
        case ARM_REQ_MOVE_PRESET: snprintf(out, n, "MOVE_PRESET:%s", r->name); break;
        case ARM_REQ_PICK: snprintf(out, n, "PICK:%s", r->name); break;
        case ARM_REQ_PLACE: snprintf(out, n, "PLACE:%s", r->name); break;
        case ARM_REQ_STATUS: snprintf(out, n, "STATUS"); break;
        default: snprintf(out, n, "STATUS"); break;
    }
}

// 白名单校验：**在拼命令之前**。合法返回 1。
static int request_args_ok(const ArmRequest* r) {
    switch (r->kind) {
        case ARM_REQ_JOINT:
            if (!arm_fsm_joint_is_valid(r->joint)) return 0;
            if (r->angle < 0 || r->angle > 180) return 0;
            if (r->ramp_ms < 0 || r->ramp_ms > 100) return 0;
            return 1;
        case ARM_REQ_MOVE_PRESET:
        case ARM_REQ_PICK:
        case ARM_REQ_PLACE:
            return arm_fsm_name_is_valid(r->name);
        default:
            return 1;
    }
}

static void begin_op(ArmFsm* f, const ArmRequest* r, uint32_t now_ms, const char* line) {
    f->operation_generation++;
    f->op_generation = f->operation_generation;
    f->op_epoch = f->link_epoch;
    f->op_id++;
    f->op_state = ARM_OP_PENDING_ACCEPTANCE;
    f->op_idempotent = (r->kind != ARM_REQ_PICK && r->kind != ARM_REQ_PLACE);
    f->op_is_home = (r->kind == ARM_REQ_HOME);
    f->op_accepted = 0;
    f->op_max_ms = -1;
    f->op_deadline_ms = 0;
    f->op_attempts = 1;
    f->op_sent_at_ms = now_ms;
    f->op_code = ARM_CODE_NONE;
    f->probe_outstanding = 0;
    snprintf(f->op_line, sizeof f->op_line, "%s", line);
}

// ---------------------------------------------------------------- on_request

ArmDecision arm_fsm_on_request(ArmFsm* f, const ArmRequest* req, uint32_t now_ms) {
    if (!request_args_ok(req)) {
        return reply_err_(ARM_CODE_BAD_ARGUMENT);
    }

    if (req->kind == ARM_REQ_STATUS) {
        if (!arm_fsm_status_needs_uart(f)) {
            return reply_ok_(ARM_REPLY_STATUS, f->op_id, f->op_max_ms);  // 缓存，零下行
        }
        f->status_query_outstanding = 1;
        f->status_query_sent_at_ms = now_ms;
        return send_("STATUS");
    }

    // 上电归位窗口零下行。此刻的急停手段是切断 12V——任何指令都会把下位机的
    // 自动归位打断成 detach → RELAXED（治具实测复现）。
    if (f->phase == ARM_PHASE_WAIT_BOOT_DONE) {
        return reply_err_(ARM_CODE_RESET);
    }

    if (req->kind == ARM_REQ_STOP) {
        f->stop_pending = 1;
        f->stop_sent_at_ms = now_ms;
        f->stop_ack_count = 0;
        return send_stop_();
    }

    if (f->phase == ARM_PHASE_VERIFY_BOOT_HOME || f->phase == ARM_PHASE_RECOVERING_HOME) {
        return reply_err_(ARM_CODE_BUSY);
    }

    if (f->phase == ARM_PHASE_LOCKED_AFTER_RESET) {
        // 位置已丢失。HOME 是唯一的恢复途径，必须放行——治具原实现把它一并挡住，
        // 照搬会导致永远无法恢复；下位机侧本就允许 RELAXED 下发 HOME（协议 §5.7）。
        if (req->kind != ARM_REQ_HOME) {
            return reply_err_(ARM_CODE_RESET);
        }
        char line[ARM_FSM_LINE_MAX];
        render_cmd(req, line, sizeof line);
        begin_op(f, req, now_ms, line);
        f->phase = ARM_PHASE_RECOVERING_HOME;
        return send_(line);
    }

    if (op_active(f)) {
        return reply_err_(ARM_CODE_BUSY);  // 本地互斥，不下发
    }

    char line[ARM_FSM_LINE_MAX];
    render_cmd(req, line, sizeof line);
    begin_op(f, req, now_ms, line);
    return send_(line);
}

// ---------------------------------------------------------------- on_line

ArmDecision arm_fsm_on_line(ArmFsm* f, const char* line, uint32_t rx_generation,
                            uint32_t now_ms) {
    f->rx_generation = rx_generation;
    ArmLineKind kind = arm_fsm_classify(line);

    if (kind == ARM_LINE_POS || kind == ARM_LINE_ST) {
        cache_pos(f, line);
    }

    // ---- 对端复位：任何状态下都立即降级 ----
    if (kind == ARM_LINE_READY) {
        parse_version(f, line + 6);
        f->link_epoch++;
        // 谁在等，谁就立刻拿到 RESET。拖到 IO 层的兜底超时才回复会突破主循环
        // 占用上界（宪法 III.2）——那条上界正是靠"决策器总在上界内给出答复"成立的。
        int waiter_op = (f->op_state == ARM_OP_PENDING_ACCEPTANCE);
        int waiter_stop = f->stop_pending;
        int waiter_query = f->status_query_outstanding;
        if (op_active(f)) {
            finish_op(f, ARM_OP_RESET, ARM_CODE_RESET);
        }
        f->stop_pending = 0;
        f->status_query_outstanding = 0;
        enter_wait_boot(f, now_ms);
        f->ready_seen = 1;
        if (waiter_op || waiter_stop || waiter_query) {
            return reply_err_(ARM_CODE_RESET);
        }
        return none_();
    }

    if (kind == ARM_LINE_PONG) {
        parse_version(f, line + 5);
        return none_();
    }

    // ---- OK:STOPPED ----
    // 身兼两职：急停请求的即时确认，以及在途动作的终结应答。两者解耦——动作可能
    // 被本地之外的途径停下（此时没有 stop_pending），终态照样要被认领。
    if (kind == ARM_LINE_OK_STOPPED) {
        int stop_first = f->stop_pending && f->stop_ack_count == 0;
        if (f->stop_pending) {
            f->stop_ack_count++;
        }
        if (can_claim_terminal(f)) {
            finish_op(f, ARM_OP_STOPPED, ARM_CODE_NONE);
        }
        if (stop_first) {
            f->stop_pending = 0;
            return reply_ok_(ARM_REPLY_STOPPED, 0, -1);
        }
        return none_();  // 三连发的多余回声：吃掉，不留给下一次请求
    }

    // ---- 上电归位确认 ----
    if (f->phase == ARM_PHASE_WAIT_BOOT_DONE) {
        // 必须先见过本纪元的 READY：没有完整的 READY → DONE 证据链，这条 DONE
        // 可能是 board 启动前那条动作的孤儿，把它当 boot 归位就是 fail-open。
        if (kind == ARM_LINE_DONE && f->ready_seen) {
            f->phase = ARM_PHASE_VERIFY_BOOT_HOME;
            return send_("STATUS");
        }
        return none_();
    }

    if (f->phase == ARM_PHASE_VERIFY_BOOT_HOME) {
        if (kind == ARM_LINE_POS || kind == ARM_LINE_ST) {
            if (st_is_idle_attached(line)) {
                f->phase = ARM_PHASE_READY;
                f->position_known = 1;
            } else {
                lock_position_lost(f);
            }
        }
        return none_();
    }

    // ---- agent 主动发起的状态查询的回报 ----
    // 放在操作分支之前：它背后有工具调用在等 REPLY，没人应答就会干等到超时。
    // 决策器自己发的探查（probe / fence / boot 确认）不走这里。
    if ((kind == ARM_LINE_POS || kind == ARM_LINE_ST) && f->status_query_outstanding &&
        !f->probe_outstanding && f->op_state != ARM_OP_QUIESCING) {
        f->status_query_outstanding = 0;
        return reply_ok_(ARM_REPLY_STATUS, f->op_id, f->op_max_ms);
    }

    // ---- 等待即时应答 ----
    if (f->op_state == ARM_OP_PENDING_ACCEPTANCE) {
        switch (kind) {
            case ARM_LINE_OK_MAXMS: {
                long ms = strtol(line + 3, NULL, 10);
                return accept_op(f, (int32_t)ms, now_ms + (uint32_t)ms + f->done_grace_ms);
            }
            case ARM_LINE_BUSY:
                if (f->op_attempts > 1) {
                    // 重发后的 BUSY = 首条其实已被受理（协议 §4.3.3）。拿不到 max_ms，
                    // 如实报未知并改用保守兜底期限——不按角度反推耗时。
                    return accept_op(f, -1, now_ms + f->fallback_deadline_ms);
                }
                finish_op(f, ARM_OP_REJECTED, ARM_CODE_BUSY);
                return reply_err_(ARM_CODE_BUSY);
            case ARM_LINE_ERROR: {
                ArmCode c = error_code_of(line);
                if (c == ARM_CODE_TIMEOUT) {
                    // TIMEOUT 是**终态**不是拒绝。受理应答之前到达 ⇒ 属上一条，丢弃。
                    return none_();
                }
                finish_op(f, ARM_OP_REJECTED, c);
                return reply_err_(c);
            }
            case ARM_LINE_ST:
            case ARM_LINE_POS:
                if (f->probe_outstanding) {
                    f->probe_outstanding = 0;
                    if (st_is_moving(line)) {
                        return accept_op(f, -1, now_ms + f->fallback_deadline_ms);
                    }
                    // IDLE **不能**判「未受理」：非幂等动作可能已整条走完只是应答全丢，
                    // 判未受理会诱导 agent 重发、再开一次爪、掉落物件。
                    // 且此刻既不知道动作有没有发生，也就不知道臂在哪 ⇒ fail-closed。
                    finish_op(f, ARM_OP_ACCEPTANCE_UNKNOWN, ARM_CODE_ACCEPTANCE_UNKNOWN);
                    lock_position_lost(f);
                    return reply_err_(ARM_CODE_ACCEPTANCE_UNKNOWN);
                }
                return none_();
            default:
                // DONE / OK:STOPPED 在即时应答之前到达 ⇒ 属上一条，丢弃
                return none_();
        }
    }

    // ---- 已受理，等终态 ----
    if (f->op_state == ARM_OP_MOVING || f->op_state == ARM_OP_QUIESCING) {
        int quiescing = (f->op_state == ARM_OP_QUIESCING);

        if (kind == ARM_LINE_DONE && can_claim_terminal(f)) {
            finish_op(f, ARM_OP_DONE, ARM_CODE_NONE);
            return none_();  // 工具早已返回，这里只收尾
        }

        if (kind == ARM_LINE_ERROR) {
            ArmCode c = error_code_of(line);
            if (c == ARM_CODE_TIMEOUT && can_claim_terminal(f)) {
                finish_op(f, ARM_OP_TIMED_OUT, ARM_CODE_TIMEOUT);
            }
            return none_();
        }

        if (quiescing && (kind == ARM_LINE_POS || kind == ARM_LINE_ST)) {
            if (st_is_moving(line)) {
                // 还在动：**保留旧世代**继续等它的终态。此刻若递增世代，那条终态会被
                // 自己作废，而 STATUS 只许单发——操作槽将永远无法释放，链路死锁。
                return none_();
            }
            if (!st_is_idle(line)) {
                // ESTOP / RELAXED / 畸形：fail-closed。下位机可能仍锁存急停，
                // 位置不再可信，不当作干净的空闲收场。
                finish_op(f, ARM_OP_LINK_FAILED, ARM_CODE_LINK);
                lock_position_lost(f);
                return none_();
            }
            // 确认空闲，才递增世代并释放操作槽
            finish_op(f, ARM_OP_LINK_FAILED, ARM_CODE_LINK);
            return none_();
        }
        return none_();
    }

    return none_();
}

// ---------------------------------------------------------------- on_tick

ArmDecision arm_fsm_on_tick(ArmFsm* f, uint32_t now_ms) {
    // 上电窗口超时：包括「board 启动晚于下位机、始终没见 READY」这一路。
    // 没有完整的 READY → DONE 证据链就不得认定位置已知。
    if ((f->phase == ARM_PHASE_WAIT_BOOT_DONE || f->phase == ARM_PHASE_VERIFY_BOOT_HOME) &&
        f->boot_window_armed && elapsed_past(now_ms, f->boot_window_start_ms, f->boot_window_ms)) {
        lock_position_lost(f);
        f->boot_window_armed = 0;
        return none_();
    }

    // 急停无应答 ⇒ 报 LINK，绝不声称已停止
    if (f->stop_pending && elapsed_past(now_ms, f->stop_sent_at_ms, f->stop_ack_wait_ms)) {
        f->stop_pending = 0;
        return reply_err_(ARM_CODE_LINK);
    }

    // 状态查询没等到回报
    if (f->status_query_outstanding &&
        elapsed_past(now_ms, f->status_query_sent_at_ms, f->ack_timeout_ms * 3)) {
        return fail_status_query(f, ARM_CODE_LINK);
    }

    if (f->op_state == ARM_OP_PENDING_ACCEPTANCE &&
        elapsed_past(now_ms, f->op_sent_at_ms, f->ack_timeout_ms)) {
        if (!f->op_idempotent) {
            // PICK/PLACE 禁止任何自动重发——重发等于再开一次爪、掉落物件。
            if (!f->probe_outstanding) {
                f->probe_outstanding = 1;
                f->op_sent_at_ms = now_ms;
                return send_("STATUS");
            }
            // 探查也没回音：既不知道动作有没有发生，也就不知道臂在哪。
            // 不得在未确认空闲的情况下回到可运动状态（FR-021a）——fail-closed 落锁。
            finish_op(f, ARM_OP_ACCEPTANCE_UNKNOWN, ARM_CODE_ACCEPTANCE_UNKNOWN);
            lock_position_lost(f);
            return reply_err_(ARM_CODE_ACCEPTANCE_UNKNOWN);
        }
        if (f->op_attempts < f->max_attempts) {
            f->op_attempts++;
            f->op_sent_at_ms = now_ms;
            return send_(f->op_line);
        }
        // 尝试用尽。
        // 归位例外：它幂等，且此刻位置本来就不可信。直接收口回锁，好让下一次
        // self.arm.home 立刻能再试——若转清场，phase 会卡在 RECOVERING_HOME，
        // 把恢复途径堵上整整一个兜底期限。
        if (f->op_is_home) {
            finish_op(f, ARM_OP_LINK_FAILED, ARM_CODE_LINK);
            return reply_err_(ARM_CODE_LINK);
        }
        // 普通运动：**不能直接释放操作槽**——下位机可能已经受理并正在运动，只是
        // 应答全丢。先答复等待者，再转清场确认下位机确实空闲。
        f->op_state = ARM_OP_QUIESCING;
        f->op_accepted = 1;          // 按"可能已受理"处理，终态仍可被认领
        f->quiesce_probe_sent = 0;
        f->op_deadline_ms = now_ms + f->fallback_deadline_ms;
        return reply_err_(ARM_CODE_LINK);
    }

    // 终态期限到期：转清场。
    if (f->op_state == ARM_OP_MOVING && f->op_deadline_ms != 0 &&
        deadline_passed(now_ms, f->op_deadline_ms)) {
        f->op_state = ARM_OP_QUIESCING;
        f->quiesce_probe_sent = 0;
        f->op_deadline_ms = now_ms + f->fallback_deadline_ms;  // 清场自身的兜底期限
    }

    // 清场：先排空已到达的行（旧终态就在里面），再单发一次 STATUS。
    if (f->op_state == ARM_OP_QUIESCING && !f->quiesce_probe_sent) {
        f->quiesce_probe_sent = 1;
        return drain_and_send_("STATUS");
    }

    // 清场也等不到回应：fail-closed 落锁，不留下"永远 QUIESCING"的死角。
    if (f->op_state == ARM_OP_QUIESCING && f->quiesce_probe_sent &&
        deadline_passed(now_ms, f->op_deadline_ms)) {
        finish_op(f, ARM_OP_LINK_FAILED, ARM_CODE_LINK);
        lock_position_lost(f);
        return none_();
    }

    // 版本未知时补一次探测：错过 READY（board 启动晚）就只能靠它，否则
    // controller_version 会永远停在 0.0，collision_guard 也无从判断。
    if (f->phase == ARM_PHASE_READY && !f->version_probe_sent && !op_active(f) &&
        !f->status_query_outstanding) {
        f->version_probe_sent = 1;
        return send_("PING");
    }

    return none_();
}
