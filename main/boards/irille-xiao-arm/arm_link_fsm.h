#ifndef IRILLE_XIAO_ARM_LINK_FSM_H_
#define IRILLE_XIAO_ARM_LINK_FSM_H_

// 机械臂链路的**纯逻辑决策器**——零 ESP-IDF 依赖，可在开发机上直接编译并断言。
//
// 这里承载全部有状态的判断：行分类、即时应答匹配、操作事务、重发决策、终态认领、
// 链路状态机、急停路径、状态查询是否触发串口、错误码到补救提示的映射。
// arm_link.cc 只做 UART 读写与任务胶水，**不含任何判断**。
//
// 分层边界就是可测边界：任何留在 .cc 里的判断都只能上板才能测，而上板测的是
// 25kg 舵机。整包 review 抓出过四处这类泄漏（急停零应答判定、重复应答清理、
// status 的串口触发条件、错误映射），已全部收回本文件。
//
// 调用方只需三个入口，每个都返回一个决策：
//   arm_fsm_on_request()  工具请求到达
//   arm_fsm_on_line()     串口收到一行
//   arm_fsm_on_tick()     时钟推进
// 时间由调用方以单调毫秒传入，本文件不读时钟。

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARM_FSM_LINE_MAX 64

// ---------------------------------------------------------------- 行分类
typedef enum {
    ARM_LINE_OK_MAXMS = 0,  // OK:<max_ms>      受理
    ARM_LINE_OK_STOPPED,    // OK:STOPPED       急停终态
    ARM_LINE_OK_RELAXED,    // OK:RELAXED
    ARM_LINE_DONE,          // DONE             正常终态
    ARM_LINE_BUSY,          // BUSY             正忙（即时）
    ARM_LINE_ERROR,         // ERROR:*          即时拒绝或超时终态
    ARM_LINE_PONG,          // PONG:<ver>
    ARM_LINE_POS,           // POS:...          状态回报
    ARM_LINE_ST,            // ST=<st>
    ARM_LINE_READY,         // READY:<ver>:<mcusr>  对端复位（事件，不是应答）
    ARM_LINE_MALFORMED,     // 认不出的残缺行
} ArmLineKind;

// ---------------------------------------------------------------- 错误码
// 与 contracts/mcp-tools.md 的表一一对应；映射到英文 recovery 文案的函数见下。
typedef enum {
    ARM_CODE_NONE = 0,
    ARM_CODE_LIMIT,
    ARM_CODE_COLLISION,
    ARM_CODE_BUSY,
    ARM_CODE_ESTOP,
    ARM_CODE_RELAXED,
    ARM_CODE_UNKNOWN_PRESET,
    ARM_CODE_INVALID_COMMAND,
    ARM_CODE_TIMEOUT,
    ARM_CODE_LINK,
    ARM_CODE_RESET,
    ARM_CODE_ACCEPTANCE_UNKNOWN,
    // 本地参数白名单校验失败（不是下位机拒绝）。调用方应把它当编程错误报，
    // 而不是当成一次正常的业务拒绝。
    ARM_CODE_BAD_ARGUMENT,
} ArmCode;

// ---------------------------------------------------------------- 操作状态
typedef enum {
    ARM_OP_IDLE = 0,           // 无活跃操作
    ARM_OP_PENDING_ACCEPTANCE, // 已下发，等即时应答
    ARM_OP_MOVING,             // 已受理，等终态
    ARM_OP_QUIESCING,          // 不确定结束，正在清场
    ARM_OP_DONE,
    ARM_OP_STOPPED,
    ARM_OP_TIMED_OUT,
    ARM_OP_RESET,
    ARM_OP_LINK_FAILED,
    ARM_OP_REJECTED,
    ARM_OP_ACCEPTANCE_UNKNOWN,
} ArmOpState;

