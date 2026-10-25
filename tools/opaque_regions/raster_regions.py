"""Discrete palette labeling and energy-driven connected-region merging."""

import heapq

import numpy as np
from scipy import ndimage
from scipy.spatial import cKDTree


def components(labels):
    result = np.full(labels.shape, -1, dtype=np.int32)
    count = 0
    for color in np.unique(labels):
        if color < 0:
            continue
        found, quantity = ndimage.label(labels == color)
        chosen = found > 0
        result[chosen] = found[chosen] + count - 1
        count += quantity
    return result, count


def nearest_labels(lab, palette, domain):
    labels = np.full(domain.shape, -1, dtype=np.int32)
    labels[domain] = cKDTree(palette).query(lab[domain], workers=1)[1]
    return labels


def source_locks(rgb, labels, palette, contrast):
    locked = np.zeros(labels.shape, dtype=bool)
    represented = palette[np.maximum(labels, 0)].astype(np.float32)
    edges = 0
    for axis in (0, 1):
        before = (slice(None, -1), slice(None)) if axis == 0 else (slice(None), slice(None, -1))
        after = (slice(1, None), slice(None)) if axis == 0 else (slice(None), slice(1, None))
        direction = rgb[after].astype(np.float32) - rgb[before]
        length = np.linalg.norm(direction, axis=2)
        projection = np.sum((represented[after] - represented[before]) * direction, axis=2) \
            / np.maximum(length, 1e-6)
        selected = (length >= contrast * np.sqrt(3)) & (projection >= contrast * .5) \
            & (labels[before] >= 0) & (labels[after] >= 0)
        locked[before] |= selected
        locked[after] |= selected
        edges += int(selected.sum())
    return locked, edges


def label_sweep(lab, labels, palette, locked, boundary, sweeps):
    height, width = labels.shape
    source, values = lab.reshape(-1, 3), labels.reshape(-1)
    palette = palette.astype(np.float64)
    norms = np.sum(palette ** 2, axis=1)
    options = np.arange(len(palette))
    checker = (np.arange(height)[:, None] + np.arange(width)) % 2
    changes = 0
    for iteration in range(sweeps):
        updated = 0
        for parity in range(2):
            indices = np.flatnonzero((checker == parity) & ~locked & (labels >= 0))
            for begin in range(0, len(indices), 32768):
                batch = indices[begin:begin + 32768]
                pixels = source[batch].astype(np.float64)
                costs = norms - 2 * (pixels @ palette.T)
                y, x = batch // width, batch % width
                for offset, valid in ((-width, y > 0), (width, y + 1 < height),
                                      (-1, x > 0), (1, x + 1 < width)):
                    neighbor = np.full(len(batch), -1, dtype=np.int32)
                    neighbor[valid] = values[batch[valid] + offset]
                    costs += boundary * ((neighbor[:, None] >= 0) & (neighbor[:, None] != options))
                choice = np.argmin(costs, axis=1)
                rows = np.arange(len(batch))
                chosen = costs[rows, choice] < costs[rows, values[batch]] - 1e-7
                values[batch[chosen]] = choice[chosen]
                updated += int(chosen.sum())
        changes += updated
        if updated == 0:
            break
    return changes


def region_graph(regions, count):
    pairs = []
    for first, second in ((regions[:, :-1], regions[:, 1:]), (regions[:-1], regions[1:])):
        chosen = (first != second) & (first >= 0) & (second >= 0)
        a, b = first[chosen].astype(np.int64), second[chosen].astype(np.int64)
        pairs.append(np.minimum(a, b) * count + np.maximum(a, b))
    encoded, lengths = np.unique(np.concatenate(pairs), return_counts=True)
    graph = [dict() for _ in range(count)]
    for key, length in zip(encoded, lengths):
        a, b = divmod(int(key), count)
        graph[a][b] = graph[b][a] = int(length)
    return graph


