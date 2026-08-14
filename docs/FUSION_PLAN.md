# openHalo × Babelfish 融合方案:PG + MySQL + TDS 三协议并行

> **版本化说明(2026-08-14)**:本文件自当日起随代码入仓(`postgresql_modified_for_babelfish/docs/`),原"不纳入版本控制"声明作废;工作区根目录的副本为工作稿,以 docs/ 内为准。
> 历史注记:早期作为本地工作记录(与 `CLAUDE.md`、`W8_HANDOFF.md` 同样处理)。
> 生成时间:2026-08-13,最后更新:2026-08-14
> 依据:`openhalo-fusion` 分支 29 个 `fusion:` 提交、`W8_HANDOFF.md`、以及对内核分派路径的独立代码核实。

## 0. 目标达成状态:✅ 已在单实例上实测验证

在同一个正在运行的 postmaster 上,对同一个表达式 `5/2`,三种协议各自返回了正确且不同的方言语义:

| 协议 | 端口 | 客户端 | `5/2` 结果 | 语义 |
|---|---|---|---|---|
| PostgreSQL | 5432 | `psql` | `2` | 标准 PG 整数除法 |
| MySQL | 13306 | `mysql` CLI (8.4.10) | `2.5000` | MySQL 除法精度递增(4 位小数) |
| TDS/T-SQL | 1433 | `python-tds`(纯 Python TDS 客户端,握手层验证) | `2.5`(`CAST(5 AS DECIMAL(10,2))/2`) | T-SQL DECIMAL 除法语义 |

三次查询在**同一次会话窗口内交替执行**,不是分别起停集群测的——这是"一个 PG 实例三协议并行"最直接的证据。详见 §5.7。

---

## 1. 目标

在**同一个 PostgreSQL 18.3 实例**上,让 PG、MySQL、TDS(T-SQL)三种线协议同时监听、互不干扰地并行服务,且每种协议的连接在解析、类型、DDL、错误封帧各层都能拿到自己方言的语义。

不是"给 Babelfish 打 MySQL 补丁",而是**用 openHalo 的 vtable 分派架构,统一收编 Babelfish 原有的全局 hook + ProtocolExtensionConfig 两套扩展机制**。

---

## 2. 三个代码库的角色

| 目录 | 分支 / HEAD | 角色 |
|---|---|---|
| `postgres` | `openhalo-update` @ `cbd6642fb03` | **架构范本 + 迁移源**。openHalo 自己在 PG 18.3 上的实现,已具备完整 vtable 三件套(`adtextapi.h` / `protocol_routine.h` / `parsereng.c`)与 `src/backend/parser/mysql/`、`contrib/aux_mysql` |
| `postgresql_modified_for_babelfish` | `openhalo-fusion` @ `d6a809f364` | **融合目标**。基线是 Babelfish 的 PG 18.3 内核(`BABEL_6_0_STABLE__PG_18_3` @ `62b343c15a`, Stamp 18.3),迁入 openHalo 的 MySQL 能力并做架构统一 |
| `babelfish_extensions` | `BABEL_6_0_STABLE` @ `beb4d1817` | TDS / T-SQL 扩展侧(内部版本 18.3.0.0),需跟随融合后的内核 API 演进 |
| `IvorySQL` | — | 目前在本融合中**无角色**,仅作参考存放 |

---

## 3. 融合架构

### 3.1 核心机制:按方言索引的统一注册表

内核维护一张按 `CompatibilityProtocolKind` 索引的表,每个方言一个条目,内含三个契约槽位:

```c
/* src/include/postmaster/compatibility.h */
typedef struct CompatibilityRoutine
{
    const struct ParserRoutine   *parser;    /* 语法分析 */
    const struct ADTExtMethod    *adtext;    /* 类型 / 排序规则 / typmod */
    const struct ProtocolRoutine *protocol;  /* 线协议与 DDL 分派 */
} CompatibilityRoutine;
```

```c
/* src/include/libpq/libpq-be.h */
typedef enum CompatibilityProtocolKind
{
    COMPAT_PROTOCOL_POSTGRES = 0,   /* 零值 = 标准 PG,保证零初始化即旧行为 */
    COMPAT_PROTOCOL_MYSQL,
    COMPAT_PROTOCOL_TDS,
    COMPAT_PROTOCOL_KIND_MAX
} CompatibilityProtocolKind;
```

### 3.2 选路链条

```
监听端口(listen_add_protocol_socket 打标)
        │
        ▼
ClientSocket->protocol_kind        [postmaster accept 时]
        │
        ▼
Port->protocol_kind                [pq_init() 或协议扩展自己的 fn_init]
        │
        ▼
MyCompatMode                       [InitCompatMode(), postinit.c]
        │  MyProcPort != NULL → 取 protocol_kind
        │  MyProcPort == NULL → 取 database_compat_mode GUC
        │                        (autovacuum / 后台 worker / 逻辑复制 apply)
        ▼
GetCompatibilityRoutine(MyCompatMode) → {parser, adtext, protocol}
```

并行 worker 通过 `parallel.c` 的 `FixedParallelState` 传播 `protocol_kind` + `babelfish_context`,在 `ParallelWorkerMain` 中恢复方言。

### 3.3 三个方言的注册现状

| 方言 | parser | adtext | protocol | 注册者 |
|---|---|---|---|---|
| PG | (内建 `StandardParserRoutine`) | (内建 `standard_adtext`) | (内建 `StandardProtocolRoutine`) | 内核 |
| MySQL | ✅ `mys_parser.c:566` | ✅ `mys_adtext.c:165` | ✅ `aux_mysql_init.c:130` | `mysql_parser` / `mysm` / `aux_mysql` |
| TDS | ❌ 空 | ❌ 空 | ❌ 空 | — 走独立的 `ProtocolExtensionConfig` |

**TDS 的空槽位是设计使然,不是缺陷。** Babelfish 完全通过自己的 `pe_config`(`ProtocolExtensionConfig`)分派,从不注册 `ProtocolRoutine`。内核的每个分派点都对此有 NULL 兜底 —— 这一点已逐点核实(见 §5.2)。

### 3.4 两套扩展机制的共存契约

| | openHalo vtable | Babelfish 全局 hook / pe_config |
|---|---|---|
| 粒度 | 按连接方言 | 进程全局单例 |
| 分派优先级 | **高**(先查 vtable) | 低(vtable 槽位为 NULL 时兜底) |
| 典型代表 | `ProtocolRoutine.process_utility` | `ProcessUtility_hook` |

这个优先级顺序是整个融合的关键不变量。违反它会导致 MySQL 连接的 DDL 跑进 Babelfish 的 T-SQL hook(历史上出现过 `unrecognized constraint type: 16`,见 `4795343644`)。

---

## 4. 已完成的工作

### 阶段一:代码导入与重叠调和(`76bfdaaebc` → `7ac4f4f76f`,8 个提交)

先导入自包含 MySQL 文件,再套用非重叠内核改动,最后逐个调和重叠文件:连接建立/认证、protocol 层 accept 分派、CTAS hook 链、DDL 分派、错误封帧、`REPLACE INTO`、MySQL 列标签、空语句序号重置、`ParameterStatus` 把 PG 封帧漏到 MySQL socket 等。

### 阶段二:三协议共存打通(`4795343644`、`7e149a34aa`)

- 单 cluster 同时加载 `'mysql_parser, mysm, aux_mysql, babelfishpg_tds'` 跑通。
- 修复 `ProcessUtility_hook` 优先级倒置(见 §3.4)。
- `database_compat_mode` 从 `PGC_POSTMASTER` 改为 `PGC_SUSET`,支持 `ALTER DATABASE ... SET`,让无客户端连接的进程也能拿到正确方言。
- 补 MySQL `DATABASE()` / `SCHEMA()`(映射到 `current_schema()`)。

### 阶段三:ADTExtMethod vtable 体系(W1–W7)

| 工作项 | 提交 | 内容 |
|---|---|---|
| W1 | `2718072e18` | 恢复 `postgres.c` 三个包装函数的 `protocol_config` 兜底(融合时被丢掉,TDS 连接会被套上 PG 的 comm-reset 语义) |
| W2 | `f386c032b6` | `aux_mysql` 的 `listen_init_hook` 改为链式调用;`varchar.c` 补 adtext NULL 守卫。**⚠ 该提交只修了 aux_mysql 一侧,TDS 侧的同名缺陷直到本轮才发现,见 §5.4** |
| W3 | `00b6fdca26` | ADTExtMethod 加 ABI 守卫(magic / struct_size / version),在出现第二个注册者前落地 |
| W4 | `774c2cec02` | 追加 13 个类型/排序规则/typmod 槽位 |
| W5 | `17220e71d6` | 24 个内核调用点改为"vtable 优先、全局 hook 兜底" |
| W5 修正 | `6b80c953c7` `5e889e7e61` `d4b2d9ff89` | 修布尔槽位跨方言 fallthrough;修 `sql_dialect` 门禁误加在 vtable 分支(`sql_dialect` 只区分 PG/TSQL,不区分 MySQL) |
| W6/W7 | `8092aa60a9` | 新增 `listen_add_protocol_socket()`;放宽 `ProtocolAuthenticate()` 的断言 |

### 阶段四:MySQL 语义与协议正确性(`a0be4897ca` → `d6a809f364`)

三轮 code review(`fix.md`)共修 23 项 + 后续专项:

- **协议层**:`RETURNING` 被静默丢弃(PG18 类型变更遗留)、lenenc 整数编码、`max_allowed_packet` 无客户端错误、`warning_count` 硬编码 0、零行结果集封帧、握手响应解析越界、DECIMAL/CHAR DDL 限制错误码。
- **`SHOW WARNINGS`**:`repalloc(NULL)` 崩溃(连接上第一条 NOTICE 即崩)、多语句批次里的诊断串味(全局 bool 改为 `Node*` 身份列表);新增 `SHOW COUNT(*) WARNINGS`。
- **排序规则语义**:`mysql.case_insensitive`(真 ICU 非确定性排序规则)下的 `INSTR`/`LOCATE`/`REPLACE`;把 `LIKE` 从错误映射到 `ILIKE` 改回 `~~`,交由操作数排序规则决定大小写敏感性。
- **DECIMAL 除法**:裁定 vtable 不是正确接缝,改用 mysql schema 运算符(`5/2 = 2.5000`、`DIV`),并标记 PARALLEL SAFE + 配套 `007_mysql_parallel.pl` 验证并行 worker 的方言传播。

---

## 5. 当前状态:W8(TDS 连接 protocol_kind 标记)

### 5.1 改动内容(`babelfish_extensions`,**未提交**)

```diff
# contrib/babelfishpg_tds/src/backend/tds/support_funcs.c — pe_create_server_port()
-        listen_add_socket(fd, protocol_config);
+        listen_add_protocol_socket(fd, protocol_config, COMPAT_PROTOCOL_TDS);

# contrib/babelfishpg_tds/src/backend/tds/tds_srv.c — pe_tds_init()
+        port->protocol_kind = client_sock->protocol_kind;
```

刻意**不**调用 `AssignProtocolRoutine()`:`GetProtocolRoutine(COMPAT_PROTOCOL_TDS)` 无注册者时返回 NULL,而 `AssignProtocolRoutine()` 拿到 NULL 会 `elog(FATAL)`。"protocol_kind 已设、ProtocolRoutine 为 NULL"是 `protocol_routine.h` 里写明的合法状态。

### 5.2 独立核实结论(本轮新增)

`W8_HANDOFF.md` 声称"零行为变化 + 两处修正",逐点核实如下:

| 分派点 | W8 前 | W8 后 | 结论 |
|---|---|---|---|
| `GetCurrentProtocolRoutine()` (`protocol_routine.c:67`) | `protocol_routine == NULL` → `&StandardProtocolRoutine` | 同左(W8 不设 `protocol_routine`) | **不变** ✅ |
| `ProcessUtility()` (`utility.c:537`) | `routine->kind == POSTGRES` → 落 `ProcessUtility_hook` | 同左 | **不变** ✅ |
| `InitADTExt()` (`adtext.c:123`) | `MyCompatMode=0` → 无注册者 → `standard_adtext` | `MyCompatMode=2` → 无注册者 → `standard_adtext` | **不变** ✅ |
| `GetDialectParserRoutine()` (`parsereng.c`) | 同上 → `StandardParserRoutine` | 同上 → `StandardParserRoutine` | **不变** ✅ |
| `report_fork_failure_to_client()` (`postmaster.c:3907`) | `client_sock->protocol_kind` 恒为 0 → 判断从未生效,给 TDS 客户端发 PG 帧错误包 | kind=2 → 正确跳过 | **修正 2 成立** ✅ |
| `InitCompatMode()` (`parsereng.c`) | TDS 连接静默解析为 POSTGRES | 正确解析为 TDS | **修正 1 成立** ✅ |

补充发现:**两处改动各自独立对应一个修正**。`support_funcs.c`(监听端口打标)单独就让修正 2 生效,因为 `postmaster.c:3907` 判断的是 `client_sock->protocol_kind`;`tds_srv.c`(`Port` 赋值)才是修正 1 的前提。

### 5.3 已验证 / 未验证

| 项 | 状态 |
|---|---|
| 内核构建 `ninja -C build` 2568/2568 targets, 0 errors | ✅ |
| `babelfishpg_tds` PGXS 构建 | ✅(需额外编译宏,见 §6 P1-2) |
| psql / Unix socket → `protocol_kind = 0` | ✅ 实测 |
| 裸 TCP → 1433 → `protocol_kind = 2` | ✅ 实测(W8 的目标效果) |
| **三协议同时监听**(5432 + 1433 + 13306) | ✅ 实测,**需先修 §5.4 的 chain 缺陷** |
| **MySQL 客户端功能回归** | ✅ 实测,见 §5.5 |
| `babelfishpg_common` / `money` 构建 | ✅ 本轮打通(修 Makefile,见 §6 P1-1) |
| `babelfishpg_tsql` 构建 | ✅ **本轮打通**(装 JRE + 源码编译 4.13.2 ANTLR runtime,见 §5.6) |
| tsql 客户端功能回归 | ✅ **本轮完成,真实 TDS 线协议**,见 §5.7 |
| `meson test` 四套件 | ✅ **13/13 全绿**(见 §8) |
| **三协议同实例并发验证** | ✅ **本轮完成**,见 §0、§5.7 |

### 5.4 本轮修复:TDS 的 `listen_init_hook` 断链(真实缺陷)

`W8_HANDOFF.md` 把 MySQL 端口连不上归因为"端口冲突 / GUC 没生效",这是**误判**。实测:集群启动后 5432 和 1433 正常监听,**13306 根本没有 bind**,日志里连一次尝试都没有。

根因在 `babelfish_extensions/contrib/babelfishpg_tds/src/backend/tds/tds_srv.c`:

```c
void pe_init(void)
{
    prev_listen_init = listen_init_hook;   /* 保存了 */
    listen_init_hook = pe_listen_init;
}

static void pe_listen_init(void)
{
    pe_create_server_ports();              /* 但从不调用 prev_listen_init() */
}
```

`listen_init_hook` 是单个全局函数指针而非注册表。`pe_init()` 保存了前值却从不调用,于是**任何先于 babelfishpg_tds 注册的监听器都被静默丢弃**。

