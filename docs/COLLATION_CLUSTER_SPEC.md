# 排序规则集群实施规格(P2-1 / P2-2 / P2-8 + 两个新发现)

## ⛔ §2.1 / §2.2 / §2.3 结论已作废:均不做(2026-08-14 上游比对核实)

**用户原则**:融合阶段若某兼容特性 **openHalo 和 Babelfish 上游本身也没实现**,则不做修复和实现,保持与上游兼容特性一致即可。

**逐项核对结果(代码核实,逐字符比对 openHalo 源仓库 `/home/hlv/openhalo-update/postgres`,分支 `openhalo-update`)**:

- **§2.1 REGEXP/RLIKE 不可用**:openHalo 源里 `mys_gram.y` 的 `REGEXP`/`RLIKE` → `~*`/`~` 映射与本融合树相同,且同样没有任何针对非确定性排序规则的算子覆盖。openHalo 本身就没有让 REGEXP 在其默认的非确定性排序规则列上可用 —— **不是融合回归,不做**。
- **§2.2 外键排序规则检查缺失**:`grep -c get_collation_isdeterministic` 在 openHalo 源的 `mys_tablecmds.c` 里同样是 **0** 处,与本融合树一致。openHalo 的 MySQL DDL 路径本来就没有移植内核那道外键一致性检查 —— **不是融合回归,不做**。⚠️ 这仍然是数据完整性性质的问题,若将来上游修复或用户改变范围决策,应优先重启此项(参见 §2.2 原文的危害说明)。
- **§2.3 / P2-8 COLLATE 后缀匹配静默丢弃**:openHalo 源里 `rectifySpecifiedColumnCollate()` 与本融合树**字符级完全一致**(同样的后缀 `_ci` 匹配 + 不匹配则 `collClause = NULL` 静默丢弃)。openHalo 本身就是这个简化实现 —— **不是融合回归,不做**。

**因此 §3 的实施优先级表全部作废,§4 验证方案里涉及 REGEXP/外键/COLLATE 报错的用例全部作废。**

**仍然成立、可以做的**:
- §2.4(P2-2 文档回填)—— 这不是新增特性,只是把 FUSION_PLAN.md 里一条已经过期的"未修复"记录更正为"早已修复",无需改代码,不违反范围原则。
- §2.5(P2-1)—— 结论本来就是"记录为已知发散,不修",不受此次核对影响。

**本文档 §1~§4 以下内容仅作为技术档案保留**(记录了架构根因、实测证据、以及若将来范围决策改变时的实施方案参照),**不构成行动建议**。

---

> 本文档不纳入版本控制,仅作本地工作记录。
> 分析完成:2026-08-14(Opus 5 + max reasoning 分析,交由 Sonnet 实施)
> 关联:[FUSION_PLAN.md](FUSION_PLAN.md) §6 P2-1 / P2-2 / P2-8

## 0. 给实施者的前置说明

**本规格里每一条结论都在运行中的集群上实测过或在代码里核实过,不要凭直觉改动。**

本次分析把三个原本分开记录的条目(P2-1/P2-2/P2-8)当作**一个子系统**来查,结果证明这个判断是对的:它们不是三个孤立 bug,而是**一个架构决策的连带后果**,并且顺带挖出两个**更严重、此前完全没有记录**的问题。实施优先级请按第 3 节,不要按 P 编号顺序。

---

## 1. 架构根因:非确定性排序规则作为默认值

### 1.1 现状

MySQL 兼容层为了模拟 MySQL 默认 `_ci`(大小写不敏感)语义,给**每一个字符串列**默认挂上 `mysql.case_insensitive`:

```sql
-- contrib/aux_mysql/aux_mysql--1.1.sql:21
CREATE COLLATION mysql.case_insensitive
    (provider = icu, locale = '@colStrength=secondary', deterministic = false);
```

挂载逻辑在 `src/backend/parser/mysql/mys_parse_utilcmd.c` 的 `rectifyColumnCollate()`(1257 行)/`rectifyColumnsCollate()`(1280 行),`isTableCollationCaseInsensitive()` 在无表选项时**返回 true**,所以裸 `CREATE TABLE t(a VARCHAR(20))` 的列也是 `case_insensitive`。实测确认:

| DDL | 实际排序规则 | 确定性 |
|---|---|---|
| `a VARCHAR(20)` | `case_insensitive` | f |
| `a VARCHAR(20) CHARACTER SET utf8mb4` | `case_insensitive` | f |
| `a VARCHAR(20) COLLATE utf8mb4_bin` | `default` | t ← 见 §2.3 |

