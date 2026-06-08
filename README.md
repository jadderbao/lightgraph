[🇬🇧 English](README.md) • [🇨🇳 中文](README.zh-CN.md)

---

# LightGraph ⚡

**The Cypher-Native Graph Database Engine — Embedded & Lightning-Fast**

[![C++20](https://img.shields.io/badge/C++20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](#license)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-lightgrey)](#platform-support)
[![Build](https://img.shields.io/badge/build-CMake%20%7C%20MSVC%20%7C%20GCC-orange)](CMakeLists.txt)

LightGraph is a **zero-dependency embedded graph database engine** written in modern **C++20**. It combines a full **Cypher query language** implementation with a high-performance **page-based storage engine** featuring **MVCC transactions**, **crash recovery**, and **B+ tree indexing** — all in a single library with no external dependencies.

> **Embedded. Transactional. Cypher-native.**
>
> Engine codename: *Cypherite*

---

## Features

### Cypher Query Engine

A complete Cypher query language implementation with lexer, parser (AST), and executor:

| Category | Supported |
|----------|-----------|
| **Reading** | `MATCH`, `OPTIONAL MATCH`, variable-length path patterns `[*1..5]` with BFS traversal |
| **Writing** | `CREATE` (nodes & relationships with properties), `MERGE` with `ON MATCH`/`ON CREATE SET` |
| **Mutation** | `SET` (property update, label change `n:Label`), `DELETE`, `DETACH DELETE`, `REMOVE` |
| **Projection** | `WITH` (pipelined, with `DISTINCT`/`WHERE`), `UNWIND` |
| **Filtering** | `WHERE`: `=`, `<>`, `<`, `>`, `<=`, `>=`, `STARTS WITH`, `ENDS WITH`, `CONTAINS`, `IS NULL`/`IS NOT NULL`, `IN`, `AND`, `OR`, `NOT` |
| **Return** | Expressions, aliases (`AS`), `DISTINCT`, `ORDER BY` (`ASC`/`DESC`), `SKIP`, `LIMIT` |
| **Conditional** | `CASE` / `WHEN` / `THEN` / `ELSE` / `END` |
| **Aggregation** | `COUNT`, `SUM`, `AVG`, `MIN`, `MAX`, `COLLECT`, `COUNT(DISTINCT expr)` |
| **Functions** | `exists()`, `type()`, `id()`, `labels()`, `keys()`, `nodes()`, `relationships()` |

### B+ Tree Indexing Engine

Seven **order-160 B+ trees** power all graph operations:

| Tree | Key → Value | Purpose |
|------|-------------|---------|
| `vertex_tree` | `VertexId → VertexPayload` (44B) | Vertex primary lookup |
| `edge_tree` | `EdgeId → EdgePayload` (52B) | Edge primary lookup |
| `out_edge_tree` | `(SrcId, EdgeId) → EdgePayload` | Out-edge traversal |
| `in_edge_tree` | `(DstId, EdgeId) → EdgePayload` | In-edge traversal |
| `prop_tree` | `(OwnerId, PropKey) → PropValue` | Property access |
| `label_tree` | `(LabelId, EntityId) → EntityId` | Label indexing |
| `prop_index_tree` | `(LabelId, PropKey, PropValue, EntityId) → EntityId` | Inverted property index |

Design highlights:
- **MVCC version chains** — per-slot `VersionNodeEx` with `older_offset` linked list for snapshot isolation
- **Sibling pointers** — leaf pages linked bidirectionally for zero-cost sequential scans
- **Prefix scanning** — efficient property tree range lookups
- **Page splitting** — upward propagation with automatic tree height growth
- **Compaction & merging** — background GC compacts pages and merges sparse leaves

### Page Storage Manager

| Component | Detail |
|-----------|--------|
| **Page size** | 4 KB with 24-byte headers |
| **I/O model** | Memory-mapped (mmap on POSIX, `CreateFileMapping` on Windows) |
| **Crash safety** | Copy-on-Write (COW) shadow paging — atomic root-page switch |
| **Durability** | Optional Write-Ahead Log (WAL) for fine-grained fsync |
| **Large values** | Overflow page chains for properties exceeding 4 KB |
| **Corruption detection** | CRC32 checksum on every page |
| **Allocation** | Version-tracked freelist (`std::set<std::pair<TxId, PageId>>`) |

### MVCC Transaction System

| Property | Implementation |
|----------|----------------|
| **Isolation** | Snapshot Isolation — consistent read-ts snapshot per transaction |
| **Concurrent reads** | Unlimited read-only transactions, lock-free |
| **Write serialization** | Single writer with Optimistic Concurrency Control (OCC) |
| **Row locking** | Optional pessimistic mode (`vertex_locks_` / `edge_locks_`) |
| **Deadlock detection** | Wait-for graph with DFS cycle detection |
| **Background GC** | Reclaims MVCC versions older than minimum active read transaction |
| **Logical deletion** | Tombstone versions preserve visibility for concurrent readers |

### Bulk Import

```cpp
std::vector<BulkVertexInput> vertices;  // 10K entries
std::vector<BulkEdgeInput> edges;       // 9999 edges
TxId tx = db.BeginTransaction(false);
auto result = db.BulkInsert(tx, vertices, edges);  // ~40× faster than per-item tx
db.Commit(tx);
```

### Cross-Platform

| Platform | Status | Notes |
|----------|--------|-------|
| Linux (x86_64) | ✅ | POSIX mmap |
| Windows (x64, MSVC) | ✅ | `CreateFileMapping` + WAL |
| macOS | ⚠️ Untested | Should work via POSIX mmap |

---

## Quick Start

### Build

```bash
cmake -B build && cmake --build build
```

### Run Tests

```bash
cd build && ctest --output-on-failure
```

### C++ API Example

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
    auto name = db.GetVertexProperty(rtx, alice, "name");
    auto out  = db.GetOutEdges(rtx, alice);
    // name = "Alice", out[0] = {edge_id, bob, 1 }
    return 0;
}
```

### Cypher Query Example

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

## Architecture

```
┌──────────────────────────────────────────────────────┐
│                   Cypher Query Engine                 │
│   Lexer → Parser (AST) → Expression Evaluator         │
│   → QueryExecutor (MATCH/CREATE/MERGE/SET/DELETE)     │
├──────────────────────────────────────────────────────┤
│                   Graph Store Layer                    │
│   Vertex/Edge CRUD · Property Index · OCC Validation  │
│   Deadlock Detection · Row Locking · Bulk Import      │
├─────────────────────┬────────────────────────────────┤
│   B+ Tree Index     │    Page Manager                 │
│   (Order-160 ×7)    │   (4KB pages, COW, mmap)       │
│   MVCC Version      │   Overflow Chains, CRC32,       │
│   Chains · Sibling  │   Freelist, Online Compaction   │
│   Pointers · GC     │                                 │
├─────────────────────┴────────────────────────────────┤
│              WAL / Crash Recovery                     │
└──────────────────────────────────────────────────────┘
```

### Layer Breakdown

1. **Page Manager** (`page_manager.h/.cpp`) — Memory-mapped file I/O, page allocation/deallocation, COW shadow paging, overflow page chains, optional WAL, CRC32 checksums.

2. **B+ Tree** (`bplus_tree.h/.cpp`) — Order-160 B+ trees with MVCC version chains on every slot, leaf-page sibling pointers for range scans, prefix scan cursors, upward-propagating page splits, compaction and leaf merging.

3. **Serializer** (`serializer.h/.cpp`) — Binary key encoding for 7 tree indexes, `VertexPayload` (44B), `EdgePayload` (52B), property blob serialization, endian-aware conversion (`HostToBig`/`BigToHost`).

4. **Graph Store** (`graph_store.h/.cpp`) — Graph semantics managing all 7 trees, transaction lifecycle (Begin/Commit/Rollback), vertex/edge CRUD, property indexing, bulk import API, deadlock detection, row-level locking, background GC.

5. **Cypher Parser** (`cypher_parser.h/.cpp`) — Lexer with tokenization of keywords/symbols/numbers/strings/parameters, recursive-descent parser producing a `CypherQuery` AST, expression evaluator with precedence climbing, `QueryExecutor` orchestrating pattern matching, BFS traversal, filtering, projection, sorting, and pagination.

---

## Performance Benchmarks

Run the benchmark suite on your hardware:

```bash
./build/test_benchmark
```

| Benchmark | Configuration | What It Tests |
|-----------|---------------|---------------|
| **Parser: simple** | `MATCH (n:Person) WHERE n.age > 25 RETURN n.name, n.age` × 1000 | Basic query parsing throughput |
| **Parser: complex** | Multi-clause JOIN/WHERE/GROUP/ORDER/LIMIT × 1000 | Complex query parsing throughput |
| **Parser: CREATE** | `CREATE (n:Person {name:'Alice', age:30}) RETURN n` × 1000 | DML statement parsing |
| **Parser: error** | Invalid syntax × 1000 | Error-path parsing performance |
| **AddVertex** | Sequential insert, 1000 ops, each in own transaction | Per-item write throughput |
| **GetVertex** | Random lookup in 1000-vertex pool, 1000 ops | Point query latency |
| **SetVertexProperty** | Sequential property updates across 1000 vertices, 1000 ops | Property mutation throughput |
| **RemoveVertex** | Bulk delete 1000 vertices | Deletion throughput |
| **BulkInsert** | 10K vertices + 9,999 edges in single transaction | Batch import (~40× faster vs per-item) |
| **Concurrent parsers** | Same query on 1/2/4/8 threads, 2,000 ops each | Parser scalability |
| **Readers + Writer** | 4 concurrent readers + 1 writer, mixed Add/Remove | Read-under-write contention |
| **Full pipeline** | Parse + execute `MATCH (n) RETURN COUNT(n)` | End-to-end query latency |

*Numbers are hardware-dependent; run `./build/test_benchmark` on your target system for accurate results.*

---

## Project Structure

```
├── include/                       # Public headers
│   ├── bplus_tree.h               # B+ tree (MVCC version chains)
│   ├── common.h                   # Base types, serialization, status codes
│   ├── cypher_parser.h            # Cypher lexer, parser, query executor
│   ├── graph_store.h              # Graph store API (CRUD, transactions)
│   ├── page_manager.h             # Page manager (mmap, allocation, COW)
│   └── serializer.h               # Vertex/Edge payloads, key encoders
├── src/                           # Implementation
│   ├── bplus_tree.cpp
│   ├── common.cpp
│   ├── cypher_parser.cpp
│   ├── graph_store.cpp
│   ├── page_manager.cpp
│   └── serializer.cpp
├── tests/                         # Test suite (~280 tests)
│   ├── test_btree.cpp             # B+ tree unit tests
│   ├── test_graphstore.cpp        # Graph engine CRUD tests
│   ├── test_cypherparser.cpp      # Cypher parser tests
│   ├── test_fuzz.cpp              # Fuzz + crash recovery tests
│   ├── test_benchmark.cpp         # Performance benchmarks
│   └── crash_test.cpp             # Crash recovery validation
├── cmds/                          # CLI tools
│   ├── db_admin.cpp               # Database administration CLI
│   └── cypher_parser.cpp          # Standalone Cypher shell
├── docs/
│   ├── GraphDb.md                 # Deep-dive design document (zh-CN)
│   └── Plan.md                    # Improvement roadmap (P0-P3)
└── CMakeLists.txt                 # C++20, MSVC/GCC/Clang
```

---

## Platform Support

| Platform | Status | Notes |
|----------|--------|-------|
| Linux (x86_64) | ✅ | POSIX mmap |
| Windows (x64, MSVC) | ✅ | `CreateFileMapping` + WAL |
| macOS | ⚠️ Untested | Should work via POSIX mmap |

---

## License

MIT License. See the [LICENSE](LICENSE) file for details.
