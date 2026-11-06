"""Export the ocean surface mesh, carrying water depth and distance to shore in its vertex colors.

Usage:
    python export_ocean.py --game-dir games/CalderaDemo [--sea-level 0.0] [--cell 8.0]

Reads <game>/source/caldera.heights only, so running this never triggers a terrain rebake. Writes
<game>/assets/meshes/ocean.mesh.json and <game>/source/ocean_export_report.json. Vertices are local
to the ocean entity, which sits at (0, sea level, 0), so a vertex's X and Z are world X and Z.

Vertex color carries data, not color: R = depth in meters, G = distance to shore in meters, B =
shelter, from 0 in a cove to 1 in open water. Same input and pinned environment give identical bytes.

Run without -I: this imports export_terrain from its own folder.
"""
import argparse
import json
from pathlib import Path
import struct
import sys

import numpy as np
from scipy import ndimage

import export_terrain as et

HEADER = '<4sIIIddd'
MIN_DEPTH, MAX_DEPTH = -2.0, 20.0  # meters; past these the material does not change
MAX_SHORE = 256.0  # meters
DEPTH_SMOOTH = 14.0  # meters; the coast's flat padding ends in hard polygon edges, which would show through shallows
OUTER_EXTENT = 1000000.0  # meters from the origin to the mesh edge; far enough that it meets the horizon from altitude
SHELTER_SIGMA = 150.0  # meters; how far around a point counts toward being open water
SHELTER_LOW, SHELTER_HIGH = 0.35, 0.85  # share of nearby water that maps to shelter 0 and 1
SHELTER_EDGE = 320.0  # meters inside the map edge over which shelter reaches open water


def read_heights(path):
    """(heights, origin X, origin Z, spacing). Row 0 is min Z and column 0 is min X."""
    data = Path(path).read_bytes()
    size = struct.calcsize(HEADER)
    if len(data) < size:
        raise et.ExportError(f'{path}: too short for a height file')
    magic, version, width, height, origin_x, origin_z, spacing = struct.unpack_from(HEADER, data)
    if magic != b'MHGT' or version != 1:
        raise et.ExportError(f'{path}: not a version 1 height file')
    if len(data) != size + width * height * 4:
        raise et.ExportError(f'{path}: size does not match its {width} x {height} header')
    return np.frombuffer(data, dtype='<f4', offset=size).reshape(height, width).astype(np.float64), origin_x, origin_z, spacing


def ocean_mask(heights, sea_level):
    """Samples under sea level and connected to the map edge. A pit inside the island is not ocean."""
    below = heights < sea_level
    labels, _ = ndimage.label(below)
    edge = np.concatenate((labels[0], labels[-1], labels[:, 0], labels[:, -1]))
    connected = np.unique(edge[edge != 0])
    if not len(connected):
        raise et.ExportError(f'no part of the map edge is below sea level {sea_level:g} m')
    return np.isin(labels, connected), below


