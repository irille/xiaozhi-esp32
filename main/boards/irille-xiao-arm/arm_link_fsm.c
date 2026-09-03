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

ArmLineKind arm_fsm_classify(const char* line) {
    if (!line || !line[0]) return ARM_LINE_MALFORMED;
    if (strcmp(line, "OK:STOPPED") == 0) return ARM_LINE_OK_STOPPED;
    if (strcmp(line, "OK:RELAXED") == 0) return ARM_LINE_OK_RELAXED;
    if (arm_fsm_has_prefix(line, "OK:") && is_digits(line + 3)) return ARM_LINE_OK_MAXMS;
    if (arm_fsm_has_prefix(line, "DONE")) return ARM_LINE_DONE;
    if (strcmp(line, "BUSY") == 0) return ARM_LINE_BUSY;
    if (arm_fsm_has_prefix(line, "ERROR:")) return ARM_LINE_ERROR;
    if (arm_fsm_has_prefix(line, "PONG:")) return ARM_LINE_PONG;
    if (arm_fsm_has_prefix(line, "POS:")) return ARM_LINE_POS;
    if (arm_fsm_has_prefix(line, "ST=")) return ARM_LINE_ST;
    if (arm_fsm_has_prefix(line, "READY:")) return ARM_LINE_READY;
    return ARM_LINE_MALFORMED;
}

// ERROR:<code> → ArmCode
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
    return ARM_CODE_INVALID_COMMAND;  // 含 INVALID_COMMAND 与未知码
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

static ArmDecision send_stop_(void) {
    ArmDecision d = none_();
    d.action = ARM_ACT_SEND_STOP;
    return d;
}

static ArmDecision reply_ok_(uint32_t op_id, int32_t max_ms) {
    ArmDecision d = none_();
    d.action = ARM_ACT_REPLY;
    d.reply.ok = 1;
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
static int can_claim_terminal(const ArmFsm* f) {
    return (f->op_state == ARM_OP_MOVING || f->op_state == ARM_OP_QUIESCING) &&
           f->op_accepted &&
           f->op_epoch == f->link_epoch &&
           f->rx_generation == f->op_generation;
}

static void finish_op(ArmFsm* f, ArmOpState st, ArmCode code) {
    f->op_state = st;
    f->op_code = code;
    f->probe_outstanding = 0;
    f->operation_generation++;  // 作废一切在途事件
}

// 归位的非成功终态使位置不可信——下位机在归位被中断时 detach（protocol.cpp
// 的 motion_abort_clear），此后位置不再可信。常态归位与恢复归位一视同仁。
static void degrade_after_failed_home(ArmFsm* f) {
    f->position_known = 0;
    f->phase = ARM_PHASE_LOCKED_AFTER_RESET;
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
    }
}

static int st_is_moving(const char* line) {
    return strstr(line, "ST=MOVING") != NULL;
}

// POS:A=90,B=70,C=80,D=90,E=90,F=170;ST=IDLE;ATT=3F → 六个角
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

static int st_is_idle_attached(const char* line) {
    return strstr(line, "ST=IDLE") != NULL && strstr(line, "ATT=3F") != NULL;
}

// ---------------------------------------------------------------- init

void arm_fsm_init(ArmFsm* fsm, const ArmFsmConfig* cfg) {
    memset(fsm, 0, sizeof(*fsm));
    fsm->phase = ARM_PHASE_WAIT_BOOT_DONE;
    fsm->op_state = ARM_OP_IDLE;
    fsm->op_max_ms = -1;
    fsm->boot_window_armed = 1;   // board 启动即开始计时：若始终等不到 READY 就落锁
    fsm->boot_window_start_ms = 0;
    fsm->ack_timeout_ms = cfg->ack_timeout_ms;
    fsm->done_grace_ms = cfg->done_grace_ms;
    fsm->fallback_deadline_ms = cfg->fallback_deadline_ms;
    fsm->boot_window_ms = cfg->boot_window_ms;
    fsm->stop_ack_wait_ms = cfg->stop_ack_wait_ms;
    fsm->max_attempts = cfg->max_attempts;
}

