# 发布说明:openHalo × Babelfish 三协议并行融合(PG + MySQL + TDS)

> 生成:2026-08-14。本文件随代码入仓(postgresql_modified_for_babelfish/docs/),配套文档见文末索引。

## 1. 定位与范围

在同一个 PostgreSQL 18.3 实例(postmaster)上并行监听三种线协议:PostgreSQL(5432)、MySQL(13306 或配置端口)、TDS/SQL Server(1433)。每种协议的连接在解析、类型、DDL、错误封帧各层拿到各自方言的语义。

**范围口径(用户指定):兼容功能只保持与 openHalo / Babelfish 上游持平即可,不实现上游没有的特性。**

## 2. 版本基线

| 组件 | 仓库 / 分支 | 基线 |
|---|---|---|
| 内核 | postgresql_modified_for_babelfish @ openhalo-fusion | PG 18.3(Babelfish BABEL_6_0_STABLE__PG_18_3)+ openHalo MySQL 能力,catversion 202506292 |
| TDS 扩展 | babelfish_extensions @ BABEL_6_0_STABLE | Babelfish 6.0.0(内部版本 18.3.0.0) |
| MySQL 侧 | 内核内置(parser/adtext/protocol 注册表)+ contrib/aux_mysql 扩展 | 与 openHalo @ cbd6642fb03 对齐(审计见 FUSION_PLAN) |

## 3. 已验证能力

- **三协议并行**:同一存活 postmaster 上 5432/13306/1433 交替验证;同一表达式 5/2 三语义:PG=2、MySQL=2.5000、T-SQL DECIMAL 除法=2.5000000000000000。
- **MySQL**:真实 mysql CLI 端到端(DDL/DML/事务/函数/预编译协议/排序规则语义);openHalo 自有测试 SQL 集 12 文件实跑,13 处差异全部归类(无功能缺失,多数优于上游,详见 FUSION_PLAN 的 openHalo 整合审计);SHOW DATABASES/SHOW STATUS/SHOW WARNINGS 等管理命令正常。
- **TDS**:真实 FreeTDS tsql 端到端(@@VERSION/IDENTITY/NVARCHAR/存储过程/游标/TRY-CATCH/SEQUENCE AS BIGINT/DECIMAL 语义/ORDER BY NULLS/CHARINDEX/REPLACE);TDS 官方 TAP 套件 001/003 通过(见 6)。
- **PG**:内核 regress 232/232 子测试全过,原生行为不受影响。
- **认证三协议共存**:同一角色同一明文口令可同时通过 5432(scram-sha-256)与 3306/13306(mysql_native_password)登录;HBA 支持 protocol= 匹配;pg_hba_file_rules 增加 protocol 展示列。
- **架构统一**:TDS/MySQL 全部经 CompatibilityRoutine 注册表分派(parser/adtext/protocol/listen_init);T-SQL DDL 分发已迁入 vtable 槽位;ProtocolExtensionConfig 机制整体退役。

## 4. 构建与部署

### 环境前置(一次性)

- JRE(ANTLR4 代码生成器,仓库自带 jar)与 cmake
- ANTLR4 C++ runtime 精确 4.13.2(apt 的 4.10 不兼容),安装到 /usr/local
- libxml2 开发包、libuuid(uuid-ossp 依赖)
- Perl 模块 IPC::Run(测试用,本机装于 ~/perl5,跑测试需 PERL5LIB=/home/hlv/perl5/lib/perl5)
- FreeTDS tsql(或任意 TDS 客户端)用于 TDS 端到端验证

### 构建

1. 内核 meson:meson setup build -Duuid=e2fs(uuid=e2fs 是持久化配置,新 checkout 需手动重配,不入 git);ninja -C build && ninja -C build install。
2. 扩展:babelfish_extensions/build-all.sh 零手动步骤一键构建并安装四个扩展(common/money/tds/tsql),含全部前置检查。

### 集群初始化