### 1.2 这个决策的系统性后果

**PostgreSQL 对非确定性排序规则的支持是不完整的。** 内核里共 7 处特殊处理(已全量枚举,`grep get_collation_isdeterministic` + 相关 ereport):

| 位置 | 行为 | 对 MySQL 列的影响 |
|---|---|---|
| `regc_pg_locale.c:262` | **正则表达式硬拒绝** | **REGEXP/RLIKE 完全不可用** ← §2.1 |
| `like.c:210` | ILIKE 硬拒绝 | 已由 `ca3e74b273` 把 LIKE 从 ILIKE 改回 `~~` 规避 |
| `varlena.c:1934` | 子串搜索硬拒绝 | 已由 `4259887286` 逐个函数打补丁规避 |
| `index.c:844` | `*_pattern_ops` 操作符类拒绝 | 前缀 LIKE 索引不可用(性能) |
| `like_support.c:414` | 非确定性时**不做 LIKE 前缀索引优化** | `LIKE 'x%'` 无法走索引(性能) |
| `like_support.c:1072` | 同上,大小写不敏感前缀路径 | 同上 |
| `tablecmds.c:10589` | **外键要求两侧确定性一致** | MySQL 副本缺失该检查 ← §2.2 |

**关键判断:项目目前是在"逐个函数打地鼠"。** 已打补丁的:`instr`/`locate`(`4259887286`)、`replace`(`fa887e0789`)、`bpcharlike`(见 §2.4,已修但文档未回填)。**没打的:正则**。这个模式说明还会有下一个洞。

> ✅ **P2-20 实证核验(2026-08-14,测试集群 EXPLAIN)**:上表 `index.c:844` 与 `like_support.c:414`/`1072` 三处代码依据全部属实。**实测补全三点**:①默认列(非确定性)建 `varchar_pattern_ops` 报 `ERROR 1295 ... not supported for operator class`;②前缀优化另有**第三道 PG 通用门槛**(`like_support.c` 范围约束路径要求索引排序规则 `collate_is_c`,即纯 C 排序规则)——本集群默认 C.UTF-8 不满足,故**确定性列 + 普通 btree 同样走不了 LIKE 前缀**(用纯 PG 表对照证实,非 MySQL 层回归);③**现行可用出路**:确定性列 + `varchar_pattern_ops` 索引,`LIKE 'al%'` 实测 Index Scan(`~>=~`/`~<~` 模式界);确定性列可经 MySQL DDL 的 `COLLATE <非_ci后缀>` 子句获得(P2-8 的静默丢弃行为恰好回落 PG 默认,`rectifySpecifiedColumnCollate` 只认 `_ci` 后缀);非确定性列只能用显式范围条件(`v >= 'al' AND v < 'am'`,普通 btree 可走)或接受 Seq Scan。

### 1.3 架构结论(重要,决定后续所有方案)

**不建议推翻"非确定性排序规则作默认"这个决策。** 理由:它是让 PG 内建算子(`=`/`<`/`ORDER BY`/`GROUP BY`/`DISTINCT`/唯一约束)一次性获得 MySQL 式大小写不敏感语义的**唯一**可行手段,替代方案(为每个算子写 mysql-schema 版本)是数量级更大的工程,且已有表的排序规则已经落盘。

**但要把"反应式打补丁"改成"主动枚举式兼容层"**:上表 7 处就是完整清单(PG18 范围内),照着逐条处理,而不是等用户踩到一个修一个。

---

## 2. 五项发现(按严重度,非 P 编号顺序)

### 2.1 【新发现·严重】REGEXP / RLIKE 在所有字符串列上完全不可用

实测:

```
mysql> SELECT COUNT(*) FROM collcmp WHERE v REGEXP 'Ali';
ERROR 1295 (HY000): nondeterministic collations are not supported for regular expressions
```

`REGEXP`/`RLIKE` 是 MySQL 的核心操作符,而**每个** MySQL 字符串列默认都是非确定性排序规则,所以这个操作符在整个数据库上都不可用。阶段二没测 REGEXP,所以此前完全没有记录。

**语法层本身没问题**,`mys_gram.y:19120-19131` 的映射是对的:

| MySQL 语法 | 映射到 | 语义 |
|---|---|---|
| `REGEXP` / `RLIKE` | `~*` | 大小写不敏感 |
| `REGEXP BINARY` | `~` | 大小写敏感 |
| `NOT REGEXP` / `NOT RLIKE` | `!~*` / `!~` | 对应取反 |

