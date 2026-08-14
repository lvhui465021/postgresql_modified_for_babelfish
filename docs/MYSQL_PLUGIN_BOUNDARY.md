# MySQL 兼容层插件化边界声明(内核 ↔ 扩展)

> 2026-08-14 定案。本文件描述 MySQL 兼容层在内核与扩展之间的边界,以及 `mysql_extensions` 独立仓库路线(与 babelfish_extensions 对称)的实施背景。

## 1. 三层边界(终态)

| 层 | 内容 | 落点 | 状态 |
|---|---|---|---|
| 内核接缝与执行器/命令 fork | vtable 注册表(CompatibilityRoutine: parser/adtext/protocol/listen_init)、parsereng/adtext 分派器、**执行器 fork**(mys_execMain/mys_execProcnode/mys_executor/mys_execPartition/mys_nodeModifyTable)、**命令 fork**(commands/mysql: mys_tablecmds/mys_sequence/mys_uservar/mys_prepare/mys_set)、共享会话状态(adapter/mysql/systemVar.c)、adt 钩子(utils/adt/mysql 与 adtext.c)、方言头文件(src/include 全部) | 内核(维持现状) | 定案:不迁出 |
| 解析模块 | mysql_parser.so: mys_gram.y/mys_scan.l/mys_parser.c/mys_expr_transform.c/mys_gram_globals.c/mys_keywords.c/mys_namespace.c/mys_parse_utilcmd.c + bison/flex/kwlist 生成 | contrib/mysql_parser(PGXS,阶段 1)→ 独立仓库(阶段 2) | 已迁出 |
| 类型/SQL 函数模块 | mysm.so: mys_adtext.c + 20 个 SQL 函数/类型文件(21 源文件) | contrib/mysm(PGXS,阶段 1)→ 独立仓库(阶段 2) | 已迁出 |
| 协议模块 | aux_mysql.so: 协议帧/认证/监听/DDL 分派 | contrib/aux_mysql(已是 PGXS)→ 独立仓库(阶段 2) | 已迁出 |

## 2. 为什么执行器/命令 fork 必须留在内核(不走 SPI,不迁出)

MySQL 的若干语义织进执行器循环内部与命令实现内部,SPI 位于执行器之上无法表达:

- 用户变量(`@a := 1` 嵌在表达式里):求值语义在表达式树内
- REPLACE INTO / ON DUPLICATE KEY UPDATE:需要改 ModifyTable 循环(mys_nodeModifyTable 是 nodeModifyTable 的 fork)
- 除法精度递增(5/2=2.5000):执行器层运算结果后处理
- LAST_INSERT_ID / AUTO_INCREMENT:命令层会话状态(mys_sequence/mys_uservar)
- DDL 语义(COLLATE 子句/ENGINE/ON UPDATE 触发器生成):mys_tablecmds 是 tablecmds 的 fork
- USE/SET 会话态命令:直接改写 mys_sqlMode/session state

若强行迁出,等于把 estate/ModifyTableState/tablecmds 内部结构当扩展 ABI 导出——脆弱且零用户价值。Babelfish 能走 SPI 是因为 T-SQL 语义恰好映射到 PG 执行器;MySQL 映射不上,openHalo 的 fork 策略是既定架构事实。

## 3. 阶段 1(已完成,2026-08-14)

- mysql_parser/mysm 迁出内核 meson 构建,变为 contrib/ 下 PGXS 扩展(生成链:bison/flex/gen_keywordlist,PG_SRC 显式传入)
- 内核 meson 摘除两个 shared_module 定义与扫描器/语法/关键字生成;src/include/meson.build 补 adapter 安装目录(openHalo 新增顶层头目录未注册)
- aux_mysql 的 G3 gate 测试与 MySQL TAP 测试移出 meson(随模块迁出);postmaster 套件拆分(004/005 走 prove)
- babelfish_extensions/build-all.sh 收编三 MySQL 模块(七扩展一键构建)
- 测试联动切换:PG 核心套件(meson)+ MySQL/TDS TAP 套件(先 install 扩展、再 prove 对 inst/ 跑,见 run-baseline.sh)

## 4. 阶段 2(规划):独立仓库 mysql_extensions

- 与 babelfish_extensions 平级:git 切分(带历史)+ 自建 build-all.sh + 双仓联调
- 本文件随仓库迁移,作为新仓库 README 的边界声明
- 内核仓库仍保留:执行器/命令 fork + 接缝 + 头文件(如上表)——不是没拆干净,是架构边界