#ifndef IRILLE_XIAO_ARM_TOOLS_H_
#define IRILLE_XIAO_ARM_TOOLS_H_

// self.arm.* 九个 Device MCP 工具的注册与**序列化**。
//
// 这里只把决策器的输出拼成 JSON，**不含任何判断**——发不发、算不算成功、
// 报哪个 code、要不要触发串口，全在 arm_link_fsm 里，那样才被主机自检覆盖。
//
// 面向 agent 的 name / description / 参数文案一律英文：这个工具面跨中文与意语
// 两个实例共享，英文不偏向任何一方（CLAUDE.md「约定」节）。
//
// ⚠️ 上游 AddTool 按名去重且无法重注册 —— description 里不得烤进运行时可变的
//    数值（关节限位、速度档毫秒数），否则改了要重启才刷新。

#include <stdexcept>
#include <string>

#include "arm_link.h"
#include "mcp_server.h"

namespace arm_tools {

// 动作类工具描述统一的收尾句：把「受理≠完成」这件事讲清楚，否则 agent 会把
// accepted 当成动作已结束，接着发下一条并撞上 BUSY。
constexpr const char* kPollSuffix =
    "\nReturns immediately with an operation id - the motion is still running. Poll "
    "self.arm.status until that operation reaches a terminal state before issuing the "
    "next motion.";

inline const char* CodeName(ArmCode c) {
    switch (c) {
        case ARM_CODE_LIMIT: return "LIMIT";
        case ARM_CODE_COLLISION: return "COLLISION";
        case ARM_CODE_BUSY: return "BUSY";
        case ARM_CODE_ESTOP: return "ESTOP";
        case ARM_CODE_RELAXED: return "RELAXED";
        case ARM_CODE_UNKNOWN_PRESET: return "UNKNOWN_PRESET";
        case ARM_CODE_INVALID_COMMAND: return "INVALID_COMMAND";
        case ARM_CODE_TIMEOUT: return "TIMEOUT";
        case ARM_CODE_LINK: return "LINK";
        case ARM_CODE_RESET: return "RESET";
        case ARM_CODE_ACCEPTANCE_UNKNOWN: return "ACCEPTANCE_UNKNOWN";
        default: return "";
    }
}

// 成功与失败同构：agent 只需一套解析逻辑。
inline std::string ReplyToJson(const ArmReply& r) {
    char buf[512];
    if (r.ok) {
        // 急停不创建 operation，载荷形态与动作类不同（contracts/mcp-tools.md）
        if (r.kind == ARM_REPLY_STOPPED) {
            return std::string("{\"ok\":true,\"state\":\"stopped\"}");
        }
        if (r.max_ms >= 0) {
            std::snprintf(buf, sizeof buf,
                          "{\"ok\":true,\"state\":\"accepted\",\"operation_id\":%u,"
                          "\"max_ms\":%d}",
                          (unsigned)r.operation_id, (int)r.max_ms);
        } else {
            // 受理是推断出来的（即时应答丢失），拿不到耗时就如实报 null，
            // 不按角度反推——那等于在 board 层做角度计算。
            std::snprintf(buf, sizeof buf,
                          "{\"ok\":true,\"state\":\"accepted\",\"operation_id\":%u,"
                          "\"max_ms\":null}",
                          (unsigned)r.operation_id);
        }
    } else {
        std::snprintf(buf, sizeof buf, "{\"ok\":false,\"code\":\"%s\",\"recovery\":\"%s\"}",
                      CodeName(r.code), arm_fsm_recovery_text(r.code));
    }
    return std::string(buf);
}

inline std::string StatusToJson(const ArmStatusSnapshot& s) {
    char joints[160] = "null";
    if (s.has_pos) {
        std::snprintf(joints, sizeof joints,
                      "{\"A\":%d,\"B\":%d,\"C\":%d,\"D\":%d,\"E\":%d,\"F\":%d}",
                      s.joints[0], s.joints[1], s.joints[2],
                      s.joints[3], s.joints[4], s.joints[5]);
    }

    char op[512];
    if (s.op_code != ARM_CODE_NONE) {
        // 失败终态与同步拒绝同构地带上 code + recovery：只报 state 不足以让
        // agent 决定下一步。
        if (s.op_max_ms >= 0) {
            std::snprintf(op, sizeof op,
                          "{\"id\":%u,\"state\":\"%s\",\"max_ms\":%d,\"code\":\"%s\","
                          "\"recovery\":\"%s\"}",
                          (unsigned)s.op_id, s.op_state, (int)s.op_max_ms,
                          CodeName(s.op_code), arm_fsm_recovery_text(s.op_code));
        } else {
            std::snprintf(op, sizeof op,
                          "{\"id\":%u,\"state\":\"%s\",\"max_ms\":null,\"code\":\"%s\","
                          "\"recovery\":\"%s\"}",
                          (unsigned)s.op_id, s.op_state,
                          CodeName(s.op_code), arm_fsm_recovery_text(s.op_code));
        }
    } else if (s.op_max_ms >= 0) {
        std::snprintf(op, sizeof op, "{\"id\":%u,\"state\":\"%s\",\"max_ms\":%d}",
                      (unsigned)s.op_id, s.op_state, (int)s.op_max_ms);
    } else {
        std::snprintf(op, sizeof op, "{\"id\":%u,\"state\":\"%s\",\"max_ms\":null}",
                      (unsigned)s.op_id, s.op_state);
    }

    char buf[1024];
    std::snprintf(buf, sizeof buf,
                  "{\"ok\":true,\"position_known\":%s,\"operation\":%s,\"joints\":%s,"
                  "\"arm_state\":\"%s\",\"controller_version\":\"%d.%d\","
                  "\"collision_guard\":%s}",
                  s.position_known ? "true" : "false", op, joints,
                  s.arm_moving ? "moving" : "idle", s.fw_major, s.fw_minor,
                  s.collision_guard ? "true" : "false");
    return std::string(buf);
}

// 本地参数白名单失败是**调用方的编程错误**，不是一次业务拒绝——按 contracts
// 的约定走 JSON-RPC error，而不是混进正常返回里让 agent 去分辨。
inline std::string ReplyToJsonOrThrow(const ArmReply& r) {
    if (!r.ok && r.code == ARM_CODE_BAD_ARGUMENT) {
        throw std::runtime_error(arm_fsm_recovery_text(r.code));
    }
    return ReplyToJson(r);
}

inline ArmRequest MakeReq(ArmReqKind k) {
    ArmRequest r = {};
    r.kind = k;
    return r;
}

// 速度档 → 每度毫秒。三档由下位机 config.h 定死（FAST=10 / NORMAL=20 / FINE=50，
// 2026-08-31 实测），这里只做名字到数值的翻译，不做任何安全判断。
// 契约限定 fast|normal|fine —— **未知值返回 -1**，由决策器的白名单拒掉，
// 不静默当成 normal（那会让打错字的 agent 以为自己选了细档）。
inline int RampMsOf(const std::string& speed) {
    if (speed == "fast") return 10;
    if (speed == "normal") return 20;
    if (speed == "fine") return 50;
    return -1;
}

inline void Register(ArmLink& link) {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.arm.home",
                std::string(
                    "Move the arm back to its home pose. This is also the only way to "
                    "recover after the arm controller has reset or gone limp: it "
                    "re-establishes a known position.") + kPollSuffix,
                PropertyList(),
                [&link](const PropertyList&) -> ReturnValue {
                    return ReplyToJsonOrThrow(link.Request(MakeReq(ARM_REQ_HOME)));
                });