`f386c032b6` 的提交信息声称"mirrors babelfishpg_tds's identical fix on the Babelfish side",但那次实际只改了 `aux_mysql` 一侧。它的验证也有盲区 —— 只测了 `'babelfishpg_tds, mysql_parser, mysm, aux_mysql'`(TDS 排第一)这一种顺序:

| `shared_preload_libraries` 顺序 | 最后注册者(链头) | 结果 |
|---|---|---|
| TDS 在前、aux_mysql 在后 | `mysql_listen_init`(**会 chain**) | 两个监听器都起来 → 掩盖了 TDS 的缺陷 |
| aux_mysql 在前、TDS 在后 | `pe_listen_init`(**不 chain**) | **MySQL 监听器被静默禁用** |

修复(与 `aux_mysql_init.c` 完全对称,无条件 chain):

```c
static void pe_listen_init(void)
{
    pe_create_server_ports();
    if (prev_listen_init != NULL)
        prev_listen_init();
}
```

修复后三个端口全部 bind:`5432`(PG) / `1433`(TDS) / `13306`(MySQL)。**这是"三协议真正并行"第一次在单实例上被实证。**

> 教训:`f386c032b6` 那轮"两个监听器都起来了"的验证结论是对的,但结论成立的原因不是它以为的那个。对称的双向缺陷必须双向测(两种加载顺序),单向验证会系统性地漏掉一半。

### 5.5 MySQL 功能回归结果(本轮实测)

真实 `mysql` CLI(8.4.10 客户端)对 13306:

| 用例 | 结果 | 对应提交 |
|---|---|---|
| `SELECT VERSION()` | `8.4.10-openhalo-1.0` | — |
| `SELECT 5/2` | `2.5000` ✅ | `c6773eb8cc` DECIMAL 除法 |
| `SELECT 7 DIV 2` | `3` ✅ | 同上 |
| `AUTO_INCREMENT` + `DECIMAL(10,2)` DDL | 正确 ✅ | 阶段一 DDL 分派 |
| `REPLACE INTO` | 正确 ✅ | `2cda87b44b` |
| `INSTR('HelloWorld','WORLD')` | `6` ✅(大小写不敏感) | `4259887286` |
| `REPLACE('abcABCabc','ABC','X')` | `abcXabc` ✅(字节匹配,不受排序规则影响) | `fa887e0789` |
| `LIKE 'alice%'` 匹配 `AliceNew` | 正确 ✅ | `ca3e74b273` |
| `DATABASE()` | `public` ✅ | `4795343644` |

### 5.6 环境搭建要点(踩坑记录)

1. **MySQL 协议只支持 `md5` HBA 方法**(`mysql_auth.c:693`,`uaMD5` 之外全部 FATAL)。`trust` 会直接报 `ERROR 1045 ... authentication method "trust" is not supported`。
2. **密码必须存成 `mysql_native_password` 格式**:
   ```sql
   SET password_encryption = 'mysql_native_password';
   ALTER USER postgres PASSWORD 'test123';
   ```
   存成 PG 的 `md5` 格式会报 `password mismatch` —— `mysql_native_password_verify()`(`crypt.c:468`)先检查 `get_password_type()`,非 `PASSWORD_TYPE_MYSQL_NATIVE_PASSWORD` 直接拒绝。
3. **`aux_mysql` 扩展不会自动安装**。普通 `initdb` 建的集群即使 `database_compat_mode = mysql`,`pg_extension` 里也没有 `aux_mysql`,此时 `5/2` 会走 PG 整数除法返回 `2`。需 `CREATE EXTENSION aux_mysql;`(或用 `initdb -m mysql`)。
4. **本机 3306/33060 被系统级 `mysql.service` 占用**(仍 active),测试务必换端口。

### 5.6 打通 `babelfishpg_tsql` 构建:ANTLR4 工具链

`babelfishpg_tsql` 的 T-SQL 语法解析器由 ANTLR4 生成,构建链路比其他扩展长得多。完整踩坑记录:

1. **JRE**:ANTLR4 代码生成器是 Java jar(仓库自带 `antlr/thirdparty/antlr/antlr-4.13.2-complete.jar`),本机原先没有 JRE,`apt install default-jre` 解决。
2. **`cmake=cmake` 变量**:`Makefile:178` 的 `cd antlr && $(cmake) . && cd ..` 里 `$(cmake)` 从未在文件内定义,必须外部传入 `make cmake=cmake`。
3. **ANTLR4 C++ runtime 版本必须精确匹配 4.13.2**:`apt` 只有 4.10(`libantlr4-runtime-dev`),装上后编译报 `antlr4::internal::OnceFlag`/`call_once` 不存在——这是 4.13.2 生成代码依赖的、4.10 runtime 没有的新 API,**证实了次版本不兼容不是理论风险,是实测会炸的**。解决:
   - 卸载 apt 的 4.10 包(`libantlr4-runtime-dev` + `libantlr4-runtime4.10`),否则 `ldconfig` 缓存会持续解析到 4.10 的 `.so`,即使 `/usr/local/lib` 装了 4.13.2 也会在运行时用错版本。
   - 从 GitHub 拉 `antlr4` 4.13.2 tag,`runtime/Cpp` 目录下 `cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local -DANTLR4_INSTALL=ON`,只编译 `antlr4_static antlr4_shared` 两个 target(不编译自带的 gtest 测试套件,省时间),`sudo make install`。
   - `CMakeLists.txt` 硬编码头文件路径 `/usr/local/include/antlr4-runtime/`,与 apt 装的路径不同,需要保证最终该路径下是 4.13.2 的头文件(不能是残留的 4.10 符号链接)。
4. **`TSQLSRC` 变量必须定义在 `include $(PGXS)` 之前**:`Makefile:98` 的 `PG_CPPFLAGS += -I$(TSQLSRC) ...` 引用了当时定义在第 175 行(远晚于 `include $(PGXS)`)的 `TSQLSRC = .`。PGXS 内部用 `:=`(立即展开)把 `PG_CPPFLAGS` 灌进 `CPPFLAGS`,那一刻 `TSQLSRC` 还没定义,展开成空,编译报 `-I -I<path>` 语法错误(第一个 `-I`后面是空的)。**与 P1-1 是同一类"变量必须在 include PGXS 之前就绪"的问题,但发生在 `babelfishpg_tsql` 而不是 `babelfishpg_common`**。修复:把 `TSQLSRC = .` 上移到 `PG_CPPFLAGS +=` 那行之前。
5. **`CXX` 在 meson 生成的 `Makefile.global` 里是空字符串**:这台内核是 meson 构建的纯 C 项目,`Makefile.global:271` 的 `CXX = ` 从未被 meson 的 PGXS 导出逻辑填充(该内核从未编译过 C++ 代码,没有探测的动机)。PGXS 隐式规则 `$(CXX) $(CXXFLAGS) ... -c $< -o $@` 展开后首个词变成裸 `g`(疑似某处 `gcc`→`g`+`++` 的字符串替换只做了一半),报 `make: g: No such file or directory`。**这是内核 meson 构建系统的一个真实缺口**,当前用 `make CXX=g++` 外部覆盖绕过,根治需要修 meson 的 PGXS Makefile.global 生成脚本探测 C++ 编译器(即使内核本身不用 C++)。
6. **GCC 13 对 ANTLR4 4.13.2 runtime 头文件报新警告**:`tree/ParseTree.h` 的 `operator==` 隐藏了 `RuleContext.h` 的同名虚函数,这是 ANTLR4 upstream 代码里的已知问题,只有较新 GCC 才会警告(`-Woverloaded-virtual`),被项目的 `-Werror` 转成硬错误。修复方式与 `Makefile` 里已有的"关闭 ANTLR runtime 头文件触发的警告"那一组注释一致:加一条 `-Wno-error=overloaded-virtual`。
7. **`MODULE_big` 产物命名与 `.control` 文件里的 `module_pathname` 不一致**:`babelfishpg_tsql.control`/`babelfishpg_common.control` 期望 `$libdir/babelfishpg_tsql-6.so`(源自 `MODULEPATH = $$libdir/$(EXTENSION)-$(MAJOR_VERSION)`),但 `MODULE_big = $(EXTENSION)` 实际产出的是不带版本号后缀的 `babelfishpg_tsql.so`,两者之间**没有任何 rename/symlink 步骤衔接**,`CREATE EXTENSION` 会 `dlopen` 失败。当前用符号链接绕过(`ln -sf babelfishpg_tsql.so babelfishpg_tsql-6.so`),根治需要 Makefile 补一步版本化 rename,或统一 `.control` 的命名约定。
8. **`uuid-ossp` 依赖但内核构建时被跳过**:`babelfishpg_tsql.control` 要求 `uuid-ossp`,但内核 meson 配置里 `uuid=none`(合入 Babelfish 基线时的默认值),contrib/uuid-ossp 从未真正编译。改用 `meson configure build -Duuid=e2fs`(用标准 Linux `libuuid`)+ 单独 `ninja -C build contrib/uuid-ossp/uuid-ossp.so` 补齐。

### 5.7 T-SQL/TDS 端到端验证:一次真实的 systematic-debugging 案例

#### 工具选择

本机没有 `tsql`/`sqlcmd`,且没有 sudo 权限自行安装系统包。绕过等待:纯 Python 实现的 TDS 协议客户端 `python-tds`(pytds)不依赖任何系统级 FreeTDS C 库,`python3 -m venv` + `pip install python-tds` 即可在用户态跑通,不需要 sudo。

#### 发现的真实 bug:postmaster 阶段的非法目录访问(已用 systematic-debugging 流程排查并修复)

第一次尝试把 `babelfishpg_tsql` 加进 `shared_preload_libraries` 并重启集群,直接 **段错误**。用 gdb 抓 backtrace 定位:

```
SearchCatCache (cache=0x0, ...)
  ← GetSysCacheOid ← get_namespace_oid("sys", missing_ok=true)
  ← init_tcode_trans_tab() [babelfishpg_common/src/typecode.c:110]
  ← babelfishpg_common's _PG_init() [babelfishpg_common.c:100]
  ← load_libraries("babelfishpg_common", ...) [babelfishpg_tsql/src/pl_handler.c:5927]
  ← babelfishpg_tsql's _PG_init()
  ← process_shared_preload_libraries() ← PostmasterMain()
```

**根因**:`process_shared_preload_libraries()` 在 **postmaster 进程本身**里跑(`PostmasterMain()` 直接调用,从未 fork 出真正的 backend),此时 `InitCatalogCache()` 从未执行过,`SysCache[]` 全是 `NULL`。`babelfishpg_common::_PG_init()` 无条件调用 `init_tcode_trans_tab()`,而它内部 `get_namespace_oid("sys", true)` 走到 `SearchCatCache(cache=0x0, ...)`,直接解引用空指针崩溃——`missing_ok=true` 只保护"目录里确实没有这一行"这种情况,不保护"目录缓存机器根本不存在"。

`typecode.c` 函数自己的注释证实了设计意图:该调用**本来就该在目录未就绪时安全地 no-op**,由 `get_tsql_type_info()` 在真正需要时懒加载重跑一次(那时目录已经就绪)——问题只在于 `get_namespace_oid()` 在无目录缓存的场景下是段错误而不是优雅返回 `InvalidOid`。

**关键教训(Phase 1 "check recent changes" 环节本该先做但没做)**:continued 修复到第二个崩溃点(`pltsql_coerce.c` 的 `init_tsql_coerce_hash_tab()`,同类问题)时,回头核实了一个更基础的假设——**真实 Babelfish 到底要不要把 `babelfishpg_tsql` 放进 `shared_preload_libraries`?** 查仓库自带的 `dev-tools.sh`/`contrib/README.md`,标准配置只放 `babelfishpg_tds`,`babelfishpg_tsql`/`babelfishpg_common` 是靠 `CREATE EXTENSION ... CASCADE` 在真实 backend(目录已就绪)里加载的。**是我自己的测试配置错了,不是标准路径上的 bug。**

**处理方式**:
- 保留已经验证有效、遵循 PG 官方 `process_shared_preload_libraries_in_progress` 惯用法的两处修复(`babelfishpg_common.c`、`pl_handler.c` 的 vector 探测)——这是合理的防御性加固,现实中确有可能有人像我一样误配置,得到优雅降级而不是段错误是纯收益。
- **没有继续**去修第三、第四个崩溃点(`init_tsql_coerce_hash_tab`/`init_tsql_datatype_precedence_hash_tab`/`init_tsql_cursor_hash_tab`/`init_catalog`),因为标准配置根本不会走到那条路径,继续修属于为一个不存在的使用场景过度设计。
- 把 `shared_preload_libraries` 改回只含 `babelfishpg_tds`(与仓库自带文档一致),`babelfishpg_tsql`/`babelfishpg_common` 走 `CREATE EXTENSION` 路径。

#### 端到端验证结果

独立数据库 `tsqldb`(`postgres` 库已被 MySQL 测试用脏,按真实部署惯例给 T-SQL 单独开库),`bbf_admin` 角色,`CALL sys.initialize_babelfish('bbf_admin')` 完成 Babelfish 目录初始化。踩的两个配置坑:
- `babelfishpg_tsql.database_name` 默认硬编码 `babelfish_db`,与自建库名不一致要 `SET babelfishpg_tsql.database_name = 'tsqldb'`(`PGC_SUSET`,写入 `postgresql.conf` 即可,不需要重启)。
- **`single-db` 模式下,TDS 登录的 `database` 字段是 Babelfish 逻辑库名,不是 PG 物理库名**——`sys.initialize_babelfish()` 只注册 `master`/`tempdb`/`msdb` 三个系统库,自建的 PG 库名本身不会自动出现在 `sys.babelfish_sysdatabases` 里。第一次 TDS 连接应该连 `master`,后续需要更多逻辑库时在 T-SQL 会话内用 `CREATE DATABASE` 创建(映射为 PG schema,不是新的 PG database)。

真实 TDS 线协议(`python-tds`,端口 1433)测试结果,全部通过:

| 用例 | 结果 |
|---|---|
| `SELECT @@VERSION` | `Babelfish for PostgreSQL with SQL Server Compatibility - 12.0.2000.8 ... PostgreSQL 18.3 ... (Babelfish 6.0.0)` |
| `DECLARE @x INT = 3; DECLARE @y INT = 4; SELECT @x + @y` | T-SQL 变量语法正确解析执行 |
| `CREATE TABLE ... (id INT IDENTITY PRIMARY KEY, name NVARCHAR(50))` | IDENTITY + NVARCHAR 建表成功 |
| `INSERT` + `SELECT TOP 1 ... ORDER BY` | T-SQL DML/查询语法正确 |
| `CREATE PROCEDURE ... AS BEGIN ... END` | 存储过程创建成功(要求单语句批次,与真实 SQL Server 行为一致) |
| `EXEC dbo.test_proc2 3, 4` | 存储过程调用正确返回 `7` |
| `DECLARE CURSOR ... OPEN ... FETCH NEXT ... WHILE @@FETCH_STATUS ...` | 游标遍历正常 |
| `BEGIN TRY ... END TRY BEGIN CATCH ... END CATCH` | 批次正常完成,无崩溃 |
| `DB_NAME()` | 正确返回 `master` |
| `CAST(5 AS DECIMAL(10,2)) / 2` | `2.5`(T-SQL DECIMAL 除法语义,与 MySQL 的 `2.5000`、PG 的 `2` 均不同且都正确) |

