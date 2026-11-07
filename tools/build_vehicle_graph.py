#!/usr/bin/env python3
"""Build vehicle_graph.bin (MCVEH1) from the parsed nodes*.dat files.

The vehicle sibling of build_graph.py: same proven readers, same
two-reader digest gate, but the VEHICLE records of every area file
instead of the ped ones. Measured structure of the vehicle data
(2026-11-07, atlas work): links never cross flood kinds - each flood
id is a sealed subgraph. flood 1 = the road network (27,083 nodes),
flood 2 = boats (1,507), floods 3..33 = small sealed segments
(special/test/mission networks). Stored links are 100% reciprocal
(the whole-corpus digest), so the CSR is the undirected union, like
the ped graph; the direction a car actually drives is a runtime rule
of the game, not stored data.

Binary layout (little-endian):
  char[6] "MCVEH1"
  u32 nodeCount, u32 edgeCount (directed CSR entries)
  nodes: nodeCount x (f32 x, f32 y, f32 z, u8 floodKind, u8 pathWidth,
                      u16 zero)   (16 bytes)
  CSR: u32 rowOffsets[nodeCount + 1]; u32 targets[edgeCount]
  trailer: u32 checksum = sum of all preceding bytes mod 2^32
"""

import glob
import json
import os
import struct
import sys
from collections import deque

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import reader1
import reader2

NODES_DIR = os.path.join(HERE, "nodes")
REPO = os.path.normpath(os.path.join(HERE, "..", "src-repo"))
BIN_OUT = os.path.join(REPO, "cs-soldier", "vehicle_graph.bin")
SUMMARY_OUT = os.path.join(HERE, "vehicle_graph_summary.json")


def main():
    d1 = reader1.digest(NODES_DIR)
    d2 = reader2.digest(NODES_DIR)
    if d1 != d2:
        print("ABORT: readers disagree")
        return 1
    print("two-reader digest agreement: True")

    paths = sorted(glob.glob(os.path.join(NODES_DIR, "nodes*.dat")),
                   key=lambda p: int(os.path.basename(p)[5:-4]))
    parsed = {}
    for p in paths:
        f = reader1.parse_file(p)
        parsed[f["area"]] = f

    coords = []
    kinds = []
    index_of = {}
    for area in sorted(parsed):
        f = parsed[area]
        for node in f["nodes"]:
            if node["is_ped"]:
                continue
            idx = len(coords)
            index_of[(area, node["node_id"])] = idx
            coords.append((node["x"], node["y"], node["z"]))
            kinds.append((node["flood_id"], node["path_width"]))
    n = len(coords)

    adjacency = [set() for _ in range(n)]
    directed = set()
    self_loops = 0
    for area in sorted(parsed):
        f = parsed[area]
        src_is_veh = {nd["index"]: not nd["is_ped"] for nd in f["nodes"]}
        for src, ta, tn in f["links"]:
            if not src_is_veh[src]:
                continue
            target = parsed[ta]["nodes"][tn]
            if target["is_ped"]:
                continue
            directed.add((index_of[(area, src)], index_of[(ta, tn)]))
    asym = sum(1 for (a, b) in directed if (b, a) not in directed)
    pairs = set()
    for a, b in directed:
        if a == b:
            self_loops += 1
            continue
        pairs.add((a, b) if a < b else (b, a))
    for a, b in pairs:
        adjacency[a].add(b)
        adjacency[b].add(a)

    row_offsets = [0] * (n + 1)
    targets = []
    for i in range(n):
        row_offsets[i + 1] = row_offsets[i] + len(adjacency[i])
        targets.extend(sorted(adjacency[i]))
    edge_count = len(targets)

    blob = bytearray()
    blob += b"MCVEH1"
    blob += struct.pack("<II", n, edge_count)
    for (x, y, z), (fl, w) in zip(coords, kinds):
        blob += struct.pack("<fffBBH", x, y, z, fl, w, 0)
    blob += struct.pack("<%dI" % len(row_offsets), *row_offsets)
    blob += struct.pack("<%dI" % len(targets), *targets)
    checksum = sum(blob) & 0xFFFFFFFF
    blob += struct.pack("<I", checksum)
    with open(BIN_OUT, "wb") as fh:
        fh.write(blob)

    # Per-kind components + the headline route proof: Grove Street to
    # Los Santos airport ON ROADS (flood 1) - the route the ped graph
    # famously cannot make (the airport is a ped island).
    def bfs_component(start):
        seen = {start}
        dq = deque([start])
        while dq:
            u = dq.popleft()
            for v in adjacency[u]:
                if v not in seen:
                    seen.add(v)
                    dq.append(v)
        return seen

    def nearest(x, y, want_kind=None):
        best, bd = -1, 1e18
        for i, (cx, cy, _cz) in enumerate(coords):
            if want_kind is not None and kinds[i][0] != want_kind:
                continue
            dd = (cx - x) ** 2 + (cy - y) ** 2
            if dd < bd:
                bd, best = dd, i
        return best

    def route(a, b):
        # Dijkstra over CSR (edges weighted by 3D distance)
        import heapq
        dist = {a: 0.0}
        prev = {}
        pq = [(0.0, a)]
        while pq:
            du, u = heapq.heappop(pq)
            if u == b:
                break
            if du > dist.get(u, 1e18):
                continue
            for v in adjacency[u]:
                w = ((coords[u][0] - coords[v][0]) ** 2 +
                     (coords[u][1] - coords[v][1]) ** 2 +
                     (coords[u][2] - coords[v][2]) ** 2) ** 0.5
                nd = du + w
                if nd < dist.get(v, 1e18):
                    dist[v] = nd
                    prev[v] = u
                    heapq.heappush(pq, (nd, v))
        if b not in dist:
            return None, None
        hops = 0
        u = b
        while u != a:
            u = prev[u]
            hops += 1
        return dist[b], hops

    grove = nearest(2480, -1660, 1)
    airport = nearest(-1420, -330, 1)
    rd, hops = route(grove, airport)
    kind_stats = {}
    for k in sorted({fl for fl, _w in kinds}):
        members = [i for i in range(n) if kinds[i][0] == k]
        comp = bfs_component(members[0])
        kind_stats[str(k)] = {"nodes": len(members),
                              "largest_component": len(comp)}
    degrees = [len(a) for a in adjacency]
    summary = {
        "source": "nodes*.dat (same corpus as nav_graph.bin, sha-verified)",
        "nodes": n,
        "csr_directed_edges": edge_count,
        "stored_directed_edges": len(directed),
        "asymmetric_stored_edges": asym,
        "self_loops_dropped": self_loops,
        "degree_min": min(degrees),
        "degree_avg": round(sum(degrees) / n, 3),
        "degree_max": max(degrees),
        "kinds": kind_stats,
        "route_proof": {
            "from": "Grove Street (2480,-1660) nearest road node %d" % grove,
            "to": "LS airport (-1420,-330) nearest road node %d" % airport,
            "distance_m": None if rd is None else round(rd, 1),
            "hops": hops,
        },
        "bin_bytes": len(blob),
        "bin_sha256": __import__("hashlib").sha256(blob).hexdigest(),
        "checksum": checksum,
    }
    with open(SUMMARY_OUT, "w") as fh:
        json.dump(summary, fh, indent=1)
    print("vehicle nodes:", n, "csr edges:", edge_count,
          "asymmetric stored:", asym)
    print("Grove->LS airport by road:", summary["route_proof"])
    print("wrote", BIN_OUT, len(blob), "bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
