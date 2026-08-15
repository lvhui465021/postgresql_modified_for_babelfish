# HANDOFF 交接文档(2026-08-15,当前状态)

> 生成:2026-08-15,基于会话核实的现场状态。W8_HANDOFF.md 为早期历史记录(W8 时代,已过时),本文件是当前权威交接。随代码入仓(内核 docs/)。

## 1. 项目一句话现状

openHalo × Babelfish 融合:同一 PG 18.3 postmaster 三协议并行(PG 5432 / MySQL 13306 / TDS 1433),兼容功能与 openHalo/Babelfish 上游持平;已到交付验收阶段;MySQL 兼容层插件化两阶段全部完成(独立仓库 mysql_extensions)。

## 2. 三个仓库(全部可 clone、可构建)

| 仓库 | 远程 | 分支 | HEAD(2026-08-15) | 角色 |
|---|---|---|---|---|
| postgresql_modified_for_babelfish | git@github.com:lvhui465021/postgresql_modified_for_babelfish.git | openhalo-fusion | f9c3f9f266 | 内核:PG 核心 + vtable 接缝 + MySQL 执行器/命令 fork + 方言头文件 + docs/ |
| mysql_extensions | git@github.com:lvhui465021/mysql_extensions.git | main | db10a8c | MySQL 三模块:mysql_parser/mysm/aux_mysql(PGXS,带切分历史) |
| babelfish_extensions | git@github.com:lvhui465021/babelfish_extensions.git | BABEL_6_0_STABLE | ac4b712be | TDS 四扩展:common/money/tds/tsql + build-all.sh |

## 3. 架构边界(一句话版)

内核保留:执行器 fork(mys_execMain/mys_nodeModifyTable 等)、命令 fork(commands/mysql)、systemVar、adt 钩子、vtable 注册表、方言头文件——MySQL 语义织进执行器循环内部,SPI 表达不了,迁出等于导内核 ABI。扩展层:解析(mysql_parser.so)、类型/SQL 函数(mysm.so)、协议(aux_mysql.so)。详见 docs/MYSQL_PLUGIN_BOUNDARY.md(新仓库 README 同源)。

## 4. 构建流程(从零)

1. 内核:meson setup build --prefix=<inst> -Duuid=e2fs -Dlibxml=enabled; ninja -C build && ninja -C build install(catversion 202506292,旧数据目录需重新 initdb)
2. TDS 扩展:babelfish_extensions/build-all.sh(内核 + 四扩展一键;前置:JRE、cmake、ANTLR4 4.13.2 runtime 在 /usr/local、libxml2、libuuid)
3. MySQL 模块:mysql_extensions/build-all.sh(前置:内核已装 + flex/bison/perl;KERNEL_DIR 默认 ../postgresql_modified_for_babelfish)

## 5. 测试基线(install-first 模式)

命令:postgresql_modified_for_babelfish/run-baseline.sh(需先全量 install)

- 组 1 meson:setup/postmaster/regress/authentication(PG 核心)
- 组 2 prove:MySQL TAP(005_mysql_compat/006_pg_dump_restore/007_mysql_parallel,路径在 ../mysql_extensions,MYSQLEXT_DIR 可覆盖)+ 004_mysql_protocol/005_mysql_listener_stability(内核树)
- 组 3 prove:TDS TAP(001_tdspasswd/003_bbfextnotloaded,需 sqlcmd shim 在 PATH)
- 当前基线:524 ok / 0 not ok(2026-08-15 实测,基于远程克隆构建的模块)

环境要点:PERL5LIB=/home/hlv/perl5/lib/perl5 + pgxs perl 目录;PG_REGRESS 指向构建树(build/src/test/regress/pg_regress,meson 不安装);TDS TAP 需 1433 独占(跑前停 testcluster)。

## 6. 环境资产

- 集群 testcluster:/home/hlv/openhalo-update/testcluster(5432/13306/1433, socket testcluster.sock;alice/p 超级用户 scram+mysql_native_password;bbf_admin/p md5,TDS 逻辑库 master→物理库 babelfish_db)
- 集群 mysqltest:/home/hlv/openhalo-update/mysqltest(5433/3306,socket mysqltest.sock;test/test 超级用户,库 unvdb_mysqldb;仅 MySQL 侧验证用)
- TDS 客户端:FreeTDS tsql(TDSVER=7.4);sqlcmd 翻译 shim:/home/hlv/openhalo-update/sqlcmd-bin/sqlcmd(TAP 套件用)
- 3306 曾被系统 mysql.service 占用(现 inactive);13306 是三协议集群的 MySQL 端口

## 7. 踩坑速查(本会话血泪)

1. TDS 复现/测试必须保证 1433 唯一占用者——并行集群会串台产生幻象行为(曾误判多次)
2. flex 2.6.4 不认 --no-backup(meson 的 pgflex 包装处理了它);模块 Makefile 已去掉
3. gen_keywordlist.pl 不在 pgxs 安装树,须从内核源码树取(PG_SRC=KERNEL_DIR 显式传)
4. src/include/meson.build 的 header_subdirs 曾漏 openHalo 的 adapter 目录(已注册)
5. 沙箱 /tmp 每次 bash 调用私有,跨调用状态必须落工作区文件
6. run_code 的 TS 解析器对反引号/特殊字符过敏,复杂命令写脚本文件再 sh
7. CREATE LOGIN 段错误(已修复 db2d961ba):P3-1 的 pe_process_command 丢了 TDS error context 压/弹,log_statement=all 时 NULL 解引用;TDS 官方 TAP 套件是这类回归的哨兵

## 8. 已知限制与明确非目标

- 发布必须告知:pg_dumpall 不导出 rolpasswordext(备份恢复需人工重设 MySQL 口令)、MySQL HBA 用 md5 名承载 mysql_native_password、TDS 不支持 scram、SHOW CREATE VIEW tokenizer 缺陷(P2-7)、MySQL 过程式语言未实现(P2-9)等——完整清单见 docs/RELEASE_NOTES.md §5
- 明确非目标:FUSION_PLAN 记录的 P2-8/9/11/12/13/14/17/18 ⛔(上游逐字一致)、A1 阶段一-b/阶段二、coalesce_typmod、P3-1 高风险项(parser_routine/mainfunc 重写)

## 9. 下一步候选(未排期)

1. 为 mysql_extensions 补 CI 入口,调用跨仓 ABI 检查(check_mysql_kernel_exports);build-graph/vtable/fork-drift 三项仍留内核,因为它们分别检查内核 Meson 图、vtable 消费者和内核 fork
2. mysql_extensions 自身补 TAP 运行说明/CI 骨架(现由内核 run-baseline.sh 驱动)
3. openHalo 上游跟踪:内核执行器/命令 fork 每 PG 版本 rebase(已有 check_mysql_fork_drift.sh 哨兵)
4. A1 阶段二(bbfCustomProcessUtility_hook 槽位)等长期项,按文档决策不主动做

## 10. 上手验证(5 分钟)

```sh
git clone git@github.com:lvhui465021/mysql_extensions.git
git clone git@github.com:lvhui465021/babelfish_extensions.git
git clone git@github.com:lvhui465021/postgresql_modified_for_babelfish.git
# 内核:meson setup + ninja install;TDS:babelfish_extensions/build-all.sh;MySQL:mysql_extensions/build-all.sh
# 起 testcluster 配置(见 FUSION_PLAN §8),psql/mysql/tsql 三客户端各查 5/2
# 跑基线:run-baseline.sh(预期 524 ok / 0 not ok)
```