随后在**同一个存活的 postmaster 上**交替验证 MySQL(`mysql` CLI,`5/2=2.5000`)和原生 PG(`psql`,`5/2=2`)仍然正常——三协议并发,不是分次独立跑通。

#### 独立交叉验证:真实 FreeTDS `tsql` 客户端

`freetds-bin` 装好后,用官方 C 库实现的 `tsql`(与之前的纯 Python `python-tds` 完全独立的两套实现)重新连接同一集群:

```
1> SELECT @@VERSION
   Babelfish for PostgreSQL with SQL Server Compatibility - 12.0.2000.8 ... (Babelfish 6.0.0)
1> SELECT 10/3 AS int_div, CAST(10 AS DECIMAL(10,2))/3 AS dec_div
   int_div=3, dec_div=3.3333333333333
1> EXEC dbo.test_proc2 5, 6
   sum_result=11
1> SELECT DB_NAME()
   master
```

两个关键点:
- `@@VERSION`/`DB_NAME()`/DECIMAL 除法语义与 python-tds 会话完全一致——两套独立客户端实现互相印证,排除了"只有某个客户端库凑巧兼容"的可能性。
- **`dbo.test_proc2` 是上一个 python-tds 会话创建的**,这次用全新的 `tsql` 会话直接 `EXEC` 成功拿到正确结果 `11`——证明 T-SQL 目录(存储过程定义)正确持久化,不同客户端、不同连接之间可以正常协作,不是单会话内的巧合。

### 5.8 MySQL 兼容性专项测试:按用户指定的标准流程

用户给出的标准测试流程(独立、干净的数据目录,而不是复用已经跑过 W8/T-SQL 混合测试的旧集群):

1. `initdb` 全新数据目录。
2. `psql` 登录,`SET password_encryption=mysql_native_password`,建 `test` 超级用户,核对 `pg_shadow`。
3. `mysql` 客户端用 `test`/`test` 登录。
4. MySQL 模式下建库、`SHOW DATABASES`、`USE`,再继续测其他 MySQL 兼容语法。

#### 环境细节

- 全新集群:`initdb -D <scratch>/mysqltest --locale=C.UTF-8 --encoding=UTF8`,`shared_preload_libraries='mysql_parser, mysm, aux_mysql'`,`mysql_port=3306`,PG 端口 `5433`(避免和之前 W8 测试用的旧集群冲突)。
- 本机系统级 `mysql.service`(真实 MySQL 8.0.46 Server,`mysql-server` apt 包,PID 697,开机自启)一开始占着 3306,用户执行 `sudo systemctl stop mysql` 后腾出端口,测试改回用户指定的 3306(非本项目的一部分,不影响其正常回滚使用 `systemctl start mysql`)。
- HBA 沿用之前发现的约束:MySQL 协议只认 `md5` 方法,`127.0.0.1/32` 那行改成 `md5`(不能用 `trust`)。
- `psql` 登录走 Unix socket 而非 TCP(避免密码认证问题),注意 socket 目录若在深层 scratchpad 路径下会超过 107 字节的 Unix socket 路径长度限制,需要显式连 `/tmp`(`unix_socket_directories` 默认值)。
- **`aux_mysql` 扩展在全新 `initdb` 后不会自动装**(这是 §5.6 已知的踩坑点在新场景下的复现):第一次 `CREATE TABLE` 报 `collation "case_insensitive" for encoding "UTF8" does not exist`,根因是 `pg_extension` 里只有 `plpgsql`,MySQL 客户端能连接(协议层由 `shared_preload_libraries` 提供)但 SQL 层的排序规则/类型定义(靠 `CREATE EXTENSION aux_mysql` 的安装脚本创建)没有跟着装。装上后 `SHOW DATABASES` 才会出现完整的 `information_schema`/`mysql`/`performance_schema`/`sys`/`mys_sys` 系统库集合。

#### 步骤 1-4 结果

`test` 用户密码正确存成 `mysql_native_password:...` 格式(`pg_shadow` 核实);`mysql -u test -ptest` 登录成功;`CREATE DATABASE unvdb_mysqldb` + `SHOW DATABASES` + `USE unvdb_mysqldb` 全部正确(`unvdb_mysqldb` 本身不是真实 PG database,是 MySQL 兼容层里映射出来的逻辑库,`DATABASE()` 底层显示的是真实的 `postgres` 库——与 T-SQL 那边"逻辑库映射成 schema"是同一套设计)。

#### 后续兼容性测试结果

| 类别 | 用例 | 结果 |
|---|---|---|
| DDL | `AUTO_INCREMENT`/`TINYINT`/`DECIMAL(10,2)`/`VARCHAR(n) UNIQUE`/`TIMESTAMP DEFAULT CURRENT_TIMESTAMP`,`ENGINE=InnoDB` | ✅ 正确 |
| DDL | `ALTER TABLE ADD COLUMN`、`MODIFY COLUMN`(TINYINT→SMALLINT 类型变更) | ✅ 正确 |
| DDL | `CREATE INDEX` + `SHOW INDEX`(PRIMARY/UNIQUE/普通索引都正确列出) | ✅ 正确 |
| DML | `INSERT` 多行、`LAST_INSERT_ID()`、`REPLACE INTO`、`UPDATE`、`DELETE` | ✅ 正确 |
| 字符串函数 | `CONCAT`/`UPPER`/`LOWER`/`INSTR`/`LOCATE`/`LENGTH`/`TRIM`/`SUBSTRING` | ✅ 正确 |
| 排序规则语义 | `LIKE 'alice%'` 大小写不敏感匹配 | ✅ 正确 |
| 算术语义 | `5/2=2.5000`(除法精度递增)、`5 DIV 2=2`、`5 MOD 2=1` | ✅ 正确 |
| 事务 | `START TRANSACTION`/`ROLLBACK`(插入后行数回退)、`COMMIT`(行数保留) | ✅ 正确 |
| 日期时间 | `NOW()`/`CURDATE()`/`DATE_ADD(...INTERVAL...)`/`DATE_FORMAT`/`DATEDIFF` | ✅ 正确 |
| 聚合 | `COUNT`/`AVG`/`GROUP BY`/`HAVING` | ✅ 正确 |
| 查询 | `LIMIT`/`OFFSET`、`ORDER BY` | ✅ 正确 |
| 条件表达式 | `IF(cond, a, b)`、`CASE WHEN ... THEN ... ELSE ... END` | ✅ 正确 |
| 管理语句 | `SHOW VARIABLES LIKE`、`CONNECTION_ID()` | ⚠ 见下 |
| 预处理协议 | `PREPARE`/`SET @var`/`EXECUTE ... USING`/`DEALLOCATE PREPARE`(`3+4=7`) | ✅ 正确 |
| JSON | `JSON_EXTRACT`/`JSON_OBJECT` | ❌ 已知 gap,见下 |

**发现并修复的真实 bug(P2-6)**:`SHOW CREATE TABLE` 输出里全是字面 `\n` 而不是真正换行(`aux_mysql--1.3--1.4.sql` 里 7 处字符串字面量少了 `E` 前缀,`standard_conforming_strings=on` 下反斜杠不转义)。已修复并验证,详见 §6 P2-6。

**发现但决定不修的真实 bug(P2-7)**:`SHOW CREATE VIEW` 的 DDL 重建逻辑在绝大多数真实场景下(单表 view 裸列名、任何带 WHERE 的 view)会输出语法错误的垃圾 DDL,项目自己的测试套件从未真正验证过这段输出(golden 文件里存的就是 bug 的产物)。详见 §6 P2-7。

**已知 gap,非新发现**:`JSON_EXTRACT`(以及 `JSON_OBJECT`)等 MySQL 风格 JSON 函数不存在(`mysql` schema 下只有 `json_object_field`/`json_path_extract_text`/`json_unquote` 内部辅助函数,没有 MySQL 命名的外部接口)。与 fusion 提交历史(`7ac4f4f76f`)记录的 `JSON_OBJECT` 缺失是同一类、更大范围的 JSON 函数覆盖不全问题。

**⚠ 严重度修正(P2-10 分析中查明)**:`SHOW VARIABLES LIKE 'version%'` 值为空当时被判定为"次要瑕疵",实际不是——`SHOW VARIABLES` 命令**系统性地**对所有变量返回空值(视图 `empty_session_variables` 硬编码空字符串,`getSystemVariableValueForShow()` 是死代码,从未被调用),不止 `version%` 这一处。详见 P2-15。

### 5.9 阶段二:SQL 全链路逐层验证(解析器/优化器/执行器/存储)

按 §7.0 规定的执行优先级,阶段一(三协议并行)完成后,阶段二对 PG/MySQL/TDS 三种方言逐层验证解析器→优化器→执行器→存储四层是否都正确工作,而不是零散的功能点测试。任务按"方言 × 层"切片(10 个任务,PG 基线 1 个 + MySQL/TDS 各 4 层 + 汇总 1 个)。

#### PG 基线

PG 方言的四层由内核自带 `regress/regress` 套件覆盖(232 subtests),本轮重跑确认仍然 232/232 全绿,作为对照基线,不需要额外手工测试。

#### MySQL 方言:逐层结果

| 层 | 验证内容 | 结果 |
|---|---|---|
| 解析器 | 反引号保留字标识符(`` `select` ``/`` `order` ``)、含空格标识符、多语句批次(一条消息 3 条 SELECT 返回 3 个结果集)、`ON DUPLICATE KEY UPDATE`、`INSERT IGNORE`、`LOCK/UNLOCK TABLES`、内联/尾随注释 | ✅ 全部正确 |
| 解析器 | 列级 `CHARACTER SET`/`COLLATE` 子句 | ⚠ 见 P2-8:`COLLATE` 被静默忽略 |
| 优化器 | `EXPLAIN`/`EXPLAIN ANALYZE`、单表索引扫描(`Bitmap Index Scan`)、JOIN 计划(`Nested Loop` + 索引侧)、代价模型 | ✅ 正确,`ANALYZE` 后与纯 PG 表计划完全一致(已用同规模 PG 表直接对照排除误判) |
| 执行器 | `LEFT JOIN`+`GROUP BY`、标量子查询、`IN`子查询、`EXISTS`子查询、CTE(`WITH`)、窗口函数(`ROW_NUMBER`/`RANK`/`PARTITION BY`) | ✅ 全部正确 |
| 执行器 | `CREATE TRIGGER`、`CREATE PROCEDURE`(游标+`HANDLER`) | ❌ 见 P2-9:过程式语言基本没实现 |
| 存储 | 外键 `ON DELETE CASCADE`、`CHECK` 约束(含真实 MySQL 错误码 3819)、`LONGTEXT` 大字段(10 万字符 TOAST 透明存取)、`ANALYZE TABLE`/`OPTIMIZE TABLE` | ✅ 全部正确 |
| 存储 | `SET SESSION/GLOBAL TRANSACTION ISOLATION LEVEL` | ❌→✅ 发现时完全静默失效,已在 P2-10 修复(SESSION 作用域;裸写/GLOBAL 见 P2-13/P2-14) |

#### TDS/T-SQL 方言:逐层结果

| 层 | 验证内容 | 结果 |
|---|---|---|
| 解析器 | `GO` 批分隔符、本地临时表(`#temp`)、表变量(`DECLARE @t TABLE`)、`OUTPUT` 子句(`INSERT...OUTPUT INSERTED.col`)、动态 SQL(`EXEC('...')`/`sp_executesql`) | ✅ 全部正确 |
| 解析器 | `TOP N WITH TIES` | ❌ 见 P2-11:完全不可用 |
| 优化器 | 索引使用、代价模型 | ✅ 正确,与同规模纯 PG 表对照计划完全一致 |
| 优化器 | `SET SHOWPLAN_ALL`/`STATISTICS IO` | ⚪ 非 bug——`tsqlUnsupportedFeatureHandler.cpp` 里有专门的已知不支持特性处理器(escape hatch 机制),是刻意设计的降级行为 |
| 执行器 | `JOIN`、`IN`子查询、CTE(`;WITH`)、窗口函数(`ROW_NUMBER OVER PARTITION BY`)、事务嵌套(`SAVE TRAN`/`ROLLBACK TRAN sp1` 精确回滚到保存点、`COMMIT TRAN`) | ✅ 全部正确 |
| 执行器 | `AFTER` 触发器(含 `inserted` 伪表)、存储过程嵌套调用 + `TRY/CATCH` + `ERROR_MESSAGE()`/`ERROR_PROCEDURE()` | ✅ 全部正确,**且明显比 MySQL 侧成熟**(Babelfish 原有代码库) |
| 存储 | 表级 `CONSTRAINT ... FOREIGN KEY ... ON DELETE CASCADE`、`CHECK` 约束、`NVARCHAR(MAX)` 大字段(10 万字符 TOAST) | ✅ 全部正确 |
| 存储 | 列内联 `FOREIGN KEY REFERENCES` 简写语法 | ❌ 见 P2-12:语法不支持(表级写法没问题) |

#### 阶段二结论

1. **优化器层在两个方言下都是协议无关的**——用同规模纯 PG 表直接对照,确认索引选择、JOIN 计划、代价估算完全一致。这一层没有 MySQL/TDS 特有缺陷,唯一要注意的是`ANALYZE`/统计信息收集这件事本身跟方言无关,忘记跑就会得到看似"退化"的计划(本轮测试中一度误判为 bug,对照后排除)。
2. **执行器层的核心查询能力(JOIN/子查询/CTE/窗口函数)两个方言都扎实**,没有发现真实缺陷。
3. **两个方言在"过程式/触发器"这个维度上成熟度差距很大**:T-SQL(Babelfish 原有代码)的触发器、存储过程、`TRY/CATCH`、嵌套调用全部正确且行为符合真实 SQL Server;MySQL(openHalo 新增部分)的触发器和存储过程基本是语法糖,没有实现真正的过程式语言。这不是这轮融合引入的回归,是 MySQL 兼容层从设计上就还没做到这个深度。
4. **存储层的基础能力(外键/CHECK/TOAST/统计信息维护)两个方言都正确**,DDL 语法细节上各有一两个窄范围的缺口(MySQL 的显式 `COLLATE` 被吞、TDS 的 `TOP WITH TIES`/列内联外键简写)。
5. 按 §7.0 的执行原则,以上 5 个新发现的 bug(P2-8~P2-12)全部只记录、不修复,阶段三再处理。

---

## 6. 遗留问题清单

### P0 — 阻塞 W8 收口

| # | 问题 | 状态 |
|---|---|---|
| P0-0 | **TDS `listen_init_hook` 断链** | ✅ **本轮修复**,见 §5.4。这是 P0-1 的真正根因 |
| P0-1 | MySQL 直连回归 | ✅ **本轮完成**,见 §5.5,全部正确 |
| P0-2 | W8 后重跑 `meson test` 四套件 | ✅ **本轮完成:13/13 全绿**(修了两处测试环境依赖,见 P1-6) |
| P0-3 | tsql 客户端端到端回归 | ✅ **本轮完成**,真实 TDS 线协议(python-tds),见 §5.7。存储过程/游标/TRY-CATCH/IDENTITY/DECIMAL 语义全部正确,且与 MySQL、PG 在同一存活集群上并发验证 |