问题**纯粹**是操作数携带非确定性排序规则,PG 正则引擎在选哪个算子之前就拒绝了。

**修复思路已实测验证可行**:

```sql
-- ci 列直接用 ~* :报错
-- 强制确定性排序规则后:
SELECT v COLLATE "default" ~* 'ali',  -- → t  (正确,大小写不敏感)
       v COLLATE "default" ~  'ali'   -- → f  (正确,大小写敏感)
FROM collcmp;
```

因为 `~*` **自身**就做大小写不敏感匹配,不需要靠排序规则提供;剥掉非确定性排序规则后语义完全正确。

**推荐实现方式:mysql-schema 算子覆盖**,直接复用本代码库已有先例 `c6773eb8cc`(MySQL DECIMAL 除法用 `mysql./` / `mysql.//` 覆盖):

- MySQL 连接的 `search_path` 是 `<dbname>, "$user", public, mysql, pg_catalog`(`mysql_session_initialize()` 设置),**`mysql` 在 `pg_catalog` 之前**,所以定义 `mysql.~*` 等同名算子会正确遮蔽内核算子。
- PG 原生连接和 TDS 连接的 search_path 里**没有** `mysql`,天然不受影响 —— 满足用户定的"三兼容模式互不干扰"原则,无需额外隔离措施。
- 需要覆盖四个:`mysql.~*`、`mysql.~`、`mysql.!~*`、`mysql.!~`,实现里判断操作数排序规则,非确定性时强制成确定性(参考 `fa887e0789` 的 `mys_replace_non_deterministic()`,它用 `C_COLLATION_OID` 重新分派,是同一套路)。

**范围边界(不要顺手扩大)**:当前语法把大小写敏感性绑定在**语法**上(`REGEXP` 恒不敏感 / `REGEXP BINARY` 恒敏感),而真实 MySQL 是由**操作数排序规则**决定(`_bin` 列上的裸 `REGEXP` 应该是敏感的)。这是一个**既有的**设计选择,本次只负责让它真正能跑,**不要**顺带改成按排序规则判定 —— 那是独立的语义细化,单独评估。

### 2.2 【新发现·严重·数据完整性】MySQL DDL 绕过外键排序规则兼容性检查

内核 `tablecmds.c:10584-10607` 有一道检查:外键两侧只要有一侧是非确定性排序规则,两侧排序规则就**必须完全相同**,否则拒绝建约束,理由是 "we need a consistent notion of equality on both columns"。

**`src/backend/commands/mysql/mys_tablecmds.c`(563KB,tablecmds.c 的 MySQL 副本)完全没有这段。** 核实证据:

- `grep -c get_collation_isdeterministic`:内核版 **3** 处,MySQL 版 **0** 处
- 内核版该段的特征串 `"key columns are not both collatable"` 在 MySQL 版中**不存在**(整段缺失,不是改写)

这是 **fork 漂移**:MySQL 副本从某个时点拷贝后,这道检查没有同步过来。

**实测对照**(同一个外键,两条路径):

```
psql(PG 原生路径):
  ERROR: foreign key constraint "fk3" cannot be implemented
  DETAIL: ...incompatible collations: "default" and "case_insensitive".
          If either collation is nondeterministic, then both collations have to be the same.

mysql(MySQL 协议路径):
  静默创建成功(pg_constraint 里 fk2 确实存在,contype='f')
```

**实际危害已验证**:父表 `k` 是 `case_insensitive`,子表 `k` 因 §2.3 的 bug 变成确定性 `default`。父表只有 `'abc'`,向子表插入 `'ABC'` **被接受** —— 外键是借父表的大小写不敏感索引匹配上的,但子列自身的相等语义认为 `'ABC' ≠ 'abc'`。也就是说这个外键的"引用同一性"依赖于一个子列自己并不使用的排序规则,正是 PG 那道检查要防止的不一致状态。

**修复**:把内核 `tablecmds.c` 的该段移植进 `mys_tablecmds.c` 的 `ATAddForeignKeyConstraint()`(MySQL 版在 12716 行)。机械移植,工作量小,价值高。

> ⚠️ 顺带提醒:`mys_tablecmds.c` 是 563KB 的完整副本,这次发现的是**一处**漂移。不排除还有别的内核检查没同步过来。**本次不要**做全量 diff 比对(工作量巨大且大量是无关的 MySQL 定制),但值得单开一个条目记录这个风险。

### 2.3 【P2-8·已根因定位】显式 COLLATE 按名字后缀匹配,不匹配的整个丢弃

`mys_parse_utilcmd.c:1241` `rectifySpecifiedColumnCollate()`:

