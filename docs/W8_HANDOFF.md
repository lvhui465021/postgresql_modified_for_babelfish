# W8 交接文档:TDS 连接 protocol_kind 标记

> 本文档未纳入版本控制(与 CLAUDE.md 同样处理方式),仅作本地工作记录。
> 生成时间:2026-08-13

## 背景

openHalo MySQL 兼容层与 Babelfish 的融合(`openhalo-fusion` 分支,29 个 `fusion:` 提交)已完成协议层双栈、DDL/错误/GUC 分派统一、MySQL+TDS 共存的 `ProcessUtility_hook` 优先级修复等工作。最终目标是 **PG + MySQL + TDS 三种协议在同一实例上完美并行**。

融合过程中的提交(`8092aa60a9`)已经为 TDS 打好了地基(`listen_add_protocol_socket()`、`ProtocolAuthenticate()` 的 NULL 容错），但明确标注"W8(给 TDS 连接打 `protocol_kind` 标记)"是**后续工作**、尚未做。本轮任务就是完成 W8。

## 已完成的代码改动

仓库:`babelfish_extensions`(**不是** `postgresql_modified_for_babelfish`——TDS 扩展的源码在这个独立仓库里,通过 `shared_preload_libraries` 加载)。

分支:`BABEL_6_0_STABLE`,当前 HEAD `beb4d1817`(改动前未动过,仓库本身与 PG 18.3 已对齐,内部版本号 `18.3.0.0`)。

### 1. `contrib/babelfishpg_tds/src/backend/tds/support_funcs.c`

```diff
-		listen_add_socket(fd, protocol_config);
+		listen_add_protocol_socket(fd, protocol_config, COMPAT_PROTOCOL_TDS);
```

监听 socket 注册从旧的 2 参数版本改为 3 参数版本,给这个监听端口打上 `COMPAT_PROTOCOL_TDS` 标记。这是内核侧早就准备好的 API(`listen_add_protocol_socket`,专为"只用自己 `ProtocolExtensionConfig`、从不注册 `ProtocolRoutine`"的场景设计,TDS 正是这种场景)。

### 2. `contrib/babelfishpg_tds/src/backend/tds/tds_srv.c` 的 `pe_tds_init()`

```diff
 	port->raddr.salen = client_sock->raddr.salen;
+
+	/* 见文件内注释:补上 protocol_kind 传递,与 pq_init() 对齐;
+	 * 刻意不调用 AssignProtocolRoutine()——TDS 未注册 ProtocolRoutine,
+	 * 调用会 elog(FATAL) */
+	port->protocol_kind = client_sock->protocol_kind;
```

**关键设计决策**:没有调用 `AssignProtocolRoutine(port)`。核对过内核 `protocol_routine.c`:`GetProtocolRoutine(COMPAT_PROTOCOL_TDS)` 在无注册者时返回 `NULL`(与 `COMPAT_PROTOCOL_POSTGRES` 不同,后者有兜底),而 `AssignProtocolRoutine()` 在拿到 `NULL` 时会 `elog(FATAL)`。TDS 从设计上就不注册 `ProtocolRoutine`(完全走自己的 `pe_config` / `ProtocolExtensionConfig`),所以只标记 `protocol_kind`、让 `port->protocol_routine` 保持 `NULL`,这正是内核文档(`protocol_routine.h`)里写明的合法状态。

## 影响范围(预期为零行为变化 + 两处修正)

- TDS 的实际协议分派完全不受影响(仍然 100% 走 `pe_config`)
- **修正 1**:`MyCompatMode`(`InitCompatMode()`)现在能正确解析出 TDS 连接的方言上下文,而不是静默退回 `COMPAT_PROTOCOL_POSTGRES`
- **修正 2**:`postmaster.c` 的 `report_fork_failure_to_client()` 现在能正确跳过给 TDS 客户端发送 PG 帧错误包(该函数已有 `protocol_kind != COMPAT_PROTOCOL_POSTGRES` 的判断,之前因为 TDS 一直被误标成 POSTGRES 而从未生效)
- 为后续注册真正的 `tsql_adtext` / T-SQL `ParserRoutine` 打地基(目前 `ADTExtMethod` 已有 13 个为 T-SQL 预留的空槽位,来自更早的融合提交)

## 构建验证记录

### 内核(postgresql_modified_for_babelfish)

```
meson setup build --prefix=$PWD/inst
ninja -C build        # 2568/2568 targets, 0 errors
ninja -C build install
```

### babelfishpg_tds(babelfish_extensions)

直接 `make USE_PGXS=1 PG_CONFIG=<inst>/bin/pg_config` 会遇到两个**与本次改动无关、纯环境性**的编译阻塞,均已找到根因并用编译宏绕过(**未改动任何 SSL/协议源码**):

```bash
cd babelfish_extensions/contrib/babelfishpg_tds
make USE_PGXS=1 \
  PG_CONFIG=/home/hlv/openhalo-update/postgresql_modified_for_babelfish/inst/bin/pg_config \
  CPPFLAGS="-I/usr/include/libxml2 -DHAVE_BIO_METH_NEW -DHAVE_OPENSSL_INIT_SSL"
make USE_PGXS=1 PG_CONFIG=... CPPFLAGS="..." install
```

根因说明(供后续处理"把 OpenSSL 适配高版本"这个待办时参考):
1. `-I/usr/include/libxml2`:PGXS 构建不像 meson 内核构建那样自动把 libxml2 的头文件路径传给依赖它的扩展。
2. `-DHAVE_BIO_METH_NEW`、`-DHAVE_OPENSSL_INIT_SSL`:`tdssecure.c`/`tds-secure-openssl.c` 里其实**已经写好了**针对 OpenSSL 1.1+/3.x 的正确分支(`BIO_meth_new()`、`OPENSSL_init_ssl()`),只是这两个特性探测宏从来没有被任何构建系统(autoconf 或 meson)定义过,导致代码永远落到 OpenSSL 1.0.x 的过时分支(直接读写不透明的 `BIO_METHOD` 结构体、调用已弃用的 `OPENSSL_config()`/`SSL_library_init()`/`SSL_load_error_strings()`)。**这不是版本不匹配问题**,只是构建系统没把这两个宏传进去,补上宏定义后现代分支立刻可以正常编译使用。
   - 后续如果要"正式"解决(而不是每次编译手动加宏),需要在 `babelfish_extensions` 自己的构建系统里加一个 OpenSSL 版本/特性探测步骤(或者简单地在 Makefile 里按 `OPENSSL_VERSION_NUMBER` 直接定义这两个宏),使其不依赖外部手动传参。

## 真实集群验证记录

测试目录:`/tmp/w8testdata`(已停止,数据目录还在)。

```
shared_preload_libraries = 'mysql_parser, mysm, aux_mysql, babelfishpg_tds'
babelfishpg_tds.port = 1433
babelfishpg_tds.listen_addresses = '127.0.0.1'
mysql_listener_on = true
mysql_port = 3306   # 注意:这台机器上系统级 mysql.service(PID 697)占着 3306!测试要换端口,见下方"未完成"
```

验证方法:在 `backend_startup.c` 的 `port = MyProcPort = (protocol_config->fn_init)(client_sock);` 之后临时加了一行 `elog(LOG, ...)` 打印 `port->protocol_kind`(测试完已移除,内核仓库已确认 `git status` 干净)。

结果:
| 连接方式 | 端口 | protocol_kind | 说明 |
|---|---|---|---|
| psql(Unix socket) | 5432 | **0**(POSTGRES) | 正确 |
| 裸 TCP 连接 | 1433(TDS) | **2**(TDS) | **正确,这就是 W8 要达成的效果**。改动前会是 0 |

## 未完成 / 待办

1. **MySQL 直连回归验证没做完**——测试时被这台机器上系统级的真实 `mysql.service`(监听 3306)干扰,第一次测试其实连到了系统 MySQL 而非测试集群;换成 13306 端口后连接被拒绝(`ERROR 2003`),原因还没查(可能是 GUC 改端口后没生效,或者需要重新确认监听状态),就被要求 handoff 中断了。
   - **理论上风险很低**:MySQL 走的是 `pq_init()`(`pqcomm.c`)路径,本次改动完全没碰这个文件,`protocol_kind` 早在之前的融合轮次里就已经验证过对 MySQL 连接正确工作。但严谨起见还是应该补一次真实客户端回归测试。
   - 排查建议:换一个明确空闲的端口(如 23306),`ss -tlnp | grep <port>` 确认监听方是 `postgres` 进程而非别的服务,再用 `mysql -h 127.0.0.1 -P <port> -u <已知密码用户>` 测试。
2. **`babelfishpg_common` / `babelfishpg_tsql` 的 Makefile 有个既有 bug**(与本次改动无关):`babelfishpg_common/Makefile` 里 `include $(PGXS)` 被写了两遍(第 54 行和第 101 行),导致 `Makefile.global` 的 `srcdir` 判定逻辑在第二次 include 时因为 `VPATH` 已被第一次 include 设成惰性展开的 `$(srcdir)` 而循环引用报错(`Recursive variable 'srcdir' references itself`)。目前只单独编译验证了 `babelfishpg_tds`,没有走完整的四扩展联合 `make all`。修复应该很简单(删掉重复的 `include $(PGXS)` 行),但没有在本轮做。
3. **两个仓库的改动都还没有 git commit**,在等确认。`babelfish_extensions` 里的改动内容见上方 diff;`postgresql_modified_for_babelfish` 仓库本身没有改动(`git status` 干净,`inst/` 是未入库的构建产物目录)。

## 如何继续

```bash
# 查看改动
cd /home/hlv/openhalo-update/babelfish_extensions
git diff contrib/babelfishpg_tds/src/backend/tds/support_funcs.c
git diff contrib/babelfishpg_tds/src/backend/tds/tds_srv.c

# 重新拉起测试集群(如果 /tmp/w8testdata 还在)
cd /home/hlv/openhalo-update/postgresql_modified_for_babelfish
./inst/bin/pg_ctl -D /tmp/w8testdata -l /tmp/w8testdata/logfile start
```
