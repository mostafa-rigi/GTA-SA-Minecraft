#pragma once
// The boss's map of San Andreas: the game's OWN pedestrian path nodes (nodes*.dat
// out of gta3.img), merged into one graph the core routes over. Built offline from
// the extracted files (see nav-assets/ and CS-BOSS-fa.md for the extraction proof);
// at runtime the bridge reads nav_graph.bin from the MinecraftSA folder and hands
// the bytes here. The core itself never touches the disk.
//
// File format (little-endian, written by the offline builder):
//   char[6] "MCNAV1"; u32 nodeCount; u32 edgeCount (directed);
//   nodes: nodeCount * (f32 x, f32 y, f32 z) - world positions, metres;
//   CSR: u32 rowOffsets[nodeCount + 1]; u32 targets[edgeCount];
//   trailer: u32 checksum = sum of all preceding bytes mod 2^32.
// A missing or corrupt file leaves the graph unloaded: the soldier falls back to
// his probe steering and the log says so loudly. The mod itself stays healthy.

#include "Core.h" // Vec3

#include <cstddef>
#include <vector>

namespace mc {

// Loads and validates the graph. Returns whether the graph is ready afterwards.
bool LoadNavGraph(const unsigned char* data, size_t size);
void UnloadNavGraph();
bool NavReady();
int NavNodeCount();
int NavEdgeCount();

// The graph node nearest to a world position (optionally only within maxDist).
// -1 when the graph is not ready or nothing is close enough.
int NavNearest(const Vec3& at, float maxDist);

// The world position of one node. False for a bad index.
bool NavNodePos(int node, Vec3* out);

// A* over the graph from the node nearest `from` to the node nearest `to`.
// Fills `out` with world waypoints (the target node last; `to` itself is NOT
// appended - the caller walks the last metres off-graph). False when no route
// exists (or the graph is not ready). A route whose ends snap to the same node
// yields that single node.
bool NavRoute(const Vec3& from, const Vec3& to, std::vector<Vec3>* out);
// The same route with a hard budget on A* node expansions: past the budget the
// search stops and the call fails, so a caller on a weak machine can bound what
// one repath may cost (the boss's combat navigation lives on this). avoidNode,
// when a valid node index, is never expanded or stepped onto (unless it is one
// of the route's own ends): the stuck-break escape hatch, so a repath after a
// stall does not walk straight back into the same pocket.
bool NavRouteBudget(const Vec3& from, const Vec3& to, std::vector<Vec3>* out, int maxExpand, int avoidNode = -1);
// Diagnostics for tests and the perf ledger: how many nodes the last route
// search expanded, and how many route searches have run since the graph loaded.
int NavLastRouteExpanded();
int NavRouteAttempts();
// The length of a route NavRoute produced, in metres.
float NavRouteLength(const std::vector<Vec3>& route);

// How many graph edges leave one node (its CSR row length); -1 for a bad
// index or an unloaded graph. Degree 1 = a dead end: where a running man
// gets cornered, and where the search now looks on purpose.
int NavNodeDegree(int node);

// ---------------------------------------------------------------------------
// The VEHICLE road graph: the other half of the game's nodes*.dat - the car
// network (30,587 nodes) the ped graph cannot see, built by
// nav-assets/build_vehicle_graph.py into vehicle_graph.bin ("MCVEH1", same
// layout spirit as MCNAV1: nodes carry a flood/segment id and a path width).
// It loads and routes NOW so the coming vehicle step starts taught; nothing
// in this build drives on it yet.
bool LoadVehicleGraph(const unsigned char* data, size_t size);
void UnloadVehicleGraph();
bool VehicleGraphReady();
int VehicleNodeCount();
int VehicleEdgeCount();
int VehicleNearestNode(const Vec3& at, float maxDist);
// A* over the road graph; fills world waypoints, and (when distOut is given)
// the route's length in metres. False when no road route exists.
bool VehicleRoute(const Vec3& from, const Vec3& to, std::vector<Vec3>* out, float* distOut);

// Named places the boss checks while hunting. Since the atlas landed these
// are no longer a hand-placed list: they are the atlas POI database itself
// (Atlas.h - extracted from the game's own files), and `at` is the place's
// DOOR where the data has one.
struct NavPoi {
    const char* name;
    Vec3 at;
};
int NavPoiCount();
const NavPoi& NavPoiAt(int i);
// The POI nearest to a position (index into NavPoiAt), or -1.
int NavNearestPoi(const Vec3& at);

} // namespace mc