```c
char *collateName = strVal(linitial(columnDef->collClause->collname));
int collateNameLen = strlen(collateName);
if ((3 < collateNameLen) &&
    (strncasecmp((collateName + (collateNameLen - 3)), "_ci", 3) == 0))
{
    setColumnCollateToCaseInsensitive(columnDef);   /* 所有 _ci 塌缩成同一个 */
}
else
{
    columnDef->collClause = NULL;                   /* 其余一切:静默丢弃 */
}
```

即**只看名字最后三个字符**:

- `_ci` 结尾 → 一律替换成 `mysql.case_insensitive`,不区分 `utf8mb4_general_ci` / `utf8mb4_unicode_ci` / `latin1_swedish_ci` 的实际语义差异
- 其余一切(`utf8mb4_bin`、`utf8mb4_0900_as_cs`,乃至合法的 PG 排序规则如 `"C"`、`"en_US"`)→ **`collClause = NULL`,整个丢掉**,列退回数据库默认排序规则

**静默丢弃比报错危险得多**:用户以为自己拿到了大小写敏感语义,实际拿到的是数据库默认;而且如 §2.2 所示,这会制造确定性/非确定性混搭,进而触发外键完整性隐患。

**修复方向**:

1. 用**真实的名字映射表**替代后缀匹配,至少覆盖常见的 `utf8mb4_bin`(→ 一个确定性排序规则)、`utf8mb4_general_ci`/`utf8mb4_unicode_ci`/`utf8mb4_0900_ai_ci`(→ `case_insensitive`)。
2. **不认识的名字要报错,不要静默丢弃** —— 这是本项修复里最重要的一点。宁可让用户看到 "unsupported collation" 也不要让他以为设置生效了。
3. 注意 `rectifyColumnCollateForAlter()`(1295 行)是同一逻辑的第二个副本,`ALTER TABLE` 路径也要一起改。

### 2.4 【P2-2·文档过期】`bpcharlike` 早已修复

FUSION_PLAN.md 记的 "`bpchar_pg18.c` 的 `bpcharlike` 不看 `PG_GET_COLLATION()`" **已经不成立**。当前实现已经正确读取排序规则并委托给内核 `textlike`,函数头注释明确写着 "The previous byte-wise matcher never consulted PG_GET_COLLATION()"。

实测确认 CHAR 与 VARCHAR 行为一致:

| 用例 | 结果 |
|---|---|
| `v LIKE 'alice%'`(VARCHAR,ci 列) | 命中 ✓ |
| `c LIKE 'alice%'`(CHAR,ci 列) | 命中 ✓ |
| `v = 'ALICEX'` / `c = 'ALICEX'` | 均命中 ✓ |

**行动:只需把 FUSION_PLAN.md 的 P2-2 标为已修复(某次早期修复顺带解决但没回填文档),无需改代码。**

### 2.5 【P2-1·建议记录不修】字面量对字面量的排序规则

实测确认存在:

```
'AbC' LIKE 'a%'  → 0    (真实 MySQL:1)
'AbC' = 'abc'    → 0    (真实 MySQL:1)
```

**根因(PG 排序规则推导规则,不是 bug)**:PG 里字符串字面量本身**没有**排序规则,它从比较的另一侧继承。所以:

- `col = 'literal'` —— 字面量继承列的 `case_insensitive`,**行为已经是对的**(这也正好等价于 MySQL 的 coercibility 规则:列优先于字面量)
- `'lit1' = 'lit2'` —— 两侧都无排序规则,PG 回退到**数据库默认排序规则**(这里是 `C.UTF-8`,确定性、大小写敏感),于是发散

**也就是说影响面比字面表述窄得多**:只有"比较双方都不含任何列"的场景才受影响。

**两条看起来显然、实际是陷阱的方案(不要采纳)**:

- ❌ **把数据库默认排序规则改成 `case_insensitive`**:会让**所有**正则操作全库报错(§1.2 第一行,字面量上的正则同样受害),而且数据库默认是 PG/MySQL/TDS **共用**的,直接违反"三兼容模式互不干扰"原则。
- ❌ **给每个字面量挂显式 `COLLATE`**:PG 中显式排序规则优先级最高,`col = 'lit'` 会因为两侧显式/隐式冲突而**报错**,把目前正确的场景弄坏。

**可行但复杂的方案(留作后续)**:在 MySQL 模式的解析分析阶段,**仅当**某个比较表达式的所有输入都不含可排序列时,才注入 `COLLATE case_insensitive`。这样精准命中缺口且不影响列参与的场景,但需要表达式树检查,且有函数返回值/参数/子查询等边界情况。