def build_mesh(heights, origin_x, origin_z, spacing, sea_level, cell):
    """(positions, colors, triangles, stats). Positions hold one grid of cells plus the outer ring."""
    step = int(round(cell / spacing))
    if step < 1 or abs(step * spacing - cell) > 1e-9 or (heights.shape[0] - 1) % step or (heights.shape[1] - 1) % step:
        raise et.ExportError(f'cell {cell:g} m does not divide the {spacing:g} m height grid')
    ocean, below = ocean_mask(heights, sea_level)
    rows, cols = (heights.shape[0] - 1) // step + 1, (heights.shape[1] - 1) // step + 1

    # Keep a cell holding ocean, or next to one, so the waterline never falls off the mesh.
    cell_ocean = np.zeros((rows - 1, cols - 1), dtype=bool)
    for r in range(rows - 1):
        for c in range(cols - 1):
            cell_ocean[r, c] = ocean[r * step:(r + 1) * step + 1, c * step:(c + 1) * step + 1].any()
    keep = ndimage.binary_dilation(cell_ocean, structure=np.ones((3, 3), dtype=bool))

    grid_rows, grid_cols = np.arange(rows) * step, np.arange(cols) * step
    clipped = np.clip(sea_level - heights, MIN_DEPTH, MAX_DEPTH)
    depth = ndimage.gaussian_filter(clipped, DEPTH_SMOOTH / spacing, mode='nearest')[np.ix_(grid_rows, grid_cols)]
    shore = np.minimum(ndimage.distance_transform_edt(~(heights >= sea_level), sampling=spacing), MAX_SHORE)[np.ix_(grid_rows, grid_cols)]
    open_share = ndimage.gaussian_filter(ocean[np.ix_(grid_rows, grid_cols)].astype(np.float64), SHELTER_SIGMA / cell, mode='nearest')
    shelter = np.clip((open_share - SHELTER_LOW) / (SHELTER_HIGH - SHELTER_LOW), 0.0, 1.0)

    # The outer ring's triangles run far past the map, so a border value that differs from the ring's
    # would smear into hard radial edges. The border matches it, and nothing reads shore this far out.
    edge = np.minimum.outer(np.minimum(np.arange(rows), rows - 1 - np.arange(rows)),
                            np.minimum(np.arange(cols), cols - 1 - np.arange(cols))) * cell
    shelter = 1.0 - (1.0 - shelter) * np.clip(edge / SHELTER_EDGE, 0.0, 1.0)
    border = edge == 0.0
    depth[border] = MAX_DEPTH
    shore[border] = MAX_SHORE

    # The border always takes part, because the outer ring shares its vertices.
    used = np.zeros((rows, cols), dtype=bool)
    used[:-1, :-1] |= keep
    used[1:, :-1] |= keep
    used[:-1, 1:] |= keep
    used[1:, 1:] |= keep
    used[0, :] = used[-1, :] = used[:, 0] = used[:, -1] = True
    index = np.full((rows, cols), -1, dtype=np.int64)
    index[used] = np.arange(int(used.sum()))

    x = origin_x + grid_cols * spacing
    z = origin_z + grid_rows * spacing
    gx, gz = np.meshgrid(x, z)
    positions = [np.stack((gx[used], np.zeros(int(used.sum())), gz[used]), axis=1)]
    colors = [np.stack((depth[used], shore[used], shelter[used]), axis=1)]

    # With Y up, (r, c), (r + 1, c), (r, c + 1) winds so its normal points up.
    r, c = np.nonzero(keep)
    a, b, d, e = index[r, c], index[r + 1, c], index[r, c + 1], index[r + 1, c + 1]
    triangles = [np.stack((a, b, d), axis=1), np.stack((d, b, e), axis=1)]

    # Each side of the border fans to its two nearest outer corners, in the same winding.
    first_outer = int(used.sum())
    half = OUTER_EXTENT
    corners = np.array([[-half, 0.0, -half], [-half, 0.0, half], [half, 0.0, half], [half, 0.0, -half]])
    positions.append(corners)
    colors.append(np.tile([MAX_DEPTH, MAX_SHORE, 1.0], (4, 1)))
    sides = (index[:, 0], index[-1, :], index[::-1, -1], index[0, ::-1])
    for number, side in enumerate(sides):
        here, there = first_outer + number, first_outer + (number + 1) % 4
        middle = len(side) // 2
        fan = [(side[i], here, side[i + 1]) for i in range(middle)]
        fan.append((side[middle], here, there))
        fan += [(side[i], there, side[i + 1]) for i in range(middle, len(side) - 1)]
        triangles.append(np.array(fan, dtype=np.int64))

    triangles = np.concatenate(triangles)
    positions, colors = np.concatenate(positions), np.concatenate(colors)
    stats = {
        'sea_level_m': sea_level,
        'cell_m': cell,
        'vertices': len(positions),
        'triangles': len(triangles),
        'grid_cells_kept': int(keep.sum()),
        'grid_cells_total': int(keep.size),
        'ocean_samples': int(ocean.sum()),
        'inland_samples_below_sea_level': int((below & ~ocean).sum()),
    }
    return positions, colors, triangles, stats


def mesh_json(positions, colors, triangles):
    """The .mesh.json text, written by hand since json.dumps is slow and large at this size."""
    vertices = [
        f'{{"position":[{p[0]:.0f},0,{p[2]:.0f}],"color":[{c[0]:.2f},{c[1]:.1f},{c[2]:.3f}],"normal":[0,1,0]}}'
        for p, c in zip(positions, colors)
    ]
    indices = ','.join(str(i) for i in triangles.ravel().tolist())
    return '{"indices":[' + indices + '],"vertices":[\n' + ',\n'.join(vertices) + '\n]}\n'


def export(game_dir, sea_level=0.0, cell=8.0):
    """Run the export; returns the report. Raises ExportError, leaving what is there, on failure."""
    source = Path(game_dir) / 'source' / 'caldera.heights'
    if not source.exists():
        raise et.ExportError(f'no height file at {source}; run export_terrain.py first')
    heights, origin_x, origin_z, spacing = read_heights(source)
    positions, colors, triangles, stats = build_mesh(heights, origin_x, origin_z, spacing, sea_level, cell)
    text = mesh_json(positions, colors, triangles)
    stats['json_bytes'] = len(text)
    et.write_atomic(Path(game_dir) / 'assets' / 'meshes' / 'ocean.mesh.json', text.encode())
    et.write_atomic(Path(game_dir) / 'source' / 'ocean_export_report.json', (json.dumps(stats, indent=2, sort_keys=True) + '\n').encode())
    return stats


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--game-dir', required=True)
    parser.add_argument('--sea-level', type=float, default=0.0)
    parser.add_argument('--cell', type=float, default=8.0)
    args = parser.parse_args()
    try:
        report = export(args.game_dir, args.sea_level, args.cell)
    except et.ExportError as e:
        print(f'export failed: {e}', file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