### P1 — 构建系统

| # | 问题 | 状态 |
|---|---|---|
| P1-1 | **重复 `include $(PGXS)`** | ✅ **本轮修复**。范围比 handoff 记录的更大:`babelfishpg_common`(第 54 / 101 行)**和** `babelfishpg_tsql`(第 141 / 253 行)都有。修复时注意默认 goal:common 删靠前那个后,`src/geo_parser.o` 会变成第一个显式 target 并夺走默认 goal,须把那三行依赖规则移到 include 之后;tsql 删靠后那个(它前面已有 `all:` 追加,且尾部本就有 `.DEFAULT_GOAL := all`) |
| P1-2 | OpenSSL 特性探测宏未定义 | ✅ **已修复(2026-08-14 下午,`3a900828f`)**。改为**代码级派生**:`tds_secure.h` 依据 `OPENSSL_VERSION_NUMBER >= 0x10100000L` 定义两个宏(所有 TDS TU 统一生效),不再依赖任何构建系统特性探测,永久消除手动 CPPFLAGS |
| P1-3 | PGXS 不自动传 libxml2 头文件路径 | ✅ **已根治(2026-08-14 下午,`afb1548a8a`)**。根因:内核带 libxml 编译时安装的 `pg_config.h` 定义 `USE_LIBXML`,`utils/xml.h` 的 include 对扩展生效但 PGXS CPPFLAGS 缺头路径。修复:meson 的 pgxs 生成里用 pkg-config 取 cflags 把 `-I/usr/include/libxml2` 并入 Makefile.global 的 CPPFLAGS(注意不能只取 .pc 的 includedir 变量——那是 /usr/include,真正的头在 includedir/libxml2) |
| P1-4 | **`babelfishpg_tsql` 需要 ANTLR4 工具链** | ✅ **本轮打通**。实际踩坑比预期多得多(JRE、`cmake=cmake`、ANTLR runtime 版本必须精确匹配 4.13.2 而非 apt 的 4.10、`TSQLSRC` 的 PGXS-include 顺序问题、meson 内核的 `CXX` 空值、GCC 13 新警告、`.control` 版本化命名、`uuid-ossp` 未构建),完整记录见 §5.6 |
| P1-5 | `PG_SRC` 语义 | ✅ **已固化(2026-08-14 下午)**:`PG_SRC` 必须传内核**源码树根**这一要求已写进 `babelfish_extensions/build-all.sh`(自动计算),不再依赖人工记忆 |
| P1-6 | **测试套件的两处环境依赖** | ✅ **本轮修复**。① `IPC::Run` 装在 `~/perl5`(local::lib),`.bashrc` 才导出 `PERL5LIB`,非交互 shell 里为空 → 8 个 TAP 测试全部 `exit status 2` 启动即死。跑测试必须显式带 `PERL5LIB=/home/hlv/perl5/lib/perl5`。② `expected/40_metadata_admin.out` 里硬编码了生成该文件的机器的 OS 用户名(`unvdb`),`SHOW CREATE FUNCTION` 回显 `aux_mysql` 自带 `mysql.version()` 的 DEFINER,换台机器必挂。已在 `run_mysql_compat.sh` 的规范化里加 `DEFINER=\`$(id -un)\`@` → `<OSUSER>`,**刻意只规范化当前 OS 用户**,让套件自己建的对象(DEFINER 为角色 `test`)仍逐字比对 |
| P1-7 | `babelfishpg_tsql/Makefile` 的 `TSQLSRC` 定义顺序 | ✅ **本轮修复**。同 P1-1 的"变量必须在 include PGXS 之前就绪"问题,但发生在不同变量(`TSQLSRC`,而非重复 include),详见 §5.6 第 4 点 |
| P1-8 | meson 内核构建的 `Makefile.global` 里 `CXX` 为空 | ✅ **已根治(2026-08-14 下午,`afb1548a8a`)**。根因:上游 meson 只在 llvm.found() 时才写 CXX。修复:LLVM 缺失时用 find_program 探测 c++/g++ 填入 CXX,不再需要 `make CXX=g++` |
| P1-9 | ANTLR4 4.13.2 runtime 头文件在 GCC 13 下的 `-Woverloaded-virtual` | ✅ **本轮修复**,加 `-Wno-error=overloaded-virtual`,详见 §5.6 第 6 点。upstream ANTLR4 自身的已知问题,非本项目代码缺陷 |
| P1-10 | `.control` 文件的 `module_pathname` 与 `MODULE_big` 实际产物名不一致 | ✅ **已根治(2026-08-14 下午,`3a900828f`)**。裁决改为**全未版本化**:上游本身自相矛盾(control 模板版本化、MODULE_big 未版本化、且 `pl_handler.c` 硬编码加载未版本化名),统一为 MODULEPATH 与 MODULE_big 均不带版本,与 money/tds 及内部加载名一致,**符号链接彻底删除**。⚠ common Makefile 里 MODULEPATH 定义过两次,两处都要改 |
| P1-11 | 内核 meson 配置 `uuid=none`,`uuid-ossp` 从未构建 | ✅ **本轮修复**,`meson configure build -Duuid=e2fs` + 单独构建该 target,详见 §5.6 第 8 点。`babelfishpg_tsql` 硬依赖 `uuid-ossp` |

### P2 — MySQL 语义 gap(已记录在案,非回归)