def merge_regions(lab, labels, palette, locked, boundary=6., region_cost=64., progress=None):
    regions, count = components(labels)
    valid = regions >= 0
    flat = regions[valid]
    sizes = np.bincount(flat, minlength=count).astype(np.float64)
    sums = np.column_stack([np.bincount(flat, weights=lab[:, :, channel][valid], minlength=count)
                            for channel in range(3)])
    colors = np.zeros(count, dtype=np.int32)
    colors[flat] = labels[valid]
    fixed = np.full(count, -1, dtype=np.int32)
    fixed[regions[locked]] = labels[locked]
    graph = region_graph(regions, count)
    norms = np.sum(palette.astype(np.float64) ** 2, axis=1)
    parent, active, version = np.arange(count), np.ones(count, dtype=bool), np.zeros(count, dtype=np.int64)
    queue = []

    def costs(size, total):
        return size * norms - 2 * (palette @ total)

    def proposal(a, b):
        if not active[a] or not active[b] or b not in graph[a]:
            return None
        if fixed[a] >= 0 and fixed[b] >= 0 and fixed[a] != fixed[b]:
            return None
        outside = {}
        for node in (a, b):
            for other, length in graph[node].items():
                if other not in (a, b):
                    outside[other] = outside.get(other, 0) + length
        lengths = np.bincount([colors[node] for node in outside],
                              weights=list(outside.values()), minlength=len(palette))
        neighbors = np.bincount([colors[node] for node in outside], minlength=len(palette))
        data = costs(sizes[a] + sizes[b], sums[a] + sums[b])
        old = costs(sizes[a], sums[a])[colors[a]] + costs(sizes[b], sums[b])[colors[b]]
        delta = data - old - boundary * (graph[a][b] + lengths) - region_cost * (1 + neighbors)
        required = max(fixed[a], fixed[b])
        color = required if required >= 0 else int(np.argmin(delta))
        if delta[color] >= -1e-6:
            return None
        return float(delta[color]), int(color), outside

    def push(a, b):
        if a > b:
            a, b = b, a
        candidate = proposal(a, b)
        if candidate is not None:
            heapq.heappush(queue, (candidate[0], a, b, int(version[a]), int(version[b]), candidate[1]))

    for a in range(count):
        for b in graph[a]:
            if a < b:
                push(a, b)
    accepted, saving = 0, 0.
    while queue:
        delta, a, b, va, vb, color = heapq.heappop(queue)
        if not active[a] or not active[b] or version[a] != va or version[b] != vb:
            continue
        candidate = proposal(a, b)
        if candidate is None:
            continue
        delta, color, outside = candidate
        joined = {a, b} | {node for node in outside if colors[node] == color}
        keep = min(joined)
        adjacency = {}
        for node in joined:
            for other, length in graph[node].items():
                if other not in joined:
                    adjacency[other] = adjacency.get(other, 0) + length
        total_size = sizes[list(joined)].sum()
        total_sum = sums[list(joined)].sum(axis=0)
        requirement = max(fixed[list(joined)])
        for node in joined:
            parent[node] = keep
            active[node] = False
            version[node] += 1
        active[keep], sizes[keep], sums[keep], fixed[keep], colors[keep] = True, total_size, total_sum, requirement, color
        for other in adjacency:
            for node in joined:
                graph[other].pop(node, None)
            graph[other][keep] = adjacency[other]
            version[other] += 1
        graph[keep] = adjacency
        for node in joined - {keep}:
            graph[node].clear()
        accepted += len(joined) - 1
        saving -= delta
        affected = set(adjacency) | {keep}
        for node in sorted(affected):
            for other in sorted(graph[node]):
                push(node, other)
        if progress and accepted % 256 < len(joined):
            progress({'merged_regions': accepted, 'remaining_regions': int(active.sum())})
    for node in range(count):
        root = node
        while parent[root] != root:
            root = parent[root]
        parent[node] = root
    output = labels.copy()
    output[valid] = colors[parent[flat]]
    return output, {'before_components': count, 'after_components': int(active.sum()),
                    'accepted_merges': accepted, 'energy_reduction': saving,
                    'boundary_weight': boundary, 'component_weight': region_cost}
