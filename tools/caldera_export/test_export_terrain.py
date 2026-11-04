"""Exporter tests on a synthetic two-tile stage. Run: python test_export_terrain.py"""
import json
from pathlib import Path
import struct
import tempfile
import unittest

import numpy as np
from pxr import Gf, Usd, UsdGeom

import export_terrain as et

STEP = 0.8128
HALF_EXTENT = 64.0
INTERVALS = 16
OFFSET_INCHES = 320.0  # the second tile's authored translate, a whole number of lattice steps


def surface(i, j):
    """Height in inches at lattice (i, j): tilted on both axes so a flip or a swap shows."""
    return 400.0 + 2.0 * i + 5.0 * j


def new_stage(path):
    stage = Usd.Stage.CreateNew(str(path))
    UsdGeom.SetStageMetersPerUnit(stage, 0.0254)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.z)
    return stage


def write_tile(root, name, i_range, j_range, translate=0.0, seam_i=None, seam_delta=0.0, hole=None):
    """The real three-layer shape: placement is in the wrapper, never in the terrain layer.

    The terrain layer's points are local, so opening it alone loses the translate.
    """
    base = Path(root) / 'map_source/prefabs/br/wz_vg/mp_wz_island/superterrrain'
    folder = base / 'season_4'
    folder.mkdir(parents=True, exist_ok=True)
    terrain = new_stage(folder / f'{name}.terrain.usd')
    UsdGeom.Xform.Define(terrain, '/terrain')
    points = []
    for j in range(*j_range):
        for i in range(*i_range):
            if hole and hole[0] <= i < hole[1] and hole[2] <= j < hole[3]:
                continue
            z = surface(i, j) + (seam_delta if i == seam_i else 0.0)
            points.append(Gf.Vec3f(i * 32.0 - translate, j * 32.0, z))
    UsdGeom.Mesh.Define(terrain, '/terrain/height').GetPointsAttr().Set(points)
    # A sibling that must be ignored.
    UsdGeom.PointInstancer.Define(terrain, '/terrain/clutter')
    terrain.GetRootLayer().Save()

    wrapper = new_stage(folder / f'{name}.usd')
    UsdGeom.Xform.Define(wrapper, f'/world/{name}')
    placed = UsdGeom.Xform.Define(wrapper, f'/world/{name}/super_terrain_1')
    placed.AddTranslateOp().Set(Gf.Vec3d(translate, 0.0, 0.0))
    placed.GetPrim().GetReferences().AddReference(f'./{name}.terrain.usd', '/terrain')
    wrapper.GetRootLayer().Save()


def write_main(root, names, lod='full'):
    base = Path(root) / 'map_source/prefabs/br/wz_vg/mp_wz_island/superterrrain'
    main = new_stage(base / 'st_main.usd')
    UsdGeom.Xform.Define(main, '/world/st_main')
    for name in names:
        prim = UsdGeom.Xform.Define(main, f'/world/st_main/{name}').GetPrim()
        variants = prim.GetVariantSets().AddVariantSet('districtLod')
        variants.AddVariant('proxy')
        variants.AddVariant('full')
        variants.SetVariantSelection('full')
        with variants.GetVariantEditContext():
            prim.GetReferences().AddReference(f'./season_4/{name}.usd', f'/world/{name}')
        variants.SetVariantSelection(lod)
    # Something beside the tiles that the population mask must leave out.
    UsdGeom.Mesh.Define(main, '/world/st_main/height')
    main.GetRootLayer().Save()


def build_root(root):
    write_tile(root, 'st_a', (-20, 1), (-20, 21), hole=(-10, -5, -4, 3))
    write_tile(root, 'st_b', (0, 21), (-20, 21), translate=OFFSET_INCHES, seam_i=0, seam_delta=2.0)
    write_main(root, ['st_a', 'st_b'])


def read_heights(game):
    data = (Path(game) / 'source/caldera.heights').read_bytes()
    magic, version, width, height, origin_x, origin_z, spacing = struct.unpack_from('<4sIIIddd', data)
    grid = np.frombuffer(data, dtype='<f4', offset=40).reshape(height, width)
    return data, (magic, version, width, height, origin_x, origin_z, spacing), grid


def run_export(root, game, **kwargs):
    options = dict(half_extent=HALF_EXTENT, intervals=INTERVALS, expected_tiles=2)
    options.update(kwargs)
    return et.export(root, game, **options)


def engine_height(grid, x, z):
    """Height the engine grid holds at integer engine coordinates."""
    return float(grid[int(z + HALF_EXTENT), int(x + HALF_EXTENT)])


def expected_height(x, z):
    """Analytic surface at engine (x, z), where USD y = -z."""
    return surface(x / STEP, -z / STEP) * 0.0254


