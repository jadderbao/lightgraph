#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <random>
#include <filesystem>
#include "cypher_parser.h"
#include "graph_store.h"

using namespace graphstore;
using ns_t = std::chrono::nanoseconds;

static double to_double_ns(const std::chrono::steady_clock::duration& dur) {
    return (double)std::chrono::duration_cast<ns_t>(dur).count();
}

struct Timer {
    std::chrono::steady_clock::time_point start;
    const char* name;
    Timer(const char* n) : start(std::chrono::steady_clock::now()), name(n) {}
    ~Timer() {
        double ns = to_double_ns(std::chrono::steady_clock::now() - start);
        printf("  %-40s %8.0f ns  (%6.2f ms)\n", name, ns, ns / 1e6);
    }
};

struct BenchGuard {
    std::string path;
    ~BenchGuard() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

static std::string db_path_for(const std::string& label) {
    return "/tmp/graphdbtest_bench_" + label;
}

static std::unique_ptr<GraphStore> make_db(const std::string& label) {
    std::string path = db_path_for(label);
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    auto db = std::make_unique<GraphStore>();
    auto st = db->Init(path, 64ULL << 20);
    if (!IsOk(st)) { std::cerr << "Init failed: " << path << std::endl; std::abort(); }
    return db;
}

const int WARMUP = 100;
const int ITERS = 1000;

// ==================== Parser benchmarks ====================

void bench_parser_simple() {
    printf("[Parser] simple MATCH-RETURN:\n");
    for (int i = 0; i < WARMUP; ++i) {
        auto q = Parser("MATCH (n:Person) WHERE n.age > 25 RETURN n.name, n.age").ParseQuery();
    }
    { Timer t("parse MATCH-RETURN");
        for (int i = 0; i < ITERS; ++i) {
            auto q = Parser("MATCH (n:Person) WHERE n.age > 25 RETURN n.name, n.age").ParseQuery();
            (void)q;
        }
    }
}

void bench_parser_complex() {
    printf("[Parser] complex multi-clause:\n");
    const char* query =
        "MATCH (a:Person)-[:KNOWS]->(b:Person) "
        "WHERE a.age > 20 AND b.name STARTS WITH 'A' "
        "RETURN a.name AS name, COUNT(b) AS friends "
        "ORDER BY friends DESC LIMIT 10";
    for (int i = 0; i < WARMUP; ++i) {
        auto q = Parser(query).ParseQuery();
    }
    { Timer t("parse multi-clause");
        for (int i = 0; i < ITERS; ++i) {
            auto q = Parser(query).ParseQuery();
            (void)q;
        }
    }
}

void bench_parser_create() {
    printf("[Parser] CREATE pattern:\n");
    for (int i = 0; i < WARMUP; ++i) {
        auto q = Parser("CREATE (n:Person {name: 'Alice', age: 30}) RETURN n").ParseQuery();
    }
    { Timer t("parse CREATE pattern");
        for (int i = 0; i < ITERS; ++i) {
            auto q = Parser("CREATE (n:Person {name: 'Alice', age: 30}) RETURN n").ParseQuery();
            (void)q;
        }
    }
}

void bench_parser_error() {
    printf("[Parser] error path:\n");
    for (int i = 0; i < WARMUP; ++i) {
        auto q = Parser("MATCH (n INVALID SYNTAX").ParseQuery();
    }
    { Timer t("parse error path");
        for (int i = 0; i < ITERS; ++i) {
            auto q = Parser("MATCH (n INVALID SYNTAX").ParseQuery();
            (void)q;
        }
    }
}

// ==================== GraphStore benchmarks ====================

void bench_store_insert_sequential() {
    printf("[GraphStore] sequential insert:\n");
    BenchGuard bg{db_path_for("insert_seq")};
    auto db = make_db("insert_seq");
    std::vector<VertexId> vids;
    for (int i = 0; i < WARMUP; ++i) {
        TxId tx = db->BeginTransaction(false);
        db->AddVertex(tx, 1);
        db->Commit(tx);
    }
    { Timer t("AddVertex sequential");
        for (int i = 0; i < ITERS; ++i) {
            TxId tx = db->BeginTransaction(false);
            VertexId vid = db->AddVertex(tx, 1);
            db->Commit(tx);
            vids.push_back(vid);
        }
    }
    { Timer t("RemoveVertex sequential");
        for (VertexId vid : vids) {
            TxId tx = db->BeginTransaction(false);
            db->RemoveVertex(tx, vid);
            db->Commit(tx);
        }
    }
}

void bench_store_get_vertex() {
    printf("[GraphStore] random GetVertex:\n");
    BenchGuard bg{db_path_for("get_vertex")};
    auto db = make_db("get_vertex");
    std::vector<VertexId> vids;
    for (int i = 0; i < 1000; ++i) {
        TxId tx = db->BeginTransaction(false);
        vids.push_back(db->AddVertex(tx, 1));
        db->Commit(tx);
    }
    std::mt19937 rng(42);
    for (int i = 0; i < WARMUP; ++i) {
        TxId tx = db->BeginTransaction(true);
        db->GetVertex(tx, vids[rng() % vids.size()]);
    }
    { Timer t("GetVertex random");
        rng.seed(42);
        for (int i = 0; i < ITERS; ++i) {
            TxId tx = db->BeginTransaction(true);
            db->GetVertex(tx, vids[rng() % vids.size()]);
        }
    }
}

void bench_store_count() {
    printf("[GraphStore] GetVertexCount:\n");
    BenchGuard bg{db_path_for("count")};
    auto db = make_db("count");
    for (int i = 0; i < 1000; ++i) {
        TxId tx = db->BeginTransaction(false);
        db->AddVertex(tx, 1);
        db->Commit(tx);
    }
    { Timer t("GetVertexCount");
        TxId count_tx = db->BeginTransaction(true);
        for (int i = 0; i < ITERS; ++i) {
            size_t count = db->GetVertexCount(count_tx);
            (void)count;
        }
    }
}

void bench_store_update_property() {
    printf("[GraphStore] update property:\n");
    BenchGuard bg{db_path_for("update")};
    auto db = make_db("update");
    std::vector<VertexId> vids;
    const int N = 1000;
    for (int i = 0; i < N; ++i) {
        TxId tx = db->BeginTransaction(false);
        Properties p;
        p.Set("name", std::to_string(i));
        p.Set("val", "0");
        vids.push_back(db->AddVertex(tx, i + 1, p));
        db->Commit(tx);
    }
    for (int i = 0; i < WARMUP; ++i) {
        TxId tx = db->BeginTransaction(false);
        db->SetVertexProperty(tx, vids[i % N], "val", "1");
        db->Commit(tx);
    }
    { Timer t("SetVertexProperty sequential");
        for (int i = 0; i < ITERS; ++i) {
            TxId tx = db->BeginTransaction(false);
            db->SetVertexProperty(tx, vids[i % N], "val", std::to_string(i));
            db->Commit(tx);
        }
    }
}

void bench_store_delete_sequential() {
    printf("[GraphStore] sequential delete:\n");
    BenchGuard bg{db_path_for("delete")};
    auto db = make_db("delete");
    std::vector<VertexId> vids;
    const int N = 1100;
    for (int i = 0; i < N; ++i) {
        TxId tx = db->BeginTransaction(false);
        vids.push_back(db->AddVertex(tx, i + 1));
        db->Commit(tx);
    }
    for (int i = 0; i < WARMUP; ++i) {
        TxId tx = db->BeginTransaction(false);
        db->RemoveVertex(tx, vids[i]);
        db->Commit(tx);
    }
    { Timer t("RemoveVertex bulk");
        for (int i = WARMUP; i < WARMUP + ITERS; ++i) {
            TxId tx = db->BeginTransaction(false);
            db->RemoveVertex(tx, vids[i]);
            db->Commit(tx);
        }
    }
}

// ==================== Executor benchmarks ====================

void bench_executor_parse_and_execute() {
    printf("[Executor] parse + execute (pipelines):\n");
    BenchGuard bg{db_path_for("exec_pe")};
    auto db = make_db("exec_pe");
    for (int i = 0; i < 100; ++i) {
        TxId tx = db->BeginTransaction(false);
        db->AddVertex(tx, 1);
        db->Commit(tx);
    }
    for (int i = 0; i < WARMUP / 4; ++i) {
        auto q = Parser("MATCH (n) RETURN COUNT(n) AS cnt").ParseQuery();
        if (!q) continue;
        QueryExecutor exec(db.get());
        exec.Execute(*q);
    }
    { Timer t("parse + execute pipeline");
        for (int i = 0; i < ITERS / 2; ++i) {
            auto q = Parser("MATCH (n) RETURN COUNT(n) AS cnt").ParseQuery();
            if (q) { QueryExecutor exec(db.get()); exec.Execute(*q); }
        }
    }
}

// ==================== Concurrent benchmarks ====================

void bench_concurrent_parsers() {
    printf("[Concurrent] parser only (N threads):\n");
    const int OPS = 2000;
    int hc = (int)std::thread::hardware_concurrency();
    if (hc > 8) hc = 8;
    for (int nth : {1, 2, 4, hc}) {
        if (nth > hc) continue;
        std::atomic<int> done{0};
        auto worker = [&]() {
            for (int i = 0; i < OPS; ++i) {
                auto q = Parser("MATCH (a:Person)-[:KNOWS]->(b:Person) "
                                "WHERE a.age > b.age "
                                "RETURN a.name, b.name, a.age - b.age AS diff "
                                "ORDER BY diff DESC").ParseQuery();
                (void)q;
            }
            done++;
        };
        std::vector<std::thread> threads;
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < nth; ++i) threads.emplace_back(worker);
        for (auto& t : threads) t.join();
        double ns = to_double_ns(std::chrono::steady_clock::now() - t0);
        long total_ops = (long)nth * OPS;
        printf("  %-36s %3d threads  %6.0f ns/op  (%ld ops in %.0f ms)\n",
               "parse multi-clause", nth, ns / total_ops,
               total_ops, ns / 1e6);
    }
}

