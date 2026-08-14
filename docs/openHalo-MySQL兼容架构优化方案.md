# openHalo MySQL 兼容架构优化方案

> **架构研发方案，不含实施。** 基于 Babelfish / IvorySQL / openHalo 三套源码的实测对照，优化 openHalo 现有兼容架构。

## Context

工作区并存三套可对照的 PG 兼容实现，均已本地取证（非类比推断）：

- **Babelfish**（`postgresql_modified_for_babelfish`，PG18.3，分支 `BABEL_6_0_STABLE__PG_18_3`）：兼容 SQL Server，兼容逻辑全在扩展
- **IvorySQL**（`IvorySQL`，PG18.3，分支 `IVORY_REL_5_STABLE`）：兼容 Oracle
- **openHalo**（`postgres`，PG18.3 + 70 自有提交，分支 `openhalo-update`）：兼容 MySQL

> ✅ **版本对齐声明（2026-07-31 确认）**：三方现已全部落在 **PG 18.3** 同一基线——这是方案一直追求的「苹果对苹果」对照，此前 Babelfish=PG17、IvorySQL=PG19devel 的记录已过时（各自随上游分支演进所致，非本方案变更）。**本节起对照表已在对齐后的分支上重新实测**，方法统一为：`git archive` 取三方当前 HEAD 与上游 `REL_18_3` 的 `src/` 树做全量比对，逐文件排除纯版权头改动后计数（见下方各行的重测批注）。
>
> ⚠️ **重要更正**：本方案早期版本依据网络检索，错误描述 IvorySQL「解析器留在内核」，并据此建议 openHalo 保持解析器编进内核。**源码证明恰恰相反**——IvorySQL 的 Oracle 语法构建为 `liboracle_parser.so`，靠 `_PG_init` 挂钩注册。该结论已全面订正，方向随之改变。

### 已定决策（无待决项）

| 决策 | 结论 | 依据 |
|---|---|---|
| 解析器打包 | **独立 `.so`** | IvorySQL 在同量级（22,367 行）已验证 |
| 代码归属 | **树内源码 + `.so` 产物**（非独立 repo） | 照搬 IvorySQL：`liboracle_parser` 在 `src/backend/`、`ivorysql_ora` 在 `contrib/`。构建链最简（复用已验证的 `Makefile.shlib`），版本天然对齐，`initdb` 集成无障碍 |
| G1 修复路线 | **`MyCompatMode` 综合设计**（集群默认 + 协议覆盖 + 并行传播） | 纯集群级会堵死后续融合 Babelfish T-SQL 模块 |
| 扩展命名 | **保留 `aux_mysql`**，只扩充内容并延伸 1.5→1.6→… 版本链 | 改名会使 `ALTER EXTENSION UPDATE` 失效（PG 不支持跨扩展名升级） |
| 目标平台 | **先按 Linux**，`PGDLLIMPORT` 审计列为待触发项 | Linux backend 以 `-Wl,--export-dynamic` 链接，扩展调内核符号无障碍 |
| 语法拼接 / DDL 钩子化 | **否决**（见第二节实测依据） | — |

---

## 一、三方架构实测对照

| 维度 | Babelfish | IvorySQL | openHalo |
|---|---|---|---|
| 方言与 PG 距离 | 近（T-SQL 同宗 SQL-92） | 远（Oracle） | 远（MySQL） |
| 语法规模 | 5,308 行片段拼接 <sup>†未重测</sup> | 22,367 行完整 fork <sup>✅重测</sup> | 24,447 行完整 fork <sup>✅重测，未变</sup> |
| **语法打包** | 扩展内 bison 重建 | **独立 `.so`** | **编进 backend（孤例）** |
| 解析器分派 | `raw_parser_hook` + `sql_dialect` GUC | `sql_raw_parser`/`ora_raw_parser` 函数指针 | `ParserRoutine` vtable |
| 加载方式 | `shared_preload_libraries` | **`initdb -m` 自动写入 preload** | 编译期固定 |
| 模式粒度 | per-session GUC | 集群级（initdb 固化） | 集群级 GUC + 连接级协议 |
| 内核文件侵入 <sup>✅重测</sup> | **263 文件 / 9,909 行** | **602 文件 / 30,970 行** | **58 文件 / 1,876 行（最少）** |
| 全局 `*_hook` 变量 <sup>✅重测</sup> | 175 | 49 | 38（= 上游在本版本的原生数，新增 0） |
| **vtable 函数指针**（统一口径） | 21 <sup>†未重测</sup> | 2 <sup>✅重测确认不变</sup> | **55**（Protocol 23 + Parser 22 + ADT 10）<sup>⚠️ 融合树口径见下方 P3-4 更正</sup> |
| 其中**有活调用点** | — | 2 | **28**（Protocol 23 + Parser 5 + ADT 0） |
| **已声明契约面合计** | **196**（175+21，hook 部分已按上行更新，vtable 部分待重测） | **51**（49+2，最精简） | **93**（38+55） |
| 解析器文件内联方言判断 <sup>✅重测，P1 更正</sup> | — | **35 处 / 10 文件**（`parse_expr.c` 9 / `parse_coerce.c` 6 / `parse_func.c` 5 / `analyze.c` 4 / `parse_package.c` 4 / `parse_clause.c` 1 / `parse_type.c` 2 / `parse_utilcmd.c` 2 / `parse_oper.c` 1 / `parse_relation.c` 1） | **0 处**（全走 vtable） |
| 专属 NodeTag | 无 | 无 | **有（5 个，中段插入，孤例）** |
| `tablecmds.c` <sup>✅重测</sup> | **214 行 / 15 处内联判断** | **981 行 / 34 处内联判断** | **17,448 行 fork（孤例，本项与上游合并文件无关，未受版本对齐影响）** |
| 扩展规模 <sup>✅重测</sup> | 173.6k C + 183.8k SQL | 21.9k C + 22.3k SQL | 37.0k SQL |
| `pg_dump` 适配 <sup>✅重测</sup> | 210 行 spec（`BabelfishDump.spec`）+ 15 处 | 16 处 + 350 行专项 TAP | **零（缺口，未变）** |
| 并行兼容上下文 | `FixedParallelState` 传播 | 不需要（集群级模式） | **缺失（缺陷）** |

**读法**：openHalo 在**内核被修改文件数**上仍领先（58 vs 263 vs 602），且新增全局 hook 仍为 0——这两点在版本对齐后**依然成立**，是真实优势，应保护。但在**解析器打包**、**DDL fork**、**专属 NodeTag**、**pg_dump 适配**四项上仍是三家中的孤例。`tablecmds.c` 一项差距在重测后进一步拉大——IvorySQL 实际有 981 行内联判断（非早期记录的 7 行），openHalo 保持零改动。
>
> ⚠️ **重测批注（2026-07-31，回应外部评审 P0）**：本表此前在三方版本未对齐时（Babelfish PG17 / IvorySQL PG19devel / openHalo PG18.3）测得，用的是各自当时的分支内容。版本对齐后，标 <sup>✅重测</sup> 的行已在当前分支（均为 PG18.3）用统一方法重新实测：`git archive` 取各仓 HEAD 的 `src/` 树与上游 `REL_18_3` 的 `src/` 树逐文件比对，排除纯版权头改动。标 <sup>†未重测</sup> 的行**仍是对齐前的旧数字**，其方法学（vtable 计数需要逐 struct 手工核验、Bbf 的语法拼接行数需要专门统计口径）与本次重测使用的整树 diff 不同，为避免混用口径造成新的错误，本轮未覆盖，标注保留待专项重测。**「已声明契约面合计」行的 Babelfish 数字部分更新（hook 已重测、vtable 未重测），故为半可靠状态，标注说明**。
>
> **度量口径（必须统一，可复现）**：vtable 数 **struct 内的函数指针成员**，含 typedef 化写法。实测 `ProtocolRoutine` **23** / `ParserRoutine` 22 / `ADTExtMethod` 10 = **55**。数全部成员则为 26/25/11 = **62**。活调用点：逐成员 `grep -rn -- "->名字(" --include='*.c' src/backend`。全局 hook 变量口径：`.c` 文件中 `TYPE_hook_type\s+name_hook\s*=` 形式的全局定义，去重计数（不同于早期版本使用的粗口径）。
>
> ⚠️ **不要用 `grep -cE '\(\*[A-Za-z_]+\)\s*\('`**（本文档早期采用，已弃用）：该命令对三个头文件含义不同——`protocol_routine.h`/`parserapi.h` 数的是 struct 成员，`adtextapi.h` 数的却是文件顶部的 **typedef 声明**（其 struct 成员全用 typedef 写法，正则匹配不到）；10 这个数字只是碰巧与成员数相等。且该命令会漏掉 `ProtocolRoutine.process_utility`（`ProcessUtility_hook_type` typedef 成员，是货真价实的回调，有活调用点），故实测 22 应为 23。
>
> 产生式对比的 408 条同名项，提取方法为 `grep -oE "^[a-z_][a-z_0-9]*:" <file> | sort -u` 后取交集（去重后的产生式头）；若不去重会得到更大数字，两者不可混用。
>
> 代码行数：`parser/mysql` 目录全部文件 43,453 行，其中 `.c/.h/.y/.l` 为 43,071 行；本文档统一采用**目录全量**口径。
>
> ⚠️ **度量更正（第二轮外部评审触发）**：早期版本称「openHalo 30 hook，三家最少，扩展面最精简」**错误**——所用 grep 只匹配全局 `*_hook` 变量，未统计 vtable。**第三轮评审进一步查出**：更正后的「88」本身也是错的，它把 Protocol 数函数指针、Parser 与 ADT 数字段，三种口径混用拼凑而成。统一口径后为 **85**（30 hook + 55 函数指针）<sup>⚠️ 第七轮 hook 口径更新后 openHalo 契约面为 **93**（38 hook + 55）</sup>。**最精简的仍是 IvorySQL 而非 openHalo**，但见下方「死契约面」——55 个 vtable 契约中有 **27** 项无活调用点。
>
> 但需同时看另一半：IvorySQL 在 51 之外还有 **35 处内联方言判断散落在 10 个解析器文件**中，openHalo 在同类文件中为 0 处。真实取舍是「大而规整的形式化 API 面（openHalo）」对「小 API 面 + 散落内联条件（IvorySQL）」。openHalo 的选择可辩护（可测试、有边界、无散落条件），但**不得再宣称「侵入最小」**。
>
> ⚠️ **该数两轮外部评审后重测，第六轮评审的复核结果同样有误差**：早期记「~25 处 / 3 文件（`analyze.c` 5 / `parse_func.c` 6 / `parse_expr.c` 14）」，分文件数偏高而总量偏低；第五轮评审更正为「31 处 / 8 文件」，仍漏了 `parse_package.c`(4) 与 `parse_type.c`(2)。第六轮评审指出漏项、给出「36 处 / 10 文件」（已含 `parse_package.c` 与 `parse_type.c`），但该数把 `parse_clause.c:3942` 的**一行注释**（`* but this GUC only valid in DB_ORACLE model`）当成了判断，故比剔注释后的精确值多 1。逐行核对（口径 `grep -cE "compatible_db|ORA_PARSER|PG_PARSER|DB_ORACLE|ORA_MODE|ivy_"`，剔除注释行，`parse_expr.c:961-962` 的跨行 `if` 条件按 1 处计）：精确值为 **35 处 / 10 文件**——`parse_expr.c` 9（非 10，两行同一条件）、`parse_coerce.c` 6、`parse_func.c` 5、`analyze.c` 4、`parse_package.c` 4、`parse_clause.c` 1（剔除注释后仅 `3944` 一处代码判断）、`parse_type.c` 2、`parse_utilcmd.c` 2、`parse_oper.c` 1、`parse_relation.c` 1。修正方向对本节论点仍是**加强**而非削弱。
>
> 另：外部评审预测「解析器外置后需新增 `raw_parser_hook`」**不成立**——`ParserRoutine.raw_parse` 已存在，外置不增内核 API 面，22 个回调早已付过。

