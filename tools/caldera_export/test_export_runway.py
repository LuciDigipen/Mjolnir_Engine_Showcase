"""Runway exporter tests on a synthetic district stage. Run: python test_export_runway.py"""
import json
from pathlib import Path
import struct
import tempfile
import unittest

import numpy as np
from pxr import Gf, Usd, UsdGeom

import export_runway as er
import export_terrain as et

BASE = 'concrete_brushed_spotty_01_rvl_bot'
LINE = 'asphalt_painted_line_02'
DROPPED = 'stain_generic_03_rvl'
HALF_EXTENT = 1024.0


def add_mesh(stage, path, points, counts, indices, subsets, purpose=None):
    mesh = UsdGeom.Mesh.Define(stage, path)
    mesh.GetPointsAttr().Set(points)
    mesh.GetFaceVertexCountsAttr().Set(counts)
    mesh.GetFaceVertexIndicesAttr().Set(indices)
    mesh.GetOrientationAttr().Set(UsdGeom.Tokens.rightHanded)
    if purpose:
        mesh.GetPurposeAttr().Set(purpose)
    for name, faces in subsets:
        UsdGeom.Subset.Define(stage, f'{path}/{name}').GetIndicesAttr().Set(faces)


def build_district(root, rotate=et.RUNWAY_ROTATE_Z, extra_material=None):
    """Two faces of base, one each of line and a dropped decal, a guide mesh; 100 by 100 inches a face."""
    path = Path(root) / et.RUNWAY_LAYER
    path.parent.mkdir(parents=True, exist_ok=True)
    stage = Usd.Stage.CreateNew(str(path))
    UsdGeom.SetStageMetersPerUnit(stage, 0.0254)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.z)
    prim = UsdGeom.Xform.Define(stage, '/world/map_airfield/building_set_a')
    prim.AddTranslateOp().Set(Gf.Vec3d(*et.RUNWAY_TRANSLATE))
    prim.AddRotateXYZOp().Set(Gf.Vec3f(0.0, 0.0, rotate))
    UsdGeom.Xform.Define(stage, '/world/map_airfield/building_set_a/airfield_grounds')
    runway = et.RUNWAY_PRIM
    UsdGeom.Xform.Define(stage, runway)
    # Local X runs along the strip; three CCW quads seen from above, then one flipped under them.
    points = []
    for i in range(4):
        points += [Gf.Vec3f(i * 1000.0, 0.0, 0.0), Gf.Vec3f(i * 1000.0, 1000.0, 0.0)]
    faces = [0, 2, 3, 1, 2, 4, 5, 3, 4, 6, 7, 5, 1, 3, 2, 0]
    subsets = [(BASE, [0, 1]), (LINE, [2]), (DROPPED, [3])]
    if extra_material:
        subsets.append((extra_material, [3]))
    add_mesh(stage, f'{runway}/patch', points, [4, 4, 4, 4], faces, subsets)
    add_mesh(stage, f'{runway}/guide', points, [4], [0, 2, 3, 1], [(BASE, [0])], purpose=UsdGeom.Tokens.guide)
    stage.GetRootLayer().Save()


def read_glb(path):
    data = Path(path).read_bytes()
    magic, version, total = struct.unpack_from('<4sII', data)
    json_length, _ = struct.unpack_from('<I4s', data, 12)
    document = json.loads(data[20:20 + json_length])
    body = data[20 + json_length + 8:]
    return magic, version, total, len(data), document, body


def accessor(document, body, index):
    a = document['accessors'][index]
    view = document['bufferViews'][a['bufferView']]
    size = {'VEC2': 2, 'VEC3': 3}[a['type']]
    return np.frombuffer(body, dtype='<f4', count=a['count'] * size, offset=view['byteOffset']).reshape(-1, size)