- catversion 已 bump 至 202506292:任何早于 P3-2(rolpasswordext)的数据目录无法被新二进制启动,需重新 initdb(保护性行为)。
- 三协议集群配置参考 FUSION_PLAN 第 8 节:shared_preload_libraries 为 mysql_parser, mysm, aux_mysql, babelfishpg_tds;端口 PG 5432 / mysql_port 13306 / babelfishpg_tds.port 1433;HBA:MySQL 协议只认 md5 方法,TDS 不支持 scram。
- MySQL 协议用户密码须以 mysql_native_password 格式存储:SET password_encryption='mysql_native_password'; ALTER USER ... PASSWORD '...'。
- 需 CREATE EXTENSION aux_mysql(或 initdb -m mysql);TDS 侧在目标库 CREATE EXTENSION babelfishpg_tsql CASCADE 后 CALL sys.initialize_babelfish(角色名)。

## 5. 已知限制(发布必须告知)

1. **pg_dumpall 不导出 rolpasswordext(MySQL 侧 verifier)**:备份恢复后 MySQL 口令不会随之恢复,需对相关角色重设 SET password_encryption='mysql_native_password'; ALTER ROLE ... PASSWORD '...'(硬性人工步骤,直至后续实现)。
2. **MySQL HBA 方法沿用 md5 名称承载 mysql_native_password 挑战应答**(语义错位,配置 pg_hba.conf 时按此理解)。
3. **TDS 不支持 scram-sha-256**(只认 md5/password/trust/gss/oauth):给 TDS 用的 HBA 规则避开 scram。
4. SHOW CREATE VIEW 的 DDL 重建 tokenizer 有缺陷(与 openHalo 上游逐字符一致,按范围原则不修)。
5. MySQL 过程式语言(CREATE TRIGGER/PROCEDURE 过程体)基本未实现(与 openHalo 上游一致,不修)。
6. MySQL 命名 JSON 函数(JSON_EXTRACT/JSON_OBJECT)覆盖不全(上游同缺,不修)。
7. TDS TOP N WITH TIES / 列内联 FOREIGN KEY 简写不支持(上游同缺,不修)。
8. MySQL 正则 REGEXP/RLIKE 在非确定性排序规则列上不可用(上游同缺,不修)。
9. 非确定性排序规则列无法使用 LIKE 前缀索引优化(设计取舍,确定性列 + varchar_pattern_ops 可走索引)。

## 6. 测试基线

- 内核五套件 meson test(setup/postmaster/aux_mysql/regress/authentication):18 OK / 0 Fail(2 环境性 skip)。
- openHalo MySQL 兼容测试 SQL 集:12 文件实跑,差异全部归类(详见 FUSION_PLAN 审计节)。
- TDS 官方 TAP 套件:001_tdspasswd、003_bbfextnotloaded 通过(sqlcmd 经翻译 shim 接 FreeTDS tsql);002 需 Kerberos 环境、004 需旧版本安装(oldinstall/installdir16),本环境跳过。
- 三协议手工端到端:psql / mysql CLI / tsql 交替验证,见 FUSION_PLAN 第 0、5.5、5.7 节。

## 7. 明确非目标(记录在案,不阻塞交付)

- P3-1 高风险项(parser_routine/mainfunc 重写)、A1 阶段一-b(删除 PG 连接 ProcessUtility_hook)、A1 阶段二(bbfCustomProcessUtility_hook 新槽位)、coalesce_typmod 槽位迁移:按设计文档决策保留现状。
- P2-7/8/9/11/12/13/14/17/18:已逐字核实与上游一致,按持平原则不修。
- P2-20 排序规则性能取舍:设计代价,接受。

## 8. 文档索引(本目录 docs/)

- FUSION_PLAN.md:总体规划/状态/审计/提交记录
- P3-2_AUTH_SPEC.md:跨协议认证方案与验收用例
- P3-A1_PROCESS_UTILITY_MIGRATION.md:T-SQL DDL 分派 vtable 化设计与试点记录
- COLLATION_STRATEGY_COMPARISON.md / COLLATION_CLUSTER_SPEC.md:排序规则对齐与集群行为
- P2-10_FIX_SPEC.md / P2-15_SHOW_VARIABLES_SPEC.md / P2-9_PROCEDURAL_ANALYSIS.md:专项分析
- W8_HANDOFF.md:早期交接记录
- openHalo-MySQL兼容架构优化方案.md:openHalo 侧架构参考