int arm_fsm_status_needs_uart(const ArmFsm* fsm) {
    // 动作进行中查询状态返回缓存：协议 §4.4 说明状态回复会关中断拖慢斜坡节拍。
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
    // 上电归位窗口：零下行。此刻的急停手段是切断 12V，不是发 STOP——
    // 开机指令会把下位机的自动归位打断成 detach → RELAXED（治具实测）。
    if (f->phase == ARM_PHASE_WAIT_BOOT_DONE) {
        return reply_err_(ARM_CODE_RESET);
    }

    if (req->kind == ARM_REQ_STOP) {
        // 急停：不参与互斥、不等在途动作自然结束、不遵守重发间隔、不占操作槽。
        f->stop_pending = 1;
        f->stop_sent_at_ms = now_ms;
        f->stop_ack_count = 0;
        return send_stop_();
    }

    if (req->kind == ARM_REQ_STATUS) {
        if (!arm_fsm_status_needs_uart(f)) {
            return reply_ok_(f->op_id, f->op_max_ms);  // 返回缓存，不触发串口
        }
        return send_("STATUS");
    }

    // 以下是运动类请求
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

ArmDecision arm_fsm_on_line(ArmFsm* f, const char* line, uint32_t now_ms) {
    f->rx_generation = f->operation_generation;
    ArmLineKind kind = arm_fsm_classify(line);

    // 状态回报一律进缓存：动作进行中的 status 查询靠它，不再触发串口
    if (kind == ARM_LINE_POS || kind == ARM_LINE_ST) {
        cache_pos(f, line);
    }

    // ---- 对端复位：任何状态下都立即降级 ----
    if (kind == ARM_LINE_READY) {
        parse_version(f, line + 6);
        f->link_epoch++;
        if (op_active(f)) {
            finish_op(f, ARM_OP_RESET, ARM_CODE_RESET);
            f->stale_terminal_credit = 1;  // 复位前那条动作的终态可能仍在路上
        }
        f->stop_pending = 0;
        enter_wait_boot(f, now_ms);
        return none_();
    }

    if (kind == ARM_LINE_PONG) {
        parse_version(f, line + 5);
        return none_();
    }

    // ---- OK:STOPPED ----
    // 它身兼两职：急停请求的即时确认，以及在途动作的终结应答。两者必须解耦——
    // 动作可能被本地之外的途径停下（此时没有 stop_pending），终态照样要被认领。
    if (kind == ARM_LINE_OK_STOPPED) {
        int stop_first = f->stop_pending && f->stop_ack_count == 0;
        if (f->stop_pending) {
            f->stop_ack_count++;
        }

        if (f->stale_terminal_credit > 0) {
            f->stale_terminal_credit--;   // 孤儿，不属于当前操作
        } else if (can_claim_terminal(f)) {
            int was_home = f->op_is_home;
            finish_op(f, ARM_OP_STOPPED, ARM_CODE_NONE);
            if (was_home) degrade_after_failed_home(f);
        }

        if (stop_first) {
            f->stop_pending = 0;
            return reply_ok_(f->op_id, -1);
        }
        // 三连发的多余回声：吃掉，不得留给下一次请求
        return none_();
    }

    // ---- 上电归位确认 ----
    if (f->phase == ARM_PHASE_WAIT_BOOT_DONE) {
        if (kind == ARM_LINE_DONE) {
            if (f->stale_terminal_credit > 0) {
                f->stale_terminal_credit--;  // 是旧操作的孤儿，不是 boot 归位
                return none_();
            }
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
                f->phase = ARM_PHASE_LOCKED_AFTER_RESET;
                f->position_known = 0;
            }
        }
        return none_();
    }

    // ---- 孤儿终态额度：先于认领判定 ----
    if (kind == ARM_LINE_DONE || kind == ARM_LINE_OK_STOPPED ||
        (kind == ARM_LINE_ERROR && error_code_of(line) == ARM_CODE_TIMEOUT)) {
        if (f->stale_terminal_credit > 0) {
            f->stale_terminal_credit--;
            return none_();
        }
    }

    // ---- 等待即时应答 ----
    if (f->op_state == ARM_OP_PENDING_ACCEPTANCE) {
        switch (kind) {
            case ARM_LINE_OK_MAXMS: {
                long ms = strtol(line + 3, NULL, 10);
                f->op_accepted = 1;
                f->op_max_ms = (int32_t)ms;
                f->op_deadline_ms = now_ms + (uint32_t)ms + f->done_grace_ms;
                f->op_state = ARM_OP_MOVING;
                return reply_ok_(f->op_id, f->op_max_ms);
            }
            case ARM_LINE_BUSY:
                if (f->op_attempts > 1) {
                    // 重发后的 BUSY = 首条其实已被受理（协议 §4.3.3）。
                    // 拿不到 max_ms，如实报 null 并改用保守兜底期限——不按角度反推耗时。
                    f->op_accepted = 1;
                    f->op_max_ms = -1;
                    f->op_deadline_ms = now_ms + f->fallback_deadline_ms;
                    f->op_state = ARM_OP_MOVING;
                    return reply_ok_(f->op_id, -1);
                }
                finish_op(f, ARM_OP_REJECTED, ARM_CODE_BUSY);
                return reply_err_(ARM_CODE_BUSY);
            case ARM_LINE_ERROR: {
                ArmCode c = error_code_of(line);
                finish_op(f, ARM_OP_REJECTED, c);
                return reply_err_(c);
            }
            case ARM_LINE_ST:
            case ARM_LINE_POS:
                if (f->probe_outstanding) {
                    f->probe_outstanding = 0;
                    if (st_is_moving(line)) {
                        // 只有 ST=MOVING 能证明已受理
                        f->op_accepted = 1;
                        f->op_max_ms = -1;
                        f->op_deadline_ms = now_ms + f->fallback_deadline_ms;
                        f->op_state = ARM_OP_MOVING;
                        return reply_ok_(f->op_id, -1);
                    }
                    // IDLE **不能**判「未受理」：非幂等动作可能已整条走完只是应答全丢，
                    // 判未受理会诱导 agent 重发、再开一次爪、掉落物件。
                    finish_op(f, ARM_OP_ACCEPTANCE_UNKNOWN, ARM_CODE_ACCEPTANCE_UNKNOWN);
                    return reply_err_(ARM_CODE_ACCEPTANCE_UNKNOWN);
                }
                return none_();
            default:
                // DONE / OK:STOPPED / TIMEOUT 在即时应答之前到达 ⇒ 属上一条，丢弃
                return none_();
        }
    }

    // ---- 已受理，等终态 ----
    if (f->op_state == ARM_OP_MOVING || f->op_state == ARM_OP_QUIESCING) {
        int quiescing = (f->op_state == ARM_OP_QUIESCING);

        if (kind == ARM_LINE_DONE && can_claim_terminal(f)) {
            int was_home = f->op_is_home;
            finish_op(f, ARM_OP_DONE, ARM_CODE_NONE);
            if (was_home) {
                if (f->phase == ARM_PHASE_RECOVERING_HOME) {
                    f->phase = ARM_PHASE_READY;
                }
                f->position_known = 1;
            }
            return none_();  // 工具早已返回，这里只收尾
        }

        if (kind == ARM_LINE_ERROR) {
            ArmCode c = error_code_of(line);
            if (c == ARM_CODE_TIMEOUT && can_claim_terminal(f)) {
                int was_home = f->op_is_home;
                finish_op(f, ARM_OP_TIMED_OUT, ARM_CODE_TIMEOUT);
                if (was_home) degrade_after_failed_home(f);
                return none_();
            }
            return none_();
        }

        if (quiescing && (kind == ARM_LINE_POS || kind == ARM_LINE_ST)) {
            if (st_is_moving(line)) {
                // 还在动：**保留旧世代**继续等它的终态。
                // 若此刻递增世代，那条终态会被自己作废，而 STATUS 只许单发——
                // 操作槽将永远无法释放，链路死锁。
                return none_();
            }
            // 下位机已空闲却始终没见终态 ⇒ 链路故障。此时递增世代并记一张孤儿额度。
            finish_op(f, ARM_OP_LINK_FAILED, ARM_CODE_LINK);
            f->stale_terminal_credit = 1;
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
        f->boot_window_armed &&
        now_ms - f->boot_window_start_ms > f->boot_window_ms) {
        f->phase = ARM_PHASE_LOCKED_AFTER_RESET;
        f->position_known = 0;
        f->boot_window_armed = 0;
        return none_();
    }

    // 急停无应答 ⇒ 报 LINK，绝不声称已停止
    if (f->stop_pending && now_ms - f->stop_sent_at_ms > f->stop_ack_wait_ms) {
        f->stop_pending = 0;
        return reply_err_(ARM_CODE_LINK);
    }

    if (f->op_state == ARM_OP_PENDING_ACCEPTANCE &&
        now_ms - f->op_sent_at_ms > f->ack_timeout_ms) {
        if (!f->op_idempotent) {
            // PICK/PLACE 禁止任何自动重发——重发等于再开一次爪、掉落物件。
            if (!f->probe_outstanding) {
                f->probe_outstanding = 1;
                f->op_sent_at_ms = now_ms;
                return send_("STATUS");
            }
            finish_op(f, ARM_OP_ACCEPTANCE_UNKNOWN, ARM_CODE_ACCEPTANCE_UNKNOWN);
            return reply_err_(ARM_CODE_ACCEPTANCE_UNKNOWN);
        }
        if (f->op_attempts < f->max_attempts) {
            f->op_attempts++;
            f->op_sent_at_ms = now_ms;
            return send_(f->op_line);
        }
        int was_home = f->op_is_home;
        finish_op(f, ARM_OP_LINK_FAILED, ARM_CODE_LINK);
        if (was_home) degrade_after_failed_home(f);
        return reply_err_(ARM_CODE_LINK);
    }

    // 终态期限到期：单发一次 STATUS 核对，转入清场。
    if (f->op_state == ARM_OP_MOVING && f->op_deadline_ms != 0 &&
        now_ms > f->op_deadline_ms) {
        f->op_state = ARM_OP_QUIESCING;
        return send_("STATUS");
    }

    return none_();
}