class ExportRunway(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name) / 'caldera'
        cls.game = Path(cls.temp.name) / 'game'
        build_district(cls.root)
        texture = cls.game / 'source/textures/concrete_color.jpg'
        texture.parent.mkdir(parents=True)
        texture.write_bytes(b'\xff\xd8not really a jpeg\xff\xd9')
        cls.report = er.export(cls.root, cls.game)
        cls.magic, cls.version, cls.total, cls.size, cls.doc, cls.body = read_glb(cls.game / 'assets/models/runway.glb')

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def primitive(self, name):
        names = [m['name'] for m in self.doc['materials']]
        p = next(p for p in self.doc['meshes'][0]['primitives'] if p['material'] == names.index(name))
        return {k: accessor(self.doc, self.body, i) for k, i in p['attributes'].items()}

    def world(self, name):
        return self.primitive(name)['POSITION'] + np.array(self.report['entity_position'])

    def test_glb_header_is_consistent(self):
        self.assertEqual((self.magic, self.version), (b'glTF', 2))
        self.assertEqual(self.total, self.size)

    def test_keeps_only_listed_materials(self):
        self.assertEqual({m['name'] for m in self.doc['materials']}, {BASE, LINE})
        self.assertEqual(self.report['triangles'], {BASE: 4, LINE: 2})

    def test_placement_maps_to_engine(self):
        # The local (0, 0, 0) corner lands at the translate, with USD (x, y, z) as engine (x, z, -y).
        expected = np.array([et.RUNWAY_TRANSLATE[0], et.RUNWAY_TRANSLATE[2], -et.RUNWAY_TRANSLATE[1]]) * 0.0254
        self.assertLess(np.abs(self.world(BASE) - expected).sum(axis=1).min(), 1e-3)
        # Local +X, rotated -135 degrees about Z, heads to USD (-0.707, -0.707): engine (-x, +z).
        along = self.world(BASE)
        far = along[np.argmax(np.linalg.norm(along - expected, axis=1))]
        self.assertLess(far[0], expected[0])
        self.assertGreater(far[2], expected[2])

    def test_normals_face_up_with_front_winding(self):
        for name in (BASE, LINE):
            p = self.primitive(name)
            np.testing.assert_allclose(p['NORMAL'], np.tile([0.0, 1.0, 0.0], (len(p['NORMAL']), 1)), atol=1e-6)
            tri = p['POSITION'][:3]
            self.assertGreater(np.cross(tri[1] - tri[0], tri[2] - tri[0])[1], 0.0)

    def test_overlays_lift_above_the_base(self):
        lift = self.world(LINE)[:, 1].max() - self.world(BASE)[:, 1].max()
        self.assertAlmostEqual(float(lift), et.RUNWAY_KEPT[LINE], places=4)

    def test_down_facing_faces_drop(self):
        # The flipped quad is a dropped material here; the real count comes from a kept one below.
        self.assertEqual(self.report['down_facing_dropped'], 0)
        flipped = {BASE: np.array([[[0, 0, 0], [0, 0, 1], [1, 0, 0]], [[0, 0, 0], [1, 0, 0], [0, 0, 1]]], dtype=np.float64)}
        _, primitives, stats = er.build_primitives(flipped)
        self.assertEqual(stats['down_facing_dropped'], 1)
        self.assertEqual(len(primitives[0]['positions']), 3)

    def test_uvs_repeat_by_world_distance(self):
        p = self.primitive(BASE)
        offset = p['POSITION'][:, [0, 2]] - p['POSITION'][0, [0, 2]]
        uv = p['TEXCOORD_0'] - p['TEXCOORD_0'][0]
        np.testing.assert_allclose(np.abs(offset) / np.array(er.UV_PERIOD), np.abs(uv), atol=1e-4)

    def test_line_has_no_texture_and_base_does(self):
        by_name = {m['name']: m['pbrMetallicRoughness'] for m in self.doc['materials']}
        self.assertIn('baseColorTexture', by_name[BASE])
        self.assertNotIn('baseColorTexture', by_name[LINE])

    def test_second_run_is_byte_identical(self):
        first = (self.game / 'assets/models/runway.glb').read_bytes()
        er.export(self.root, self.game)
        self.assertEqual(first, (self.game / 'assets/models/runway.glb').read_bytes())