void bench_concurrent_readers() {
    printf("[Concurrent] readers + single writer:\n");
    BenchGuard bg{db_path_for("concur_rw")};
    auto db = make_db("concur_rw");
    for (int i = 0; i < 200; ++i) {
        TxId tx = db->BeginTransaction(false);
        db->AddVertex(tx, 1);
        db->Commit(tx);
    }
    std::atomic<bool> stop{false};
    std::atomic<int> read_ops{0};
    const int WRITE_OPS = 1000;

    auto reader = [&]() {
        while (!stop) {
            TxId tx = db->BeginTransaction(true);
            db->GetVertexCount(tx);
            read_ops++;
        }
    };
    auto writer = [&]() {
        std::mt19937 rng(99);
        std::vector<VertexId> mine;
        for (int i = 0; i < WRITE_OPS; ++i) {
            int op = rng() % 10;
            if (op < 6) {
                TxId tx = db->BeginTransaction(false);
                mine.push_back(db->AddVertex(tx, 1));
                db->Commit(tx);
            } else if (!mine.empty()) {
                size_t idx = rng() % mine.size();
                VertexId d = mine[idx];
                mine[idx] = mine.back();
                mine.pop_back();
                TxId tx = db->BeginTransaction(false);
                db->RemoveVertex(tx, d);
                db->Commit(tx);
            }
        }
        for (auto v : mine) {
            for (int retry = 0; retry < 5; ++retry) {
                TxId tx = db->BeginTransaction(false);
                if (db->RemoveVertex(tx, v)) break;
                db->Commit(tx);
            }
        }
        stop = true;
    };

    int nreaders = 4;
    if (nreaders > (int)std::thread::hardware_concurrency())
        nreaders = (int)std::thread::hardware_concurrency();
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> readers;
    for (int i = 0; i < nreaders; ++i) readers.emplace_back(reader);
    std::thread w(writer);
    w.join();
    for (auto& t : readers) t.join();
    double ns = to_double_ns(std::chrono::steady_clock::now() - t0);
    printf("  %-36s %3d readers + writer  %6.0f ns/read  (%d reads in %.0f ms)\n",
           "concurrent GetVertexCount", nreaders,
           ns / read_ops.load(), read_ops.load(), ns / 1e6);
}