    mcp.AddTool("self.arm.stop",
                "Cancel whatever the arm is doing right now. Closed-loop motions stop "
                "where they are. This is a functional cancel, not a safety guarantee: a "
                "homing move is open-loop and will finish travelling even after this "
                "returns, and the only true emergency stop is cutting the arm's 12 V supply.",
                PropertyList(),
                [&link](const PropertyList&) -> ReturnValue {
                    return ReplyToJsonOrThrow(link.Request(MakeReq(ARM_REQ_STOP)));
                });

    mcp.AddTool("self.arm.gripper_open",
                std::string("Open the gripper.") + kPollSuffix, PropertyList(),
                [&link](const PropertyList&) -> ReturnValue {
                    return ReplyToJsonOrThrow(link.Request(MakeReq(ARM_REQ_GRIP_OPEN)));
                });

    mcp.AddTool("self.arm.gripper_close",
                std::string("Close the gripper.") + kPollSuffix, PropertyList(),
                [&link](const PropertyList&) -> ReturnValue {
                    return ReplyToJsonOrThrow(link.Request(MakeReq(ARM_REQ_GRIP_CLOSE)));
                });

    mcp.AddTool("self.arm.move_joint",
                std::string(
                    "Turn a single joint to an absolute angle. Joints are named A to F from "
                    "the base outwards: A rotates the base, B the shoulder, C the elbow, "
                    "D the wrist pitch, E the wrist roll, F the gripper. Angles outside the "
                    "joint's mechanical range are rejected by the arm controller - nothing "
                    "moves. Speed is one of fast, normal or fine; fine is for delicate "
                    "approaches.") + kPollSuffix,
                PropertyList({Property("joint", kPropertyTypeString),
                              Property("angle", kPropertyTypeInteger, 0, 180),
                              Property("speed", kPropertyTypeString, std::string("normal"))}),
                [&link](const PropertyList& p) -> ReturnValue {
                    ArmRequest r = MakeReq(ARM_REQ_JOINT);
                    std::string j = p["joint"].value<std::string>();
                    r.joint = j.empty() ? '?' : j[0];
                    r.angle = p["angle"].value<int>();
                    r.ramp_ms = RampMsOf(p["speed"].value<std::string>());
                    return ReplyToJsonOrThrow(link.Request(r));
                });

