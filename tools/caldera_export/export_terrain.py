"""Export Caldera's terrain height meshes as a mir.heightmap source and recipe.

Usage:
    python -I export_terrain.py --caldera-root <dir> --game-dir games/CalderaDemo

Writes <game>/source/caldera.heights, <game>/assets/terrain/caldera.terrain.json and
<game>/source/caldera_export_report.json. Exits nonzero when a quality gate fails, and
writes nothing in that case. Same input and pinned environment give identical bytes.

Conventions: USD (x, y, z) becomes engine (x, z, -y); heights are meters.
"""
import argparse
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys

import numpy as np
from pxr import Sdf, Usd, UsdGeom
from scipy import ndimage, sparse
from scipy.sparse.linalg import spsolve

MAIN_LAYER = 'map_source/prefabs/br/wz_vg/mp_wz_island/superterrrain/st_main.usd'
MAIN_PRIM = '/world/st_main'
PINNED_COMMIT = '03feaa1886c0a39bc50ca0df86c321f74ed4eee6'
LATTICE_UNITS = 32.0  # stage units (inches)
LATTICE_TOLERANCE = 1e-3  # fraction of a lattice step
MAX_HEIGHT_MAGNITUDE = 10000.0  # kMaxHeightMagnitude in TerrainRecipe.h
OUTLIERS_REPORTED = 20
OCEAN_DEPTH = 20.0  # meters below sea level at full falloff
OCEAN_FALLOFF = 150.0  # meters from the nearest terrain to full depth
OCEAN_SMOOTH_REACH = 30.0  # meters from terrain where nearest-height streaks are fully smoothed
OCEAN_SMOOTH_SIGMA = 8.0  # meters
EDGE_BLEND = 32.0  # meters inside the map edge forced down to full depth
MAX_MEAN_ERROR = 0.02
MAX_P99_ERROR = 0.25
MAX_SHARED_SAMPLE_SHARE = 0.05  # tiles only meet along seams; more means they are stacked


class ExportError(Exception):
    pass


def fnv1a64(data):
    """Same hash as the engine's Fnv1a64, as 16 lowercase hex digits."""
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return f'{h:016x}'


def read_tiles(root, expected_tiles):
    """World-space height points per tile, in stage units, with meters per unit.

    Composes through st_main because tile placement lives in wrapper layers: a terrain
    layer opened alone puts every tile at the origin. The mask keeps districts unloaded.
    """
    path = Path(root) / MAIN_LAYER
    layer = Sdf.Layer.FindOrOpen(str(path))
    main = layer.GetPrimAtPath(MAIN_PRIM) if layer else None
    if not main:
        raise ExportError(f'no {MAIN_PRIM} in {path}')
    names = sorted(child.name for child in main.nameChildren if child.name.startswith('st_'))
    if len(names) != expected_tiles:
        raise ExportError(f'expected {expected_tiles} terrain tiles, found {len(names)} under {root}')
    mask = Usd.StagePopulationMask()
    for name in names:
        mask.Add(Sdf.Path(f'{MAIN_PRIM}/{name}'))
    stage = Usd.Stage.OpenMasked(str(path), mask, Usd.Stage.LoadAll)
    found = {}
    for prim in stage.Traverse():
        if prim.GetName() != 'height' or prim.GetTypeName() != 'Mesh':
            continue
        name = str(prim.GetPath()).split('/')[3]
        if name in found:
            raise ExportError(f'{name}: more than one height mesh')
        xf = np.array(UsdGeom.Xformable(prim).ComputeLocalToWorldTransform(Usd.TimeCode.Default()))
        points = np.array(UsdGeom.Mesh(prim).GetPointsAttr().Get(), dtype=np.float64)
        found[name] = points @ xf[:3, :3] + xf[3, :3]
    missing = [name for name in names if name not in found]
    if missing:
        raise ExportError(f'no height mesh for {", ".join(missing)}; is districtLod set to full?')
    return [(name, found[name]) for name in names], UsdGeom.GetStageMetersPerUnit(stage)