// ---------------------------------------------------------------- 链路状态
typedef enum {
    ARM_PHASE_WAIT_BOOT_DONE = 0,  // 零下行：一个字节都不发
    ARM_PHASE_VERIFY_BOOT_HOME,    // 已见候选 DONE，内部单发一次 STATUS
    ARM_PHASE_READY,               // 全部工具可用
    ARM_PHASE_LOCKED_AFTER_RESET,  // 只放行 STOP/PING/STATUS/HOME
    ARM_PHASE_RECOVERING_HOME,     // 只放行 STOP/PING/STATUS
} ArmPhase;

// ---------------------------------------------------------------- 请求
typedef enum {
    ARM_REQ_HOME = 0,
    ARM_REQ_STOP,
    ARM_REQ_GRIP_OPEN,
    ARM_REQ_GRIP_CLOSE,
    ARM_REQ_JOINT,
    ARM_REQ_MOVE_PRESET,
    ARM_REQ_PICK,
    ARM_REQ_PLACE,
    ARM_REQ_STATUS,
} ArmReqKind;

typedef struct {
    ArmReqKind kind;
    char joint;        // JOINT: 'A'..'F'
    int  angle;        // JOINT: 度
    int  ramp_ms;      // JOINT: 每度毫秒，0 = 省略
    char name[16];     // MOVE_PRESET / PICK / PLACE
} ArmRequest;

// ---------------------------------------------------------------- 决策
typedef enum {
    ARM_ACT_NONE = 0,       // 什么也不做
    ARM_ACT_SEND,           // 发送 line 一行
    ARM_ACT_DRAIN_AND_SEND, // 先把已到达的行**全部读完并回喂**，再发 line。
                            // 清场用：确保旧终态在 STATUS 回复之前被消费掉。
    ARM_ACT_SEND_STOP,      // 连发三行 STOP（间隔由 IO 层按 config.h 掌握）
    ARM_ACT_REPLY,          // 工具调用可以返回了
} ArmActionKind;

// 成功回复的形态。STOP 与动作类不同构：它不创建 operation。
typedef enum {
    ARM_REPLY_ACCEPTED = 0,  // 动作已受理
    ARM_REPLY_STOPPED,       // 急停已确认
    ARM_REPLY_STATUS,        // 状态查询的回复
} ArmReplyKind;

typedef struct {
    int          ok;            // 1 = 成功/受理，0 = 失败/拒绝
    ArmReplyKind kind;          // ok==1 时的形态
    ArmCode      code;          // ok==0 时的原因
    uint32_t     operation_id;  // 受理时的操作编号
    int32_t      max_ms;        // 受理时的预计耗时；-1 表示未知（推断受理）
} ArmReply;

typedef struct {
    ArmActionKind action;
    char          line[ARM_FSM_LINE_MAX];  // action==ARM_ACT_SEND 时要发的行（不含 \n）
    ArmReply      reply;                   // action==ARM_ACT_REPLY 时的结果
} ArmDecision;

