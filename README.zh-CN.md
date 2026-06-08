[🇬🇧 English](README.md) • [🇨🇳 中文](README.zh-CN.md)

---

# LightGraph ⚡

**Cypher 原生嵌入式图数据库引擎 — 轻量极速**

[![C++20](https://img.shields.io/badge/C++20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](#许可证)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-lightgrey)](#平台支持)
[![Build](https://img.shields.io/badge/build-CMake%20%7C%20MSVC%20%7C%20GCC-orange)](CMakeLists.txt)

LightGraph 是一个**零外部依赖的嵌入式图数据库引擎**，使用现代 **C++20** 编写。它将完整的 **Cypher 查询语言** 实现与高性能的**分页存储引擎**相结合，提供 **MVCC 事务**、**崩溃恢复** 和 **B+ 树索引** 能力——全部整合在一个无需任何外部依赖的库中。

> **嵌入式 · 事务性 · Cypher 原生**
>
> 引擎内部代号：*Cypherite*

---

## 核心特性

### Cypher 查询引擎

完整的 Cypher 查询语言实现，包含词法分析器、解析器（AST）和执行器：

| 类别 | 支持语法 |
|------|----------|
| **读取** | `MATCH`、`OPTIONAL MATCH`、变长路径模式 `[*1..5]`（BFS 遍历） |
| **写入** | `CREATE`（节点/关系 + 属性）、`MERGE` + `ON MATCH`/`ON CREATE SET` |
| **修改** | `SET`（属性更新、标签变更 `n:Label`）、`DELETE`、`DETACH DELETE`、`REMOVE` |
| **投影** | `WITH`（管道化，支持 `DISTINCT`/`WHERE`）、`UNWIND` |
| **过滤** | `WHERE`：`=`、`<>`、`<`、`>`、`<=`、`>=`、`STARTS WITH`、`ENDS WITH`、`CONTAINS`、`IS NULL`/`IS NOT NULL`、`IN`、`AND`、`OR`、`NOT` |
| **返回** | 表达式、别名（`AS`）、`DISTINCT`、`ORDER BY`（`ASC`/`DESC`）、`SKIP`、`LIMIT` |
| **条件** | `CASE` / `WHEN` / `THEN` / `ELSE` / `END` |
| **聚合** | `COUNT`、`SUM`、`AVG`、`MIN`、`MAX`、`COLLECT`、`COUNT(DISTINCT expr)` |
| **函数** | `exists()`、`type()`、`id()`、`labels()`、`keys()`、`nodes()`、`relationships()` |

### B+ 树索引引擎

七棵 **Order-160 的 B+ 树** 为所有图操作提供索引支持：

| 树 | 键 → 值 | 用途 |
|------|-------------|--------|
| `vertex_tree` | `VertexId → VertexPayload` (44B) | 顶点主键查找 |
| `edge_tree` | `EdgeId → EdgePayload` (52B) | 边主键查找 |
| `out_edge_tree` | `(SrcId, EdgeId) → EdgePayload` | 出边遍历 |
| `in_edge_tree` | `(DstId, EdgeId) → EdgePayload` | 入边遍历 |
| `prop_tree` | `(OwnerId, PropKey) → PropValue` | 属性访问 |
| `label_tree` | `(LabelId, EntityId) → EntityId` | 标签索引 |
| `prop_index_tree` | `(LabelId, PropKey, PropValue, EntityId) → EntityId` | 属性倒排索引 |

技术亮点：
- **MVCC 版本链** — 每个 slot 的 `VersionNodeEx` 通过 `older_offset` 链表实现快照隔离
- **兄弟指针** — 叶子页双向链表，零成本顺序扫描
- **前缀扫描** — 高效属性树范围查询
- **页分裂** — 向上传播，自动增加树高度
- **压缩与合并** — 后台 GC 压缩页空间、合并稀疏叶子页

### 页存储管理器

| 组件 | 详情 |
|-----------|--------|
| **页大小** | 4 KB，24 字节页头 |
| **I/O 模型** | 内存映射文件（POSIX mmap / Windows `CreateFileMapping`） |
| **崩溃安全** | COW 影子分页——原子切换根页 |
| **持久性** | 可选 WAL 预写日志，实现细粒度 fsync |
| **大值处理** | 溢出页链，支持超过 4 KB 的属性值 |
| **损坏检测** | 每页 CRC32 校验和 |
| **分配管理** | 版本追踪的空闲页列表（`std::set<std::pair<TxId, PageId>>`） |

### MVCC 事务系统

| 属性 | 实现 |
|----------|----------------|
| **隔离级别** | 快照隔离——每个事务看到一致的 read-ts 快照 |
| **并发读** | 无限只读事务，无锁 |
| **写串行化** | 单写者 + OCC（乐观并发控制） |
| **行锁** | 可选悲观模式（`vertex_locks_` / `edge_locks_`） |
| **死锁检测** | 等待图 + DFS 环路检测 |
| **后台 GC** | 回收比最小活跃读事务更旧的 MVCC 版本 |
| **逻辑删除** | Tombstone 版本保留对并发读取器的可见性 |

### 批量导入

```cpp
std::vector<BulkVertexInput> vertices;  // 1 万个顶点
std::vector<BulkEdgeInput> edges;       // 9999 条边
TxId tx = db.BeginTransaction(false);
auto result = db.BulkInsert(tx, vertices, edges);  // 比逐条事务快 ~40 倍
db.Commit(tx);
```

### 跨平台支持

| 平台 | 状态 | 备注 |
|----------|--------|-------|
| Linux (x86_64) | ✅ | POSIX mmap |
| Windows (x64, MSVC) | ✅ | `CreateFileMapping` + WAL |
| macOS | ⚠️ 未测试 | 应可通过 POSIX mmap 工作 |

---

## 快速开始

### 构建

```bash
cmake -B build && cmake --build build
```

### 运行测试

```bash
cd build && ctest --output-on-failure
```

### C++ API 示例

```cpp
#include <graph_store.h>
using namespace graphstore;

int main() {
    GraphStore db;
    db.Init("/tmp/mydb.db");

    TxId tx = db.BeginTransaction(false);
    VertexId alice = db.AddVertex(tx, 0, Properties{{"name", "Alice"}, {"age", "30"}});
    VertexId bob   = db.AddVertex(tx, 0, Properties{{"name", "Bob"},   {"age", "25"}});
    db.AddEdge(tx, alice, bob, 1, Properties{{"since", "2020"}});
    db.Commit(tx);

    TxId rtx = db.BeginTransaction(true);
    auto name = db.GetVertexProperty(rtx, alice, "name");       // "Alice"
    auto out  = db.GetOutEdges(rtx, alice);                     // [{edge_id, bob, 1}]
    return 0;
}
```

### Cypher 查询示例

```cpp
QueryExecutor exec(&db);
auto result = exec.Execute(
    Parser("MATCH (a:Person)-[:KNOWS]->(b:Person) WHERE a.name = 'Alice' RETURN b.name")
        .ParseQuery().value()
);
for (auto& row : result.rows) {
    std::cout << row.bindings["b.name"].as_string() << std::endl;  // "Bob"
}
```

---

## 架构

```
┌──────────────────────────────────────────────────────┐
│                   Cypher 查询引擎                      │
│   词法分析 → 解析器 (AST) → 表达式求值器              │
│   → 查询执行器 (MATCH/CREATE/MERGE/SET/DELETE)       │
├──────────────────────────────────────────────────────┤
│                   图存储层                             │
│   顶点/边 CRUD · 属性索引 · OCC 验证                  │
│   死锁检测 · 行锁 · 批量导入                          │
├─────────────────────┬────────────────────────────────┤
│   B+ 树索引         │    页管理器                     │
│   (Order-160 ×7)    │   (4KB 页, COW, mmap)          │
│   MVCC 版本链       │   溢出链, CRC32,                │
│   兄弟指针 · GC     │   空闲列表, 在线压缩            │
├─────────────────────┴────────────────────────────────┤
│              WAL 预写日志 / 崩溃恢复                   │
└──────────────────────────────────────────────────────┘
```

### 各层说明

1. **页管理器** (`page_manager.h/.cpp`) — 内存映射文件 I/O、页分配/释放、COW 影子分页、溢出页链、可选 WAL、CRC32 校验和。

2. **B+ 树** (`bplus_tree.h/.cpp`) — Order-160 B+ 树，每个 slot 带 MVCC 版本链、叶子页兄弟指针实现范围扫描、前缀扫描游标、向上传播的页分裂、压缩和叶子合并。

3. **序列化器** (`serializer.h/.cpp`) — 7 棵树索引的二进制键编码、`VertexPayload` (44B)、`EdgePayload` (52B)、属性 blob 序列化、端序转换（`HostToBig`/`BigToHost`）。

4. **图存储** (`graph_store.h/.cpp`) — 管理全部 7 棵树的图语义、事务生命周期（Begin/Commit/Rollback）、顶点/边 CRUD、属性索引、批量导入 API、死锁检测、行锁、后台 GC。

5. **Cypher 解析器** (`cypher_parser.h/.cpp`) — 词法分析器（关键字/符号/数字/字符串/参数识别）、递归下降解析器生成 `CypherQuery` AST、带优先级攀升的表达式求值器、`QueryExecutor` 编排模式匹配、BFS 遍历、过滤、投影、排序和分页。

---

## 性能基准测试

在自己的机器上运行基准测试：

```bash
./build/test_benchmark
```

| 基准测试 | 配置 | 测试内容 |
|-----------|---------------|---------------|
| **解析器：简单** | `MATCH (n:Person) WHERE n.age > 25 RETURN n.name, n.age` × 1000 | 基础查询解析吞吐 |
| **解析器：复杂** | 多子句 JOIN/WHERE/GROUP/ORDER/LIMIT × 1000 | 复杂查询解析吞吐 |
| **解析器：CREATE** | `CREATE (n:Person {name:'Alice', age:30}) RETURN n` × 1000 | DML 语句解析 |
| **解析器：错误** | 非法语法 × 1000 | 错误路径解析性能 |
| **AddVertex** | 顺序插入 1000 顶点，每事务一个 | 逐条写入吞吐 |
| **GetVertex** | 1000 顶点池中随机查找 × 1000 | 点查询延迟 |
| **SetVertexProperty** | 跨 1000 顶点顺序更新属性 × 1000 | 属性修改吞吐 |
| **RemoveVertex** | 批量删除 1000 顶点 | 删除吞吐 |
| **BulkInsert** | 单事务导入 1 万顶点 + 9999 条边 | 批量导入吞吐（比逐条快 ~40 倍）|
| **并发解析器** | 1/2/4/8 线程同查询 × 2000 次/线程 | 解析器可扩展性 |
| **读写者** | 4 并发读 + 1 写（混合 Add/Remove）| 读压下写入性能 |
| **全流水线** | 解析 + 执行 `MATCH (n) RETURN COUNT(n)` | 端到端查询延迟 |

*数据与硬件相关；在目标系统上运行 `./build/test_benchmark` 获取准确结果。*

---

## 项目结构

```
├── include/                       # 公共头文件
│   ├── bplus_tree.h               # B+ 树（MVCC 版本链）
│   ├── common.h                   # 基础类型、序列化、状态码
│   ├── cypher_parser.h            # Cypher 词法/解析/执行器
│   ├── graph_store.h              # 图存储 API（CRUD、事务）
│   ├── page_manager.h             # 页管理器（mmap、分配、COW）
│   └── serializer.h               # 顶点/边载荷、键编码器
├── src/                           # 实现代码
│   ├── bplus_tree.cpp
│   ├── common.cpp
│   ├── cypher_parser.cpp
│   ├── graph_store.cpp
│   ├── page_manager.cpp
│   └── serializer.cpp
├── tests/                         # 测试套件（约 280 个测试）
│   ├── test_btree.cpp             # B+ 树单元测试
│   ├── test_graphstore.cpp        # 图引擎 CRUD 测试
│   ├── test_cypherparser.cpp      # Cypher 解析器测试
│   ├── test_fuzz.cpp              # 模糊测试 + 崩溃恢复测试
│   ├── test_benchmark.cpp         # 性能基准测试
│   └── crash_test.cpp             # 崩溃恢复验证
├── cmds/                          # CLI 工具
│   ├── db_admin.cpp               # 数据库管理 CLI
│   └── cypher_parser.cpp          # Cypher 交互式 Shell
├── docs/
│   ├── GraphDb.md                 # 深度设计文档（中文）
│   └── Plan.md                    # 改进路线图（P0-P3）
└── CMakeLists.txt                 # C++20，支持 MSVC/GCC/Clang
```

---

## 平台支持

| 平台 | 状态 | 备注 |
|----------|--------|-------|
| Linux (x86_64) | ✅ | POSIX mmap |
| Windows (x64, MSVC) | ✅ | `CreateFileMapping` + WAL |
| macOS | ⚠️ 未测试 | 应可通过 POSIX mmap 工作 |

---

## 许可证

MIT 协议。详情请参阅 [LICENSE](LICENSE) 文件。