| # | 问题 | 备注 |
|---|---|---|
| P2-1 | ⬜ **建议记录不修**:MySQL 字面量对字面量的排序规则发散 | 详见 [COLLATION_CLUSTER_SPEC.md](COLLATION_CLUSTER_SPEC.md) §2.5。**根因是 PG 排序规则推导规则,不是 bug**:PG 里字符串字面量本身无排序规则,从比较另一侧继承,所以 `col = 'lit'` **已经是对的**(等价于 MySQL coercibility 规则);只有"双方都不含列"才回退到数据库默认排序规则而发散(`'AbC' LIKE 'a%'` → 0,MySQL 为 1)。影响面比字面描述窄得多。**两个看似显然实为陷阱的方案**:改数据库默认排序规则会让全库正则报错且污染 PG/TDS;给字面量挂显式 COLLATE 会让目前正确的 `col='lit'` 因排序规则冲突报错 |
| P2-2 | ✅ **文档过期,早已修复**(2026-08-14 核实) | `bpchar_pg18.c` 的 `bpcharlike` 现已正确读 `PG_GET_COLLATION()` 并委托内核 `textlike`,函数注释明确写着 "The previous byte-wise matcher never consulted PG_GET_COLLATION()"。实测 CHAR 与 VARCHAR 的 `LIKE`/`=` 行为一致,均正确大小写不敏感。某次早期修复顺带解决但未回填文档 |
| P2-17 | ⛔ **不做(与 openHalo 上游一致)**:REGEXP / RLIKE 在所有 MySQL 字符串列上不可用 | 详见 [COLLATION_CLUSTER_SPEC.md](COLLATION_CLUSTER_SPEC.md) §2.1。实测 `WHERE v REGEXP 'Ali'` → `ERROR 1295: nondeterministic collations are not supported for regular expressions`(每个 MySQL 字符串列默认挂非确定性 `case_insensitive`,PG 正则引擎 `regc_pg_locale.c:262` 硬拒绝)。**但已核实 openHalo 源仓库有完全相同的 `~*` 映射且同样没有任何非确定性规避,即上游 openHalo 的 REGEXP 同样不可用** —— 非融合引入的回归,按"不超越上游"原则不做。技术分析(含已验证可行的修复路径)保留在规格文档里备查 |
| P2-18 | ⛔ **不做(与 openHalo 上游一致)**:MySQL DDL 绕过外键排序规则兼容性检查 | 详见 [COLLATION_CLUSTER_SPEC.md](COLLATION_CLUSTER_SPEC.md) §2.2。`mys_tablecmds.c` 缺失内核 `tablecmds.c:10584-10607` 的外键确定性检查,实测同一外键 psql 报错拒绝而 MySQL 协议静默创建成功。**但已核实 openHalo 源仓库的 `mys_tablecmds.c` 同样是 `get_collation_isdeterministic` 计数 0**,即上游本就缺失 —— 非融合引入的回归,按原则不做。⚠️ 这是数据完整性性质的问题,若将来上游修复或用户改变范围决策,应优先重启此项 |
| P2-19 | ✅ **已核实,风险降级为可控(2026-08-14)** | 与 openHalo 源(17448 行)做了全量 `diff`,融合树(17551 行)仅 13 处差异。**全部可解释,无遗漏的内核检查逻辑**:①头文件 include 差异 ②`mys_setval3_oid` 的本地 stub 被移除(融合整理)③`mys_transformAlterTableStmt`/`createAutoIncrementTriggerFunc`/`getCurrentNamespaceOid` 等改为经 `mys_parser_exports` vtable 间接调用(W4/W8 阶段既定的融合架构调整,非漂移)④融合树独有新增 `create_mysql_ctas_on_update_triggers`/`mys_ctas_post_hook`/`InitMysCtasHook`(约 110 行,`CREATE TABLE AS`场景下继承源列的 MySQL `ON UPDATE CURRENT_TIMESTAMP` 触发器)——**这一项本身超出了 openHalo 原有范围,是本项目组之前某次会话额外做的功能,不在本轮改动,仅记录供知悉** |
| P2-20 | ✅ **已实证核验并定案(2026-08-14)**:非确定性排序规则的性能代价,维持"设计取舍,不修" | 代码依据全部属实(`index.c:844` 拒 pattern_ops、`like_support.c:414`/`1072` 禁前缀优化)。**实测补全三点**(测试集群 EXPLAIN):①默认列(非确定性)建 `varchar_pattern_ops` 被拒(`ERROR 1295`);②前缀优化还有**第三道 PG 通用门槛**——范围约束路径要求索引排序规则为纯 C(`collate_is_c`),本集群默认 C.UTF-8 不满足,故**确定性列 + 普通 btree 也走不了 LIKE 前缀**(纯 PG 表同样如此,非 MySQL 层回归);③**现行可用出路**:确定性列 + `varchar_pattern_ops` 索引,`LIKE 'al%'` 实测 Index Scan;确定性列可经 MySQL DDL 的 `COLLATE <非_ci后缀>` 子句获得(P2-8 的"静默丢弃"行为恰好回落 PG 默认);非确定性列只能用显式范围条件(`v >= 'al' AND v < 'am'`,普通 btree 可走)或接受 Seq Scan。详见 [COLLATION_CLUSTER_SPEC.md](COLLATION_CLUSTER_SPEC.md) §1.2 |
| P2-3 | `texteq_mys.c` 的 `<=>` 同样忽略排序规则 | 已查明为 dead code(实际走 `mysql.is_and_eq()` → 普通 `=`),暂不需修 |
| P2-4 | `expr_typmod` / `adjust_numeric_result` / `detect_numeric_overflow` 三槽位保留 NULL | 已裁定 vtable 不是 DECIMAL 精度推导的正确接缝(执行器热路径,需完整 typmod 推导引擎),改用 mysql schema 运算符 |
| P2-5 | **`babelfishpg_common`/`babelfishpg_tsql` 的 `_PG_init()` 在 postmaster 阶段做目录访问会段错误** | ✅ **本轮修复**(见 §5.7)。仅在管理员误把 `babelfishpg_tsql`/`babelfishpg_common` 放进 `shared_preload_libraries`(标准配置不这样做,仅 `babelfishpg_tds` 需要 preload)时触发。已加 `process_shared_preload_libraries_in_progress` 守卫,跳过 `init_tcode_trans_tab()`(babelfishpg_common.c)和 `vector` 扩展探测(pl_handler.c)两处;`_PG_init()` 里还有 3 处同类调用(`init_tsql_coerce_hash_tab`/`init_tsql_datatype_precedence_hash_tab`/`init_tsql_cursor_hash_tab`/`init_catalog`)**未修**,因为标准配置走不到那条路径,继续修属于为不存在的场景过度设计 |
| P2-6 | ✅ **本轮修复**:`aux_mysql--1.3--1.4.sql` 里 7 处 `pg_catalog.concat`/`replace` 用了不带 `E` 前缀的 `'\n'`/`'\r'` 字符串字面量 | `standard_conforming_strings=on`(PG9.1+ 默认)下,不带 `E` 前缀的单引号字符串里反斜杠不转义,`'\n'` 是字面 2 字符而非换行符。影响两个函数:`show_create_table_impl` 的 5 处(`SHOW CREATE TABLE` 输出里全是字面 `\n`,不是真正换行)、`show_create_view_impl` 的 2 处(view 定义规范化时 `replace(definition, '\n', ' ')` 永远匹配不上真实换行符,是空操作)。已全部改成 `E'\n'`/`E'\r'`,`SHOW CREATE TABLE` 验证修复生效(输出恢复真正的多行格式) |
| P2-7 | ⛔ **不做(与 openHalo 上游一致,2026-08-14 核实)**:`SHOW CREATE VIEW` 的 DDL 重建逻辑根本性缺陷 | `show_create_view_impl` 用空格切词模拟 SQL 解析:只有列名是 `table.column` 两段式时才在列间插逗号,但 PG 对单表 view 通常存**不带前缀的裸列名**(`SELECT id, name FROM t`,无歧义时不加限定符),导致列名直接拼接无分隔符(`` `id``name` ``);`FROM` 之后的每个 token(表名/`WHERE`/运算符/括号表达式)全部走同一个"裸套反引号、无分隔符"分支,任何带 WHERE/JOIN/GROUP BY 的 view 输出都是语法错误的垃圾。**已用真实 mysql 客户端复现两种情况**:无 WHERE 的双列 view(`` select `id``name` from `users` ``)、带 WHERE 的单表 view(`` ...where``(balance``>``(0)::numeric) ``)。**项目自己的测试套件从未发现这个问题**——`t/mysql_compat/expected/35_routines_triggers.out` 的"标准答案"里 `SHOW CREATE VIEW` 那行本身就是这个 bug 的输出(同样的漏逗号、字面 `\n`),测试只断言了一个不相关的 `COUNT(*)=2`,从未真正校验过 DDL 文本内容是否是合法 SQL——是同一类"自动化绿灯掩盖真实 bug"问题(参见 §8"手工验证纪律"提到的 `LIKE`→`ILIKE` 先例)。修复需要重写这段 tokenizer,让它正确处理裸列名(始终按位置插逗号,而不是只在两段式时插)和任意 WHERE/JOIN/GROUP BY 子句(不能假设 FROM 后只有表名),工作量明显大于 P2-6 的一行转义修复,决定先记录、暂不投入。**上游比对(2026-08-14)**:`mysql.show_create_view_impl` 函数体与 openHalo 源 `aux_mysql--1.3--1.4.sql` **逐字符完全一致**,这就是 openHalo 自己的 tokenizer 实现方式,非融合回归,按原则不做 |
| P2-8 | ⛔ **不做(与 openHalo 上游一致,2026-08-14 核实)**:MySQL 列级显式 `COLLATE <name>` 子句被完全忽略 | 详见 [COLLATION_CLUSTER_SPEC.md](COLLATION_CLUSTER_SPEC.md) §2.3。精确隔离(见 §5.10):`CHARACTER SET` 单独用没问题,`COLLATE` 单独用或搭配 `CHARACTER SET` 都会让列的排序规则回退成 PG 的 `default`。根因是 `rectifySpecifiedColumnCollate()`(`mys_parse_utilcmd.c:1237`)只看名字后缀 `_ci`,不匹配则 `collClause = NULL` 静默丢弃。**已核实 openHalo 源仓库的同名函数字符级完全一致** —— 非融合引入的回归,按"不超越上游"原则不做 |
| P2-9 | ⛔ **不做(与 openHalo 上游一致)**:MySQL `CREATE TRIGGER`/`CREATE PROCEDURE`/`CREATE FUNCTION` 缺少过程式语言支持。已核实 openHalo 源仓库的 `routine_body_stmt: stmt \| ReturnStmt` 与本树**完全相同**,即上游本就没实现 MySQL 过程式语言,非融合回归。架构分析(含工作量参照:Babelfish T-SQL 的等价实现约 19000 行)保留在 [P2-9_PROCEDURAL_ANALYSIS.md](P2-9_PROCEDURAL_ANALYSIS.md) 备查,其中的"路线 B"建议已作废 | 见 §5.10。`CreateTrigStmt` 语法(`mys_gram.y:8792-8798`)的触发器体只接受单条裸 `InsertStmt`,没有 `BEGIN...END`/`SET`/`IF`/循环/游标;`CREATE PROCEDURE`/`FUNCTION` 映射到 PG 原生 SQL-standard `BEGIN ATOMIC...END`(纯顺序 SQL 语句,无 `DECLARE` 局部变量、无控制流、无游标、无 `HANDLER` 异常处理);`CALL` 不会把过程体内嵌套 `SELECT` 的结果集转发给客户端(与纯 PG `CALL` 语义一致,但和真实 MySQL 行为不同)。三者共同说明 MySQL 真正的存储过程语言目前没有实现,只有一层薄的语法糖接到 PG 原生能力上。**与之对比强烈**:同一轮测试里 T-SQL 侧的 `AFTER` 触发器(含 `inserted` 伪表)、存储过程嵌套调用 + `TRY/CATCH` + `ERROR_MESSAGE()`/`ERROR_PROCEDURE()` 全部正确工作(Babelfish 原有代码更成熟) |
| P2-10 | ✅ **已修复(2026-08-14)**。MySQL `SET SESSION TRANSACTION ISOLATION LEVEL` 曾完全静默失效,`@@transaction_isolation` 曾主动汇报假值。分析过程见 [P2-10_FIX_SPEC.md](P2-10_FIX_SPEC.md)(Opus 5 + max reasoning 分析,Sonnet 5 实施) | 根因是三层独立缺陷叠加:①写路径未接线(`mys_gram.y` 转成从未处理的自定义 GUC 名);②MySQL/PG 隔离级别字符串格式不匹配(附带修复了 `START TRANSACTION ISOLATION LEVEL x` 的既有硬报错 `ERROR 1210`);③读路径与真实 PG 状态完全脱节,`@@transaction_isolation` 曾从静态目录表读死值 `REPEATABLE-READ`,而集群实际运行在 `read committed`——**这一层的危害高于"SET 不生效"本身**,是主动误导。改动:`contrib/aux_mysql/src/mys_utility.c`(格式转换 + `SET SESSION TRANSACTION` 接线)、`src/backend/commands/mysql/mys_uservar.c`(读路径改为查真实 GUC `default_transaction_isolation` 并反向转换格式)。已用独立的 `current_setting()` 交叉验证 MySQL 侧的 `SET` 确实改变了 PG 后端状态,并确认会话级隔离(未泄漏到集群默认值)。`meson test` 四套件 13/13,PG/TDS 连接不受影响。裸 `SET TRANSACTION`(仅下一事务)和 `SET GLOBAL TRANSACTION` 有意未接线,归入下面的 P2-13/P2-14 |
| P2-13 | ⛔ **不做(2026-08-14 裁决,与 openHalo 上游持平)**:MySQL 裸 `SET TRANSACTION ISOLATION LEVEL x`("仅下一事务"语义)未实现 | openHalo 上游同样只存自有变量表、从不影响 PG 后端,按用户定下的"兼容功能与 openHalo/tds 持平即可"口径不实现。实测现状(2026-08-14):裸 `SET TRANSACTION` 对后端是 no-op,而读路径因 P2-10 已改读真实 GUC,`@@transaction_isolation` 汇报真实值、不再误导。与已生效的 `SET SESSION TRANSACTION`(P2-10,超出上游但已验收)的不对称是**已接受的最终状态**。详见 P2-10_FIX_SPEC.md §5 项 A |
| P2-14 | ⛔ **不做(2026-08-14 裁决,与 openHalo 上游持平)**:MySQL `SET GLOBAL TRANSACTION ISOLATION LEVEL x` 未实现 | openHalo 上游同样只存自有变量表、从不影响 PG 后端,按持平口径不实现。实测现状(2026-08-14):`@@global.transaction_isolation` 读 MySQL 自有全局存储(种子 `REPEATABLE-READ`),与"GLOBAL 未接线"自洽(`mys_uservar.c` 注释已说明);`SHOW default_transaction_isolation` 证实 PG 侧从未被改变。详见 P2-10_FIX_SPEC.md §5 项 B |
| P2-15 | ⛔ **不做(与 openHalo 上游一致,2026-08-14 核实)**:MySQL `SHOW VARIABLES` 全部返回空值 | 详见 [P2-15_SHOW_VARIABLES_SPEC.md](P2-15_SHOW_VARIABLES_SPEC.md)。视图 `mys_informa_schema.empty_session_variables`(`aux_mysql--1.3--1.4.sql:3531`)对所有变量硬编码 `''::varchar(1024) as value`;`getSystemVariableValueForShow()` 定义了但**从未被任何代码调用**(死代码)。**已核实该视图定义、`SHOW VARIABLES` 语法翻译逻辑(`mys_gram.y` 8 条规则)、`getSystemVariableValueForShow()` 死代码状态三处均与 openHalo 源仓库字符级一致**,且该文件是提交 `bcee6d44b5` 从 openHalo 原样导入、未经修改 —— 上游本身就是"用空 value 占位视图应付 SHOW VARIABLES"的设计,非融合回归,按原则不做 |
| P2-16 | ✅ **已解决(2026-08-14)**。MySQL 连接默认隔离级别按会话所属兼容模式设置,实例级默认不变 | 关键澄清(用户指出,原判断有误):方案不是"改 `postgresql.conf` 的 `default_transaction_isolation`"(那样会污染 PG/TDS,因为是三协议共用的单一集群级 GUC),而是利用 openHalo vtable 架构里已有的 `ProtocolRoutine.session_initialize` 每连接钩子——MySQL 专属的 `mysql_session_initialize()`(`contrib/aux_mysql/src/mysql_protocol.c`)在会话建立时用 `set_config_option(..., PGC_S_SESSION, ...)` 把**当前会话**的 `default_transaction_isolation` 设为 `repeatable read`(MySQL 真实默认,InnoDB 5.x 至 8.4 未变),不碰共享 GUC。TDS 侧不需要改代码——**实测确认**真实 SQL Server 默认就是 `READ COMMITTED`(`sys.dm_exec_sessions.transaction_isolation_level`,已用真实 TDS 连接验证),与实例默认天然一致。三边独立验证:MySQL 新连接不显式 `SET` 即为 `REPEATABLE-READ`(`current_setting()` 交叉验证过是真 GUC,非模拟层);两个独立 PG 集群的 `SHOW default_transaction_isolation` 均为 `read committed`,不受影响;TDS 仍为 `ReadCommitted`,不受影响;`SET SESSION TRANSACTION ISOLATION LEVEL`(P2-10)显式覆盖仍正常工作。`meson test` 四套件 13/13 |
| P2-11 | ⛔ **不做(与 Babelfish 上游一致,2026-08-14 核实)**:TDS `TOP N WITH TIES` 完全不可用 | 见 §5.10。报错 `Target list missing from TOP clause`(`gram-tsql-rule.y` 的 `simple_select` 规则,`$3 != NULL && $4 == NULL` 触发)。**已核实 `babelfish_extensions` 仓库(`BABEL_6_0_STABLE` 分支,`git status` 干净、无本地改动,落后 upstream 官方仓库仅 1 个不相关 commit)的 `tsql_top_clause` 产生式(6 条:`TOP(expr)`/`TOP n`/`TOP(subquery)`/两种 `PERCENT` 变体)本来就完全没有 `WITH TIES` 分支** —— `WITH` 出现在这个位置时被规约成空产生式,才触发人工写的报错。这是 Babelfish 官方从未实现的语法,非融合回归,按"不超越上游"原则不做 |
| P2-12 | ⛔ **不做(与 Babelfish 上游一致,2026-08-14 核实)**:TDS 列内联 `FOREIGN KEY REFERENCES` 简写语法不支持 | `customer_id INT FOREIGN KEY REFERENCES t(id)` 这种列级简写报语法错误;表级 `CONSTRAINT fk_name FOREIGN KEY (col) REFERENCES t(col)` 完全正常。**已核实 `babelfish_extensions` 仓库的 `tsql_ColConstraintElem`(T-SQL 列约束扩展产生式)只有 `UNIQUE`/`PRIMARY KEY`/`IDENTITY`/`ROWGUIDCOL`/`NOT FOR REPLICATION` 五种,完全没有 `FOREIGN KEY REFERENCES` 分支**,同一份文件无本地修改。上游本就没写这个简写语法,非融合回归,按原则不做 |

### P3 — 架构演进

| # | 问题 | 备注 |
|---|---|---|
| P3-1 | TDS 三个 vtable 槽位仍为空 | ✅ **低风险整合已完成(2026-08-14 晚,`14970f4e7e` + `039f9407e`)**。TDS 注册完整 `ProtocolRoutine`(init/startup_exchange/mainfunc/read_command/process_command/authenticate/create_dest_receiver/end_command/send_ready_for_query/send_error/report_parameter_status/direct_ssl_handshake + 防御性 no-op 成员),**`ProtocolExtensionConfig` 机制整体删除**(内核 ListenConfig、default_protocol_config、libpq_* 包装、全部 fn_* 回退)。过程中发现并修复一个真实回归:`PerformAuthentication` 尾部负责清除的 `ClientAuthInProgress` 在 vtable 直调路径下不再被清除,导致 MySQL 连接上 NOTICE/WARNING 被静默压制、warning_count 恒为 0——已在 InitPostgres 的 vtable 认证路径补上。实测:三协议集群两种 preload 顺序、五套件基线 18/18 OK。**parser/adtext 槽位仍未注册**(高风险部分,按 §7 第四步建议不动)。**组 B 已铺开完成(2026-08-14 晚,`75219d366` 试点 + `0584cbc3f` 全量)**:13 个 W5 槽位中 **12 个已迁入 `tsql_adtext` vtable**(expr_typmod/validate_var_datatype_scale/param_collation/default_collation/strpos_non_deterministic/replace_non_deterministic/adjust_numeric_result/detect_numeric_overflow/identity_datatype/sequence_datatype/sortby_nulls/unique_constraint_nulls_ordering),对应 12 个全局 hook 的声明/安装/卸载/save-chain 全部删除;结构体与注册放在 hooks.c 的 InstallExtendedHooks()(实现全在该文件,免去跨文件 static 问题;identity/sequence 两个 map 函数在 pl_handler.c,改为 extern 并在 pltsql.h 声明)。**仅 `coalesce_typmod` 刻意保留 hook**:vtable 槽不按 cexpr->tsql_is_null 门控,而实现不复制标准 PG 答案,迁移会改变 TDS 连接上普通 COALESCE 的行为。实测:IDENTITY/SEQUENCE AS/ORDER BY NULLS 语义/DECIMAL(38,5)/CHARINDEX/REPLACE 全对,PG/MySQL 不受影响,基线 18/18 OK。**12/13 槽位迁移完成,仅剩 coalesce_typmod 一项记录为不迁**。**A1 阶段一试点已完成(2026-08-14 晚,`c5f87dc02a` + `29b251591`)**:TDS 连接 DDL 分派迁入 vtable process_utility 槽(内核新增 `SetProtocolRoutineProcessUtility` 槽位级 API + 注册表可变拷贝),实测插桩日志证实 TDS 批次 DDL 全部走 vtable 分支;ProcessUtility_hook 安装**保留**(核实 `bbf_ProcessUtility` 承担 PG 连接的视图定义保护/ALTER OWNER 限制/DROP 处理,A1 文档原"删安装"前提不成立);PG 对 T-SQL 视图的 REPLACE 仍被正确阻止,三协议语义与基线 18/18 全绿不变。详见 [P3-A1_PROCESS_UTILITY_MIGRATION.md](P3-A1_PROCESS_UTILITY_MIGRATION.md) |
| P3-2 | **认证体系无法真正三协议共存**(本轮发现) | ✅ **已完成**(不适用"不超越上游"原则,详见下方及 §7 第五步)。M1-M7 全部实施并通过 A1-A13 验收用例:同一角色同一明文口令可同时通过 5432(scram)、3306(mysql_native_password)登录。**收口补充 `f9b664c2b1`(2026-08-14)**:补上 M5 撤回的 `pg_hba_file_rules` `protocol` 展示列、补 catversion bump(rolpasswordext 提交漏改)、修复两处被 P3-2 打破的自动化基线(misc_sanity.out / 005_mysql_compat.pl)。方案见 [P3-2_AUTH_SPEC.md](P3-2_AUTH_SPEC.md) |
| P3-3 | **`listen_init_hook` 是单指针而非注册表** | ✅ **已根治(2026-08-14 下午,`40402d7126` + `f367417ac`)**。方案选型见 [COLLATION_STRATEGY_COMPARISON.md](COLLATION_STRATEGY_COMPARISON.md) 同批讨论之外的正文下方 §7 第四步末尾——最终采用**按方言槽位注册**(在既有 `CompatibilityRoutine` 注册表上加 `listen_init` 槽 + `RegisterListenInitRoutine()`),而非通用回调列表:按协议种类天然有序、每方言一槽防重复注册、复用架构既有概念。旧 `listen_init_hook` 保留为兼容垫片(仍会被调用一次,无需再 save/chain)。实测:两种 `shared_preload_libraries` 顺序下 5432/13306/1433 均正常监听、三协议登录全过。**垫片已于 2026-08-14 晚彻底删除**(`e4873a9712`:全局变量、typedef、postmaster 调用点全清,两仓库零残留引用),vtable 契约注释同步完善(no-op vs NULL 语义、多语句字段合法 NULL) |
| P3-4 | `openHalo-MySQL兼容契约裁决表.md` 已过时 | ✅ **已处理(2026-08-14)**:核实该文件并不存在(引用落空),真正的过时声明在 [openHalo-MySQL兼容架构优化方案.md](openHalo-MySQL兼容架构优化方案.md) 第 91/113 行("ProtocolRoutine 23/23 全活")——已按论文自身批注风格加 P3-4 更正:`mainfunc` 活调用点是融合轮次 `4a43654840` 才补上(openHalo 源上并非严格全活);P3-1 后融合树 ProtocolRoutine 扩至 26 个括号成员(+accept/close/direct_ssl_handshake)、`ProtocolExtensionConfig` 19 字段机制整体退役,实测 28 个函数指针型成员全部有活调用点/读取点 |

