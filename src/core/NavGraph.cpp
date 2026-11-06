#include "NavGraph.h"

#include <cmath>
#include <cstring>
#include <queue>

#include "Core.h" // Log

namespace mc {
namespace {

std::vector<Vec3> gNodes;
std::vector<uint32_t> gRow; // CSR offsets, nodeCount + 1
std::vector<uint32_t> gTargets;
int gLastExpanded = 0;  // diag: nodes the last route search closed
int gRouteAttempts = 0; // diag: route searches since the graph loaded

struct Reader {
    const unsigned char* p = nullptr;
    size_t left = 0;
    bool Take(void* out, size_t n) {
        if (n > left)
            return false;
        std::memcpy(out, p, n);
        p += n;
        left -= n;
        return true;
    }
    template <typename T> bool Get(T& v) { return Take(&v, sizeof(T)); }
};

// Los Santos places of interest: where a hunted man runs, hides, heals or re-arms.
// Coordinates from documented San Andreas map knowledge (approximate, +-20 m) -
// they aim a search, they do not pretend to be door positions.
const NavPoi kPois[] = {
    { "Grove Street", Vec3(2480, -1660, 13) },
    { "Johnson safehouse", Vec3(2495, -1688, 14) },
    { "County General Hospital", Vec3(2030, -1410, 13) },
    { "All Saints Hospital", Vec3(1240, -1330, 13) },
    { "LSPD headquarters", Vec3(1540, -1670, 25) },
    { "Ammu-Nation Market", Vec3(1365, -1278, 14) },
    { "Idlewood Pay'n'Spray", Vec3(2070, -1830, 11) },
};

} // namespace

bool LoadNavGraph(const unsigned char* data, size_t size) {
    UnloadNavGraph();
    if (!data || size < 14) {
        Log("nav_graph.bin: missing or empty - the soldier has no map and will steer by probes only");
        return false;
    }
    uint32_t sum = 0;
    for (size_t i = 0; i + 4 < size; ++i)
        sum += data[i];
    Reader rd{ data, size };
    char magic[6];
    uint32_t nodes = 0, edges = 0, storedSum = 0;
    if (!rd.Get(magic) || std::memcmp(magic, "MCNAV1", 6) != 0 || !rd.Get(nodes) || !rd.Get(edges) ||
        nodes == 0 || nodes > 200000 || edges > 2000000) {
        Log("nav_graph.bin: bad header - the soldier has no map and will steer by probes only");
        return false;
    }
    const size_t expect = 6 + 4 + 4 + (size_t)nodes * 12 + ((size_t)nodes + 1) * 4 + (size_t)edges * 4 + 4;
    if (expect != size) {
        Log("nav_graph.bin: size %u does not match its header (%u nodes, %u edges) - no map loaded",
            (unsigned)size, nodes, edges);
        return false;
    }
    std::vector<Vec3> nn(nodes);
    for (uint32_t i = 0; i < nodes; ++i) {
        if (!rd.Get(nn[i].x) || !rd.Get(nn[i].y) || !rd.Get(nn[i].z) ||
            nn[i].x < -6100.0f || nn[i].x > 6100.0f || nn[i].y < -6100.0f || nn[i].y > 6100.0f ||
            nn[i].z < -100.0f || nn[i].z > 2100.0f) { // the data range: streets at -46, interior heavens at 2023
            Log("nav_graph.bin: node %u out of the map's bounds - no map loaded", i);
            return false;
        }
    }
    std::vector<uint32_t> row(nodes + 1);
    for (uint32_t i = 0; i <= nodes; ++i)
        if (!rd.Get(row[i])) {
            Log("nav_graph.bin: truncated offsets - no map loaded");
            return false;
        }
    if (row[0] != 0 || row[nodes] != edges) {
        Log("nav_graph.bin: offsets do not bound the edge list - no map loaded");
        return false;
    }
    for (uint32_t i = 1; i <= nodes; ++i)
        if (row[i] < row[i - 1]) {
            Log("nav_graph.bin: offsets run backwards - no map loaded");
            return false;
        }
    std::vector<uint32_t> tg(edges);
    for (uint32_t i = 0; i < edges; ++i) {
        if (!rd.Get(tg[i]) || tg[i] >= nodes) {
            Log("nav_graph.bin: edge %u points outside the graph - no map loaded", i);
            return false;
        }
    }
    if (!rd.Get(storedSum) || storedSum != sum) {
        Log("nav_graph.bin: checksum mismatch - no map loaded");
        return false;
    }
    gNodes = std::move(nn);
    gRow = std::move(row);
    gTargets = std::move(tg);
    Log("nav_graph.bin loaded: %d ped nodes, %d directed edges - the soldier knows the map",
        (int)gNodes.size(), (int)gTargets.size());
    return true;
}

void UnloadNavGraph() {
    gNodes.clear();
    gRow.clear();
    gTargets.clear();
    gLastExpanded = 0;
    gRouteAttempts = 0;
}

bool NavReady() { return !gNodes.empty(); }
int NavNodeCount() { return (int)gNodes.size(); }
int NavEdgeCount() { return (int)gTargets.size(); }

float NavRouteLength(const std::vector<Vec3>& route) {
    float total = 0.0f;
    for (size_t i = 1; i < route.size(); ++i)
        total += (route[i] - route[i - 1]).Length();
    return total;
}

int NavNearest(const Vec3& at, float maxDist) {
    if (gNodes.empty())
        return -1;
    int best = -1;
    float bd = maxDist * maxDist;
    for (size_t i = 0; i < gNodes.size(); ++i) {
        const float d = (gNodes[i] - at).LengthSq();
        if (d < bd) {
            bd = d;
            best = (int)i;
        }
    }
    return best;
}

bool NavNodePos(int node, Vec3* out) {
    if (node < 0 || node >= (int)gNodes.size() || !out)
        return false;
    *out = gNodes[node];
    return true;
}

bool NavRoute(const Vec3& from, const Vec3& to, std::vector<Vec3>* out) {
    return NavRouteBudget(from, to, out, 1 << 29, -1); // the unbounded original, for calm callers
}

int NavLastRouteExpanded() { return gLastExpanded; }
int NavRouteAttempts() { return gRouteAttempts; }

bool NavRouteBudget(const Vec3& from, const Vec3& to, std::vector<Vec3>* out, int maxExpand, int avoidNode) {
    if (!out || gNodes.empty())
        return false;
    out->clear();
    ++gRouteAttempts;
    gLastExpanded = 0;
    const int start = NavNearest(from, 60.0f);
    const int goal = NavNearest(to, 60.0f);
    if (start < 0 || goal < 0)
        return false;
    if (start == goal) {
        out->push_back(gNodes[goal]);
        return true;
    }
    if (avoidNode == start || avoidNode == goal)
        avoidNode = -1; // a route's own ends are never banned
    const int n = (int)gNodes.size();
    std::vector<float> g(n, 1e18f);
    std::vector<int> came(n, -1);
    std::vector<char> closed(n, 0);
    typedef std::pair<float, int> QN;
    std::priority_queue<QN, std::vector<QN>, std::greater<QN>> open;
    g[start] = 0.0f;
    open.push(QN((gNodes[start] - gNodes[goal]).Length(), start));
    while (!open.empty()) {
        const QN cur = open.top();
        open.pop();
        const int u = cur.second;
        if (closed[u])
            continue;
        closed[u] = 1;
        ++gLastExpanded;
        if (u == goal)
            break;
        if (gLastExpanded >= maxExpand)
            return false; // the budget is a wall, not a suggestion: the caller falls back to probe steering
        for (uint32_t e = gRow[u]; e < gRow[u + 1]; ++e) {
            const int v = (int)gTargets[e];
            if (v == avoidNode)
                continue; // the pocket he just stalled in does not get a second chance this repath
            const float w = (gNodes[v] - gNodes[u]).Length();
            if (g[u] + w < g[v]) {
                g[v] = g[u] + w;
                came[v] = u;
                open.push(QN(g[v] + (gNodes[v] - gNodes[goal]).Length(), v));
            }
        }
    }
    if (came[goal] < 0 && goal != start)
        return false;
    std::vector<Vec3> rev;
    for (int u = goal; u >= 0; u = came[u]) {
        rev.push_back(gNodes[u]);
        if (u == start)
            break;
    }
    out->assign(rev.rbegin(), rev.rend());
    return true;
}

int NavPoiCount() { return (int)(sizeof(kPois) / sizeof(kPois[0])); }
const NavPoi& NavPoiAt(int i) { return kPois[i]; }

int NavNearestPoi(const Vec3& at) {
    int best = -1;
    float bd = 1e18f;
    for (int i = 0; i < NavPoiCount(); ++i) {
        const float d = (kPois[i].at - at).LengthSq();
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

} // namespace mc