**建议:本轮记录为已知发散,不修。** 理由:纯字面量字符串比较在真实应用里罕见(有列参与的场景本来就是对的),而任何"看起来简单"的修法都有上面那两个陷阱。

---

## 3. 实施优先级与范围

| 顺序 | 项 | 类型 | 建议 |
|---|---|---|---|
| 1 | §2.2 外键检查移植 | 数据完整性 | **必做**,机械移植,风险低 |
| 2 | §2.1 REGEXP 修复 | 核心功能不可用 | **必做**,mysql-schema 算子覆盖,有 `c6773eb8cc` 先例 |
| 3 | §2.3 COLLATE 映射 | 静默错误 | **必做**,重点是"不认识就报错" |
| 4 | §2.4 P2-2 文档回填 | 文档 | 顺手做 |
| 5 | §2.5 P2-1 | 已知发散 | **不修**,只更新文档说明根因和两个陷阱 |

**明确不在本次范围**:
- 推翻"非确定性排序规则作默认"的架构决策(§1.3 已论证不建议)
- REGEXP 按操作数排序规则判定大小写敏感性(§2.1 范围边界)
- `mys_tablecmds.c` 与内核 `tablecmds.c` 的全量漂移比对(§2.2 提醒)
- `like_support.c` 相关的 LIKE 前缀索引性能问题(纯性能,非正确性)

---

## 4. 验证方案

### 4.1 环境

测试集群:`/tmp/claude-1000/.../scratchpad/mysqltest`,PG 端口 5433,MySQL 端口 3306,用户 `test`/`test`,库 `unvdb_mysqldb`。分析期间建的临时表(`collcmp`/`fkparent`/`fkchild`/`fkchild2`/`fkchild3`)可以直接 drop。

### 4.2 功能验证

| 用例 | 修复前 | 修复后应为 |
|---|---|---|
| `SELECT ... WHERE v REGEXP 'Ali'`(ci 列) | **ERROR 1295** | 正常返回,大小写不敏感命中 |
| `SELECT ... WHERE v REGEXP BINARY 'Ali'` | ERROR 1295 | 正常返回,大小写敏感 |
| `NOT REGEXP` / `RLIKE` / `NOT RLIKE` | ERROR 1295 | 均正常,语义对应 |
| 父 `case_insensitive` + 子 `COLLATE utf8mb4_bin` 建外键 | **静默成功** | 报错拒绝,信息与 PG 原生一致 |
| 父子同为默认(均 ci)建外键 | 成功 | 仍成功(不能误伤) |
| `COLLATE utf8mb4_bin` 列 | 静默变 `default` | 得到确定性排序规则(或明确报错) |
| `COLLATE 不存在的名字` | 静默丢弃 | **报错** |
| `COLLATE utf8mb4_general_ci` | `case_insensitive` | 仍 `case_insensitive`(不能误伤) |

**排序规则实际落盘值必须用 psql 直接查 `pg_attribute`/`pg_collation` 核对**,不要只看 `SHOW CREATE TABLE`(它自己有 P2-7 的 tokenizer bug)。

### 4.3 回归

```bash
cd /home/hlv/openhalo-update/postgresql_modified_for_babelfish
PERL5LIB=/home/hlv/perl5/lib/perl5 meson test -C build \
  --suite setup --suite postmaster --suite aux_mysql --suite regress
```

基线 **13/13**,必须保持。

### 4.4 跨方言隔离(用户核心原则)

- `psql` 连 5433 / 5432:正则、外键行为**必须完全不变**(mysql-schema 算子不在其 search_path)
- TDS 连 1433:同上
- 特别注意:§2.2 的外键检查是加在 `mys_tablecmds.c`(MySQL 专属副本),不要误改内核 `tablecmds.c`

---

## 5. 完成后要更新的文档

1. `FUSION_PLAN.md` P2-2 → 标为早已修复(文档过期)
2. `FUSION_PLAN.md` P2-8 → 标为已修复,附根因(后缀匹配 + 静默丢弃)
3. `FUSION_PLAN.md` P2-1 → 补充根因(PG 排序规则推导)与两个陷阱,标为"已知发散,有意不修"
4. `FUSION_PLAN.md` 新增条目:REGEXP 不可用(已修)、外键检查缺失(已修)、`mys_tablecmds.c` fork 漂移风险(未查全)、非确定性排序规则的性能代价(`text_pattern_ops`/前缀索引不可用)
5. `FUSION_PLAN.md` §9 待提交清单:加入本次改动文件