**P3-2 展开 —— 认证是当前"三协议并行"最实际的一处不完整:**

`pg_hba.conf` 是全局的,无法按 `protocol_kind` 区分认证方法;而密码在 `pg_authid` 里**每个角色只能存一种格式**。三者互斥:

| 协议 | 需要的 HBA 方法 | 需要的密码格式 |
|---|---|---|
| PG (psql) | `scram-sha-256` / `md5` | `SCRAM-SHA-256$...` / `md5...` |
| MySQL | **只能 `md5`**(其余 FATAL) | **只能 `mysql_native_password:...`** |
| TDS | Babelfish 自有路径 | SCRAM / md5 |

实测后果:把 `postgres` 的密码改成 `mysql_native_password` 格式后,mysql 客户端能登录,但 **psql 的 md5 认证立刻失败**(只剩 Unix socket 的 `trust` 可用)。也就是说**同一个角色目前无法同时通过密码认证使用 PG 和 MySQL 两种协议**。

这不是 bug,是"HBA 按 host/user 匹配、密码按角色单值存储"这套模型与多协议并行的结构性冲突。可能的方向(需专门设计):
- HBA 增加按 `protocol_kind` 匹配的能力,让同一角色在不同协议上走不同认证方法;
- 或允许角色持有多份不同格式的 verifier(类似 MySQL 自己的多 auth plugin),由协议层各取所需。

在此之前,三协议共存集群的现实做法是**按协议分角色**(如 `mysql_app` 用 native password、`pg_app` 用 SCRAM)。

---

## 7. 后续路线

### 7.0 执行优先级(用户指定,2026-08-14)

后续工作按三个阶段推进,**阶段间不并行,前一阶段收口才进入下一阶段**:

1. **阶段一:三协议并行架构** —— ✅ **已完成**(§0)。PG/MySQL/TDS 三协议同一实例并行监听、可用,已实测验证。
2. **阶段二:SQL 全链路逐层走通** —— ✅ **已完成**(§5.9)。解析器(Parser)→ 优化器(Optimizer)→ 执行器(Executor)→ 存储(Storage),PG(基线,内核 regress 套件 232/232)+ MySQL + TDS 三种协议逐层验证,10 个任务全部完成。优化器/执行器核心能力(JOIN/子查询/CTE/窗口函数/索引/代价模型)两个方言都扎实;过程式语言(触发器/存储过程)成熟度差距明显(T-SQL 完整,MySQL 基本未实现);发现 5 个新 bug(P2-8~P2-12),按下面的原则全部只记录不修。
3. **阶段三:修复阶段二发现的 bug** —— 当前所在阶段。不是发现一个修一个,修复时有**两条**硬约束:

   > **约束二(2026-08-14 新增,范围原则):若某兼容特性 openHalo 和 Babelfish 上游本身也没实现,则不做修复和实现,保持与上游兼容特性一致即可。**
   >
   > 关键判别:**融合引入的回归**(必须修) vs **上游本就没有的特性**(不做)。写实施规格**之前**就要做上游比对(openHalo 源在 `/home/hlv/openhalo-update/postgres`,Babelfish 在 `babelfish_extensions`),不要等分析完才发现白做。已按此原则否决 P2-9 / P2-17 / P2-18 / P2-8 / P2-15 五项(均与 openHalo 逐字一致)。

   > ✅ **上游比对已补全(2026-08-14)**:
   > - **P2-8**:`rectifySpecifiedColumnCollate()` 与 openHalo 源字符级一致 → ⛔ 不做
   > - **P2-15**:`empty_session_variables` 视图、`SHOW VARIABLES` 语法翻译、`getSystemVariableValueForShow()` 死代码状态三处均与 openHalo 源一致(该文件系 `bcee6d44b5` 从 openHalo 原样导入)→ ⛔ 不做
   > - **P3-2**:**不适用本原则**,应该做。这不是方言兼容特性缺失,而是三协议融合本身产生的新场景(openHalo/Babelfish 各自都不需要同一账户横跨三协议认证),根因虽继承自 openHalo 但冲突本身从未在上游暴露过。核实过程中发现并修正了 `P3-2_AUTH_SPEC.md` 里一处影响设计的事实错误:`babelfishpg_tds` 源码其实就在 `babelfish_extensions` 仓库,且 TDS 认证直接复用 PG 原生密码格式,不需要专属 verifier 设计,方案因此被简化。

   约束一(数据格式冲突):

   > **三种兼容模式必须互不干扰。如果同一份数据在三种协议下的数据格式存在冲突(类型表示、排序规则、精度语义等),不强求单一统一格式,而是在实例内为冲突的协议各自创建一份该数据的兼容格式副本,用副本解决格式冲突,而不是让一种协议的格式要求牺牲另一种协议的正确性。**

   这条原则的实际含义:比如同一张表被 MySQL 和 T-SQL 会话共同访问,若某列的类型/精度/排序规则语义两边要求不同(类似已经发现的 MySQL `case_insensitive` vs T-SQL 排序规则差异),解决方案不是选边站(逼另一边将就),而是允许该数据在不同协议视角下有各自的物化副本(如 dialect-specific 的类型转换层、视图或独立列),用空间换正确性,三方言各自看到自己应得的语义。

**本文档记录的 P2-6~P2-16 等已发现 bug** 遵循这个顺序:阶段二完成前不批量投入修复,仅记录。已经顺手修的 P2-6(`E` 前缀转义)例外,因为是一行级别的低风险修复,顺带在测试过程中处理,不算违反这个顺序。P2-8/P2-9/P2-11/P2-12/P2-13/P2-14/P2-15 七项均已按"与上游持平"口径裁决为 ⛔ 不做(其中 P2-13/P2-14 于 2026-08-14 终裁);P2-10、P2-16 已在阶段三修复。

---

### 第一步:W8 收口 —— ✅ 已完成

1. ✅ 修 TDS `listen_init_hook` 断链,三协议同时监听验证通过(§5.4)
2. ✅ MySQL 客户端回归通过(§5.5)
3. ✅ `meson test` 四套件 13/13(P0-2)
4. ✅ tsql 端到端回归,真实 TDS 线协议(P0-3,§5.7)
5. ⬜ **待办**:提交 `babelfish_extensions` 的改动(现共 **5 处**代码 + 2 处 Makefile,清单见 §9)、`postgresql_modified_for_babelfish` 的测试规范化改动(P1-6)

### 第二步:构建系统清理 —— 大部分已完成

6. ✅ 修两个 Makefile 的重复 include(P1-1)
7. ✅ 装 JRE + 源码编译匹配版本的 ANTLR runtime,打通 `babelfishpg_tsql` 构建(P1-4,§5.6)
8. ✅ OpenSSL 特性宏改为代码级派生(`tds_secure.h`,P1-2,`3a900828f`)
9. ✅ meson 内核构建补 `CXX` 探测(P1-8,`afb1548a8a`);顺带根治 P1-3(libxml 头路径进 PGXS CPPFLAGS)
10. ✅ 模块命名统一为未版本化,符号链接删除(P1-10,`3a900828f`)
11. ✅ **四扩展零手动参数联合构建走通,固化为 `babelfish_extensions/build-all.sh`**(`4f8842f03`,已实测端到端);另修复 tsql Makefile 的 `cmake` 未定义默认值

### 第三步:MySQL 排序规则策略(P2-1 / P2-2)

**✅ 已完成对照裁决(2026-08-14 下午)**:应要求做了 openHalo 源与本融合树的逐点对照,结论写入 [COLLATION_STRATEGY_COMPARISON.md](COLLATION_STRATEGY_COMPARISON.md)——openHalo 的策略是"全库单一非确定性排序规则 + 完全依赖 PG 内核推导,无任何会话/库级排序规则状态",融合树与之**同构**(11 项中 9 项逐字符一致),仅有的两处分歧(LIKE→`~~`、bpcharlike 委托内核)都是融合轮次已验收的上游缺陷修复。**因此不存在需要追赶的差距**,P2-1/P2-20 按记录不修,原计划的独立设计文档不再需要。

(以下为原待设计描述,已由对照结论替代:字面量默认排序规则从哪来,bpcharlike 是否委托内核 textlike——后者实测早已修复,P2-2。)

### 第四步:TDS 入 vtable(P3-1 / P3-3)——`ProtocolExtensionConfig` 与 `ProtocolRoutine` 整合分析

**✅ 低风险部分已实施完成(2026-08-14 晚,`14970f4e7e` + `039f9407e`),本节的字段比对表即实施方案。** 最终形态与本节建议一致:16 个字段并入 `ProtocolRoutine`(含新增 accept/close/direct_ssl_handshake),TDS 注册完整 vtable,`pe_config`/`ListenConfig`/`libpq_*` 全部删除;高风险部分(parser_routine/process_utility/多语句批次)按建议保留 NULL 不动。实施中修正了一处本节未覆盖的细节:`fn_init↔init` 并非"签名一致直接可并"——fn_init(ClientSocket*)->Port* 的 Port 创建职责由 pq_init() 承接(现在所有连接统一走 pq_init + AssignProtocolRoutine),fn_init 里的 TDS 专属状态初始化(TdsClientInit/pltsql rendezvous/hooks)转为 vtable init(Port*) 钩子。

#### 背景:两套并行的协议扩展机制

当前架构里,协议层扩展点实际上有**两条独立的注册路径**:

- **openHalo vtable**(`ProtocolRoutine`,`src/include/postmaster/protocol_routine.h`):按 `CompatibilityProtocolKind` 索引的静态数组 `compatibility_routines[COMPAT_PROTOCOL_KIND_MAX]`(见 §3.1/§3.2),MySQL 通过它注册。
- **Babelfish 自己的 `ProtocolExtensionConfig`**(`pe_config`,`src/include/libpq/libpq-be.h:111`):TDS 用的是这一套,独立于上面那张表,由 `postmaster.c` 的 `ListenConfig[]` 单独分发。

两者字段高度重叠但不是同一套接口,这正是 W1–W8 反复出现"TDS 走另一条路、每个分发点都要单独判断 NULL"这类修复的根源。以下是逐字段比对,评估两者是否能整合。

#### 逐字段比对

`ProtocolExtensionConfig` 共 19 个字段(`fn_accept` ... `fn_direct_ssl_handshake`),`ProtocolRoutine` 共约 20 个字段(`init` ... `authenticate`)。核对结果:

| 类别 | 字段 | 结论 |
|---|---|---|
| **完全对应**(9 个) | `fn_init`↔`init`、`fn_start`↔`startup_exchange`、`fn_mainfunc`↔`mainfunc`、`fn_send_message`↔`send_error`、`fn_send_cancel_key`↔`send_backend_key_data`、`fn_comm_reset`↔`comm_reset`、`fn_is_reading_msg`↔`is_reading_msg`、`fn_send_ready_for_query`↔`send_ready_for_query`、`fn_read_command`↔`read_command` | 签名一致,直接可并 |
| **近似对应,签名微调**(4 个) | `fn_authenticate`↔`authenticate`(多一个 out 参数)、`fn_end_command`↔`end_command`(多一个 `force_undecorated_output`)、`fn_report_param_status`↔`report_parameter_status`(仅 const 限定符)、`fn_process_command`↔`process_command`(vtable 版把命令字节和缓冲区显式传参,是更晚设计的接口) | 需要统一签名,但都是同一件事 |
| **同一概念,不同抽象层级**(1 组 4 字段) | `fn_printtup`/`fn_printtup_startup`/`fn_printtup_shutdown`/`fn_printtup_destroy` ↔ `create_dest_receiver` | pe 版直接镜像 `dest.c` 内部回调名;vtable 版是"返回一个完整 DestReceiver 对象"。整合需要重写 TDS 侧的实现,不只是改签名 |
| **表面缺口,实为历史包袱**(2 个) | `fn_accept`/`fn_close` | 已核实 TDS 的实现只是薄封装(`pe_accept`→`AcceptConnection()`,`pe_close`→`closesocket()`,均为内核通用函数的直接转发,无 TDS 特有逻辑)。vtable 没有这两个字段是因为 MySQL/PG 从不需要自定义 accept/close。**折进 vtable 成本接近零**,大概率永远是 NULL(走默认) |
| **真实协议特定缺口**(1 个) | `fn_direct_ssl_handshake` | TDS 的 PRELOGIN 包协商加密方式与 PG 的 `SSLRequest` 字节完全不同,这是真正的 TDS 专属钩子。可以作为 vtable 新增的第 20 个字段直接加,不难 |
| **vtable 独有,TDS 结构性用不了**(3 类) | `parser_routine`、`process_utility`、5 个多语句批次策略字段(`allow_multi_statements` 等) | 见下,这是真正的架构障碍 |

13 / 19 个字段可以直接或稍加改造地统一,真正的障碍集中在最后一类。

#### 三个真正的架构性障碍

这三类不是接口层面能补的,是**执行模型不兼容**:

1. **`parser_routine`**:vtable 假设"某处调用 `raw_parse()`,按 `MyCompatMode` 查表换方言解析器"——MySQL 正是这样接的(`PostgresMain()` 内的 raw-parse 调用点)。但 TDS 的 `fn_mainfunc`/`mainfunc` **整个替换掉了 `PostgresMain()`**,T-SQL 的 ANTLR 解析在 `babelfishpg_tsql` 自己的批处理循环深处调用,根本不存在一个"raw_parse 调用点"可以插。
2. **`process_utility`**:vtable 这个槽位对应"按连接方言查表分发 DDL",MySQL 走这条路。但 Babelfish 的 T-SQL DDL 分发从一开始就是走全局单例 `ProcessUtility_hook`(`4795343644` 修的那个优先级倒置问题,本质就是这两套机制打架的产物)。
3. **5 个多语句批次策略字段**:专为 MySQL 简单查询协议(一条消息里分号分隔多条语句,期待返回多个结果集)设计。TDS 的多语句批次走自己的 `TDS_SQLBATCH` token 结构,完全是另一套东西。

#### 建议路线:只做低风险整合,不碰 T-SQL 执行模型

1. **低风险部分**(13 个重叠字段 + `fn_accept`/`fn_close`/`fn_direct_ssl_handshake` 3 个新增字段):把这 16 个字段并进 `ProtocolRoutine`,统一签名(`fn_process_command`/`fn_printtup*` 需要重写 TDS 侧实现以适配 `create_dest_receiver` 的抽象层级),`postmaster.c` 的 `ListenConfig[]` 分发改成查 `compatibility_routines[kind].protocol` 而不是单独维护一张 `ProtocolExtensionConfig*` 表。这一步纯粹是消除两套并行注册机制的重复,行为不变,风险可控,能顺带清掉 protocol_routine.h 里那一大段"TDS 是例外,每个调用点都要判断 NULL"的注释和防御性代码。
2. **高风险部分**(`parser_routine`/`process_utility`/多语句批次字段):不建议动。要统一这三类,等价于把 `babelfishpg_tsql` 的整个执行模型(`PostgresMain()` 替换、DDL hook 分发)重写一遍,工作量接近重写这个子系统,而当前 `ProcessUtility_hook` 优先级已经修对(`4795343644`),继续留着不影响正确性。**vtable 应该正式承认这三类字段允许合法地为 NULL/不适用**——这本来就是当前 `parser`/`adtext` 槽位对 TDS 留空的既有约定,不是新增例外。