UP_TRIANGLE = [[0, 0, 0], [0, 0, 1], [1, 0, 0]]
DOWN_TRIANGLE = [[0, 0, 0], [1, 0, 0], [0, 0, 1]]
DEGENERATE_TRIANGLE = [[0, 0, 0], [1, 0, 0], [2, 0, 0]]


class EmptiedMaterials(unittest.TestCase):
    def test_material_with_nothing_left_is_skipped(self):
        for dropped in (DOWN_TRIANGLE, DEGENERATE_TRIANGLE):
            triangles = {BASE: np.array([UP_TRIANGLE], dtype=np.float64), LINE: np.array([dropped], dtype=np.float64)}
            _, primitives, stats = er.build_primitives(triangles)
            self.assertEqual([p['name'] for p in primitives], [BASE])
            self.assertEqual(stats['triangles'], {BASE: 1})
            magic = er.glb_bytes(primitives, b'image')[:4]
            self.assertEqual(magic, b'glTF')

    def test_nothing_left_at_all_fails(self):
        triangles = {BASE: np.array([DOWN_TRIANGLE, DEGENERATE_TRIANGLE], dtype=np.float64)}
        with self.assertRaisesRegex(et.ExportError, 'no runway triangles left'):
            er.build_primitives(triangles)


class Failures(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_moved_placement_fails(self):
        build_district(self.root, rotate=-90.0)
        with self.assertRaisesRegex(et.ExportError, 'placement changed'):
            et.read_runway(self.root)

    def test_material_without_a_decision_fails(self):
        build_district(self.root, extra_material='brand_new_material')
        with self.assertRaisesRegex(et.ExportError, 'no decision'):
            et.read_runway(self.root)

    def test_missing_layer_fails(self):
        with self.assertRaisesRegex(et.ExportError, 'no runway district layer'):
            et.read_runway(self.root)

    def test_missing_texture_fails(self):
        build_district(self.root)
        with self.assertRaisesRegex(et.ExportError, 'no concrete texture'):
            er.export(self.root, self.root / 'game')


class Stamp(unittest.TestCase):
    def setUp(self):
        n = int(HALF_EXTENT * 2) + 1
        self.grid = np.full((n, n), 30.0, dtype=np.float32)
        # A 20 by 10 m flat pad at 22 m, from x -10 to 10 and z -5 to 5.
        a, b, c, d = (-10.0, 22.0, -5.0), (-10.0, 22.0, 5.0), (10.0, 22.0, 5.0), (10.0, 22.0, -5.0)
        self.triangles = {BASE: np.array([[a, b, c], [a, c, d]], dtype=np.float64)}
        self.stats = et.stamp_runway(self.grid, self.triangles, HALF_EXTENT, 1.0)

    def at(self, x, z):
        return float(self.grid[int(z + HALF_EXTENT), int(x + HALF_EXTENT)])

    def test_covered_samples_sit_under_the_surface(self):
        self.assertEqual(self.stats['covered_samples'], 21 * 11)
        self.assertAlmostEqual(self.at(0, 0), 22.0 - et.STAMP_DEPTH, places=4)
        self.assertAlmostEqual(self.at(10, 5), 22.0 - et.STAMP_DEPTH, places=4)

    def test_pad_extends_past_the_footprint(self):
        self.assertAlmostEqual(self.at(0, 5 + et.STAMP_PAD), 22.0 - et.STAMP_DEPTH, places=4)

    def test_blends_back_without_a_step(self):
        line = [self.at(0, z) for z in range(5, 20)]
        self.assertTrue(all(b >= a - 1e-4 for a, b in zip(line, line[1:])), line)
        self.assertLess(max(b - a for a, b in zip(line, line[1:])), 2.0)

    def test_terrain_beyond_the_blend_stays(self):
        self.assertEqual(self.at(0, 5 + et.STAMP_PAD + et.STAMP_BLEND + 1), 30.0)
        self.assertLess(self.at(0, 5 + et.STAMP_PAD + et.STAMP_BLEND - 2), 30.0)


if __name__ == '__main__':
    unittest.main()