    mcp.AddTool("self.arm.move_to_preset",
                std::string(
                    "Move the arm to a named preset pose. The gripper is not part of a "
                    "preset - use the gripper tools for that.") + kPollSuffix,
                PropertyList({Property("name", kPropertyTypeString)}),
                [&link](const PropertyList& p) -> ReturnValue {
                    ArmRequest r = MakeReq(ARM_REQ_MOVE_PRESET);
                    std::snprintf(r.name, sizeof r.name, "%s",
                                  p["name"].value<std::string>().c_str());
                    return ReplyToJsonOrThrow(link.Request(r));
                });

    mcp.AddTool("self.arm.pick_from_preset",
                std::string(
                    "Pick an object from a named preset location: approach from above, open "
                    "the gripper, descend, close, and lift back up. The whole sequence is one "
                    "motion. If the result comes back as ACCEPTANCE_UNKNOWN, do NOT call this "
                    "again - check self.arm.status first, because repeating it would reopen "
                    "the gripper and drop whatever is held.") + kPollSuffix,
                PropertyList({Property("name", kPropertyTypeString)}),
                [&link](const PropertyList& p) -> ReturnValue {
                    ArmRequest r = MakeReq(ARM_REQ_PICK);
                    std::snprintf(r.name, sizeof r.name, "%s",
                                  p["name"].value<std::string>().c_str());
                    return ReplyToJsonOrThrow(link.Request(r));
                });

    mcp.AddTool("self.arm.place_to_preset",
                std::string(
                    "Place the held object at a named preset location: approach from above, "
                    "descend, open the gripper, and lift back up. The whole sequence is one "
                    "motion. If the result comes back as ACCEPTANCE_UNKNOWN, do NOT call this "
                    "again - check self.arm.status first.") + kPollSuffix,
                PropertyList({Property("name", kPropertyTypeString)}),
                [&link](const PropertyList& p) -> ReturnValue {
                    ArmRequest r = MakeReq(ARM_REQ_PLACE);
                    std::snprintf(r.name, sizeof r.name, "%s",
                                  p["name"].value<std::string>().c_str());
                    return ReplyToJsonOrThrow(link.Request(r));
                });

    mcp.AddTool("self.arm.status",
                "Report what the arm is doing: the state of the most recent operation, the "
                "current joint angles, and whether the arm's position is currently trusted. "
                "Poll this after a motion tool returns accepted to find out whether the "
                "motion finished, was stopped, or timed out. While a motion is running this "
                "returns a cached snapshot and does not disturb the arm.",
                PropertyList(),
                [&link](const PropertyList&) -> ReturnValue {
                    // 是否需要打串口由决策器裁定；这里无条件走 Request，
                    // 它在已知 moving 时会直接返回缓存。
                    ArmReply r = link.Request(MakeReq(ARM_REQ_STATUS));
                    if (!r.ok) {
                        // 查询本身失败（链路断、对端复位）就如实报——
                        // 拿旧快照冒充成功，等于向 agent 谎报机械臂的状态。
                        return ReplyToJsonOrThrow(r);
                    }
                    ArmStatusSnapshot s = {};
                    link.Snapshot(&s);
                    return StatusToJson(s);
                });
}

}  // namespace arm_tools

#endif  // IRILLE_XIAO_ARM_TOOLS_H_
