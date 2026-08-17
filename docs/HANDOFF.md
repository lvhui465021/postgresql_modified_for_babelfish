# HANDOFF 交接文档

> 更新:2026-08-17。本文记录内核与兼容层的实现边界；产品的当前版本清单、端口、构建验收和发布结论以超级仓 `openhalo-babelfish/docs/` 为准。W8_HANDOFF.md 为早期历史记录。

## 1. 项目一句话现状

openHalo × Babelfish 融合:同一 PG 18.3 postmaster 三协议并行(PG 5432 / MySQL 3306 / TDS 1433),兼容功能与 openHalo/Babelfish 上游持平;已完成 `v18.3-fusion.2` 的集成验收;MySQL 兼容层插件化两阶段全部完成(独立仓库 mysql_extensions)。13306 仅用于 3306 已被系统 MySQL 占用时的开发覆盖。

## 2. 三个仓库(全部可 clone、可构建)

| 仓库 | 远程 | 分支 | `v18.3-fusion.2` manifest | 角色 |
|---|---|---|---|---|
| postgresql_modified_for_babelfish | git@github.com:lvhui465021/postgresql_modified_for_babelfish.git | openhalo-fusion | d14bca64a9 | 内核:PG 核心 + vtable 接缝 + MySQL 执行器/命令 fork + 方言头文件 + docs/ |
| mysql_extensions | git@github.com:lvhui465021/mysql_extensions.git | main | 621c31dfa | MySQL 三模块:mysql_parser/mysm/aux_mysql(PGXS,带切分历史) |
| babelfish_extensions | git@github.com:lvhui465021/babelfish_extensions.git | openhalo-fusion | 63e6cebcf | TDS 四扩展:common/money/tds/tsql + build-all.sh |

## 3. 架构边界(一句话版)

内核保留:执行器 fork(mys_execMain/mys_nodeModifyTable 等)、命令 fork(commands/mysql)、systemVar、adt 钩子、vtable 注册表、方言头文件——MySQL 语义织进执行器循环内部,SPI 表达不了,迁出等于导内核 ABI。扩展层:解析(mysql_parser.so)、类型/SQL 函数(mysm.so)、协议(aux_mysql.so)。详见 docs/MYSQL_PLUGIN_BOUNDARY.md(新仓库 README 同源)。

## 4. 构建流程(从零)

从产品仓递归 clone 后执行 `scripts/build-all.sh`；它会按内核、四个 Babelfish 扩展、三个 MySQL PGXS 模块的依赖顺序安装。前置仍为 JRE、cmake、ANTLR4 C++ runtime 4.13.2、libxml2、libuuid、flex/bison 与 Perl。内核 catversion 为 202506292，旧数据目录必须重新 initdb。

## 5. 测试基线(install-first 模式)

产品命令:`openhalo-babelfish/scripts/run-baseline.sh`(需先全量 install)。TDS 验收必须使用真实 Microsoft SQLCMD 18；非 TLS TAP 夹具以 `SQLCMD_OPTIONS=-No` 运行，该参数不得用于生产部署。

- 组 1 meson:setup/postmaster/regress/authentication(PG 核心)
- 组 2 prove:MySQL TAP(005_mysql_compat/006_pg_dump_restore/007_mysql_parallel,路径在 ../mysql_extensions,MYSQLEXT_DIR 可覆盖)+ 004_mysql_protocol/005_mysql_listener_stability(内核树)
- 组 3 prove:TDS TAP(001_tdspasswd/003_bbfextnotloaded,需真实 Microsoft SQLCMD 在 PATH 或 `SQLCMD_BIN_DIR`)
- 当前集成基线:`v18.3-fusion.2` 在干净远端递归浅 clone 上通过：PG-core 12 OK（2 个预期 skip）、MySQL 31 个独立 TAP + 485 个内核 TAP、TDS 8 个 TAP；详见超级仓 `docs/VALIDATION.md`。

环境要点:基线脚本已消除机器特定 `PERL5LIB` 依赖；PG_REGRESS 指向构建树(build/src/test/regress/pg_regress,meson 不安装)；TDS TAP 使用动态临时端口，不要求生产默认 1433 空闲。

## 6. 环境资产

- 不把任何会话中的 testcluster、mysqltest、账号或口令视为交付资产；部署必须新建数据目录和凭据。
- TDS TAP 使用真实 Microsoft SQLCMD 18；FreeTDS `tsql` 仅可作开发排障，不是验收替代品。
- 产品默认端口为 5432/3306/1433；若本机 3306 被占用，可在开发环境显式改为 13306。

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

1. 超级仓 CI 已提供 Gitlink/脚本检查和供 Ubuntu 24.04 self-hosted runner 执行的完整基线入口；后续可为 mysql_extensions 增加更细粒度的独立 CI。
2. openHalo 上游跟踪:内核执行器/命令 fork 每 PG 版本 rebase(已有 check_mysql_fork_drift.sh 哨兵)。PG major 升级不是本固定 18.3 产品的例行维护。
3. A1 阶段二(bbfCustomProcessUtility_hook 槽位)等长期项,按文档决策不主动做。

## 10. 上手验证(5 分钟)

```sh
git clone --recurse-submodules git@github.com:lvhui465021/openhalo-babelfish.git
cd openhalo-babelfish
scripts/build-all.sh
SQLCMD_BIN_DIR=/opt/mssql-tools18/bin SQLCMD_OPTIONS=-No scripts/run-baseline.sh
```

生产部署前还必须完成 TLS、备份恢复和工作负载验收；清单见超级仓 `docs/DEPLOYMENT.md`。