class ExportTerrain(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name) / 'caldera'
        cls.game = Path(cls.temp.name) / 'game'
        build_root(cls.root)
        cls.report = run_export(cls.root, cls.game)
        cls.data, cls.header, cls.grid = read_heights(cls.game)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_fnv_matches_published_vectors(self):
        self.assertEqual(et.fnv1a64(b''), 'cbf29ce484222325')
        self.assertEqual(et.fnv1a64(b'a'), 'af63dc4c8601ec8c')

    def test_height_file_layout(self):
        self.assertEqual(self.header, (b'MHGT', 1, 129, 129, -HALF_EXTENT, -HALF_EXTENT, 1.0))
        self.assertEqual(len(self.data), 40 + 129 * 129 * 4)
        self.assertTrue(np.isfinite(self.grid).all())

    def test_axes_map_to_engine(self):
        # Away from the seam and the hole; a flipped Z or swapped axis misses by meters.
        for x, z in ((5, -5), (5, 5), (-5, -5), (-5, 8)):
            self.assertAlmostEqual(engine_height(self.grid, x, z), expected_height(x, z), delta=0.01, msg=f'{x}, {z}')

    def test_second_tile_translate_applies(self):
        self.assertAlmostEqual(engine_height(self.grid, 10, 0), expected_height(10, 0), delta=0.01)

    def test_seam_averages(self):
        # Tile b lifts column 0 by 2 inches and tile a does not, so the merge sits 1 inch up.
        self.assertEqual(self.report['source']['seam_shared_samples'], 41)
        self.assertEqual(self.report['source']['seam_over_1cm'], 41)
        self.assertAlmostEqual(self.report['source']['seam_max_m'], 2.0 * 0.0254, places=4)

    def test_interior_hole_follows_surface(self):
        # A plane is harmonic, so a Laplace fill must reproduce it.
        self.assertEqual(self.report['source']['hole_samples'], 5 * 7)
        self.assertEqual(self.report['source']['hole_components'], 1)
        self.assertAlmostEqual(engine_height(self.grid, -6, 0), expected_height(-6, 0), delta=0.01)

    def test_ocean_descends_to_below_sea_level_at_edge(self):
        edges = np.concatenate((self.grid[0], self.grid[-1], self.grid[:, 0], self.grid[:, -1]))
        self.assertLess(float(edges.max()), 0.0)

    def test_recipe_names_the_written_heights(self):
        recipe = json.loads((self.game / 'assets/terrain/caldera.terrain.json').read_text())
        self.assertEqual(recipe['generator'], {'id': 'mir.heightmap', 'version': 1})
        self.assertEqual(recipe['parameters']['contentHash'], et.fnv1a64(self.data))
        self.assertEqual(recipe['parameters']['source'], '../../source/caldera.heights')
        self.assertEqual(recipe['layout']['intervalsPerTile'], INTERVALS)
        source = (self.game / 'assets/terrain' / recipe['parameters']['source']).resolve()
        self.assertEqual(source, (self.game / 'source/caldera.heights').resolve())

    def test_second_run_is_byte_identical(self):
        with tempfile.TemporaryDirectory() as other:
            run_export(self.root, other)
            for name in ('source/caldera.heights', 'assets/terrain/caldera.terrain.json', 'source/caldera_export_report.json'):
                self.assertEqual((self.game / name).read_bytes(), (Path(other) / name).read_bytes(), name)

    def test_wrong_tile_count_fails(self):
        with tempfile.TemporaryDirectory() as other:
            with self.assertRaisesRegex(et.ExportError, 'expected 3 terrain tiles'):
                run_export(self.root, other, expected_tiles=3)

    def test_failed_gate_writes_nothing(self):
        with tempfile.TemporaryDirectory() as other:
            with self.assertRaisesRegex(et.ExportError, 'mean error'):
                run_export(self.root, other, max_mean_error=-1.0)
            self.assertEqual(list(Path(other).iterdir()), [])

    def test_map_must_be_whole_tiles(self):
        # 144 m is nine 16 m tiles wide, but its edges miss the tile edges, which start at 0.
        with tempfile.TemporaryDirectory() as other:
            with self.assertRaisesRegex(et.ExportError, 'whole number'):
                run_export(self.root, other, half_extent=72.0)


class BadStages(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'caldera'
        self.game = Path(self.temp.name) / 'game'

    def test_off_lattice_points_fail(self):
        write_tile(self.root, 'st_a', (0, 3), (0, 3), translate=17.0)
        # The points were authored for a whole-step world position; move them off it.
        layer = next(self.root.rglob('st_a.terrain.usd'))
        stage = Usd.Stage.Open(str(layer))
        mesh = UsdGeom.Mesh(stage.GetPrimAtPath('/terrain/height'))
        mesh.GetPointsAttr().Set([p + Gf.Vec3f(5.0, 0.0, 0.0) for p in mesh.GetPointsAttr().Get()])
        stage.GetRootLayer().Save()
        write_main(self.root, ['st_a'])
        with self.assertRaisesRegex(et.ExportError, 'off the'):
            run_export(self.root, self.game, expected_tiles=1)

    def test_stacked_tiles_fail(self):
        # What opening terrain layers without their wrappers produced on the real dataset.
        write_tile(self.root, 'st_a', (-5, 6), (-5, 6))
        write_tile(self.root, 'st_b', (-5, 6), (-5, 6))
        write_main(self.root, ['st_a', 'st_b'])
        with self.assertRaisesRegex(et.ExportError, 'stacked'):
            run_export(self.root, self.game)

    def test_proxy_lod_fails(self):
        write_tile(self.root, 'st_a', (-5, 6), (-5, 6))
        write_main(self.root, ['st_a'], lod='proxy')
        with self.assertRaisesRegex(et.ExportError, 'no height mesh for st_a'):
            run_export(self.root, self.game, expected_tiles=1)


if __name__ == '__main__':
    unittest.main()