// ==================== Main ====================

// ==================== Bulk Insert Benchmark ====================

void bench_bulk_insert() {
    printf("[GraphStore] bulk insert:\n");
    BenchGuard bg{db_path_for("bulk_insert")};
    {
        auto db = make_db("bulk_insert");
        const size_t BULK_SIZE = 10000;
        std::vector<GraphStore::BulkVertexInput> vertices;
        std::vector<GraphStore::BulkEdgeInput> edges;
        for (size_t i = 0; i < BULK_SIZE; i++) {
            GraphStore::BulkVertexInput v;
            v.label = static_cast<LabelId>(1 + (i % 10));
            if (i % 5 == 0) v.props.Set("name", "v" + std::to_string(i));
            vertices.push_back(std::move(v));
        }
        for (size_t i = 1; i < BULK_SIZE; i++) {
            GraphStore::BulkEdgeInput e;
            e.src = static_cast<VertexId>(i - 1);
            e.dst = static_cast<VertexId>(i);
            e.label = 1;
            edges.push_back(std::move(e));
        }
        { Timer t("BulkInsert 10k vertices + 9999 edges");
            TxId tx = db->BeginTransaction(false);
            auto r = db->BulkInsert(tx, vertices, edges);
            db->Commit(tx);
            printf("    -> %zu vertices, %zu edges\n", r.vertex_ids.size(), r.edge_ids.size());
        }
    }
    // Compare with per-item inserts (smaller batch for speed)
    {
        auto db = make_db("bulk_insert_ref");
        const size_t SMALL = 1000;
        { Timer t("AddVertex x 1000 (per-item tx)");
            for (size_t i = 0; i < SMALL; i++) {
                TxId tx = db->BeginTransaction(false);
                db->AddVertex(tx, static_cast<LabelId>(1 + (i % 10)));
                db->Commit(tx);
            }
        }
        { Timer t("BulkInsert x 1000 (single tx)");
            std::vector<GraphStore::BulkVertexInput> vs;
            for (size_t i = 0; i < SMALL; i++) {
                GraphStore::BulkVertexInput v;
                v.label = static_cast<LabelId>(1 + (i % 10));
                vs.push_back(std::move(v));
            }
            TxId tx = db->BeginTransaction(false);
            db->BulkInsert(tx, vs, {});
            db->Commit(tx);
        }
    }
}

int main() {
    std::cout << "=== Performance Benchmarks ===\n" << std::endl;

    bench_parser_simple();
    bench_parser_complex();
    bench_parser_create();
    bench_parser_error();
    std::cout << std::endl;

    bench_store_insert_sequential();
    bench_store_update_property();
    bench_store_delete_sequential();
    bench_store_get_vertex();
    bench_store_count();
    bench_bulk_insert();
    std::cout << std::endl;

    bench_executor_parse_and_execute();
    std::cout << std::endl;

    bench_concurrent_parsers();
    bench_concurrent_readers();

    std::cout << "\nDone." << std::endl;
    return 0;
}
