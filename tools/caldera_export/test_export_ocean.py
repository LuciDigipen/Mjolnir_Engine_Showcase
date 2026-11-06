"""Ocean exporter tests on synthetic heights. Run: python test_export_ocean.py"""
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import numpy as np

import export_ocean as eo
import export_terrain as et

HALF = 64.0
SIZE = 129  # samples at 1 m


def island():
    """Deep water with a square island, a pit inside it, and an extra bay on its +X side."""
    grid = np.full((SIZE, SIZE), -20.0)
    grid[44:85, 44:85] = 10.0
    grid[60:69, 60:69] = -5.0  # the pit: below sea level, cut off from the edge
    grid[60:69, 80:85] = -1.0  # the bay, open to the sea
    return grid


class BuildMesh(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Unsmoothed, so depths can be read directly.
        with mock.patch.object(eo, 'DEPTH_SMOOTH', 0.0):
            cls.positions, cls.colors, cls.triangles, cls.stats = eo.build_mesh(island(), -HALF, -HALF, 1.0, 0.0, 8.0)

    def vertex(self, x, z):
        hits = np.nonzero((self.positions[:, 0] == x) & (self.positions[:, 2] == z))[0]
        self.assertEqual(len(hits), 1, (x, z))
        return self.colors[hits[0]]

    def test_every_triangle_faces_up(self):
        tri = self.positions[self.triangles]
        cross = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
        self.assertTrue((cross[:, 1] > 0).all())
        self.assertLess(np.abs(cross[:, [0, 2]]).max(), 1e-6)

    def test_surface_is_flat_and_local_to_the_entity(self):
        self.assertTrue((self.positions[:, 1] == 0.0).all())

    def test_border_matches_the_outer_ring(self):
        low, high = -HALF, HALF
        x, z = self.positions[:, 0], self.positions[:, 2]
        border = (np.abs(x) <= high) & (np.abs(z) <= high) & ((x == low) | (x == high) | (z == low) | (z == high))
        self.assertTrue(border.any())
        np.testing.assert_allclose(self.colors[border], np.tile([eo.MAX_DEPTH, eo.MAX_SHORE, 1.0], (int(border.sum()), 1)))

    def test_outer_ring_reaches_the_extent(self):
        self.assertEqual(self.positions[:, 0].min(), -eo.OUTER_EXTENT)
        self.assertEqual(self.positions[:, 2].max(), eo.OUTER_EXTENT)

    def test_covers_the_plane_with_no_overlap_outside_the_island(self):
        # A closed top surface: total area is the outer square less the dropped island cells.
        tri = self.positions[self.triangles]
        cross = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
        area = float(cross[:, 1].sum()) / 2.0
        dropped = (self.stats['grid_cells_total'] - self.stats['grid_cells_kept']) * 64.0
        self.assertAlmostEqual(area / ((2 * eo.OUTER_EXTENT) ** 2 - dropped), 1.0, places=9)

    def test_edges_are_shared_so_there_are_no_t_junctions(self):
        # Every edge is used once, on the mesh boundary or a hole's rim, or twice in opposite directions.
        directed = {}
        for a, b, c in self.triangles.tolist():
            for edge in ((a, b), (b, c), (c, a)):
                directed[edge] = directed.get(edge, 0) + 1
        self.assertEqual(max(directed.values()), 1)
        # A T-junction would leave an unmatched edge with a grid vertex along it. Unmatched edges
        # here all have length one cell or the outer square's side.
        lengths = set()
        for (a, b) in directed:
            if (b, a) not in directed:
                lengths.add(round(float(np.linalg.norm(self.positions[a] - self.positions[b])), 3))
        self.assertEqual(lengths - {8.0, 2 * eo.OUTER_EXTENT}, set())

    def test_depth_comes_from_height_and_clamps(self):
        self.assertAlmostEqual(self.vertex(-64.0, -64.0)[0], eo.MAX_DEPTH)
        self.assertAlmostEqual(self.vertex(-24.0, 0.0)[0], eo.MAX_DEPTH)  # just off the island
        self.assertAlmostEqual(self.vertex(-16.0, -16.0)[0], eo.MIN_DEPTH)  # on land
        self.assertAlmostEqual(self.vertex(16.0, 0.0)[0], 1.0)  # in the bay

    def test_depth_is_smoothed_across_a_hard_shelf_edge(self):
        grid = np.full((SIZE, SIZE), -20.0)
        grid[:, 60:] = -1.0
        grid[44:85, 80:] = 10.0
        positions, colors, _, _ = eo.build_mesh(grid, -HALF, -HALF, 1.0, 0.0, 8.0)
        def depth(x):
            return colors[(positions[:, 0] == x) & (positions[:, 2] == -40.0)][0][0]
        # The source steps from 20 m to 1 m at X = -4; the mesh ramps between them.
        self.assertGreater(depth(-40.0), depth(-8.0))
        self.assertGreater(depth(-8.0), depth(0.0))
        self.assertGreater(depth(0.0), depth(16.0))
        self.assertLess(depth(-8.0), 19.0)
        self.assertGreater(depth(0.0), 2.0)

    def test_shore_distance_grows_offshore(self):
        self.assertAlmostEqual(self.vertex(-16.0, -16.0)[1], 0.0)
        self.assertAlmostEqual(self.vertex(-24.0, 0.0)[1], 4.0)
        self.assertAlmostEqual(self.vertex(-48.0, 0.0)[1], 28.0)

    def test_z_is_not_flipped(self):
        # A bump only at high Z must show up at high Z.
        grid = island()
        grid[120:124, 60:68] = 3.0
        with mock.patch.object(eo, 'DEPTH_SMOOTH', 0.0):
            positions, colors, _, _ = eo.build_mesh(grid, -HALF, -HALF, 1.0, 0.0, 8.0)
        def depth(x, z):
            return colors[(positions[:, 0] == x) & (positions[:, 2] == z)][0][0]
        self.assertAlmostEqual(depth(0.0, 56.0), eo.MIN_DEPTH)
        self.assertAlmostEqual(depth(0.0, -56.0), eo.MAX_DEPTH)

    def test_shelter_is_lower_near_land(self):
        # This island is small, so a blur to match it.
        with mock.patch.object(eo, 'SHELTER_SIGMA', 16.0):
            positions, colors, _, _ = eo.build_mesh(island(), -HALF, -HALF, 1.0, 0.0, 8.0)
        def shelter(x, z):
            return colors[(positions[:, 0] == x) & (positions[:, 2] == z)][0][2]
        self.assertLess(shelter(-24.0, 0.0), shelter(-64.0, -64.0))
        self.assertTrue(((colors[:, 2] >= 0.0) & (colors[:, 2] <= 1.0)).all())

    def test_a_pit_inside_the_island_is_not_ocean(self):
        self.assertEqual(self.stats['inland_samples_below_sea_level'], 81)
        ocean, below = eo.ocean_mask(island(), 0.0)
        self.assertTrue(below[64, 64])
        self.assertFalse(ocean[64, 64])
        self.assertTrue(ocean[64, 82])

    def test_cells_well_inland_are_dropped(self):
        self.assertLess(self.stats['grid_cells_kept'], self.stats['grid_cells_total'])
        tri = self.positions[self.triangles]
        center = tri.mean(axis=1)
        self.assertFalse(((np.abs(center[:, 0] + 4.0) < 4.0) & (np.abs(center[:, 2]) < 4.0)).any())

    def test_a_raised_sea_level_keeps_more_cells(self):
        grid = island()
        grid[44:85, 44:85] = 1.0
        _, _, _, low = eo.build_mesh(grid, -HALF, -HALF, 1.0, 0.0, 8.0)
        _, _, _, high = eo.build_mesh(grid, -HALF, -HALF, 1.0, 1.5, 8.0)
        self.assertGreater(high['grid_cells_kept'], low['grid_cells_kept'])

    def test_a_map_with_no_low_edge_fails(self):
        with self.assertRaisesRegex(et.ExportError, 'below sea level'):
            eo.build_mesh(np.full((SIZE, SIZE), 5.0), -HALF, -HALF, 1.0, 0.0, 8.0)

    def test_a_cell_that_does_not_divide_the_grid_fails(self):
        with self.assertRaisesRegex(et.ExportError, 'does not divide'):
            eo.build_mesh(island(), -HALF, -HALF, 1.0, 0.0, 7.0)


class Export(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.game = Path(self.temp.name)
        et.write_atomic(self.game / 'source/caldera.heights', et.height_file_bytes(island(), HALF, 1.0))

    def test_mesh_file_matches_what_the_engine_parses(self):
        report = eo.export(self.game)
        mesh = json.loads((self.game / 'assets/meshes/ocean.mesh.json').read_text())
        self.assertEqual(len(mesh['vertices']), report['vertices'])
        self.assertEqual(len(mesh['indices']), report['triangles'] * 3)
        self.assertLess(max(mesh['indices']), report['vertices'])
        self.assertEqual(sorted(mesh['vertices'][0]), ['color', 'normal', 'position'])
        self.assertEqual(json.loads((self.game / 'source/ocean_export_report.json').read_text()), report)

    def test_same_input_gives_identical_bytes(self):
        eo.export(self.game)
        first = (self.game / 'assets/meshes/ocean.mesh.json').read_bytes()
        eo.export(self.game)
        self.assertEqual(first, (self.game / 'assets/meshes/ocean.mesh.json').read_bytes())

    def test_missing_heights_fail(self):
        with self.assertRaisesRegex(et.ExportError, 'no height file'):
            eo.export(self.game / 'nowhere')

    def test_a_wrong_size_fails(self):
        path = self.game / 'source/caldera.heights'
        path.write_bytes(path.read_bytes()[:-4])
        with self.assertRaisesRegex(et.ExportError, 'does not match'):
            eo.export(self.game)

    def test_a_bad_magic_fails(self):
        path = self.game / 'source/caldera.heights'
        path.write_bytes(b'XXXX' + path.read_bytes()[4:])
        with self.assertRaisesRegex(et.ExportError, 'not a version 1'):
            eo.export(self.game)


if __name__ == '__main__':
    unittest.main()
