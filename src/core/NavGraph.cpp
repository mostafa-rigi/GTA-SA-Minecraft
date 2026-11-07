#include "NavGraph.h"

#include <cmath>
#include <cstring>
#include <queue>

#include "Atlas.h"
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

// The vehicle road graph's state (the MCVEH1 file): same shape as the ped
// graph plus the per-node flood/segment id and path width the builder kept.
std::vector<Vec3> vNodes;
std::vector<uint8_t> vFlood;
std::vector<uint32_t> vRow;
std::vector<uint32_t> vTargets;

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

int NavNodeDegree(int node) {
    if (node < 0 || node >= (int)gNodes.size())
        return -1;
    return (int)(gRow[node + 1] - gRow[node]);
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
    // the search's scratch lives between calls: three node-sized arrays were
    // allocated and first-touched on EVERY route, and the very first route of
    // a session paid the whole pile at once inside one brain frame (a 111 ms
    // spawn tick was measured in the game). Reused, a route costs its search
    // and nothing else. The search is never re-entered, so sharing is safe.
    static std::vector<float> g;
    static std::vector<int> came;
    static std::vector<char> closed;
    if ((int)g.size() < n) {
        g.resize(n);
        came.resize(n);
        closed.resize(n);
    }
    std::fill(g.begin(), g.begin() + n, 1e18f);
    std::fill(came.begin(), came.begin() + n, -1);
    std::fill(closed.begin(), closed.begin() + n, (char)0);
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

// ---------------------------------------------------------------------------
// The vehicle road graph (vehicle_graph.bin, "MCVEH1"): the car half of the
// game's own nodes*.dat, extracted by nav-assets/build_vehicle_graph.py.
// Header: char[6] magic; u32 nodeCount; u32 edgeCount;
// nodes: nodeCount * (f32 x, y, z; u8 flood; u8 width; u16 reserved);
// CSR: u32 rowOffsets[nodeCount + 1]; u32 targets[edgeCount];
// trailer: u32 checksum (sum of every preceding byte, matching the builder).

bool LoadVehicleGraph(const unsigned char* data, size_t size) {
    UnloadVehicleGraph();
    if (!data || size < 14) {
        Log("vehicle_graph.bin: missing or empty - the road graph stays dark (the car step will need it)");
        return false;
    }
    uint32_t sum = 0;
    for (size_t i = 0; i + 4 < size; ++i)
        sum += data[i];
    Reader rd{ data, size };
    char magic[6];
    uint32_t nodes = 0, edges = 0, storedSum = 0;
    if (!rd.Get(magic) || std::memcmp(magic, "MCVEH1", 6) != 0 ||
        !rd.Get(nodes) || !rd.Get(edges) ||
        nodes == 0 || nodes > 200000 || edges > 2000000) {
        Log("vehicle_graph.bin: bad header - the road graph stays dark");
        return false;
    }
    vNodes.resize(nodes);
    vFlood.resize(nodes);
    for (uint32_t i = 0; i < nodes; ++i) {
        float x = 0, y = 0, z = 0;
        uint8_t flood = 0, width = 0;
        uint16_t res = 0;
        if (!rd.Get(x) || !rd.Get(y) || !rd.Get(z) || !rd.Get(flood) || !rd.Get(width) || !rd.Get(res)) {
            Log("vehicle_graph.bin: truncated node table - the road graph stays dark");
            UnloadVehicleGraph();
            return false;
        }
        vNodes[i] = Vec3(x, y, z);
        vFlood[i] = flood;
    }
    vRow.resize(nodes + 1);
    for (uint32_t i = 0; i <= nodes; ++i)
        if (!rd.Get(vRow[i])) {
            Log("vehicle_graph.bin: truncated CSR rows - the road graph stays dark");
            UnloadVehicleGraph();
            return false;
        }
    if (vRow[0] != 0 || vRow[nodes] != edges) {
        Log("vehicle_graph.bin: CSR does not add up - the road graph stays dark");
        UnloadVehicleGraph();
        return false;
    }
    vTargets.resize(edges);
    for (uint32_t i = 0; i < edges; ++i)
        if (!rd.Get(vTargets[i]) || vTargets[i] >= nodes) {
            Log("vehicle_graph.bin: truncated CSR targets - the road graph stays dark");
            UnloadVehicleGraph();
            return false;
        }
    if (!rd.Get(storedSum) || storedSum != sum) {
        Log("vehicle_graph.bin: checksum mismatch - the road graph stays dark");
        UnloadVehicleGraph();
        return false;
    }
    Log("vehicle graph loaded: %u road nodes, %u directed edges - the car network is in his head",
        nodes, edges);
    return true;
}

void UnloadVehicleGraph() {
    vNodes.clear();
    vFlood.clear();
    vRow.clear();
    vTargets.clear();
}

bool VehicleGraphReady() { return !vNodes.empty(); }
int VehicleNodeCount() { return (int)vNodes.size(); }
int VehicleEdgeCount() { return (int)vTargets.size(); }

int VehicleNearestNode(const Vec3& at, float maxDist) {
    int best = -1;
    float bd = maxDist * maxDist;
    for (size_t i = 0; i < vNodes.size(); ++i) {
        const float d = (vNodes[i] - at).LengthSq();
        if (d < bd) {
            bd = d;
            best = (int)i;
        }
    }
    return best;
}

bool VehicleRoute(const Vec3& from, const Vec3& to, std::vector<Vec3>* out, float* distOut) {
    if (!out || vNodes.empty())
        return false;
    out->clear();
    if (distOut)
        *distOut = 0.0f;
    const int start = VehicleNearestNode(from, 80.0f);
    const int goal = VehicleNearestNode(to, 80.0f);
    if (start < 0 || goal < 0)
        return false;
    if (start == goal) {
        out->push_back(vNodes[goal]);
        return true;
    }
    const int n = (int)vNodes.size();
    static std::vector<float> g;
    static std::vector<int> came;
    static std::vector<char> closed;
    if ((int)g.size() < n) {
        g.resize(n);
        came.resize(n);
        closed.resize(n);
    }
    std::fill(g.begin(), g.begin() + n, 1e18f);
    std::fill(came.begin(), came.begin() + n, -1);
    std::fill(closed.begin(), closed.begin() + n, (char)0);
    typedef std::pair<float, int> QN;
    std::priority_queue<QN, std::vector<QN>, std::greater<QN>> open;
    g[start] = 0.0f;
    open.push(QN((vNodes[start] - vNodes[goal]).Length(), start));
    while (!open.empty()) {
        const QN cur = open.top();
        open.pop();
        const int u = cur.second;
        if (closed[u])
            continue;
        closed[u] = 1;
        if (u == goal)
            break;
        for (uint32_t e = vRow[u]; e < vRow[u + 1]; ++e) {
            const int v = (int)vTargets[e];
            const float w = (vNodes[v] - vNodes[u]).Length();
            if (g[u] + w < g[v]) {
                g[v] = g[u] + w;
                came[v] = u;
                open.push(QN(g[v] + (vNodes[v] - vNodes[goal]).Length(), v));
            }
        }
    }
    if (came[goal] < 0)
        return false;
    std::vector<Vec3> rev;
    for (int u = goal; u >= 0; u = came[u]) {
        rev.push_back(vNodes[u]);
        if (u == start)
            break;
    }
    out->assign(rev.rbegin(), rev.rend());
    if (distOut)
        *distOut = g[goal];
    return true;
}

// The named places are the atlas now (Atlas.h): the hand-placed checkpoint
// list is gone, replaced by the extracted database. `at` is the place's
// door - the point a search actually walks to.
int NavPoiCount() { return AtlasPoiCount(); }

const NavPoi& NavPoiAt(int i) {
    static NavPoi sPoi{ "", Vec3(0, 0, 0) };
    AtlasPoiInfo info;
    if (AtlasPoiGet(i, info)) {
        sPoi.name = info.name;
        sPoi.at = info.door;
    }
    return sPoi;
}

int NavNearestPoi(const Vec3& at) {
    int best = -1;
    float bd = 1e18f;
    for (int i = 0; i < AtlasPoiCount(); ++i) {
        AtlasPoiInfo info;
        if (!AtlasPoiGet(i, info))
            break;
        const float d = (info.door - at).LengthSq();
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

} // namespace mc
