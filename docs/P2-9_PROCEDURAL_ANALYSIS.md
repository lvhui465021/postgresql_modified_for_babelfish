# P2-9 架构分析:MySQL 存储过程/触发器的过程式语言

> 本文档不纳入版本控制,仅作本地工作记录。
> 分析:2026-08-14(Opus 5 + max reasoning)
> 关联:[FUSION_PLAN.md](FUSION_PLAN.md) §6 P2-9

---

## ⛔ 结论已作废:本项不做(2026-08-14 用户定的范围原则)

**用户原则**:融合阶段若某兼容特性 **openHalo 和 Babelfish 上游本身也没实现**,则不做修复和实现,保持与上游兼容特性一致即可。

**核对结果(代码核实)**:openHalo 源仓库 `/home/hlv/openhalo-update/postgres`(分支 `openhalo-update`,PG18.3)的
`src/backend/parser/mysql/mys_gram.y` 里,过程体语法与本融合树**完全一致**:

```
routine_body_stmt:
			stmt
			| ReturnStmt
		;
```

即 **openHalo 上游本身就没有实现 MySQL 过程式语言**,这不是融合过程引入的回归。

**因此 §4 推荐的"路线 B:转译成 PL/pgSQL"以及 §6 的下一步建议全部作废** —— 那属于超越上游去实现 MySQL 全部特性,不符合本项目"三协议融合、保持上游兼容特性不变"的目标。

**本文档以下内容仅作为技术档案保留**(记录了当前实现范围、工作量参照系、以及若将来真要做时的路线对比),**不构成行动建议**。

---

## 1. 当前状态(代码核实)

### 1.1 存储过程/函数

`src/backend/parser/mysql/mys_gram.y` 的过程体语法:

```
routine_body_stmt:
			stmt
			| ReturnStmt
		;
```

过程体**只接受普通 SQL 语句或 `RETURN`**,零过程式构造。配合 11703 行的 `BEGIN_P ATOMIC routine_body_stmt_list END_P`,可以确认:MySQL 的 `CREATE PROCEDURE`/`CREATE FUNCTION` 直接复用了 **PostgreSQL 原生 SQL-standard 函数体**(PG14+ 的 `BEGIN ATOMIC`),一行都没扩展。

实测(阶段二)确认的后果:

| 写法 | 结果 |
|---|---|
| `BEGIN ATOMIC SELECT 1; END` | 可以创建 |
| `BEGIN SELECT 1; END`(MySQL 标准写法) | **语法错误** |
| `DECLARE x INT DEFAULT 5;` | **语法错误** |
| `IF/WHILE/LOOP/REPEAT` | **语法错误** |
| `DECLARE CURSOR` + `DECLARE ... HANDLER` | **语法错误** |
| `CALL p()` 里过程体的 `SELECT` 结果 | **不返回给客户端**(与 PG `CALL` 一致,与 MySQL 不一致) |

### 1.2 触发器

`mys_gram.y:8792` 的 `CreateTrigStmt` 第一条产生式:

```
CREATE opt_or_replace TRIGGER name TriggerActionTime TriggerEvents ON
qualified_name FOR EACH ROW InsertStmt
```

触发器体**只接受单条裸 `InsertStmt`**。没有 `BEGIN...END`、没有 `SET NEW.col = ...`、没有条件与循环。真实 MySQL 触发器最常见的写法(`BEGIN ... SET NEW.x = ...; INSERT INTO audit ...; END`)全部不可用。

### 1.3 没有 MySQL 的 PL handler

`src/pl/` 下只有 `plperl` / `plpgsql` / `plpython` / `tcl`,**不存在 MySQL 过程语言的 handler**。

---

## 2. 工作量参照系(代码核实)

同一棵树里的 Babelfish T-SQL **已经**有完整的过程式语言实现,可作为"从零写"的量级参照:

| 文件 | 行数 |
|---|---|
| `babelfishpg_tsql/src/pl_exec.c` | 10584 |
| `babelfishpg_tsql/src/pl_gram.y` | 8381 |
| **仅这两个文件小计** | **≈19000** |

而 `contrib/babelfishpg_tsql/src/` 下共有 **10 个** `pl_*.c` 文件(`pl_comp.c`/`pl_comp-2.c`/`pl_exec.c`/`pl_exec-2.c`/`pl_funcs.c`/`pl_funcs-2.c`/`pl_gram.c`/`pl_handler.c`/`pl_scanner.c`/`pl_explain.c`)。

**结论:"给 MySQL 从零写一个 PL handler"是万行级、数月量级的工程。** 这个数字应该直接决定选型——不要在没有对比过替代方案前就走这条路。

---

## 3. 关键架构洞察:MySQL 过程式语言与 PL/pgSQL 高度同构

这是本次分析最有价值的发现。MySQL 的存储过程语言与 PL/pgSQL 在**结构上几乎一一对应**:

