# openHalo 排序规则策略对照:上游 vs 融合树

> 生成时间:2026-08-14。方法:逐点 grep/diff `/home/hlv/openhalo-update/postgres`(openhalo-update 分支,openHalo 源)与 `/home/hlv/openhalo-update/postgresql_modified_for_babelfish`(openhalo-fusion 分支,融合树)。
> 本文档不纳入版本控制,仅作本地工作记录。关联:[FUSION_PLAN.md](FUSION_PLAN.md) §6 P2-1/P2-2/P2-17/P2-18/P2-20、[COLLATION_CLUSTER_SPEC.md](COLLATION_CLUSTER_SPEC.md)。

## 0. 一句话结论

**openHalo 的排序规则策略是:把全部 MySQL 排序语义押在"一张全局默认的 ICU 非确定性排序规则"上,其余全部依赖 PG 内核自身的排序规则推导;不存在任何会话级/库级排序规则状态。** 融合树由于相关文件系原样导入(或经已验收修复),**在策略上与 openHalo 完全同构**;仅有的两处有意分歧(LIKE 映射、bpcharlike)都是融合轮次里修复过的"上游本身有错"的改进,不是新缺口。因此 P2-1/P2-20 等记录项是 openHalo 设计取舍的直接继承,按"与上游持平"口径无需修改。

## 1. 逐点对照表

| # | 维度 | openHalo 源 | 融合树 | 对照结论 |
|---|---|---|---|---|
| 1 | 默认排序规则 | 所有 MySQL 字符串列默认挂 `mysql.case_insensitive`(ICU 非确定性),创建于 `aux_mysql--1.1.sql:21` | `aux_mysql--1.1.sql:21` 同一语句(+额外的 ignore_accents) | **一致** |
| 2 | 赋列位置 | `mys_parse_utilcmd.c:1231` `collateClause->collname = "case_insensitive"` | 同逻辑,行号 +4(版权头) | **一致** |
| 3 | 会话/库级排序规则状态 | **不存在**。`collation_connection`/character_set_client 仅在 SHOW 类输出里硬编码 `'utf8mb4_general_ci'`/`'utf8mb4'` 静态串,无 GUC、无运行时切换 | 同(原样导入) | **一致** |
| 4 | 表达式字面量推导 | 无任何字面量排序规则注入,完全依赖 PG 内核推导(从另一操作数继承) | 同 | **一致** → P2-1 的"字面量对字面量发散"是上游固有 |
| 5 | `COLLATE <name>` 列级子句 | `rectifySpecifiedColumnCollate()` 仅按后缀 `_ci` 匹配,不匹配则静默丢弃 | **字符级一致**(早前已核实) | **一致** → P2-8 |
| 6 | 外键排序规则确定性检查 | `mys_tablecmds.c` 中 `get_collation_isdeterministic` 计数 0,缺失内核检查 | 同 | **一致** → P2-18 |
| 7 | REGEXP/RLIKE 映射 | `REGEXP→~`、`REGEXP BINARY→~`、`RLIKE→~*`、`NOT REGEXP→!~*` 等,无任何非确定性排序规则规避 | 逐字符一致(本round diff 核实,行号偏移 34) | **一致** → P2-17 |
| 8 | **LIKE 映射** | **`LIKE→ILIKE(~~*)` 无条件** | **`LIKE→~~`,交由操作数排序规则决定**(`4553c8dac6`) | **有意分歧(融合修复)**。openHalo 的 ILIKE 映射在 mysql.case_insensitive 下会硬报错(PG 内核 Generic_Text_IC_like 拒绝非确定性排序规则),融合树修复为委托排序规则 |
| 9 | **bpcharlike(CHAR 列 LIKE)** | **自带逐字节、大小写敏感的 `mysql_like_match()`**,完全不读 PG_GET_COLLATION() | **删除该 matcher,委托内核 collation-aware `textlike`**(P2-2 早期修复) | **有意分歧(融合修复)**。openHalo 的 CHAR LIKE 在默认 _ci 排序规则下仍是大小写敏感(与真实 MySQL 不符),融合树修复 |
| 10 | 性能代价 | 非确定性默认排序规则 → `*_pattern_ops` 不可用、LIKE 前缀索引优化禁用 | 同 | **一致** → P2-20 系设计取舍 |

## 2. 两处有意分歧的定性

- 均来自融合轮次中已验收的修复(提交 `4553c8dac6` 与更早的 bpcharlike 修复),性质与 P2-10 相同:"上游本身有错/不可用,修复后已验收"。
- 它们使融合树在 LIKE 维度**优于** openHalo 上游,不违反"与上游持平"原则(该原则约束的是"不新增上游没有的兼容特性",而不是"禁止修复上游的坏行为")。
- 若未来要把融合树与 openHalo 上游再对齐(例如上游改进了自己的实现),这两处需要重新对照,防止回归。

## 3. 由此衍生的最终裁决(与 FUSION_PLAN 一致)

| 项 | 裁决 | 依据 |
|---|---|---|
| P2-1 字面量对字面量发散 | 记录不修 | 上游同构;任何"修复"都会破坏当前正确的 `col='lit'` 或污染 PG/TDS |
| P2-17 REGEXP 不可用 | ⛔ 不做 | 与上游逐字符一致 |
| P2-18 外键检查缺失 | ⛔ 不做(数据完整性注意) | 与上游一致 |
| P2-20 LIKE 走不了索引 | ✅ 已实证核验,维持设计取舍(2026-08-14) | 与上游同构,非 bug;实测补全第三道门槛(`collate_is_c`)与可用出路(pattern_ops 索引),详见 [FUSION_PLAN.md](FUSION_PLAN.md) §6 P2-20 与 [COLLATION_CLUSTER_SPEC.md](COLLATION_CLUSTER_SPEC.md) §1.2 批注 |
| P2-8 COLLATE 子句丢弃 | ⛔ 不做 | 与上游字符级一致 |

## 4. 用户问题"能否对照"的答案

**能,且对照后结论是"没有需要追赶的差距"**:融合树的排序规则行为与 openHalo 同构(9/11 项一致),仅有的两处分歧都是融合期间修复过的上游缺陷(LIKE/bpcharlike),方向是优于上游。若用户期望的是"引入 openHalo 的会话级排序规则能力"——openHalo **本身没有**这种能力(硬编码静态串),因此按持平口径也不应新增。