// ---------------------------------------------------------------- 状态机本体
typedef struct {
    ArmPhase phase;
    uint32_t link_epoch;          // 每收到一次 READY 自增
    uint32_t operation_generation;// 每次新事务自增
    int      position_known;

    // 当前操作
    ArmOpState op_state;
    uint32_t   op_id;
    uint32_t   op_generation;     // 本操作创建时的 generation 快照
    uint32_t   op_epoch;          // 本操作创建时的 link_epoch 快照
    int        op_idempotent;     // 0 = PICK/PLACE，禁止任何自动重发
    int        op_is_home;        // 归位动作：非成功终态要降级 position_known
    int        op_accepted;
    int32_t    op_max_ms;         // -1 = 未知
    uint32_t   op_deadline_ms;
    int        op_attempts;
    uint32_t   op_sent_at_ms;
    char       op_line[ARM_FSM_LINE_MAX];
    ArmCode    op_code;           // 终态若是失败类，这里是原因

    // 本次处理的这一行在**接收时刻**的世代快照（由 IO 层在开始等这一行之前取样、
    // 随行传入）。用它参与认领判定，才能识破"旧操作已放弃、新操作已就位"时才
    // 到达的迟到终态——用处理时刻的当前值取样等于恒真，那道条件形同虚设。
    uint32_t rx_generation;

    // boot 归位确认
    uint32_t boot_window_start_ms;
    // 本纪元是否真的见过 READY。没有完整的 READY → DONE 证据链就不得认定位置已知：
    // board 启动晚于下位机时，一个孤儿 DONE 不能被当成 boot 归位。
    int      ready_seen;

    // 急停
    int      stop_pending;        // 已发出 STOP，正在等 ACK
    uint32_t stop_sent_at_ms;
    int      stop_ack_count;

    // 探查（非幂等受理探查 / deadline 核对 / 清场 fence）——每处至多一次
    int      probe_outstanding;

    // agent 主动发起的状态查询正在等回复。与 probe_outstanding 分开：那个是
    // 决策器自己发的探查，这个背后有一个工具调用在等 REPLY，复位或超时都必须
    // 给它一个明确答复，否则调用方会干等到上层超时并误报链路故障。
    int      status_query_outstanding;
    uint32_t status_query_sent_at_ms;

    // 清场只发一次 STATUS（协议 §4.4：动作期间的状态回复会拖慢斜坡节拍）
    int      quiesce_probe_sent;

    // 版本探测只补发一次。错过 READY（board 启动晚于下位机）时靠它，
    // 否则 controller_version 会永远停在 0.0。
    int      version_probe_sent;

    // 下位机版本
    int      fw_major;
    int      fw_minor;
    int      collision_guard;     // fw >= 1.1

    // 最近一次状态回报的缓存：动作进行中查询状态返回它，不触发串口
    int      has_pos;
    int      joints[6];           // A..F 的当前指令角
    int      arm_moving;          // 最近一次回报里下位机是否在动

    // 参数（由调用方在 init 时按 config.h 注入，便于主机测试改小）
    uint32_t ack_timeout_ms;
    uint32_t done_grace_ms;
    uint32_t fallback_deadline_ms;
    uint32_t boot_window_ms;
    uint32_t stop_ack_wait_ms;
    int      max_attempts;
} ArmFsm;

typedef struct {
    uint32_t ack_timeout_ms;
    uint32_t done_grace_ms;
    uint32_t fallback_deadline_ms;
    uint32_t boot_window_ms;
    uint32_t stop_ack_wait_ms;
    int      max_attempts;
} ArmFsmConfig;

void arm_fsm_init(ArmFsm* fsm, const ArmFsmConfig* cfg);

// 三个入口，各返回一个决策。
//
// `rx_generation` 是 IO 层在**开始等这一行之前**取的世代快照，随行传入；
// 详见结构体里同名字段的说明。
ArmDecision arm_fsm_on_request(ArmFsm* fsm, const ArmRequest* req, uint32_t now_ms);
ArmDecision arm_fsm_on_line(ArmFsm* fsm, const char* line, uint32_t rx_generation,
                            uint32_t now_ms);
ArmDecision arm_fsm_on_tick(ArmFsm* fsm, uint32_t now_ms);

// 参数白名单校验（供测试直接调用）：名字 [A-Z0-9_]{1,14}、关节 A-F。
// **不做转义**——不合规直接拒绝，免得把换行拼进命令行、注入第二条指令。
int arm_fsm_name_is_valid(const char* name);
int arm_fsm_joint_is_valid(char joint);

// 供测试与状态查询共用的纯函数
ArmLineKind arm_fsm_classify(const char* line);
int         arm_fsm_has_prefix(const char* s, const char* prefix);
const char* arm_fsm_recovery_text(ArmCode code);
const char* arm_fsm_code_name(ArmCode code);
const char* arm_fsm_op_state_name(ArmOpState st);

// 位置不再可信时的唯一落锁点
void lock_position_lost(ArmFsm* fsm);

// 状态查询是否需要触发串口：已知在动时返回 0（返回缓存即可）。
int arm_fsm_status_needs_uart(const ArmFsm* fsm);

// 从 POS 行提取六个关节角。成功返回 1。
int arm_fsm_parse_pos(const char* line, int out_joints[6]);

#ifdef __cplusplus
}
#endif

#endif  // IRILLE_XIAO_ARM_LINK_FSM_H_