| MySQL | PL/pgSQL | 差异 |
|---|---|---|
| `DECLARE v INT DEFAULT 0;` | `DECLARE v INT := 0;` | 仅语法糖 |
| `SET v = expr;` | `v := expr;` | 仅语法糖 |
| `IF ... THEN ... ELSEIF ... END IF` | `IF ... THEN ... ELSIF ... END IF` | 关键字拼写 |
| `WHILE ... DO ... END WHILE` | `WHILE ... LOOP ... END LOOP` | 仅语法糖 |
| `LOOP ... END LOOP` | `LOOP ... END LOOP` | 相同 |
| `REPEAT ... UNTIL cond END REPEAT` | `LOOP ... EXIT WHEN cond; END LOOP` | 结构改写 |
| `LEAVE label` / `ITERATE label` | `EXIT label` / `CONTINUE label` | 仅语法糖 |
| `DECLARE c CURSOR FOR ...` | `DECLARE c CURSOR FOR ...` | 几乎相同 |
| `OPEN/FETCH/CLOSE` | `OPEN/FETCH/CLOSE` | 几乎相同 |
| `DECLARE CONTINUE HANDLER FOR NOT FOUND ...` | `EXCEPTION WHEN no_data_found ...` | **语义有实质差异**,见下 |

**因此存在一条工作量低一个数量级的路径:把 MySQL 过程体转译成 PL/pgSQL,复用 PG 已有的成熟执行器,而不是从零写 PL handler。**

### 3.1 转译路径的真实难点(不要低估)

不是所有构造都能平凡转译:

1. **`DECLARE ... HANDLER` 的 CONTINUE 语义**。PL/pgSQL 的 `EXCEPTION` 块捕获后**退出**该块(等价于 MySQL 的 `EXIT HANDLER`);MySQL 的 `CONTINUE HANDLER` 要求处理完**回到出错语句的下一条继续执行**。PL/pgSQL 没有直接对应,需要把受保护的语句序列逐条包进独立的子块——转译器要做控制流重写,不是文本替换。

2. **裸 `SELECT` 返回结果集(最难的一块)**。MySQL 过程体里的 `SELECT` 会把结果集直接发给客户端,且**可以有多个**。PL/pgSQL 没有这个概念,PG 的 `CALL` 也不转发。这一块**转译解决不了**,必须协议层配合。
   - 好消息:MySQL 线协议本身支持多结果集,且本项目已实现相关机制(`aux_mysql` 的 `mysql_end_command()` 有 `more_results` 处理,`allow_multi_statements`/`set_simple_query_more_results` 等 ProtocolRoutine 槽位已存在)。
   - 所以这是"把已有的多结果集能力接到过程调用上",而不是从零造。

3. **`sql_mode` 相关的运行时语义差异**(除零行为、字符串截断等)——已有 `mys_sqlMode` 基础设施,但过程体内是否生效需要单独验证。

---

## 4. 三条候选路线对比

| 路线 | 工作量 | 兼容度上限 | 风险 |
|---|---|---|---|
| **A. 从零写 MySQL PL handler** | 万行级(参照 T-SQL ≈19000 行) | 最高,可 100% 对齐 MySQL | 极高;且与 PG 执行器演进长期脱节,维护负担重 |
| **B. 转译成 PL/pgSQL** | 千行级(转译器 + 语法扩展) | 高,但 CONTINUE HANDLER、多结果集等需专门处理 | 中;错误消息/行号会指向转译后的代码,调试体验差 |
| **C. 现状(不做)** | 0 | 极低,MySQL 存储过程基本不可用 | 迁移场景直接卡死 |

**倾向推荐 B**,但**必须先做一个原型验证**再定:选一个覆盖了 `DECLARE`/`IF`/`WHILE`/游标/`CONTINUE HANDLER` 的真实 MySQL 存储过程,人工转译成 PL/pgSQL 跑通,确认语义能对齐、错误处理可接受。原型通过再投入写转译器。

**不推荐 A**,除非原型证明 B 的语义鸿沟无法弥合。

---

## 5. 与 T-SQL 侧的对比(重要参照)

阶段二实测:同一轮测试里 T-SQL 侧的 `AFTER` 触发器(含 `inserted` 伪表)、存储过程嵌套调用 + `TRY/CATCH` + `ERROR_MESSAGE()`/`ERROR_PROCEDURE()` **全部正确工作**。

这不是因为 T-SQL 更容易,而是因为 **Babelfish 已经付出了那 19000 行的代价**。MySQL 侧要达到同等成熟度,要么付同样的代价(路线 A),要么找到更省的路(路线 B)。

**这个对比应该纳入决策:三协议并行架构里,MySQL 的过程式能力目前是明显短板,且不是靠小修小补能补上的。**

---

## 6. 建议的下一步(按顺序)

1. **决策**:先由用户拍板走路线 B 还是 A(或明确接受现状)。这是投入数周工程量的决定,不应由实施者默认。
2. 若选 B:做 §4 说的**人工转译原型**,验证语义鸿沟(重点验 `CONTINUE HANDLER` 和多结果集)。
3. 原型通过后,再写实施规格,拆成:①过程体语法扩展 ②转译器 ③多结果集协议对接 三个独立可验证的阶段。
4. 触发器(§1.2)可以作为**第一个落地目标**——它比存储过程简单(无参数、无返回值、无多结果集问题),可以先用它把"过程体语法 + 转译"这条链走通,风险最小。

---

## 7. 明确不在本文档范围

- 具体的转译器实现设计(要等路线决策)
- MySQL `sql_mode` 在过程体内的语义对齐
- `SHOW CREATE PROCEDURE` 的输出格式(与 P2-7 的 `SHOW CREATE VIEW` tokenizer 问题同源)
- 存储过程的权限模型(`SQL SECURITY DEFINER/INVOKER`)