这个整合不阻塞已经达成的三协议并行目标,属于纯粹的代码质量/架构一致性投入,优先级低于 P2 的 MySQL 排序规则策略缺口。

### 第五步:认证体系(P3-2)

同一角色目前无法同时用密码认证走 PG 和 MySQL 两种协议,需要专门设计(HBA 按 protocol_kind 匹配,或角色持有多份 verifier)。

**上游比对结论(2026-08-14)**:P3-2 **不适用"不超越上游"否决原则,应该做**。这不是某个方言的兼容特性缺失,而是三协议并行架构本身带来的新问题——openHalo 只有单一 MySQL 兼容模式、Babelfish 只融合 PG+TDS,两者都从未面对"同一账户体系横跨三种协议认证"的场景。已核实根因(`get_role_password()` 无协议感知、MySQL 只认 `uaMD5`)在 openHalo 源里本来就有,只是从未暴露成冲突。

详细方案见 [P3-2_AUTH_SPEC.md](P3-2_AUTH_SPEC.md),已修正一处影响设计的事实错误:文档原以为 `babelfishpg_tds` 无源码、TDS 认证不在可实施范围,**实际源码就在 `babelfish_extensions` 仓库**,且读源码后发现 TDS 认证(`tdslogin.c` 的 `CheckAuthPassword()`)直接复用内核原生 `get_role_password()` + `plain_crypt_verify()`,**完全兼容 PG 的 SCRAM/MD5 密码格式,不需要任何 TDS 专属 verifier 设计**——真正的密码格式互斥只发生在 MySQL 一侧。这个发现简化了方案:`get_role_password_ext(kind=TDS, ...)` 可以直接复用 PG 分支,零额外设计成本。

**实施进度(2026-08-14)**:M1-M7 全部完成并实测验证。

- **M6(修 D4:MySQL 认证失败不调用 `ClientAuthentication_hook`)**:逐行核对 PG 原生 `ClientAuthentication()` 后发现规格建议的 4 处改动有 2 处判断错误——`uaReject`/`uaImplicitReject` 分支 PG 自己也不调用 hook,不应该加;只在"取不到密码"和"密码比对失败"两处加了 hook 调用。用 `auth_delay` 扩展(3000ms)端到端验证:密码错误 3.007s、用户不存在 3.009s(延迟生效),HBA reject 0.007s(符合"不该加"的判断,作为对照组)。
- **M7(修 D1 错误注释)**:已完成。
- **M5(HBA `protocol_mask`)**:核心过滤逻辑(`hba.h`/`hba.c` 三处改动)已完成,不需要 initdb。`pg_hba_file_rules` 视图展示新列这部分因为要改 `pg_proc.dat`(触发 catversion bump)已撤回,留给 M1 一起做。三协议交叉验证:PG(5432)和 TDS(1433)共用一条 `protocol="postgres,tds"` 规则登录成功,MySQL(13306)被独立的 `protocol=mysql reject` 规则拦下且未误落到其他行。顺带确认 TDS 认证(`tdslogin.c:1687`)确实调用 `hba_getauthmethod()`,`protocol_mask` 对 TDS 真实生效;也发现规格 §4.2 示例本身漏了引号(`protocol=postgres,tds` 会被 tokenizer 按逗号拆分,正确写法是 `protocol="postgres,tds"`),以及 TDS 不支持 `scram-sha-256`,只认 `md5`/`password`/`trust`/`gss`/`oauth`。
- **M1(`pg_authid` 加 `rolpasswordext` 列)**:已完成,用户同意后执行了 initdb(旧数据目录备份为 `/tmp/w8testdata.pre_rolpasswordext_bak`)。规格 §3.2 一处"推断"被证伪:genbki **不会**自动给 `.dat` 里缺失的尾部可空列补 `_null_`,17 条记录都要手动补全,否则编译报 `missing values for field(s)`。
- **M2(`get_role_password_ext()`)**:已完成,且实现比规格表格更精确——按上游比对更正,`kind == COMPAT_PROTOCOL_TDS` 和 `POSTGRES` 走同一分支(只读 `rolpassword`),不是"同 MySQL 规则换类型"。只有 MySQL 才走 `rolpasswordext` 优先、`rolpassword` 类型匹配才回落的双源查找,并实现 IV-1/IV-2/IV-3 的 fail-closed 校验。
- **M3(`user.c` 写入分流)**:已完成,CREATE/ALTER ROLE 都按加密后的实际类型(而非 `password_encryption` GUC)分流,`PASSWORD NULL` 改为两列全清。发现并修正一处不安全写法:读旧 `rolpasswordext` 值最初用 `SysCacheGetAttr(AUTHNAME, tuple, ...)`,但 `get_rolespec_tuple()` 在 `CURRENT_USER` 场景下返回的是 `AUTHOID` 缓存的 tuple,改用文件里 `RenameRole` 已经在用的 `heap_getattr()` 惯用法。
- **M4(`mysql_auth.c` 接线)**:已完成。
- **A1-A13 验收用例**:全部通过。核心目标达成——同一角色 `alice`,同一明文密码 `p`,`psql`(5432,scram-sha-256)与 `mysql`(3306,mysql_native_password)都能登录;`DROP ROLE` 重建同名角色旧口令失效;`PASSWORD NULL` 两协议同时失效;手工写入畸形 `rolpasswordext` 触发 fail-closed 拒绝且有日志;`pg_shadow`/`pg_roles` 不暴露新列;TDS 认证路径回归正常(认证本身通过,只是测试环境未初始化 `tsqldb` 数据库,不属于本次范围)。
- **收口提交 `f9b664c2b1`(2026-08-14 下午)**:①补上 M5 撤回的 `pg_hba_file_rules` `protocol` 展示列(`hbafuncs.c` + `pg_proc.dat` 签名 +1 列,默认规则渲染为 `postgres,mysql,tds`,解析错误行渲染 NULL),client-auth.sgml 补 `protocol=` 选项文档;②**补 catversion bump(`202506291`→`202506292`)**——`992100e923` 加了 `rolpasswordext` 列却漏改 `catversion.h`,旧数据目录将无法被启动检测拒绝,属于必须补的真 bug;③修复两处被 P3-2 打破的自动化基线(`regress/expected/misc_sanity.out` 补新列、`aux_mysql/t/005_mysql_compat.pl` 改为断言新存储契约),`004_file_inclusion.pl` 与 `rules.out` 跟随新列。实测:四套件 13/13 重跑全绿;全新三协议集群(`/home/hlv/openhalo-update/testcluster`,PG 5432 / MySQL 13306 / TDS 1433)验证 `protocol=` 强制力(MySQL reject 规则只拦 13306,psql 与 tsql 不受影响)、双 verifier 登录(scram + mysql_native_password)与 TDS md5 登录(`@@VERSION` 正常返回)。

---

## 8. 验证基线与方法

### 自动化基线

```bash
cd /home/hlv/openhalo-update/postgresql_modified_for_babelfish
PERL5LIB=/home/hlv/perl5/lib/perl5 meson test -C build --suite setup --suite postmaster --suite aux_mysql --suite regress
```

⚠ **`PERL5LIB` 不能省**(见 P1-6),否则 8 个 TAP 测试会以 `exit status 2` 假失败。

当前基线 **13/13**(2026-08-13 首测;2026-08-14 复测,`f9b664c2b1` 修复了 P3-2 打破的两处基线后重跑全绿):

| 测试 | 子测试 |
|---|---|
| `postmaster/001_basic` / `002_connection_limits` / `003_start_stop` | 9 / 30 / 25 |
| `postmaster/004_mysql_protocol` | **459** |
| `postmaster/005_mysql_listener_stability` | 25 |
| `aux_mysql/005_mysql_compat` | 5(P3-2 后新增 rolpasswordext 断言) |
| `aux_mysql/006_pg_dump_restore` | 18 |
| `aux_mysql/007_mysql_parallel` | 8 |
| `aux_mysql/regress` | 1 |
| `regress/regress` | **232** |

### 手工验证纪律(重要)

**自动化套件绿 ≠ 功能正确。** `LIKE` → `ILIKE` 那个 bug 就是套件全绿、真实 `mysql` CLI 才暴露的(套件覆盖了 C 函数的正确性,没覆盖真实 LIKE 语法实际分派到哪个运算符)。每轮改动应同时用真实 `mysql` CLI 与真实 `tsql` CLI 做端到端确认。

### 代码修改后如何让验证集群生效(2026-08-14 固化)

**不需要重新 initdb,除非 catversion 变化。** 常规流程:

```bash
# 1) 一次构建装好内核 + 四扩展(零手动参数)
cd /home/hlv/openhalo-update/babelfish_extensions && ./build-all.sh
# 2) 重启集群加载新二进制(数据目录原样保留)
/home/hlv/openhalo-update/postgresql_modified_for_babelfish/inst/bin/pg_ctl -D /home/hlv/openhalo-update/testcluster restart
```

只有改了 catalog 布局(`pg_*.dat` 加列等,伴随 catversion bump)才必须重新 `initdb` 重建数据目录。只改了 babelfish 扩展(.so)时,`build-all.sh` 的内核部分自动 no-op,只重编扩展。旧 scratchpad 集群(5433,catversion 前二进制)已于 2026-08-14 清理停掉。

### 共存集群配置

**⚠ 不要用 `initdb -m mysql` 搭测试集群**(用户 2026-08-14 明确指示)。`initdb -m mysql`(`src/bin/initdb/initdb.c:1439-1455`)只是早期单方言隔离测试用的便捷入口:它把 `shared_preload_libraries` 设成 `'mysql_parser, mysm, aux_mysql'`,**不包含 `babelfishpg_tds`**,也不配置 TDS 端口——只实现三种兼容模式里的一种。这不代表最终架构,只是历史遗留的临时方案。

最终目标架构是三协议同时可用,标准做法是先 `initdb`(不带 `-m`),再手工把下面的配置写进 `postgresql.conf`:

```conf
shared_preload_libraries = 'mysql_parser, mysm, aux_mysql, babelfishpg_tds'
babelfishpg_tds.port = 1433
babelfishpg_tds.listen_addresses = '127.0.0.1'
mysql_listener_on = true
mysql_port = 23306   # 注意:本机系统级 mysql.service 占用 3306/33060,勿用(除非用户已手动 systemctl stop mysql)
```

注意 `shared_preload_libraries` 的顺序敏感性已由 `f386c032b6`(`listen_init_hook` 链式调用)消除,但改动该顺序后仍建议确认两个监听端口都正常 bind。

`database_compat_mode` 这个 GUC(`initdb -m` 设置的就是它)在三协议并行架构下作用很小——真实客户端连接的方言由 `protocol_kind`(连接落在哪个监听端口)决定,不受这个 GUC 影响;它只影响没有客户端 Port 的进程(autovacuum、后台 worker)的默认方言,详见 §3.2。

---

## 9. 提交记录(2026-08-14,已 commit + push)

所有改动已提交并推送到各自的 `origin`(用户自己的 GitHub fork,非上游共享仓库)。提交信息不含任何 AI/Claude 署名(见 `.claude` 记忆 `feedback-no-ai-attribution`)。

### `babelfish_extensions`(分支 `BABEL_6_0_STABLE`)

| 提交 | 内容 | 归属 |
|---|---|---|
| `f29846ee0` `fusion: stamp TDS listeners with protocol_kind, fix listen_init_hook chain break` | `support_funcs.c`(`listen_add_socket()`→`listen_add_protocol_socket(..., COMPAT_PROTOCOL_TDS)`)+ `tds_srv.c`(`pe_tds_init()` 补 `protocol_kind` 传递,`pe_listen_init()` 补 chain 调用) | W8 + §5.4 |
| `1a7986c10` `build: fix duplicate PGXS include, TSQLSRC ordering, ANTLR4 GCC13 warning` | `babelfishpg_common/Makefile` + `babelfishpg_tsql/Makefile` | P1-1/P1-7/P1-9 |
| `587ac1617` `fusion: guard postmaster-time catalog access in _PG_init, avoid segfault` | `babelfishpg_common.c` + `pl_handler.c` 的 `process_shared_preload_libraries_in_progress` 守卫 | P2-5,§5.7 |
| `3a900828f` `build: derive OpenSSL 1.1+ feature macros from headers, unify module naming` | `tds_secure.h` 按 `OPENSSL_VERSION_NUMBER` 定义两个宏;common/tsql 的 `MODULEPATH`/`MODULE_big` 统一未版本化(符号链接删除);tsql 的 `cmake ?= cmake` | P1-2 + P1-10 |
| `f367417ac` `fusion: register TDS listener through per-dialect registry slot` | `tds_srv.c` 改用 `RegisterListenInitRoutine(COMPAT_PROTOCOL_TDS, pe_create_server_ports)`,删除 save/chain 与 `pe_listen_init` | P3-3 |
| `4f8842f03` `build: add zero-manual-step build-all.sh for the whole stack` | 一键构建内核 + 四扩展,消除全部手动 CPPFLAGS/CXX/符号链接步骤 | P1 收口 |
| `039f9407e` `fusion: register a full TDS ProtocolRoutine, drop pe_config` | `tds_srv.c` 注册完整 vtable(init 承接 TDS 状态初始化、process_command/authenticate/end_command 签名适配、printtup 四件套转 create_dest_receiver)+ `support_funcs.c` 的 listen_add_protocol_socket 调用更新 | P3-1 |
| `75219d366` `fusion: pilot-migrate identity-datatype hook into the TDS ADT vtable` | 首个 T-SQL 全局 hook 迁入 ADTExtMethod 槽位(tsql_adtext 注册 identity_datatype,移除 hook 安装/卸载与 save-chain) | P3-1 组 B 试点 |
| `29b251591` `fusion: wire T-SQL DDL dispatcher into the TDS ProtocolRoutine vtable slot (A1 phase-1 pilot)` | pl_handler.c 在 `_PG_init` 调 `SetProtocolRoutineProcessUtility(COMPAT_PROTOCOL_TDS, bbf_ProcessUtility)`(fini 置 NULL),TDS 连接 DDL 改走 vtable 分支;**ProcessUtility_hook 安装保留**(它还承载 PG 连接的保护逻辑);详见 [P3-A1_PROCESS_UTILITY_MIGRATION.md](P3-A1_PROCESS_UTILITY_MIGRATION.md) | P3-1 A1 阶段一试点 |

### `postgresql_modified_for_babelfish`(分支 `openhalo-fusion`)