### 澄清：外置**不会**让契约面收敛（第二轮外部评审触发；标题原用旧数 88，现口径为 93）

第二轮评审提出「阶段 2-3 把 ADT(11)+Protocol(22)+Parser(25) 外移后，内核扩展面将趋近 30，与 IvorySQL 的 41 同量级，当前 88 只是未解耦的暂时臃肿」。**该推断不成立。**

vtable 声明是**内核与模块间的契约**；外置的是*实现*，不是*声明*——否则内核无从调用模块。**IvorySQL 即是反证**：它已完成解析器外置，而 `sql_raw_parser`/`ora_raw_parser` 仍声明于内核 [parser.h:67-68](IvorySQL/src/include/parser/parser.h#L67)，3 个辅助 hook 仍在内核 `src/include/oracle_parser/`。**51 是 IvorySQL 外置后的数字（49 hook + 2 函数指针，第七轮口径），不是外置前的。**

openHalo 同理：阶段 3 完成后 `postgres.c` 仍调 `routine->read_command()`（实测 `ProtocolRoutine` **23 个回调全部有活调用点**）。**已声明的契约面不因外置而缩小**，且性质变差——从「内部 API（可随重编译自由变更）」变为「跨二进制 ABI（变更会破坏已加载模块，需版本管理）」。外置使契约面更**硬**，而非更少。

> ⚠️ **本节早期版本的两处举例系虚构，已删除**（第三轮外部评审查出）：曾称「`numeric.c` 仍调 `adtext->pre_numeric_in()`」「`analyze.c` 仍调 `p_parser_routine->transformSelectStmt()`」。实测 `numeric.c` 对 `adtext` **零引用**；`analyze.c` 只调 `transformOptionalSelectInto`(287-288) 与 `transformOnConflictArbiter`(1245-1246)，**无 `transformSelectStmt` 调用点**。结论（外置不缩小契约面）依据 IvorySQL 实证仍然成立，但论据必须换成上面已核实的 `ProtocolRoutine` 数据。

### 真实收敛路径：清理死契约面（非外置、非降低深度）

> 本节结论已被第三轮评审推翻并重写。早期版本称「openHalo 方言侵入至 parse-analysis 层、IvorySQL 止步 raw-parse 层，差距根源是集成深度」——**该论断错误**，它把「已声明但未接线的槽位」误当成了「实际集成」。

**实测活调用点**（`grep "parser_routine->X\|parserengine->X"`）：

| vtable | 已声明函数指针 | **有活调用点** | 死槽位 |
|---|---|---|---|
| `ParserRoutine` | 22 | **5** | 17 |
| `ProtocolRoutine` | **23** | **23（全活）** | **0**<sup>⚠️ P3-4 更正,见下方</sup> |
| `ADTExtMethod` | 10 | **0** | 10 |
| 合计 | **55** | **28** | **27** |

> ⚠️ **本表 `ProtocolRoutine` 行的早期数据「16 活 / 6 死」系伪造，已更正**（第四轮外部评审查出）。成因：当时的验证脚本**手工列举了 16 个回调名**，把输出行数 16 当成「22 个中有 16 个活」，剩余 6 个（`set_remote_dest_receiver_params` / `allow_multi_statements` / `simple_query_statement_ends_xact` / `set_simple_query_more_results` / `before_simple_query_statement` / `report_parameter_status`）**从未测试**，「6 死」是用 22 减出来的。改用从头文件自动提取成员名后实测：**全部有活调用点，零死槽位**，分布于 `postgres.c`(9) / `dest.c`(5) / `backend_startup.c`(3) / `postinit.c` / `elog.c` / `guc.c` / `utility.c` 各 1。
>
> 口径补充：`ProtocolRoutine` 的 `process_utility` 是 `ProcessUtility_hook_type` typedef 型指针，不匹配 `(*name)(` 提取式。它是真回调且在 `utility.c` 有活调用点，故本表按 **23** 计（早期的 22 是提取式漏项，非设计差异）。

`ParserRoutine` 仅 5 个活：`raw_parse` / `transformOptionalSelectInto` / `transformOnConflictArbiter` / `transform_expr_node` / `figure_colname`。`MySQLParserRoutine` 结构体实测也只赋值这 5 个字段，其余 17 个为 `NULL`。

**这直接修正了三方对比的核心论断**：IvorySQL 解析器集成 = 2 个函数指针 + 3 个 hook = **5 个活接入点**；openHalo = **5 个活接入点**。**活接入点数相当，但层级分布不同**——不存在「openHalo 集成点更多」，措辞需精确到层级（第五轮评审要求补限定语，其自身的层级归属两边均有误，下表为实测更正）：

| 层 | openHalo 5 个活回调 | IvorySQL 5 个活接入点 |
|---|---|---|
| **raw-parse** | `raw_parse`（[postgres.c:775](postgres/src/backend/tcop/postgres.c#L775)） | `sql_raw_parser`（[parser.c:49](IvorySQL/src/backend/parser/parser.c#L49)）、`ora_raw_parser`（[guc_funcs.c:111](IvorySQL/src/backend/utils/misc/guc_funcs.c#L111) 按 GUC 切换） |
| **parse-analysis** | `transformOptionalSelectInto`（[analyze.c:288](postgres/src/backend/parser/analyze.c#L288)）、`transformOnConflictArbiter`（[analyze.c:1246](postgres/src/backend/parser/analyze.c#L1246)）、`transform_expr_node`（[parse_expr.c:156](postgres/src/backend/parser/parse_expr.c#L156)）、`figure_colname`（[parse_target.c:106](postgres/src/backend/parser/parse_target.c#L106)） | **0**——同层集成改由 35 处内联判断承担 |
| **内省 / deparse / contrib** | 0 | `get_keywords_hook`（[misc.c:433](IvorySQL/src/backend/utils/adt/misc.c#L433)，`pg_get_keywords()`）、`quote_identifier_hook`（[ruleutils.c:13533](IvorySQL/src/backend/utils/adt/ruleutils.c#L13533)）、`fill_in_constant_lengths_hook`（[pg_stat_statements.c:2942](IvorySQL/contrib/pg_stat_statements/pg_stat_statements.c#L2942)，在 contrib 不在内核） |

> **第五轮评审两处归属错误的更正**：评审称「openHalo 的 5 个含 **2** 个分析层钩子」——实测为 **4** 个；称「IvorySQL 的 5 个全在 raw-parse/关键字层」——实测只有 2 个在 raw-parse，其余 3 个分散在内省函数、deparse、contrib 三个不同层，且**无一在 parse-analysis 层**。
>
> **正确的对比结论**：两家在 parse-analysis 层都要集成，差别只在**形式**——openHalo 形式化为 4 个 vtable 槽位（可枚举、可测试、可 CI 校验），IvorySQL 散落为 35 处内联 `if`（不可枚举、无边界）。这与本节上文「大而规整 API 面 vs 小 API 面+散落内联」的取舍判断一致，**不是 openHalo「集成更深」**。

**死契约全部集中在 `ParserRoutine`(17) 与 `ADTExtMethod`(10)——`ProtocolRoutine` 是完全接线的健康样板**，其 23/23 活调用点说明「vtable 契约面大」本身不是问题，问题是声明了却不接线。

> ⚠️ **P3-4 更正（2026-08-14，融合树实测）**：本节及上方活调用点表测的是 **openHalo 源仓库**（`postgres` repo，ProtocolRoutine 22 个括号成员 + `process_utility` = 23 个函数指针）。两点需要按融合树现状订正：①「23/23 全活」在 openHalo 源上并不严格成立——`mainfunc` 的活调用点（`backend_startup.c` 的主循环分派）是融合轮次提交 `4a43654840` 才补上的，此前该槽位无调用点；②融合轮次（P3-1，`14970f4e7e`）把 `ProtocolRoutine` 扩至 **26 个括号成员**（新增 `accept`/`close`/`direct_ssl_handshake`），并**整体退役了与 vtable 并行的 `ProtocolExtensionConfig`（19 字段）机制**（TDS 从 pe_config 迁入 vtable，ListenConfig/default_protocol_config/libpq_* 包装全部删除）。实测融合树当前 28 个函数指针型成员（26 括号 + `process_utility` + `parser_routine` 字段读取）**全部有活调用点或读取点**，分布于 postmaster.c(accept/close) / backend_startup.c(mainfunc/init/startup_exchange/direct_ssl_handshake) / postgres.c / dest.c / elog.c / guc.c / postinit.c / utility.c。即「完全接线的健康样板」这一结论在融合树上**仍然成立且更强**——两套并行机制并存的历史包袱已消除。

**死代码有两层**：
1. 内核侧 17 个已声明但未赋值的 `ParserRoutine` 槽位 + 10 个零消费者的 `ADTExtMethod` 函数指针
2. MySQL 侧对应实现（`mys_transformSelectStmt` / `mys_ParseFuncOrColumn` / `mys_func_get_detail` / `mys_transformGroupClause` 等）**定义存在但从未接线**，属休眠代码

#### 休眠实现的量化（第六轮评审要求补，逐函数实测）

对 17 个死槽位逐个查全仓库调用点，结论**不是 17 个实现全休眠**——**其中 2 个是活的，走直调不走 vtable**：

| 实现 | 状态 | 依据 |
|---|---|---|
| `mys_transformCreateStmt` | **活** | [mys_utility.c:913](postgres/src/backend/tcop/mysql/mys_utility.c#L913) 直调 |
| `mys_transformAlterTableStmt` | **活** | [mys_tablecmds.c:3521](postgres/src/backend/commands/mysql/mys_tablecmds.c#L3521) 直调 |
| 其余 15 个 | **休眠** | 全仓库外部引用**仅头文件声明**，零调用点（`mys_ParseFuncOrColumn` / `mys_func_get_detail` 的两处命中经核实是注释） |

这两个活实现都在 `mys_parse_utilcmd.c`（5,633 行），**该文件不得计入休眠量**。净量：

| 项 | 行数 |
|---|---|
6 个文件：`mys_parse_expr.c` 3,443 / `mys_parse_func.c` 1,528 / `mys_analyze.c` 1,470 / `mys_parse_clause.c` 1,115 / `mys_parse_agg.c` 987 / `mys_parse_oper.c` 582（其中 `mys_analyze.c`、`mys_parse_clause.c` 各含一段经 vtable 接线的活代码，非整文件零可达，见下方口径说明） | **9,125（占 `parser/mysql` 43,453 的 20.9%）** |
| 其中仍有活路径的部分：`mys_transformOptionalSelectInto`(40) + `mys_transformOnConflictArbiter`(78) + 其静态助手 `resolve_unique_index_expr`(86) | −204 |
| **净休眠** | **约 8,921 行** |

> **口径提醒**：9,125 是**文件全量**口径（含上述 204 行活代码），8,921 是**净休眠**口径。第六轮评审给的「~9.1k / 21%」等于文件全量口径，数值可用，但引用时须标明它包含 204 行活代码，且**不含**被误判的 `mys_parse_utilcmd.c`。
>
> **这些文件是真的编进二进制的**：`parser/meson.build:69-75` 把 6 个文件全部加入 `parser` static_library（67-68 行加入的是另外两个活文件 `mys_expr_transform.c` 与 `mys_gram_globals.c`/`mys_namespace_stubs.c`，不属于本节讨论范围）。清理它们等于净减 ~8.9k 行编译产物。
>
> **「零可达文件」措辞需要限定**（第七轮评审指出）：6 个文件中 `mys_analyze.c` 的 `mys_transformOptionalSelectInto`（40 行）与 `mys_parse_clause.c` 的 `mys_transformOnConflictArbiter`（78 行）+ 静态助手 `resolve_unique_index_expr`（86 行）经 `ParserRoutine` vtable 接线，**是活代码**（见上文「实测活调用点」表）。严格说这 6 个文件是「以整文件计入休眠量、但内部含 204 行活代码」，不是全部零可达。**净休眠 8,921 行的结论不受影响**——它本就是从 9,125 中扣除这 204 行得出的，此处只是澄清表述，不改数字。

### `ADTExtMethod` 的精确构成（第四轮评审精确化）

实测 `ADTExtMethod` = **10 个函数指针 + 1 个 bool**（`allow_zero_length_char_typmod`）。`mys_adtext`（[mys_adtext.c:23](postgres/src/backend/utils/adt/mysql/mys_adtext.c#L23)）实际填充情况：

| 类别 | 数量 | 明细 | 裁决倾向 |
|---|---|---|---|
| 有 MySQL 实现、**零消费者** | 4 | `pre_time_in` / `post_time_out` / `date_in` / `timestamp_in` | **产品决策**，非技术评估（见下方前置决策点） |
| 显式 `NULL`、**无实现无消费者** | 6 | `pre_numeric_in` / `post_numeric_out` / `pre_timetz_in` / `post_timetz_out` / `pre_timestamp_in` / `post_timestamp_out` | **删除**（纯空壳） |
| bool 成员 | 1 | `allow_zero_length_char_typmod = true` | **接线**——第五节阶段 1 的 `varchar.c` 消费的是**这一项** |

> ⚠️ **早期表述已更正**：本方案曾称「`ADTExtMethod` 的 10 个字段…属待接线脚手架」。**不准确**——`varchar.c` 规划消费的是第 11 个 bool 成员，**不在那 10 个函数指针之内**；10 个函数指针中**无一有已规划消费者**。故裁决结果与「脚手架」定性相反：多数应导向**删除**（尤其 6 个空壳项）。这反过来印证了「逐项裁决、不一律处理」原则的必要性——若按早期定性一律保留接线，会留下 6 个永远填不上的空槽。

#### 前置决策点：MySQL type-input 语义是不是需求？（第五轮评审补充）

那 4 个「有实现零消费者」的函数不能停在「需评估」——实测证据表明这是**产品决策**，且当前处于**静默缺失**状态：

- **无替代承载机制**：`mys_date_in` / `mys_timestamp_in` 在 `pg_proc.dat` 与扩展 SQL 中**零注册**（实测均为 0）。全仓库引用仅 4 类：头文件声明、`mys_adtext.c:32-33` 的赋值（该结构体从不被解引用）、函数定义本身、`adtext.c:62` 的一行注释。**这些函数运行时永不执行，且不存在第二条路径承载 type-input 语义。**
- **测试完全无覆盖**：`mysql_compat` 套件**零 type-input 用例**——无「`INSERT ... VALUES ('2020-1-1')` 到 date 列」这类字面量输入测试；仅 `30_functions.sql` / `60_extension_catalog.sql` 覆盖**函数路径**（`str_to_date` / `date_format`，走扩展 SQL）。

**含义**：

1. **「454/454 通过」不代表日期输入兼容**——该路径完全无覆盖，测试通过与该能力无关。
2. 裁决二选一，须由产品定：
   - 若 MySQL type-input 语义**是需求** → 4 个函数必须**接线并补测试**（当前是静默缺失，用户拿到的是 PG 语义而非 MySQL 语义）
   - 若**不是需求** → 连同 6 个空壳项一并**删除**，`ADTExtMethod` 整体退役

##### 决策所需的证据：差分实验（第六轮评审建议，用例经实测更正）

「454/454 通过」不足以支撑决策，产品方需要的是**具体失败清单**。设计如下（成本约 0.5 人天，**排在阶段 0 之内**，见下方依赖说明）：

| 项 | 内容 |
|---|---|
| **方法** | **差分扫描**，非手挑用例：同一批日期/时间字面量分别喂给 ① 走 mysql 协议的 `INSERT INTO t(d DATE/DATETIME/TIME) VALUES (...)` ② 真实 MySQL 实例。记录三态：两边都接受且值相同 / 两边都拒绝 / **行为分歧**（接受性不同，或值不同，或错误码文本不同）。只有第三类进决策清单 |
| **语料** | 覆盖：任意标点分隔符、无分隔符定宽、2 位年、零日期（`0000-00-00`、`2020-00-15`）、越界日（`2020-02-30`）、`TIME` 超 24 小时与负值（`838:59:59` / `-01:30:00`）、带时区偏移、小数秒位数 |
| **判据** | 输出「MySQL 接受而当前实现拒绝」+「两边都接受但结果值不同」+「错误码/文本不同」三张表 |

> ⚠️ **评审给的两个用例不判别，不可直接采用**（实测）：`'2020-1-1'` 与 `'20200101'` **PG `date_in` 自己就接受**——前者走 [datetime.c](postgres/src/backend/utils/adt/datetime.c) `ParseDateTime` 的 `DTK_DATE` 分支，后者走 `DecodeNumberField`（`len >= 6` 时「末 2 位＝日、次 2 位＝月、其余＝年」）。按这两条测会得到**空失败清单**，反而误证「无缺口」。
>
> ⚠️ **不能靠读源码代替实验**：PG 的日期解析器比预期宽得多。`ParseDateTime` 含 `/* ignore other punctuation but use as delimiter */ else if (ispunct(*cp)) { cp++; continue; }`——**任意标点做分隔符 PG 也接受**，与 `mys_decodeStringDatetime`（[mys_timestamp.c:373](postgres/src/backend/utils/adt/mysql/mys_timestamp.c#L373)）的 `ispunct` 分支重叠。两边源码读不出可靠差集，故必须实跑差分。
>
> **源码能确定的判别例目前只有一条**，可作为实验的冒烟用例：14 位无分隔符 DATETIME `'20200101123045'`——PG `DecodeNumberField` 取 `mday=45` 越界报错，MySQL 的定宽内部格式接受。
>
> **依赖说明（第六轮评审的排期建议不可行）**：评审建议「前置到阶段 0 之前，趁基线构建的空档做」。**做不到**——实验要通过 mysql 协议连库，需要可运行的构建产物，而构建产物正是阶段 0 的交付物。正确排法：**阶段 0 构建完成后立刻做**，与基线回归并行，不占关键路径（见第七节阶段 0）。

#### 后果一：ADT 兼容链路整条空转，且 4 个 MySQL 函数运行时从不执行

`adtext` 全局在 [postinit.c:1269](postgres/src/backend/utils/init/postinit.c#L1269) 由 `InitADTExt()` 赋值，**赋值后全仓库无任何解引用**——`grep -rn '\badtext\b' src/` 的全部命中只有 `adtext.c` 自身、`adtext.h` 的 `extern` 声明、`postinit.c` 的 `#include`，以及 `varchar.c:54` 的**一行注释**。

因此 `mys_pre_time_in` / `mys_post_time_out` / `mys_date_in` / `mys_timestamp_in` 这 4 个已实现的 MySQL 函数**在运行时从未被调用**（`mys_pre_time_in_for_subtime` 例外，它被 `mysm/time_func.c` 直接调用，不经 vtable）。MySQL 的日期/时间输入语义当前**并非通过 ADT 链路生效**。

**这直接改写 G1 的定级**（见第四节）：`adtext.c:77` 的 `MyProcPort` 读取虽是缺陷模式，但既然无人消费 `adtext`，它目前是**潜伏缺陷而非活缺陷**。但**「只在 `parsereng.c`」的表述不完整**（第五轮评审更正）——[createas.c:363](postgres/src/backend/commands/createas.c#L363) 的 `MyProcPort->protocol_kind` 直读同样是**活路径**：MySQL 模式执行 CTAS 时必定走到（用于创建 ON UPDATE 触发器）。G1 优先级应分三档：

| 档 | 位置 | 性质 | 缺陷场景 |
|---|---|---|---|
| **P0 活缺陷** | `parsereng.c` | `parserengine` 有 5 个活调用点 | 并行/后台进程中方言解析退回标准 PG |
| **P1 活缺陷（窄）** | `createas.c:363` | CTAS 路径必经 | bgworker / apply worker 执行 CTAS 时 `MyProcPort=NULL`，**静默跳过触发器创建** |
| **P2 潜伏** | `adtext.c:77` | `adtext` 无人消费，暂不发作 | 一旦 ADT 接线即转为活缺陷 |

三者阶段 1 都改走 `MyCompatMode`，按上述优先级排序；`adtext.c` 可与死契约裁决合并处理。

#### 后果二：先裁决，再决定要不要修 `varchar.c`

第五节把 `varchar.c` 改走 `ADTExtMethod.allow_zero_length_char_typmod` 列为阶段 1 项。鉴于整张表零消费者，该改动等于**为一张死表创建第一个消费者**。方向本身合理（消除硬编码 `COMPAT_PROTOCOL_MYSQL`），但**执行顺序应调整**：先完成 ADT 的 11 项裁决，确认这张表要留，再接线；否则可能刚接上就随表一起删。

**收敛路径**：对 55 个已声明契约逐项标注「活 / 接线 / 删除」，删除无计划项及其休眠实现。

**裁决准则**（第五轮评审建议恢复，四问定档；`ParserRoutine` 17 项与 `ADTExtMethod` 10 项逐条过）：

| 问 | 判定 |
|---|---|
| 1. 有已规划的功能需求吗？ | 有 → 候选「接线」；无 → 转问 2 |
| 2. MySQL 侧有实现吗？ | 无实现 → **直接删**（纯空壳，如 ADT 那 6 项）；有实现（休眠代码）→ 转问 3 |
| 3. 影响阶段 3 外置吗？ | 影响 → 需在外置前定夺，不可拖延；不影响 → 转问 4 |
| 4. 是为 T-SQL 等未来方言预留的吗？ | 是 → 保留但**必须登记预留理由与目标版本**，否则视同投机设计删除 |

**裁决表作为阶段 1 的交付物**，逐项记录判定依据，避免下次评审重新论证。

**建议纳入阶段 1**（成本远低于外置，且零功能风险）。这是比外置**更便宜、更安全**的收敛手段。

### 关键实证片段

```c
/* IvorySQL: 内核只留函数指针 —— src/backend/parser/parser.c:33 */
raw_parser_hook_type sql_raw_parser = standard_raw_parser;
raw_parser_hook_type ora_raw_parser = NULL;

/* liboracle_parser.so 的 _PG_init 挂钩 */
ora_raw_parser = oracle_raw_parser;
get_keywords_hook = oracle_pg_get_keywords;

/* initdb.c:1525 —— initdb 自动写入，消除「模式设了但库没加载」半初始化态 */
shared_preload_libraries = 'liboracle_parser, ivorysql_ora'
```

`src/backend/oracle_parser/Makefile`：`NAME = liboracle_parser` / `all: all-shared-lib` / `include Makefile.shlib`——在 PG 源码树内，但产出独立 `.so`。openHalo 的 `utils/ddsm/mysm/` **已是同一形态**，说明构建模式本身已验证。

---

## 二、明确否决的路线（附实测依据）

### 否决：Babelfish 式语法拼接

实测 `mys_gram.y` 与 `gram.y` 的 408 条同名产生式：完全相同 264（65%）/ 微差 ≤4 行 52 / **中改 5–30 行 66** / **重改 >30 行 26**。重改的是表达式语法核心：`func_expr_common_subexpr`(diff=566)、`a_expr`(438)、`alter_table_cmd`(394)、`bare_label_keyword`(149)、`c_expr`(101)、`unreserved_keyword`(100)。

`include.pl` 只能在锚点**追加**，无法表达「修改 `a_expr` 的第 N 个备选分支」。套用需往内核 `gram.y` 塞 **144 处**内联方言判断。

**IvorySQL 独立佐证**：面对同等距离的 Oracle，同样选择 22,367 行完整 fork 而非拼接。**拼接是 T-SQL 近距离方言的特例，不是通用解。**

### 否决：`mys_tablecmds.c` 钩子化（导出内核 static）

实测 123 个同名函数（11,519 行）：完全相同 45 / 微改 ≤10 行 28 / 中改 25 / 重改 >50 行 25。消化 73 个低风险函数需从内核**导出 72 个 static**——把上游随时可改签名的内部实现变成必须兼容的 API，rebase 风险不降反升。

> **补充说明（三方对照后）**：Babelfish(+27) 与 IvorySQL(+7) 都不 fork `tablecmds.c`，但它们走的是**内核内联方言判断**——代码留在 core 内，故不需要导出 static。openHalo 选择了**平行路径**（`mys_utility.c:1114` → `mysAlterTable()`），换来内核 `tablecmds.c` 零改动，代价是复制。
>
> 考虑到 MySQL DDL 的 64 个独有子命令（`AT_ModifyColumn`/`AT_ChangeColumn`/`AT_TableOption`/`AT_DropIndex` 等，PG 无等价物）远多于 Oracle 的差异面，内联路线会把约 6k 行 MySQL DDL 塞进内核 `tablecmds.c`，比 IvorySQL 的 7 行严重两个数量级。**维持平行路径，但需承认这是三家中的孤例，并记录 45 个逐字相同函数为已知冗余。**

---

## 三、核心决策：解析器改为独立 `.so`

**依据**：IvorySQL 在同量级（22,367 行）已验证；openHalo 的 `mysm` 已是 `.so` 形态；三家中 openHalo 是唯一解析器不可独立演进的。

| 项 | 内容 |
|---|---|
| **外置对象** | `src/backend/parser/mysql/` 43,453 行 → `src/backend/mysql_parser/` 产出 `libmysql_parser.so`（**树内源码，`.so` 产物**，与 IvorySQL `src/backend/oracle_parser/` 布局对齐）。**实际外置量取决于阶段 1**：清理 8,921 行净休眠后约为 34.5k 行，与 IvorySQL 的 22.4k 同量级 |
| **内核保留** | `raw_parser` 函数指针 + `ParserRoutine` vtable 骨架 |
| **注册方式** | `_PG_init` 挂钩（照搬 `liboracle_parser.c` 模式） |
| **构建模式** | 复用 openHalo 已有的 `Makefile.shlib` + `all-shared-lib`（`mysm` 同款，已验证） |
| **关键字表生成** | 参照 IvorySQL 的 `src/tools/ora_gen_keywordlist.pl`，为 `mys_kwlist.h` 建独立生成规则。⚠️ **起点比原以为的低**：现有 `mys_kwlist_d.h`（1,376 行）的生成规则**不在构建图内**——它只写在 `parser/mysql/Makefile:28` 与 `parser/mysql/meson.build:30`，而这两个文件分别因「父 `Makefile` 无 `SUBDIRS`」和「`parser/meson.build` 无 `subdir('mysql')`」**均未被引入**。当前构建用的是签入仓库的生成产物（提交 `c65476ee204`），改 `mys_kwlist.h` **不会重新生成**。故本项是「新建并接入生成规则」，不是「搬迁已有规则」 |
| **专属 NodeTag（5 个）** | **必须消除**——IvorySQL 零专属节点即证明可做到。`UserVarRef`/`UserVarAssign`/`SysVarRef` 在 `.so` 内降解为 `FuncExpr`（调已有的 `mys_get_user_var`/`mys_set_user_var`/`mys_get_system_variable`）；`MysVariableSetStmt`/`MysSelectIntoStmt` 在 `process_utility` 内直接消费。**可复用已有机制**：[mys_parser.c:459](postgres/src/backend/parser/mysql/mys_parser.c#L459) 已实现「MySQL 语法动作降级为标准 SQL 文本 → 交 `raw_parser` 重解析」（`if (IsA(rawstmt->stmt, String)) raw_parser(strVal(...), RAW_PARSE_DEFAULT)`）。**但须注意其覆盖面**：该机制是**语句级**替换，只能覆盖 `MysVariableSetStmt`/`MysSelectIntoStmt` 两个 utility 节点；`UserVarRef`/`UserVarAssign`/`SysVarRef` 是**表达式内节点**，无法整句替换成文本，仍须走 `FuncExpr` 降解。故它降低阶段 3 约一半风险，非全部 |
| **引用面（实测更正）** | 早期版本记「树外引用仅 4 处」且与后文「6 处」自相矛盾，**均低估**。实测：**真内核 6 处**（`gram.y` 2 / `parse_target.c` 2 / `tcop/utility.c` 2）＋ **MySQL 自有目录 25 处**（`tcop/mysql/mys_utility.c` 16 / `commands/mysql/mys_prepare.c` 4 / `adapter/mysql/mysql_stmt.c` 3 / `mysql_protocol.c` 2）。后者随代码一并迁移，但仍须逐处改写降解表示 |
| **编号性质更正** | `mys_parsenodes.h` 头注称「NodeTag 480–484 显式分配」**与代码不符**——该头文件只有 `pg_node_attr(no_query_jumble)`，**无 `nodetag_number` 属性**，实为 `gen_node_support.pl` 自动续号；证据是 `$last_nodetag_no` 由 479 改为 484。确切编号需构建后查生成的 `nodetags.h` 方能确认，本文档不再断言具体数值 |
| **插入位置（第四轮新发现，风险升级）** | 更严重的问题不是编号值，而是**插入位置**。`gen_node_support.pl:53` 的 `@all_input_files` 中，`nodes/mysql/mys_parsenodes.h` 排在**第 7 位**（`execnodes.h` 之后、`access/amapi.h` 之前），上游该位置是 `access/amapi.h`。因此这 5 个 tag **插在枚举中段**，把 `amapi.h` 起的**全部标准 PG NodeTag 数值整体后移 5 位**——`$last_nodetag_no` 479→484 只是尾部后果。这意味着 openHalo 与上游 PG18.3 的 NodeTag 数值在大范围内不一致，**消除 NodeTag 由「优雅性诉求」升级为「ABI 兼容性诉求」**。最低成本的临时缓解：把该行移到**末尾**，使 5 个 tag 落在尾部而不扰动标准 tag 编号（不解决根本问题，但成本近乎为零，可在阶段 1 顺手做）。**必须三处同步改，见第七节阶段 1 第 4 项** |
| **`initdb` 集成** | 照搬 IvorySQL：`initdb -m mysql` 自动写入 `shared_preload_libraries`，根治「`database_compat_mode='mysql'` 但库未加载」的半初始化态 |

---

## 四、核心决策：统一兼容上下文（G1 修复 + 多方言可扩展）

### 问题

实测 `InitADTExt()`（[adtext.c:66](postgres/src/backend/utils/adt/adtext.c#L66)）与 `InitParserEngine()`（[parsereng.c](postgres/src/backend/parser/parsereng.c)）用同一模式：

```c
case MYSQL_COMPAT_MODE:
    if ((MyProcPort != NULL) &&                    /* parallel worker 里为 NULL */
        (MyProcPort->protocol_kind == COMPAT_PROTOCOL_MYSQL))
        adtext = GetMysADTExt();
    else
        adtext = GetStandardADTExt();              /* 静默退回 PG 语义 */
```

`database_compat_mode` 是 `PGC_POSTMASTER`（fork 后继承，worker 里有效），但叠加的 `MyProcPort->protocol_kind` 在 parallel worker / bgworker / 逻辑复制 apply worker 中为 NULL → **兼容语义静默退回**。同一条 SQL 串行与并行结果可能不一致。

> ⚠️ **两处缺陷的定级不同（第四轮实测修正）**：`adtext.c` 与 `parsereng.c` 虽是同一缺陷模式，但**风险等级不同**——
>
> - **`parsereng.c` 是活缺陷**：其产出的 `parserengine` 有 5 个活调用点（`raw_parse` 等），并行下确实会静默退回 PG 语义。**必修，阶段 1 优先级最高**。
> - **`adtext.c` 目前是潜伏缺陷**：`adtext` 全局赋值后**全仓库无解引用**（见第三节「后果一」），无人消费即无从触发。**不必单独抢修**，随 `ADTExtMethod` 的 11 项裁决一并处理即可——若裁决结论是删表，该缺陷自然消失；若结论是接线，则必须与接线同批修复，否则接线当天即引入活缺陷。
>
> 早期版本把两者并列为同等紧急，会导致阶段 1 在一个无人消费的表上投入等量工作量。

### 设计：`MyCompatMode` 后端级上下文（IvorySQL 稳定性 + Babelfish 可扩展性）

纯 IvorySQL 式（集群级单模式）风险最小，但**一个集群无法既是 mysql 又是 tsql**，会堵死后续融合 Babelfish T-SQL 模块的路。纯 Babelfish 式（协议决定方言）灵活但需要传播机制且非 parallel 场景仍需单独处理。取二者之长：

| 层次 | 设计 |
|---|---|
| **默认值** | 后端启动时 `MyCompatMode = database_compat_mode`（IvorySQL 式集群默认，保证 worker 里永远有确定值，绝不退回 Standard） |
| **协议覆盖** | 兼容协议监听器接受连接时覆写 `MyCompatMode`（保留多方言并存能力） |
| **传播** | 随并行状态显式传播（Babelfish `FixedParallelState` 模式），覆盖 parallel worker / bgworker / apply worker |
| **消费** | 所有方言决策点**只读 `MyCompatMode`**，禁止直接读 `MyProcPort`——固化为架构纪律 |

**多方言可扩展性**（为后续融合 Babelfish 铺路）：

当前 `DatabaseCompatModeType`（`POSTGRESQL`/`MYSQL`）与 `CompatibilityProtocolKind`（`POSTGRES`/`MYSQL`/`KIND_MAX`）是两个平行枚举，属设计冗余。应合并为单一**方言注册表**：每个方言注册 `{ParserRoutine, ADTExtMethod, ProtocolRoutine}` 三元组，`MyCompatMode` 索引之。新增 T-SQL 方言时只需注册，不改分派代码。

---

## 五、内部一致性修复

| 位置 | 改动 |
|---|---|
| [varchar.c](postgres/src/backend/utils/adt/varchar.c) +19 | 硬编码 `routine->kind == COMPAT_PROTOCOL_MYSQL` 改走 `ADTExtMethod.allow_zero_length_char_typmod`（**该字段已存在**于 [adtextapi.h](postgres/src/include/utils/adtextapi.h)，未被使用） |
| [createas.c](postgres/src/backend/commands/createas.c) +80 | 新增 `ExecCreateTableAs_post_hook`，删除对 `commands/mysql/mys_tablecmds.h` 的直接 `#include` |
| [gram.y](postgres/src/backend/parser/gram.y) +33 | 删除 `MYSQL_USER_VARIABLE` token 与 `opt_mysql_assignment` 产生式——MySQL 语法不应污染纯 PG 方言语法 |
| [parse_target.c](postgres/src/backend/parser/parse_target.c) +13 | `FigureColnameInternal` 的 `T_SysVarRef` 分支改由已有的 `ParserRoutine.figure_colname` 承接 |
| [protocol_routine.c:15,34](postgres/src/backend/postmaster/protocol_routine.c) | 删除静态 `#include` 与 `&MySQLProtocolRoutine` 引用，改纯动态注册（`RegisterProtocolRoutine()` **已存在**） |
| [postmaster.c:1273](postgres/src/backend/postmaster/postmaster.c) | `InitializeProtocolListeners()` → `listen_init_hook`（与 Babelfish `postmaster.c:1282` 调用点位置相同，1:1 可映射）。`ListenProtocolServerPort()` 已 `extern`、已带 `kind` 参数，**直接复用不改造** |

**接受的务实残留（~300 行）**：`crypt.c` MySQL 口令类型（catalog 层 `pg_authid.rolpassword` 编码语义）、`nodeModifyTable.c` `ONCONFLICT_REPLACE`、`reloptions.c` `mysql_default_kind`（PG 无扩展注册属性 reloption 机制）。

---

## 六、目标架构

```
┌─ 内核 (openHalo PG18) ────────────────────────────────────┐
│  方言注册表: {ParserRoutine, ADTExtMethod, ProtocolRoutine}│
│  MyCompatMode (集群默认 + 协议覆盖 + 并行传播)  ← 综合两家 │
│  raw_parser 函数指针 / listen_init_hook        ← IvorySQL │
│  ExecCreateTableAs_post_hook                              │
│  DDL 平行路径 commands/mysql/ 19k (孤例，已论证)          │
│  务实残留 ~300 行                                          │
└───────────────────────────────────────────────────────────┘
   ↕ _PG_init 挂钩 (initdb -m mysql 自动写入 preload)
┌─ src/backend/mysql_parser/ ┐ ┌─ contrib/aux_mysql/ ───────┐
│  → libmysql_parser.so  43k │ │ → aux_mysql.so + 扩展 SQL  │
│  mys_gram.y / mys_scan.l   │ │ protocol/ 6.2k             │
│  零专属 NodeTag ← IvorySQL  │ │ types/ 12.8k (mysm 已 .so) │
└────────────────────────────┘ │ sql/ 36k，版本链 1.5→1.6→  │
                               └────────────────────────────┘
   均为树内源码 + .so 产物（IvorySQL 模式），非独立 repo
```

---

## 七、实施路线

**阶段 0 — 基线实测 + type-input 差分实验**
**前置动作**：工作区须位于**纯 ASCII 路径**——✅ **已迁移（2026-07-31）**：`/home/unvdb/桌面/pg-mysql-tds` → `/home/unvdb/pg-mysql-tds`（原路径含中文导致 regress 编码类失败，属环境问题，见九节验证前提）；**迁移后重跑基线**。
✅ **2026-07-31 已实跑（meson）**：构建成功（2368/2368）；基线 `meson test` 结果 **316 ok / 4 fail / 27 skip / 1 timeout**，根因：

- `regress/copyencoding` + `misc_functions`：**环境问题**——原工作区路径 `/home/unvdb/桌面/` 含非 ASCII 字符，psql 变量替换的路径与 `pg_read_file(postmaster.pid)` 读回原始字节触发编码错误（任何 PG 置于此路径同样失败；**已迁至纯 ASCII 路径 `/home/unvdb/pg-mysql-tds`，重跑后应消除**）；
- `test_misc/003_check_guc`：**真实缺陷**（点号 GUC 命名冲突）→ **G7**；
- `postmaster/004_mysql_protocol`：**真实缺陷 + 挂起**（SCHEMATA 语义 + 1000s 超时）→ **G8**；
- `pg_upgrade/002`、`recovery/027`（regress 级联）：待复核，疑环境。

**结论**：基线**不通过**，须先消除 G7/G8 与环境因素，才能作为后续「无回归」判据的锚点。

> ⚠️ **必须用 meson 构建，autoconf/make 路径构建不出 MySQL 兼容代码**（第六轮评审期间实测发现，本方案原缺此项）。5 个 `mysql/` 子目录（`parser/` `commands/` `tcop/` `adapter/` `utils/adt/`）的父 `Makefile` **均无 `SUBDIRS`**，make 递归永不进入；MySQL 源码只经 meson 侧接入（`src/backend/parser/meson.build:61-75` 直接把 9 个 `mys_*.c` 加进 `parser` static_library，其余四处走各自的 `subdir('mysql')`）。`postgres/configure` 虽在，但 make 构建产不出兼容能力。
>
> 连带的孤儿文件：`src/backend/parser/mysql/Makefile`（只列 4 个 OBJS）与 `src/backend/parser/mysql/meson.build`（`parser/meson.build` **无 `subdir('mysql')`**）**两者都不在构建图内**。后果是 `mys_kwlist_d.h` 的生成规则只存在于这两个孤儿文件中、**从不执行**——当前用的是签入仓库的生成产物（提交 `c65476ee204`）。**改 `mys_kwlist.h` 不会触发重新生成**，这是一个静默陷阱，阶段 0 应顺手确认并记录。

**并行子项 — type-input 差分实验**（构建产物就绪后立即做，约 0.5 人天，不占关键路径）：按第一节「前置决策点 → 差分实验」执行，产出三张分歧表提交产品裁决。**该实验必须在阶段 0 内完成**，因为阶段 1 的 ADT 裁决依赖其结论。

**阶段 1 — 兼容上下文统一 + 一致性修复 + 死契约清理**（第四、五节 + 第一节「清理死契约面」）
新增子项：对 55 个已声明 vtable 契约逐项标注「活（28）/ 接线 / 删除」，删除无接线计划项及其休眠实现（`mys_transformSelectStmt` 等）。零功能风险，成本远低于外置。
`MyCompatMode` 设计落地（含并行传播）+ 6 处一致性修复。**G1 的 `parsereng.c` 部分必须与本阶段同批**（它是活缺陷，有 5 个活调用点）——否则改走 `ADTExtMethod` 只是把并行缺陷换个位置（`ADTExtMethod` 同为进程本地状态）。

**本阶段内部执行顺序（第四轮实测后调整）**：
1. 先做 `ADTExtMethod` 11 项裁决（该表零消费者，裁决结果决定后续两项是否还需要做）
2. 裁决若为「保留」，再做 `varchar.c` 接线 + `adtext.c` 的 G1 修复（两者必须同批，否则接线当天引入活缺陷）；裁决若为「删表」，这两项一并取消
3. G1 修复按三档优先级推进（见第一节「G1 优先级」表）：`parsereng.c`（P0 活缺陷）不依赖上述裁决，可并行推进；`createas.c:363`（P1 活缺陷，CTAS 路径必经）同批修；`adtext.c`（P2 潜伏）随第 2 项裁决处理
3b. **ADT 裁决前需先答产品问题**：MySQL type-input 语义是否为需求（见第一节「前置决策点」）——该问题未定则第 1、2 项无法收敛。

   > **兜底默认（第六轮评审要求，防外部决策卡死本阶段）**：差分实验报告提交后**等待上限 5 个工作日**；逾期未答复则**按「不是需求」推进**——删除 4 个零消费者函数与 6 个空壳项，`ADTExtMethod` 整体退役。理由：删除路径是可逆的（实现留在 git 历史，接线路径日后可按差分清单重建），而接线路径不可逆（一旦接线即引入 P2→活缺陷转化与测试债）。**默认值一旦触发须写入裁决表并知会产品**，不得静默执行。
   >
   > 该默认使阶段 1 的第 1、2 项**在任何情况下都能在 5 个工作日内解锁**，故不进关键路径估算；仅当产品答「是需求」时，才追加接线与补测试工作量（见第八点五估算表脚注）。
4. 把 `mys_parsenodes.h` 移至列表末尾，消除对标准 NodeTag 编号的扰动。**三处必须在同一个提交内一并改**（第五轮评审发现，原方案只提了 `gen_node_support.pl` 一处；第六轮评审要求点明「同一提交」）：

   | 文件 | 列表 | 当前序号 |
   |---|---|---|
   | `src/backend/nodes/gen_node_support.pl` | `@all_input_files`（第 60 行） | 第 7 位 |
   | `src/backend/nodes/Makefile` | `node_headers`（第 48 行，注释明示 *re-ordering this list risks ABI breakage!*） | 第 7 位 |
   | `src/include/nodes/meson.build` | `node_support_input_i`（第 10 行） | 第 7 位 |

   > **失败模式更正**：评审称「脚本只校验文件数不校验顺序，改漏会导致两个构建系统 tag 顺序静默不一致」。**实测不成立**——`gen_node_support.pl` **数量与顺序都校验**：第 190-191 行 `die "wrong number of input files"` 查数量，第 218-219 行 `die "wrong input file ordering, expected @all_input_files"` 逐个比对 `@ARGV` 与 `@all_input_files`。故改漏任一处会**直接构建失败**，不会静默产生 ABI 分歧——错误自曝，无需额外校验机制。
   >
   > ⚠️ **正因为构建强制，三处务必写进同一个提交**：只改一处的中间状态**无法构建**，不能拆成增量提交，也不能只改一处后先推。执行者若不知情会困惑于「明明按方案改了却编不过」。提交信息中注明本条。
   >
   > **稳定性常量无需改动**：`$last_nodetag = 'WindowObjectData'`（第 111 行）与 `$last_nodetag_no = 484`（第 112 行）的两处 `die` 检查（第 638、640 行）在移动后仍通过——`WindowObjectData` 来自 `@extra_tags`，恒排在全部文件派生 tag 之后，`$last_tag` 不变；tag 总数不变，`$tagno` 仍为 484。
**验收**：裸内核 `make check-world` 与上游 PG18.3 一致；MySQL 用例在 `parallel_setup_cost=0` 强制并行下与串行结果一致。

**阶段 2 — 协议层与类型层外置**（并入 `contrib/aux_mysql/`，**不改扩展名**）
迁移顺序按依赖倒序：`types/`（`mysm` 已是 `.so`，最简，验证构建链）→ `protocol/` → `sql/`。
新增对象通过 `aux_mysql--1.5--1.6.sql` 等增量脚本交付，已部署实例可 `ALTER EXTENSION aux_mysql UPDATE` 平滑升级。
同步固化 **G3 跨边界契约**：在 `src/include/` 下建立「内核 → 兼容库」导出符号清单头文件，CI 增加符号存在性与签名校验（当前已知跨边界调用：`mysql_auth.c:594` → 内核 `mysql_native_password_verify()`）。

**阶段 3 — 解析器 `.so` 化**（第三节，独立课题）
先消除 5 个专属 NodeTag（真内核引用 6 处 + MySQL 自有目录 25 处），再照搬 `liboracle_parser` 构建模式，最后做 `initdb -m` 集成。

**阶段 4 — `pg_dump` 适配**（G2，独立课题）
参照 IvorySQL `src/bin/pg_dump/t/oracle/001_pg_dump_ora_specific.pl` 与 pg_dump.c 内 19 处适配。覆盖 `mysql_default_kind` reloption、AUTO_INCREMENT 序列绑定、ENUM/SET domain 组合、MySQL 口令编码。

---

## 八、其余待处理项

| 项 | 状态 |
|---|---|
| **G2 `pg_dump` 零适配** | openHalo 对 `src/bin/pg_dump/` 零改动；Babelfish 与 IvorySQL 均有适配 → 阶段 4 |
| **G3 跨边界依赖未列契约** | ✅ **已定方案**：建立「内核 → 兼容库」导出符号清单头文件 + CI 符号/签名校验，随阶段 2 落地。已知跨边界调用：[mysql_auth.c:594](postgres/src/backend/adapter/mysql/mysql_auth.c#L594) → 内核 `mysql_native_password_verify()` |
| **G4 扩展改名断升级路径** | ✅ **已定方案**：**保留 `aux_mysql` 名字**，只扩充内容并延伸版本链（1.5→1.6→…）。零迁移成本，升级路径完整保留 |
| **G5 版本声明不一致** | ✅ **已修复 2026-07-31**：`default_version` 1.1→1.5（原先新装只拿到约 1% 内容）；连带修正 `sql/versioning.sql`、`expected/versioning.out`、以及全仓库扫描发现的 `t/005_mysql_compat.pl:68`（454 用例套件入口，不改会导致整套测试首步崩溃）。附带确认：1.2–1.5 内容一直被 454 测试覆盖 |
| **G6 构建体系单轨且有孤儿规则**（第六轮评审期间实测新增） | ⚠️ **待决**。两个事实：① autoconf/make 路径**完全不含** 5 个 `mysql/` 子目录（父 `Makefile` 均无 `SUBDIRS`），MySQL 兼容只能用 meson 构建，但 `configure`/`Makefile` 仍在仓库中，会诱导使用者走错路径；② `parser/mysql/Makefile` 与 `parser/mysql/meson.build` **均不在构建图内**，其中的 `mys_kwlist_d.h` 生成规则从不执行，仓库靠签入产物工作。**待定的是取舍**：补齐 make 侧（工作量大、双轨维护）vs 明确单轨 meson 并删除/标注孤儿 Makefile（推荐，上游 PG 亦在向 meson 收敛）。阶段 0 确认现状，阶段 2 或 3 落地取舍 |
| **G7 点号 GUC 命名与上游 `check_guc` 不兼容**（阶段 0 基线实测新增，2026-07-31） | ⚠️ **待决**。基线 `test_misc/003_check_guc` 失败：上游测试解析正则 `^#?([_[:alnum:]]+) = ` **不支持点号（`.`）**，openHalo 的 5 个 `mysql.*` GUC（`mysql.listener_on` / `mysql.port` / `mysql.backend_database` / `mysql.max_allowed_packet` / `mysql.server_version`）在 `postgresql.conf.sample` 中解析不出 → 报「5 缺失」（另有 1 项「sample 有而 guc_tables 无」待精确定位）。**待定取舍**：① **改名去点号**（如 `mysql_listener_on`，推荐——与上游测试兼容，改动面小，顺带避免第三方工具对点号 GUC 的潜在问题）；② 修改上游测试（维护 fork 测试差异，不推荐）。建议随阶段 1 的 6 处一致性修复同批处理 |
| **G8 `information_schema.SCHEMATA` 语义缺陷 + 协议 TAP 挂起**（阶段 0 基线实测新增，2026-07-31） | ⚠️ **待修**。基线 `postmaster/004_mysql_protocol` 两问题：① `schemata includes postgres database` 断言失败——SCHEMATA 查询返回 `information_schema` 而非 `postgres`（MySQL 语义实现缺陷）；② 该断言后测试 **1000s 超时被杀**（挂起点待单独复跑定位，疑协议连接阻塞）。建议阶段 1 排查语义缺陷与挂起点，并补 TAP 用例覆盖 |

---

## 八点五、工作量评估

> **口径**：1 名熟悉 PG 内核的开发者，含编码、自测、回归、问题修复，不含需求澄清与评审。单位＝人天。区间下限＝顺利，上限＝遇到常规阻碍。

### 实测驱动因子

| 阶段 | 核心驱动 | 实测量 |
|---|---|---|
| 0 | 构建 + 跑基线 | regress 233 用例 / MySQL 套件 1,048 行 SQL / 协议 TAP 2,502 行；**只能用 meson**（make 不含 mysql/ 子目录） |
| 0 | type-input 差分实验 | 3 类字面量语料 × 3 个类型（DATE/DATETIME/TIME），产出三张分歧表 |
| 1 | `MyCompatMode` 迁移面 | **仅 3 处**读 `MyProcPort->protocol_kind`（`adtext.c:77`/`parsereng.c`/`createas.c:363`） |
| 1 | 6 处一致性修复 | 待重构合计 405 行（`protocol_routine.c` 133 / `postmaster.c` 127 / `createas.c` 80 / `gram.y` 33 / `varchar.c` 19 / `parse_target.c` 13） |
| 2 | 类型层外置 | `mysm` 9,957 行（**已是 `.so`**）+ `adt/mysql` 2,681 行，109 个被调内核符号 |
| 2 | 协议层外置 | `adapter/mysql` 6,191 行，**371** 个被调内核符号 |
| 3 | 解析器外置 | `parser/mysql` 43,453 行，**591** 个被调内核符号；**阶段 1 清理后预计降至约 34.5k 行**（−8,921 净休眠），外置量与符号面同步收缩 |
| 1 | 死契约清理（新增） | 55 声明 / 28 活 / **27 需裁决**（Parser 17 + ADT 10；**Protocol 0**——已全部接线）；MySQL 侧休眠实现 **6 个文件 9,125 行（占 `parser/mysql` 20.9%），净休眠 8,921 行**（`mys_parse_expr.c` 3,443 / `mys_parse_func.c` 1,528 / `mys_analyze.c` 1,470 / `mys_parse_clause.c` 1,115 / `mys_parse_agg.c` 987 / `mys_parse_oper.c` 582）。**`mys_parse_utilcmd.c` 5,633 行不计入**——其 `mys_transformCreateStmt`/`mys_transformAlterTableStmt` 走直调，是活代码 |
| 3 | NodeTag 消除 | 5 个节点类型；真内核引用 **6 处** + MySQL 自有目录 **25 处**（`mys_utility.c` 16 / `mys_prepare.c` 4 / `mysql_stmt.c` 3 / `mysql_protocol.c` 2），后者随迁移仍须逐处改写 |
| 3 | 构建链迁移 | meson `custom_target`（`mys_scan`/`mys_gram`）需移入 `.so` 构建；**`mys_kwlist_d.h` 的生成规则当前根本不在构建图内**（只存在于孤儿的 `parser/mysql/{Makefile,meson.build}`，用的是签入产物），需**先让它真正接入**再谈迁移——这是新增工作而非搬迁 |
| 4 | `pg_dump` 适配 | 4 类对象；IvorySQL 参照＝19 行代码 + 350 行 TAP |

### 分阶段估算

| 阶段 | 内容 | 人天 | 风险 | 依赖 |
|---|---|---|---|---|
| **0** | 基线实测（meson）+ type-input 差分实验 | **2–4** | 中 | — |
| **1** | `MyCompatMode`（含并行传播）+ 6 处一致性修复 + 死契约清理（27 项裁决）+ **G7/G8**（估各 1–2 人天，未计入本行，待定） | **12–15** | 低 | 阶段 0；**外加产品裁决门（有兜底，见下）** |
| **2** | 类型层 + 协议层外置、G3 契约与 CI、版本链延伸 | **19–23** | 中 | 阶段 1 |
| **3** | NodeTag 消除 → 构建链迁移 → 解析器 `.so` 化 → `initdb -m` 集成 | **23–30** | **高** | 阶段 2 |
| **4** | `pg_dump` 适配 | **8–12** | 中 | 可与 2/3 并行 |
| | **合计** | **64–84 人天**（约 3–4 人月） | | |

阶段 0→1→2→3 存在硬依赖，必须串行；阶段 4 可与 2/3 并行。**若单人推进，日历工期约 3.5–4.5 个月**（含常规中断）；若阶段 4 由第二人并行承担，关键路径约 **2.5–3.5 个月**。

> **阶段 1 的外部决策门及其兜底（第六轮评审提出，本估算已吸收）**：阶段 1 第 3b 项需产品答复「MySQL type-input 语义是否为需求」。处理方式：
>
> - **证据前置**：差分实验并入阶段 0（+1 人天，已计入 0 的 2–4），产品在阶段 1 启动时即持有失败清单，不是从零讨论；
> - **等待封顶**：设 5 个工作日答复上限 + 「逾期按删除处理」兜底（见第七节阶段 1 第 3b 项）。故**等待时间不进人天估算，也不进关键路径**；
> - **条件追加**：仅当产品答「是需求」，阶段 1 追加 **3–5 人天**（4 个函数接线 + 补 type-input 测试用例 —— 该路径当前**零测试覆盖**，测试是主要成本）。此情形下阶段 1 为 15–20 人天，合计上限 89 人天。
>
> 上表列的是**默认（删除）路径**的数字。
>
> ⚠️ **G7/G8 阶段 1 新增范围（2026-07-31 基线实测后补）**：G7 点号 GUC 改名、G8 SCHEMATA 排查与挂起定位各估 **1–2 人天**，**未计入上表 12–15**；待 G8 挂起点定位后定数并入（见第八节）。

### 估算中的三项不确定性（区间上限可能被突破）

1. **基线未经验证（最大变量）**——748 subtests / 454-454 取自提交信息，工作区从未 configure。若基线实际不通过，阶段 0 可能从 3 人天膨胀到 1–2 周，且后续所有阶段的「无回归」判据失去锚点。**这是唯一一个可能整体改变估算的因素，应最先消除。** ✅ **已触发（2026-07-31 实测）**：基线确认不通过（316 ok / 4 fail / 1 timeout，根因见第七节阶段 0 与 G7/G8），阶段 0 成本进入上限区间；须先消除 G7/G8 与环境因素（ASCII 路径）才能重跑获取有效基线。
2. **解析器的 static 符号依赖未做分析**——DDL 侧已实测（1,653 符号中真外部 static 依赖仅 `truncate_check_activity` 1 个），但**解析器侧的 591 个符号未做同类分析**。若其中存在多个内核 static 依赖，阶段 3 的链接调试会显著超出 5–8 人天的预留。**建议在阶段 2 期间提前做此分析**，成本约 0.5 人天，可大幅收窄阶段 3 的估算区间。
3. **NodeTag 降解的语义风险**——`@var` 在表达式内赋值的副作用顺序，降解为 `FuncExpr` 后可能改变求值时机。属于「不难写但难验」类工作，估算已含 5–8 人天，但若 MySQL 语义比对暴露系统性偏差，可能需要重新设计降解方案。

### 不在本估算内

- **`mys_tablecmds.c` / `mys_gram.y` 的 fork 漂移治理**——方案明确不解决，仅新增 CI 检测（约 2 人天，可并入阶段 2）
- **per-database 模式粒度**——独立课题，触及 catalog 与共享内存布局
- **Windows 支持**——已定先按 Linux；若触发，`PGDLLIMPORT` 全量审计预估另加 5–8 人天，且解析器 `.so` 化后此面明显扩大
- **多方言注册表重构**（为融合 Babelfish T-SQL 铺路）——方案中为分期项，未计入

---

## 九、验证

> ⚠️ **基线已实测（2026-07-31）**：构建成功（meson 2368/2368）；`meson test` 结果 **316 ok / 4 fail / 27 skip / 1 timeout**（根因见第七节阶段 0 与 G7/G8）。下列 748 subtests / 454-454 取自提交信息（`d085fc84aa3`、`e68892523ce`），与实测的对应关系待 ASCII 路径重跑后确认。
>
> ⚠️ **工作区路径前提**：必须位于**纯 ASCII** 路径——✅ **已迁移（2026-07-31）**至 `/home/unvdb/pg-mysql-tds`（原 `/home/unvdb/桌面/` 含中文触发 regress 编码类失败——`copyencoding`/`misc_functions`，属环境问题非内核缺陷）。**基线判据须在 ASCII 路径下取得**，否则失真。
>
> ⚠️ **必须走 meson，不能用 make**（第六轮评审期间实测发现，本节原命令有误）：5 个 `mysql/` 子目录的父 `Makefile` 均无 `SUBDIRS`，autoconf/make 构建**产不出任何 MySQL 兼容代码**，用它跑出的「基线」只是裸 PG。详见第七节阶段 0。

```bash
# 构建（meson，唯一可用路径）
cd postgres && meson setup build --prefix=$PWD/inst && ninja -C build
meson test -C build                      # 内核 + MySQL 兼容全量
cd contrib/aux_mysql/t/mysql_compat && ./run_mysql_compat.sh
```

> 原先记录的 `make check-world` / `make -C src/test/postmaster check` **仅对裸内核部分有效**，可作为「硬指标 1」（不加载兼容库时与上游一致）的辅助手段，不能作为 MySQL 侧基线。

- **硬指标 1（解耦成功）**：不加载兼容库的裸内核 `make check-world` 与上游 PG18.3 完全一致
- **硬指标 2（扩展可用）**：`initdb -m mysql` 后真实客户端 `mysql -h 127.0.0.1 -P 3306` 连通，`mysql_compat` 454/454
- **硬指标 3（架构纪律）**：内核非 MySQL 目录下 `MyProcPort->protocol_kind` 直读命中数降至 0（务实残留列白名单）
- **硬指标 4（G1）**：`parallel_setup_cost=0` 下 MySQL 类型语义用例串并行结果一致
- **硬指标 5（NodeTag）**：`nodes.h`/`nodetags.h` 与上游 PG18.3 一致（除 `ONCONFLICT_REPLACE` 一行）；`gen_node_support.pl` 的 `$last_nodetag_no` 回落至上游值 **479**，且 `@all_input_files` 顺序与上游一致（阶段 1 的临时缓解只保证编号不扰动，硬指标要求彻底移除该行）
- **硬指标 6（死契约清零）**：CI 校验「已声明 vtable 函数指针」与「有活调用点者」数量一致——即不允许存在声明了却无人调用的槽位。当前基线 55 声明 / 28 活，缺口 27（全部在 `ParserRoutine` 17 与 `ADTExtMethod` 10；`ProtocolRoutine` 已达标）。
- **硬指标 7（休眠实现清零）**：`parser/mysql` 下不存在「全仓库引用仅头文件声明」的导出函数。当前基线为 6 个零可达文件共 9,125 行（净休眠 8,921 行），阶段 1 后应降至 0，`parser/mysql` 总量由 43,453 行降至约 34,532 行。校验方式：对每个 `mys_*` 导出符号做全树调用点计数，为 0 即失败（`mys_transformCreateStmt` / `mys_transformAlterTableStmt` 走直调，计数不为 0，不受影响）。
  > **需支持待接线白名单**（第五轮评审补充）：本指标为**终态**要求，阶段 1 中途会误报——例如 `ADTExtMethod.allow_zero_length_char_typmod` 在 `varchar.c` 接线完成前无调用点。CI 应支持登记「计划内接线项」白名单，否则该指标无法在阶段 1 过程中运行。白名单条目须带责任人与目标阶段，阶段结束时清空。

---

## 十、遗留风险

| 风险 | 缓解 |
|---|---|
| 解析器/DDL fork 副本**静默语义漂移**（`mys_gram.y` 24k、`mys_tablecmds.c` 17k） | 远距离方言的行业常态——IvorySQL 同样维护 22,367 行独立 Oracle 语法。**缓解措施见下方专项** |
| `mys_tablecmds.c` 是三家中唯一的 DDL fork | 已论证：MySQL 独有子命令 64 个远多于 Oracle，内联路线会把 ~6k 行塞进内核。维持平行路径，45 个逐字相同函数记为已知冗余 |

### 专项：fork 副本漂移的检测机制（外部评审触发，本方案原缺具体措施）

**先厘清风险性质**（外部评审此处判断有误，需更正）：评审称 45 个逐字相同函数是「rebase 合并冲突热点」。**实测不成立**——`mys_tablecmds.c` 在 git 中是新增文件（`--diff-filter=A`），与上游 `tablecmds.c` 无血缘；且内核 `tablecmds.c` 自 openHalo 基线以来**零改动**。上游修改会干净应用，**永不冲突**。

真实风险是**静默语义漂移**：上游修了 bug，fork 副本不报错、不冲突，只是悄悄保持旧行为。**合并冲突会自曝，静默漂移不会**——因此 CI 检测不是可选优化，而是该风险的**唯一发现途径**。评审的建议因此比其自身论证的更必要。

| 措施 | 内容 |
|---|---|
| **CI 漂移告警** | 对 `mys_tablecmds.c` / `mys_gram.y` 与上游对应文件的同名函数/产生式做逐对 diff，漂移行数超阈值即告警。基线取当前实测值（`tablecmds` 45 相同 / 28 微改 / 25 中改 / 25 重改；`gram` 264 相同 / 52 微差 / 66 中改 / 26 重改） |
| **大版本同步** | 每个 PG 大版本做一次系统性对账，重点核对上游在 45 个「逐字相同」函数中的安全性/正确性修复 |
| **冗余收敛** | 45 个逐字相同函数是纯冗余。若未来上游导出其中部分符号，可择机消化——但**不主动申请导出**（见第二节否决理由） |
| 解析器 `.so` 化需先消除 5 个专属 NodeTag | IvorySQL 零专属节点即证明可行；实测真内核引用 6 处 + MySQL 自有目录 25 处。作为阶段 3 前置条件。**另注**：这 5 个 tag 中段插入枚举，已扰动上游标准 tag 编号（见第三节「插入位置」），缓解手段是把 `@all_input_files` 中该行移至末尾 |
| 多方言注册表触及 catalog 与共享内存布局 | 阶段 1 只做 `MyCompatMode` 与传播；注册表重构可分期 |
| Windows：兼容库读取的内核全局变量需 `PGDLLIMPORT` | **已定：先按 Linux 做**，审计列为待触发项。触发条件＝确定要支持 Windows。注意解析器 `.so` 化后此面会明显扩大，届时需全量审计 |
| 「独立发布」目标已明确降级 | **已定：树内源码 + `.so` 产物**，不做独立 repo。三家实际都做不到脱离补丁内核单独发布（IvorySQL 的 `liboracle_parser` 同在 PG 源码树内构建）。本方案「解耦」的实际含义是：**协议、类型、解析器三层各自可独立加载/替换/演进，且内核在不加载它们时行为与上游完全一致** |
