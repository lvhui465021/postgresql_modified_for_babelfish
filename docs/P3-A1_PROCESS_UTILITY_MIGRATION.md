# A1 设计文档:T-SQL DDL 分发迁入 ProtocolRoutine.process_utility

> 性质:设计文档 + 试点实施记录。目标:为"是否把 Babelfish 的 DDL 分发从全局 hook 迁入 vtable"做决策依据,并在阶段一试点后记录实际结果。
> 生成时间:2026-08-14,试点完成更新:2026-08-14。关联 FUSION_PLAN.md §7 第四步。本文档不纳入版本控制。

## 0. 结论摘要(试点后更新)

**阶段一(ProcessUtility_hook → vtable)已于 2026-08-14 试点完成,结论:可行、无损、建议保留。** 但实施方式与原设计有两处差异:

1. **原设计"删除 ProcessUtility_hook 安装"的前提不成立**。逐行核实 `bbf_ProcessUtility`(pl_handler.c:2784)发现该 hook 并非"只服务 TDS":它有明确的 PG 方言专属逻辑(2834 视图定义保护、3413/3425 ALTER OWNER 限制、5448 T_DropStmt 处理、2810 explain-only 模式检查)。在 Babelfish 集群里 PG 协议的 DDL 保护靠它承载。删除安装会让 PG 连接的行为回退。
2. **接线机制需要内核新增槽位级 API**。TDS 的 `ProtocolRoutine` 由 babelfishpg_tds 在 preload 时注册,而 `bbf_ProcessUtility` 在 babelfishpg_tsql 里、在 TDS 登录时才加载;且 `port->protocol_routine` 在 pq_init(早于 tsql 加载)时解析。因此内核注册表改为**内核自有可变拷贝** + 新增 `SetProtocolRoutineProcessUtility(kind, fn)` 槽位更新 API:tsql 登录时原地更新内核拷贝,已解析的 Port 指针天然可见,无重新注册、无指针同一性破坏、无跨 .so 直接符号引用(tsql 只调内核 API,与 RegisterADTExt 同一模式)。

## 1. 现状:T-SQL DDL 分发有两条 hook + 一个 vtable 槽位

内核 utility.c 的 DDL 分派链(按执行顺序):

| 层 | 机制 | 内核位置 | T-SQL 实现 | 有无 vtable 对应 |
|---|---|---|---|---|
| 0 | bbfCustomProcessUtility_hook(早期拦截,返回 bool) | utility.c:619-621 | pltsql_bbfCustomProcessUtility(hooks.c:548) | 无 |
| 1 | vtable process_utility 槽(仅非 PG 方言且非 NULL) | utility.c:539-541 | **试点后:bbf_ProcessUtility(经 SetProtocolRoutineProcessUtility 接线)** | 有,MySQL 已用、TDS 试点后已用 |
| 2 | ProcessUtility_hook(标准全局替换) | utility.c:542-545 | bbf_ProcessUtility(pl_handler.c:6154) | 有(即 process_utility) |
| 3 | 兜底 standard_ProcessUtility | utility.c:550 | 无 | 无 |

注意:**层 2 的 ProcessUtility_hook 槽位实际是一条两环链**:bbf_ProcessUtility(tsql) → tdsutils_ProcessUtility(tds,角色/数据库安全限制 + 提权) → standard。原设计文档把"两条 hook"理解为 bbfCustomProcessUtility_hook + ProcessUtility_hook,漏掉了 tdsutils 这一环。

## 2. 两个 hook 各自做什么(试点后补充)

- bbfCustomProcessUtility_hook(早期 bool 拦截):在 readonly 检查之后、switch(nodeTag) 之前运行;返回 true 表示 T-SQL 已完整处理。用于不能落到标准 switch 分发的 T-SQL 命令。没有 vtable 槽位可对应,迁移需新增槽位。
- ProcessUtility_hook 链头 bbf_ProcessUtility:**TDS 与 PG 双服务**。T-SQL 侧做完整 DDL 替换;PG 侧做 Babelfish 对象保护(视图定义一致性、ALTER OWNER 限制、DROP 处理、explain-only 检查),fall-through 走 prev 链。
- ProcessUtility_hook 链尾 tdsutils_ProcessUtility:**PG 与 TDS 双服务**(babelfish 角色锁、DROP DATABASE/ROLE 限制、真实超级用户短路)。MySQL 连接不经过它(vtable 优先)。

