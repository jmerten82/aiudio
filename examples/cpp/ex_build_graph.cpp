// Example: build and validate a graph with the G1 IR, and run its nodes on a
// block by hand. There is no executor yet (that's G2) — this shows the IR + node
// contract directly, and how to test them. Cross-platform, no audio device.
//
//   ./ex_build_graph
//
// Graph:  GainNode(0.5) ┐
//                        ├─► SumNode(2) ─► (output)
//         GainNode(0.8) ┘

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/graph.hpp"
#include "aiudio/graph/sum_node.hpp"

using namespace aiudio::graph;

int main() {
    bool ok = true;

    // 1) Build a valid DAG and validate it.
    Graph g;
    const NodeId gainL = g.addNode(std::make_unique<GainNode>(0.5f));
    const NodeId gainR = g.addNode(std::make_unique<GainNode>(0.8f));
    const NodeId mixer = g.addNode(std::make_unique<SumNode>(2));
    g.connect(gainL, 0, mixer, 0);
    g.connect(gainR, 0, mixer, 1);

    std::printf("graph: %zu nodes, %zu edges\n", g.nodeCount(), g.edges().size());
    for (const Edge& e : g.edges()) {
        std::printf("  %s[%u] -> %s[%u]\n", g.node(e.src)->typeName(), e.srcPort,
                    g.node(e.dst)->typeName(), e.dstPort);
    }
    const auto v = g.validate();
    std::printf("validate(): %s%s\n", v.ok ? "OK" : "FAIL: ", v.ok ? "" : v.error.c_str());
    ok = ok && v.ok;

    // 2) Show validation catching a cycle and a bad port.
    {
        Graph bad;
        const NodeId a = bad.addNode(std::make_unique<GainNode>());
        const NodeId b = bad.addNode(std::make_unique<GainNode>());
        bad.connect(a, 0, b, 0);
        bad.connect(b, 0, a, 0);  // cycle
        const auto r = bad.validate();
        std::printf("cycle graph validate(): %s (%s)\n", r.ok ? "OK?!" : "rejected", r.error.c_str());
        ok = ok && !r.ok;
    }
    {
        Graph bad;
        const NodeId a = bad.addNode(std::make_unique<GainNode>());  // 1 out
        const NodeId b = bad.addNode(std::make_unique<GainNode>());
        bad.connect(a, 7, b, 0);  // output port 7 doesn't exist
        const auto r = bad.validate();
        std::printf("bad-port graph validate(): %s (%s)\n", r.ok ? "OK?!" : "rejected",
                    r.error.c_str());
        ok = ok && !r.ok;
    }

    // 3) Run the nodes on one block by hand (what the G2 executor will automate).
    constexpr std::uint32_t N = 8;
    float l[N], r[N], t0[N], t1[N], out[N];
    for (std::uint32_t i = 0; i < N; ++i) { l[i] = 1.0f; r[i] = 1.0f; }
    float* lCh[1] = {l};   float* rCh[1] = {r};
    float* t0Ch[1] = {t0}; float* t1Ch[1] = {t1}; float* outCh[1] = {out};
    AudioBuffer lBuf{lCh, 1, N}, rBuf{rCh, 1, N}, t0Buf{t0Ch, 1, N}, t1Buf{t1Ch, 1, N}, outBuf{outCh, 1, N};

    static_cast<GainNode*>(g.node(gainL))->process(&lBuf, &t0Buf, N, TimeInfo{});  // 1.0 * 0.5
    static_cast<GainNode*>(g.node(gainR))->process(&rBuf, &t1Buf, N, TimeInfo{});  // 1.0 * 0.8
    AudioBuffer mixIns[2] = {t0Buf, t1Buf};
    g.node(mixer)->process(mixIns, &outBuf, N, TimeInfo{});                        // 0.5 + 0.8

    std::printf("manual run: gainL=0.5, gainR=0.8 -> mix out[0] = %.3f (expect 1.300)\n", out[0]);
    ok = ok && (out[0] > 1.299f && out[0] < 1.301f);

    std::printf("\n%s\n", ok ? "all checks passed" : "FAILURES");
    return ok ? 0 : 1;
}