def merge_lattice(tiles, units, half_extent):
    """Average tile samples onto a lattice covering the whole map.

    Returns (heights in meters with NaN gaps, lattice step in meters, index of world 0, stats).
    Row is USD Y and column USD X, both ascending.
    """
    step = LATTICE_UNITS * units
    origin = math.ceil(half_extent / step) + 1
    side = origin * 2 + 1
    total = np.zeros((side, side), dtype=np.float64)
    count = np.zeros((side, side), dtype=np.int32)
    low = np.full((side, side), np.inf, dtype=np.float64)
    high = np.full((side, side), -np.inf, dtype=np.float64)
    tile_duplicates = 0
    for name, world in tiles:
        lattice = world[:, :2] / LATTICE_UNITS
        ij = np.round(lattice).astype(np.int64)
        off = float(np.abs(lattice - ij).max())
        if off > LATTICE_TOLERANCE:
            raise ExportError(f'{name}: {off:.4f} of a step off the {LATTICE_UNITS:g}-unit lattice')
        meters = ij * step
        if np.abs(meters).max() > half_extent:
            raise ExportError(f'{name}: samples reach {np.abs(meters).max():.1f} m, beyond half extent {half_extent:g} m')
        rows, cols = ij[:, 1] + origin, ij[:, 0] + origin
        tile_duplicates += len(ij) - len(np.unique(rows * side + cols))
        z = world[:, 2] * units
        np.add.at(total, (rows, cols), z)
        np.add.at(count, (rows, cols), 1)
        np.minimum.at(low, (rows, cols), z)
        np.maximum.at(high, (rows, cols), z)
    known = count > 0
    heights = np.full((side, side), np.nan, dtype=np.float64)
    heights[known] = total[known] / count[known]
    shared = count > 1
    disagreement = (high - low)[shared]
    stats = {
        'tiles': [name for name, _ in tiles],
        'samples': int(known.sum()),
        'duplicate_samples_within_a_tile': int(tile_duplicates),
        'seam_shared_samples': int(shared.sum()),
        'seam_over_1cm': int((disagreement > 0.01).sum()),
        'seam_max_m': float(disagreement.max()) if disagreement.size else 0.0,
        'seam_p99_m': float(np.percentile(disagreement, 99)) if disagreement.size else 0.0,
    }
    return heights, step, origin, stats


def classify_gaps(known):
    """(outer, holes): gaps reaching the lattice border, and gaps enclosed by terrain."""
    labels, _ = ndimage.label(~known)
    border = np.unique(np.concatenate((labels[0], labels[-1], labels[:, 0], labels[:, -1])))
    outer = np.isin(labels, border[border != 0])
    return outer, ~known & ~outer


def fill_holes(heights, holes):
    """Laplace-fill each enclosed hole from its boundary, exactly, one component at a time."""
    labels, components = ndimage.label(holes)
    for index, window in enumerate(ndimage.find_objects(labels), start=1):
        rows = slice(window[0].start - 1, window[0].stop + 1)
        cols = slice(window[1].start - 1, window[1].stop + 1)
        mask = labels[rows, cols] == index
        ids = np.full(mask.shape, -1, dtype=np.int64)
        ids[mask] = np.arange(mask.sum())
        patch = heights[rows, cols]
        b = np.zeros(int(mask.sum()))
        at_row, at_col, at_value = [], [], []
        for dr, dc in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            neighbor_ids = np.roll(ids, (-dr, -dc), axis=(0, 1))
            neighbor_heights = np.roll(patch, (-dr, -dc), axis=(0, 1))
            own = ids[mask]
            other = neighbor_ids[mask]
            inside = other >= 0
            at_row.append(own[inside])
            at_col.append(other[inside])
            at_value.append(np.full(int(inside.sum()), -1.0))
            np.add.at(b, own[~inside], neighbor_heights[mask][~inside])
        n = int(mask.sum())
        at_row.append(np.arange(n))
        at_col.append(np.arange(n))
        at_value.append(np.full(n, 4.0))
        system = sparse.csr_matrix((np.concatenate(at_value), (np.concatenate(at_row), np.concatenate(at_col))), shape=(n, n))
        solved = spsolve(system, b, permc_spec='COLAMD')
        patch[mask] = solved
    return int(components)