## 3. 迁移边界分析(试点后修正)

阶段一(已试点完成):TDS 槽位接 bbf_ProcessUtility,保留 ProcessUtility_hook 安装。效果:

- TDS 连接:utility.c 层 1(vtable 优先)→ bbf_ProcessUtility → 内部 prev 链(tdsutils)→ standard。**与试点前行为逐字节相同**,但分派不再依赖全局单例,改由 protocol_kind 驱动。
- PG 连接:层 2 hook 链(bbf → tdsutils)→ standard。与试点前相同。
- MySQL 连接:层 1 mys routine。不变。
- 单用户/后台进程:无变化。

阶段一-b(原设计的"删除 hook 安装",**未做,需决策**):要把 PG 连接也迁出全局 hook,必须同时处理链上两环(bbf + tdsutils),而 utility.c 的 vtable 槽每方言只有一个。可行的方向:给 PG 方言也注册带 process_utility 的 routine,并在其上叠加 tdsutils 语义(合并成一个入口)。成本中等、收益仅剩"消除 PG 连接的 hook 单例",且 bbfCustomProcessUtility_hook(层 0)依旧无条件先跑,**建议不做**,维持"TDS 走 vtable、PG 走 hook"的现状。

阶段二(bbfCustomProcessUtility_hook,需新增槽位):状态不变,仍是长期项,不主动做。

明确不迁:bbf_ProcessUtility 内部对 Babelfish 目录/类型的深层依赖,保持原样,只改变入口(试点只改了入口,未动函数体)。

## 4. 试点实施清单

内核(postgresql_modified_for_babelfish):

1. `src/backend/postmaster/protocol_routine.c`:`protocol_slots[kind]` 内核自有可变拷贝 + `protocol_slot_registered` 标志;`RegisterProtocolRoutine` 改为拷贝注册;新增 `SetProtocolRoutineProcessUtility(kind, fn)`(未注册 kind 时安全 no-op,fn==NULL 恢复注册值)。
2. `src/include/postmaster/protocol_routine.h`:setter 声明;契约注释更新(process_utility 可注册后经 setter 接线)。
3. `src/backend/tcop/utility.c`:分派注释更新(优先级不变量 + TDS 现在注册 vtable + hook 继续承载 PG)。

扩展(babelfish_extensions):

4. `contrib/babelfishpg_tsql/src/pl_handler.c`:`#include "postmaster/protocol_routine.h"`;`_PG_init` 在 ProcessUtility_hook 安装后调 `SetProtocolRoutineProcessUtility(COMPAT_PROTOCOL_TDS, bbf_ProcessUtility)`;`_PG_fini` 对应置 NULL。**ProcessUtility_hook 安装保持不变**。

## 5. 验证要点(试点验收)

- TDS:DDL 全套(IDENTITY/SEQUENCE/DECIMAL(38,5)/ORDER BY NULLS 等组 B 用例)仍通过;`SELECT 5/2` 等类型语义不变。
- PG:`5/2=2` 不变;**关键回归点**:PG 协议对 T-SQL 视图的 ALTER VIEW 仍被阻止(证明 hook 仍服务 PG)。
- MySQL:`5/2=2.5000` 不变。
- 五套件基线 meson test 全绿。
- 分派路径变更的证据:bbf_ProcessUtility 对 TDS 的入口从 hook 分支变为 vtable 分支(utility.c 层 1),由代码路径保证;行为测试只证明"不变"。

## 6. 建议(试点后)

1. 阶段一试点结果为**保留**——架构收益(TDS DDL 分派 vtable 化)拿到,零行为风险。
2. 阶段一-b(把 PG 连接也迁出 hook)收益低、动链上两环,记录为长期项,不主动做。
3. 阶段二(bbfCustomProcessUtility_hook 新槽位)保持"不主动做"。