| 提交 | 内容 | 归属 |
|---|---|---|
| `735b768adc` `fusion: fix mysql_compat suite portability and SHOW CREATE newline escaping` | `run_mysql_compat.sh`(DEFINER 规范化)+ `aux_mysql--1.3--1.4.sql`(7 处 `E` 前缀)+ 两个 golden `.out` 文件重新生成 | P1-6 + P2-6 |
| `59eeb3089e` `fusion: wire up MySQL transaction isolation level end to end` | `mys_utility.c` + `mys_uservar.c`(P2-10 三层修复)+ `mysql_protocol.c`(P2-16 会话默认隔离级别) | P2-10 + P2-16,详见 [P2-10_FIX_SPEC.md](P2-10_FIX_SPEC.md) |
| `04c17db8aa` `fusion: cross-protocol HBA matching and fix auth_delay hook gap on MySQL` | `hba.h`/`hba.c` 加 `protocol_mask` + `protocol=` 解析与 `check_hba()` 判定;`mysql_auth.c` 修 D4;`crypt.h` 注释修正 | P3-2 M5/M6/M7 |
| `992100e923` `fusion: let a role hold both a PG-native and a MySQL password verifier` | `pg_authid` 加 `rolpasswordext` 列(17 条 `.dat` 记录补 `_null_`)、`get_role_password_ext()`、`user.c` 写入分流、`mysql_auth.c` 接线 | P3-2 M1-M4 |
| `f9b664c2b1` `fusion: expose protocol in pg_hba_file_rules, bump catversion for rolpasswordext` | 视图 `protocol` 列(`hbafuncs.c`+`pg_proc.dat`)、**补 catversion bump**、两处自动化基线修复(`misc_sanity.out`/`005_mysql_compat.pl`)、`004_file_inclusion.pl`/`rules.out` 跟随、client-auth.sgml 文档 | P3-2 收口 |
| `afb1548a8a` `build: propagate CXX and libxml include path into PGXS Makefile.global` | meson pgxs 生成:LLVM 缺失时探测 c++/g++ 填 CXX;libxml 启用时经 pkg-config 把 `-I/usr/include/libxml2` 并入 CPPFLAGS | P1-3 + P1-8 |
| `40402d7126` `fusion: replace listen_init_hook chaining with per-dialect registry slot` | `CompatibilityRoutine` 加 `listen_init` 槽 + `RegisterListenInitRoutine()`;postmaster 按协议种类顺序调用,旧 hook 保留为兼容垫片;`aux_mysql_init.c` 迁移 | P3-3 |
| `14970f4e7e` `fusion: retire ProtocolExtensionConfig, resolve everything through ProtocolRoutine` | ProtocolRoutine 加 accept/close/direct_ssl_handshake;ListenConfig/default_protocol_config/libpq_* 包装与全部 fn_* 回退删除;所有连接统一走 pq_init;修复 ClientAuthInProgress 未清除导致 MySQL NOTICE/WARNING 被压制的问题 | P3-1 |
| `e4873a9712` `fusion: drop the listen_init_hook compatibility shim, document vtable NULL semantics` | 删除 listen_init_hook 全局变量/typedef/调用点(两注册者均已迁注册表,零残留引用);ProtocolRoutine 契约注释完善(no-op vs NULL、多语句字段合法 NULL) | P3-3 收尾 |
| `c5f87dc02a` `fusion: route T-SQL DDL dispatch through the TDS vtable slot (A1 phase-1 pilot)` | protocol_routine 注册表改内核自有可变拷贝 + `SetProtocolRoutineProcessUtility(kind, fn)` 槽位更新 API(tsql 晚于 tds 加载,须原地更新已解析的 Port 指针);utility.c 分派注释更新 | P3-1 A1 阶段一试点 |

> (meson 配置)`uuid=e2fs`:非源码改动,是 `meson configure` 的持久化状态(写入 `build/meson-private/`),**不随 git 走**,新 checkout 需要手动重新配置(P1-11)。
>
> `t/mysql_compat/expected/35_routines_triggers.out` 里 `SHOW CREATE VIEW` 那一行的**视图定义文本本身**(逗号缺失、反引号紧挨,如 `` `id``value_int``... ``)仍然是 P2-7 描述的 tokenizer bug 的输出——本轮只修了换行格式(P2-6),没有修 P2-7,不要把这处残留误当成"P2-7 也修好了"。

另外,`postgresql_modified_for_babelfish` 的 `openhalo-fusion` 分支此前有 28 个提交(`92ba169385`..`ca3e74b273`)带 `Co-Authored-By: Claude ...` trailer,已于 2026-08-14 用 `git filter-branch --msg-filter` 清除(只改消息,树哈希/作者/提交者/日期均核实未变),`force-with-lease` 推送到 `origin`。**上表所列 hash 均为重写后的实际值**(重写前旧 hash 见本文件历史版本,勿再引用)。

> `inst/` 是未入库的构建产物目录,不要提交。`build/` 同理。

## 10. 状态总结

**核心目标已达成并实测验证**:一个 PostgreSQL 18.3 实例上,PG(5432)/MySQL(13306)/TDS-T-SQL(1433)三种协议同时监听、同时可用,且三者对同一表达式各自返回正确但不同的方言语义(§0)。`meson test` 四套件 13/13,MySQL 与 T-SQL 均有真实客户端端到端验证。

**阶段三(bug 修复)已修复 2 项**:
- P2-10(MySQL 事务隔离级别设置静默失效,含主动误导的 `@@transaction_isolation` 假值),采用 Opus 5 深度分析 + Sonnet 5 实施的协作模式,规格见 [P2-10_FIX_SPEC.md](P2-10_FIX_SPEC.md)。修复中额外发现 3 项相关但有意排除在首轮外的问题(P2-13/P2-14/P2-15)。
- P2-16(MySQL 默认隔离级别按会话所属兼容模式设置,PG/TDS 实例级默认不受影响)——用户指出了"改共享 GUC 会污染 PG/TDS"这个关键问题,方案改为利用 openHalo vtable 架构里已有的 `ProtocolRoutine.session_initialize` 每连接钩子,不碰集群级配置。过程中还实测确认了真实 SQL Server 默认隔离级别是 `READ COMMITTED`(与 PG 一致,与 MySQL 的 `REPEATABLE READ` 不同)。

**§9 列出的所有改动已提交并推送**(2026-08-14,九个提交,两个仓库,详见 §9;`4c2ae987af` 为 P2-13/P2-14 终裁后的代码注释同步)。

### 最终状态(2026-08-14 目标收口)

用户定的目标口径"**三协议并行,pg+mysql+tds 功能正常,兼容功能与 openHalo/tds 持平即可**"已全部满足:

1. **三协议并行** ✅:同一 postmaster 上 5432/13306/1433 同时监听,§0 的 `5/2` 三语义(`2` / `2.5000` / T-SQL `2` 与 `CAST(... DECIMAL)/2=2.500000`)在同会话窗口交替实测复验通过。
2. **三兼容模式功能正常** ✅:PG(psql,scram)与 MySQL(mysql CLI,native_password,`SET SESSION TRANSACTION` 真正生效)与 TDS(tsql,md5,`@@VERSION` 正常)均有真实客户端端到端验证;`meson test` 五套件(setup/postmaster/aux_mysql/regress/authentication)20 测试 **18 OK / 0 Fail**(2 个环境性 skip),其中 regress 232、mysql_protocol 459 子测试全过。
3. **兼容功能与上游持平** ✅:全部遗留 gap(P2-8/9/11/12/13/14/15/17/18)均已做上游比对并裁决 ⛔;P2-10/P2-16/P3-2 等结构性新问题已修复;认证、HBA `protocol=`、`pg_hba_file_rules.protocol` 列均已落地。P3-2 已知限制(pg_dumpall 不导出 MySQL verifier 等)见上方"已知限制"块。

**2026-08-14 下午追加完成**(用户四项指示全部落地):①**P1 构建固化**(P1-2/P1-3/P1-5/P1-8/P1-10 全修复 + `build-all.sh` 零手动步骤一键构建,四扩展联合构建实测通过);②**P3-3 listen_init 注册表**(按方言槽位,两种 preload 顺序实测三协议全过);③**排序规则上游对照**([COLLATION_STRATEGY_COMPARISON.md](COLLATION_STRATEGY_COMPARISON.md):融合树与 openHalo 同构,无追赶差距,原设计文档免做);④**旧 scratchpad 集群已清理**,验证集群重建/重启流程已固化进 §8。

**遗留的明确非目标项**(不阻塞目标,文档记录在案):P3-4(旧契约文档过时标注)、P2-20 排序规则性能取舍(设计代价)。P3-1 低风险整合已于 2026-08-14 晚完成,高风险部分(parser/adtext 槽位)按 §7 第四步建议保留 NULL 不注册。

### 待决策(2026-08-14 更新)

1. **P1 构建系统清理 —— ✅ 已完成(2026-08-14 下午,用户拍板投入)**。P1-2/P1-3/P1-5/P1-8/P1-10 全部修复,四扩展零手动参数联合构建固化为 `babelfish_extensions/build-all.sh`(提交 `afb1548a8a`/`3a900828f`/`4f8842f03`)。剩余环境级前提(ANTLR 4.13.2 runtime、JRE、cmake 等)在脚本里做了存在性检查并给出提示。
2. **上游比对结论已全部补全**:P2-8/P2-9/P2-11/P2-12/P2-15/P2-17/P2-18 七项均已核实与 openHalo/Babelfish 上游逐字一致,判定⛔不做。**这里有个方法论教训**:P2-11/P2-12 此前被认为"根因已查清到具体代码行,常规实施即可,不需要额外深度分析"——事后证明这个判断是错的,"根因清楚"不等于"该修",两者是独立的两件事,上游比对必须每一项都做,不能因为看起来像"简单机械修复"就跳过。
3. **P3-2(跨协议认证)—— 已完成并收口(2026-08-14)**。M1-M7 全部落地(`04c17db8aa`、`992100e923`),收口提交 `f9b664c2b1` 补齐视图列与 catversion bump。⚠ **catversion 已 bump(`202506291`→`202506292`):任何建于 P3-2 之前的旧数据目录(包括 8-13 遗留的 scratchpad 集群)都无法再被新二进制启动,需重新 initdb——这是保护性行为,不是故障。** 本轮验证集群在 `/home/hlv/openhalo-update/testcluster`(PG 5432 / MySQL 13306 / TDS 1433,`pg_ctl -D ... stop` 可停)。
4. **P2-19、P2-7 已核实**:P2-19 风险降级为可控(13 处 diff 全部可解释,详见 §6 表格)。P2-7 与上游字符级一致,⛔不做。**P3-3 已于 2026-08-14 下午完成**(注册表),**P3-1 低风险整合已于 2026-08-14 晚完成**(见 §6),均实测验证。P3-4 是架构演进类,非紧急,未做。

5. **P2-13/P2-14 —— 已终裁(2026-08-14):⛔ 不做,接受不对称**。用户定下的目标口径"兼容功能只保持与 openHalo/tds 持平即可"直接回答了此前的待决问题:openHalo 上游对裸 `SET TRANSACTION`/`SET GLOBAL TRANSACTION` 同样是"只存自有变量表、从不影响 PG 后端",因此两项均按持平原则判定 ⛔ 不做,不再投入 pending 会话状态 / shmem 全局变量的设计。P2-10(SET SESSION 真正生效)作为"已越界但已验收"的既成事实保留,**两者并存的不对称是已接受的最终状态**(读路径已在 P2-10 改读真实 GUC,不再有任何误导性展示;`@@global.transaction_isolation` 读 MySQL 自有全局存储、与"GLOBAL 未接线"自洽)。§6 两行的状态已更新,P2-10_FIX_SPEC.md §5 项 A/B 已标注裁决。

### P3-2 已知限制(发布说明必须包含,来源 P3-2_AUTH_SPEC.md §6.2)

- **`pg_dumpall` 不会导出 `rolpasswordext`(MySQL 侧 verifier)**。备份恢复后 MySQL 口令不会随之恢复,需要重设:恢复后对每个需要走 3306 的角色执行 `SET password_encryption='mysql_native_password'; ALTER ROLE ... PASSWORD '...'`。在实现 G5(§7)之前,这是三协议集群备份恢复流程的**硬性人工步骤**。
- MySQL 侧 HBA 方法仍沿用 `md5` 名称承载 mysql_native_password 挑战应答(语义错位,G2 未做),配置 pg_hba.conf 时按此现状理解。
- TDS 不支持 `scram-sha-256`(只认 `md5`/`password`/`trust`/`gss`/`oauth`),给 TDS 使用的 HBA 规则要避开 scram。
### openHalo 整合审计(2026-08-14,用户要求验证 "openHalo mysql 行为是否全部整合")

**文件面**(openHalo @ cbd6642fb03 全量对比):61 文件逐字一致;65 文件差异全部可归类(路径重组 adapter/* 迁往 contrib/aux_mysql/src/、utils/adt/mysql/mys_* 迁往 ddsm/mysm/;集成适配 vtable 分派/adtext API/P3-2 认证/P2-6/warning 传递/错误文案/gram 与 PG18.3 对齐;融合独有新增 aux_mysql--1.5--1.6.sql、session_state、CTAS ON UPDATE、7 个测试工具)。**6+1 文件未导入**(mys_analyze.c、mys_parse_clause/expr/func/oper/agg.c、mys_parsenodes.h)= **架构决策**:openHalo 的并行 parse-analysis 层被 "语法直接产出标准 PG 节点(MysInsertStmt/MysUpdateStmt 在融合语法里是文法非终结符而非节点类型)+ PG 核心 analyze(parse_analyze_varparams_with_routine,postgres.c:938)+ 适配的 mys_expr_transform.c" 取代。

**行为面**(用户标准:openHalo 自有测试 SQL 集全过;干净集群 3306 + test/test 实跑):13 处差异全部归类 = 1) Note 1105 warning 恢复显示(约 10 文件,融合修掉 openHalo 的 warning 压制,见 P3-1 ClientAuthInProgress 修复) 2) 错误文案更贴 MySQL(80:Incorrect datetime value,错误码相同) 3) P2-6 真实换行(P2-7 SHOW CREATE VIEW 两方同残缺,持平) 4) SHOW PLUGINS 返回 Empty set(openHalo 是 Query OK) 5) GUC 命名差异(00_session 的 mysql.listener_on 在融合不存在,改名集成) 6) 刻意分歧 literal_like_case_insensitive 1→0(LIKE ~~ vs ILIKE,见 COLLATION_STRATEGY_COMPARISON) 7) **90_known_failures:openHalo 13 个已知失败 → 融合仅复现 2 个**(collation 哈希歧义 1105、多表 UPDATE 1064,与 openHalo 同错持平);11 个在融合不再报错(SHOW DATABASES LIKE/SHOW STATUS 三种/TO_BASE64/LOAD_FILE/CAST UNSIGNED/触发器体/PARTITION OF/LOW_PRIORITY/字面量加数字);4 个已知失败断言全部翻转为通过(json_dollar_path/named_lock_state/zero_length_char_varchar/information_schema_statistics)。

**结论**:openHalo MySQL 行为已整合且多数优于上游;未发现融合独有的功能缺失。三模式验证面:五套件基线 18/18(含 005_mysql_compat)+ 标准流程(新集群 3306)+ openHalo 套件 + TDS tsql 端到端 + A1 vtable 试点全部通过。