def smoothstep(t):
    t = np.clip(t, 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def fill_ocean(heights, outer, step, origin, half_extent, sea_level):
    """Slope the outer gap from its nearest terrain down to the seabed, reaching it at the map edge."""
    distance, nearest = ndimage.distance_transform_edt(outer, sampling=step, return_indices=True)
    source = heights[nearest[0], nearest[1]]
    smooth = ndimage.gaussian_filter(source, OCEAN_SMOOTH_SIGMA / step, mode='nearest')
    mix = np.clip(distance / OCEAN_SMOOTH_REACH, 0.0, 1.0)
    source = source * (1.0 - mix) + smooth * mix
    coords = (np.arange(heights.shape[0]) - origin) * step
    edge = half_extent - np.maximum(np.abs(coords)[:, None], np.abs(coords)[None, :])
    t = np.maximum(smoothstep(distance / OCEAN_FALLOFF), smoothstep(1.0 - edge / EDGE_BLEND))
    seabed = sea_level - OCEAN_DEPTH
    heights[outer] = (source * (1.0 - t) + seabed * t)[outer]


def resample(heights, step, origin, half_extent, spacing):
    """Bilinear samples on the engine grid: row 0 is min engine Z, which is max USD Y."""
    n = int(round(half_extent * 2 / spacing)) + 1
    engine = -half_extent + np.arange(n) * spacing
    fx = engine / step + origin
    x0 = np.floor(fx).astype(np.int64)
    tx = fx - x0
    out = np.empty((n, n), dtype=np.float32)
    for row, z in enumerate(engine):
        fy = -z / step + origin
        y0 = int(math.floor(fy))
        ty = fy - y0
        line = heights[y0] * (1.0 - ty) + heights[y0 + 1] * ty
        out[row] = line[x0] * (1.0 - tx) + line[x0 + 1] * tx
    return out


def measure_error(original, grid, step, origin, half_extent, spacing):
    """Bilinear readback of the engine grid at each original sample."""
    rows, cols = np.nonzero(~np.isnan(original))
    x = (cols - origin) * step
    z = -(rows - origin) * step
    gx = (x + half_extent) / spacing
    gz = (z + half_extent) / spacing
    x0 = np.minimum(np.floor(gx).astype(np.int64), grid.shape[1] - 2)
    z0 = np.minimum(np.floor(gz).astype(np.int64), grid.shape[0] - 2)
    tx = gx - x0
    tz = gz - z0
    back = (grid[z0, x0] * (1 - tz) * (1 - tx) + grid[z0, x0 + 1] * (1 - tz) * tx +
            grid[z0 + 1, x0] * tz * (1 - tx) + grid[z0 + 1, x0 + 1] * tz * tx)
    error = np.abs(back - original[rows, cols])
    worst = np.argsort(error, kind='stable')[::-1][:OUTLIERS_REPORTED]
    return {
        'compared_samples': int(error.size),
        'mean_m': float(error.mean()),
        'p99_m': float(np.percentile(error, 99)),
        'p999_m': float(np.percentile(error, 99.9)),
        'max_m': float(error.max()),
        'worst': [{'x': float(x[i]), 'z': float(z[i]), 'source_m': float(original[rows[i], cols[i]]), 'error_m': float(error[i])}
                  for i in worst],
    }


def check_gates(grid, error, stats, sea_level, max_mean, max_p99):
    failures = []
    if stats['seam_shared_samples'] > stats['samples'] * MAX_SHARED_SAMPLE_SHARE:
        failures.append(f"{stats['seam_shared_samples']} of {stats['samples']} samples are shared: tiles are stacked, not placed")
    if not np.isfinite(grid).all():
        failures.append('output has non-finite heights')
    if np.abs(grid).max() > MAX_HEIGHT_MAGNITUDE:
        failures.append(f'heights exceed +-{MAX_HEIGHT_MAGNITUDE:g} m')
    edges = np.concatenate((grid[0], grid[-1], grid[:, 0], grid[:, -1]))
    if edges.max() >= sea_level:
        failures.append(f'map edge reaches {edges.max():.2f} m, not below sea level {sea_level:g} m')
    if error['mean_m'] > max_mean:
        failures.append(f"mean error {error['mean_m']:.4f} m exceeds {max_mean:g} m")
    if error['p99_m'] > max_p99:
        failures.append(f"p99 error {error['p99_m']:.4f} m exceeds {max_p99:g} m")
    return failures


def write_atomic(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + '.tmp')
    temp.write_bytes(data)
    os.replace(temp, path)


def height_file_bytes(grid, half_extent, spacing):
    header = struct.pack('<4sIIIddd', b'MHGT', 1, grid.shape[1], grid.shape[0], -half_extent, -half_extent, spacing)
    return header + grid.astype('<f4').tobytes()


def recipe_text(half_extent, spacing, intervals, source, content_hash):
    recipe = {
        'recipeVersion': 2,
        'generator': {'id': 'mir.heightmap', 'version': 1},
        'seed': 0,
        'layout': {
            'sampleSpacing': spacing,
            'intervalsPerTile': intervals,
            'bounds': {'minX': -half_extent, 'minZ': -half_extent, 'maxX': half_extent, 'maxZ': half_extent},
        },
        'parameters': {'source': source, 'contentHash': content_hash},
        'precision': {'defaultSpacing': spacing, 'tiles': []},
    }
    return json.dumps(recipe, indent=2) + '\n'


def caldera_commit(root):
    result = subprocess.run(['git', '-C', str(root), 'rev-parse', 'HEAD'], capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else None


def export(root, game_dir, sea_level=0.0, half_extent=1792.0, spacing=1.0, intervals=256, expected_tiles=14,
           max_mean_error=MAX_MEAN_ERROR, max_p99_error=MAX_P99_ERROR):
    """Run the export; returns the report. Raises ExportError, writing nothing, when a gate fails."""
    # Engine tiles start at sample 0, so a half extent off a tile edge widens the map past the bounds.
    tile_size = intervals * spacing
    if half_extent % tile_size != 0:
        raise ExportError(f'half extent {half_extent:g} m is not a whole number of {tile_size:g} m tiles')

    tiles, units = read_tiles(root, expected_tiles)
    heights, step, origin, stats = merge_lattice(tiles, units, half_extent)
    original = heights.copy()
    known = ~np.isnan(heights)
    outer, holes = classify_gaps(known)
    stats['hole_samples'] = int(holes.sum())
    stats['hole_components'] = fill_holes(heights, holes)
    stats['outer_samples'] = int(outer.sum())
    fill_ocean(heights, outer, step, origin, half_extent, sea_level)

    grid = resample(heights, step, origin, half_extent, spacing)
    error = measure_error(original, grid, step, origin, half_extent, spacing)
    failures = check_gates(grid, error, stats, sea_level, max_mean_error, max_p99_error)
    if failures:
        raise ExportError('; '.join(failures))

    data = height_file_bytes(grid, half_extent, spacing)
    content_hash = fnv1a64(data)
    source_dir = Path(game_dir) / 'source'
    recipe_path = Path(game_dir) / 'assets' / 'terrain' / 'caldera.terrain.json'
    commit = caldera_commit(root)
    report = {
        'caldera_commit': commit,
        'caldera_commit_is_pinned': commit == PINNED_COMMIT,
        'sea_level_m': sea_level,
        'grid': {'width': grid.shape[1], 'height': grid.shape[0], 'spacing_m': spacing, 'half_extent_m': half_extent},
        'height_range_m': [float(grid.min()), float(grid.max())],
        'content_hash': content_hash,
        'source': stats,
        'resample_error': error,
    }
    # The recipe goes last among the outputs the bake reads, so its hash never names missing heights.
    write_atomic(source_dir / 'caldera.heights', data)
    write_atomic(recipe_path, recipe_text(half_extent, spacing, intervals, '../../source/caldera.heights', content_hash).encode())
    write_atomic(source_dir / 'caldera_export_report.json', (json.dumps(report, indent=2, sort_keys=True) + '\n').encode())
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--caldera-root', required=True)
    parser.add_argument('--game-dir', required=True)
    parser.add_argument('--sea-level', type=float, default=0.0)
    args = parser.parse_args()
    try:
        report = export(args.caldera_root, args.game_dir, sea_level=args.sea_level)
    except ExportError as e:
        print(f'export failed: {e}', file=sys.stderr)
        return 1
    if not report['caldera_commit_is_pinned']:
        print(f"warning: Caldera is at {report['caldera_commit']}, expected {PINNED_COMMIT}", file=sys.stderr)
    error = report['resample_error']
    print(f"{report['grid']['width']} x {report['grid']['height']} heights, hash {report['content_hash']}, "
          f"range {report['height_range_m'][0]:.1f} to {report['height_range_m'][1]:.1f} m")
    print(f"resample error: mean {error['mean_m']:.4f} m, p99 {error['p99_m']:.4f} m, max {error['max_m']:.2f} m")
    return 0


if __name__ == '__main__':
    sys.exit(main())
