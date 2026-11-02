"""Building exporter tests on a synthetic district stage. Run: python test_export_buildings.py"""
import json
from pathlib import Path
import struct
import tempfile
import unittest

import numpy as np
from pxr import Gf, Sdf, Usd, UsdGeom

import export_buildings as eb
import export_terrain as et

METERS = 1.0 / 0.0254
SET = '/world/map_airfield/building_set_a'


def box(center, size):
    """Corner points and six outward quads, in inches."""
    cx, cy, cz = center
    sx, sy, sz = (s / 2.0 for s in size)
    points = [Gf.Vec3f(float(cx + x * sx) * METERS, float(cy + y * sy) * METERS, float(cz + z * sz) * METERS)
              for z in (-1, 1) for y in (-1, 1) for x in (-1, 1)]
    faces = [0, 2, 3, 1, 4, 5, 7, 6, 0, 1, 5, 4, 2, 6, 7, 3, 0, 4, 6, 2, 1, 3, 7, 5]
    return points, faces


def add_box(stage, path, center, size, material='concrete_wall_01', guide=False, left_handed=False):
    mesh = UsdGeom.Mesh.Define(stage, path)
    points, faces = box(center, size)
    mesh.GetPointsAttr().Set(points)
    mesh.GetFaceVertexCountsAttr().Set([4] * 6)
    mesh.GetFaceVertexIndicesAttr().Set(faces)
    if left_handed:
        mesh.GetOrientationAttr().Set(UsdGeom.Tokens.leftHanded)
    if guide:
        mesh.GetPurposeAttr().Set(UsdGeom.Tokens.guide)
    UsdGeom.Subset.Define(stage, f'{path}/{material}').GetIndicesAttr().Set(list(range(6)))


def add_building(stage, name, translate=(0.0, 0.0, 0.0), yaw=0.0, size=(16.0, 20.0, 6.0)):
    path = f'{SET}/{name}'
    xform = UsdGeom.Xform.Define(stage, path)
    xform.AddTranslateOp().Set(Gf.Vec3d(*[t * METERS for t in translate]))
    xform.AddRotateXYZOp().Set(Gf.Vec3f(0.0, 0.0, yaw))
    # Offset from the origin, so a wrong rotation moves it. An inner box stands for the interior.
    add_box(stage, f'{path}/shell', (2.0, 0.0, size[2] / 2.0), size, 'roof_metal_01')
    add_box(stage, f'{path}/interior', (2.0, 0.0, size[2] / 2.0), (2.0, 2.0, 2.0), 'desk_wood_01')
    return path


def add_lod_part(stage, building):
    """A part with two authored LODs: lod0 is two boxes, lod1 is one."""
    part = UsdGeom.Xform.Define(stage, f'{building}/detail').GetPrim()
    variants = part.GetVariantSets().AddVariantSet(eb.LOD_VARIANT_SET)
    for name in ('lod0', 'lod1'):
        variants.AddVariant(name)
        variants.SetVariantSelection(name)
        with variants.GetVariantEditContext():
            add_box(stage, f'{building}/detail/high' if name == 'lod0' else f'{building}/detail/low', (0.0, 0.0, 7.0), (2.0, 2.0, 2.0))
            if name == 'lod0':
                add_box(stage, f'{building}/detail/extra', (3.0, 0.0, 7.0), (2.0, 2.0, 2.0))
    variants.SetVariantSelection('lod0')


def build_district(root):
    path = Path(root) / et.RUNWAY_LAYER
    path.parent.mkdir(parents=True, exist_ok=True)
    stage = Usd.Stage.CreateNew(str(path))
    UsdGeom.SetStageMetersPerUnit(stage, 0.0254)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.z)
    UsdGeom.Xform.Define(stage, SET)
    add_building(stage, 'shed_01', (100.0, 200.0, 5.0), 0.0)
    add_building(stage, 'shed_02', (-50.0, 80.0, 5.0), 90.0)
    barn = add_building(stage, 'barn_01', (0.0, -100.0, 5.0), 0.0, size=(24.0, 14.0, 8.0))
    add_lod_part(stage, barn)
    add_box(stage, f'{barn}/guide', (0.0, 0.0, 30.0), (2.0, 2.0, 2.0), guide=True)
    add_building(stage, 'airfield_fencing_01', (0.0, 0.0, 0.0), 0.0, size=(40.0, 40.0, 4.0))
    add_building(stage, 'tiny_crate', (10.0, 10.0, 0.0), 0.0, size=(2.0, 2.0, 2.0))
    add_building(stage, 'low_slab', (20.0, 20.0, 0.0), 0.0, size=(30.0, 30.0, 1.0))
    # A neighborhood: too wide as a whole, so its children are the buildings.
    UsdGeom.Xform.Define(stage, f'{SET}/neighborhood')
    for name, x in (('house_a', -100.0), ('house_b', 100.0)):
        house = f'{SET}/neighborhood/{name}'
        xform = UsdGeom.Xform.Define(stage, house)
        xform.AddTranslateOp().Set(Gf.Vec3d(x * METERS, 500.0 * METERS, 0.0))
        add_box(stage, f'{house}/shell', (0.0, 0.0, 3.0), (15.0, 15.0, 6.0))
    stage.GetRootLayer().Save()
    return stage


def read_glb(data):
    json_length, _ = struct.unpack_from('<I4s', data, 12)
    document = json.loads(data[20:20 + json_length])
    return document, data[20 + json_length + 8:]


def accessor(document, body, index):
    a = document['accessors'][index]
    offset = document['bufferViews'][a['bufferView']]['byteOffset']
    if a['type'] == 'SCALAR':
        return np.frombuffer(body, dtype='<u4', count=a['count'], offset=offset)
    return np.frombuffer(body, dtype='<f4', count=a['count'] * 3, offset=offset).reshape(-1, 3)


class ExportBuildings(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name) / 'caldera'
        cls.game = Path(cls.temp.name) / 'game'
        build_district(cls.root)
        scene = cls.game / 'scenes/default'
        (scene / 'entities').mkdir(parents=True)
        (scene / 'scene.json').write_text(json.dumps({'entities': [1, 2], 'name': 'default', 'nextId': 3}))
        cls.report = eb.export(cls.root, cls.game)
        cls.placed = {p['name']: p for p in cls.report['placed']}

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def glb(self, name):
        return read_glb((self.game / 'assets' / self.placed[name]['model']).read_bytes())

    def entry(self, name):
        return self.report['models'][self.placed[name]['model']]

    def entity(self, name):
        for path in (self.game / 'scenes/default/entities').glob('*.json'):
            data = json.loads(path.read_text())
            if [c['fields']['name'] for c in data['components'] if c['type'] == 'Name'] == [name]:
                return data
        self.fail(f'no entity {name}')

    def part(self, name, part='shared'):
        document, body = self.glb(name)
        mesh = next(m for m in document['meshes'] if m['name'] == part)
        primitive = mesh['primitives'][0]
        names = {'POSITION': 'positions', 'NORMAL': 'normals', 'COLOR_0': 'colors'}
        fields = {names[k]: accessor(document, body, i) for k, i in primitive['attributes'].items()}
        fields['indices'] = accessor(document, body, primitive['indices'])
        return fields

    def test_selects_buildings_only(self):
        self.assertEqual(set(self.placed), {'shed_01', 'shed_02', 'barn_01', 'house_a', 'house_b'})
        self.assertTrue(any('airfield_fencing_01' in p for p in self.report['excluded_by_name']))

    def test_repeats_share_one_model(self):
        self.assertEqual(self.placed['shed_01']['model'], self.placed['shed_02']['model'])
        self.assertEqual(self.placed['house_a']['model'], self.placed['house_b']['model'])
        self.assertNotEqual(self.placed['shed_01']['model'], self.placed['barn_01']['model'])
        self.assertEqual(len(self.report['models']), 3)

    def test_placement_maps_to_engine(self):
        np.testing.assert_allclose(self.placed['shed_02']['position'], [-50.0, 5.0, -80.0], atol=1e-3)
        self.assertAlmostEqual(self.placed['shed_02']['yaw_degrees'], 90.0, places=3)

    def test_rotation_places_the_model_where_the_source_has_it(self):
        # The shell sits 2 m along the local +X; USD yaw 90 puts that at USD +Y, which is engine -Z.
        fields = next(c for c in self.entity('shed_02')['components'] if c['type'] == 'Transform')['fields']
        x, y, z, w = fields['rotation']
        rotated = np.array([1 - 2 * y * y, 2 * (x * y + z * w), 2 * (x * z - y * w)]) * 2.0
        np.testing.assert_allclose(rotated, [0.0, 0.0, -2.0], atol=1e-6)

    def test_every_authored_triangle_is_kept_including_the_interior(self):
        # Two boxes of 6 quads each.
        self.assertEqual(self.entry('shed_01')['near_triangles'], 24)
        positions = self.part('shed_01')['positions']
        inner = np.linalg.norm(positions - np.array([2.0, 3.0, 0.0]), axis=1)
        # Each of its 8 corners splits across 3 flat faces.
        self.assertEqual(int((inner < 2.0).sum()), 24)

    def test_positions_keep_the_source_extent(self):
        positions = self.part('shed_01')['positions']
        np.testing.assert_allclose(positions.max(axis=0) - positions.min(axis=0), [16.0, 6.0, 20.0], atol=1e-3)

    def test_vertices_weld_within_a_face_and_split_at_creases(self):
        # A box has 4 vertices per flat-shaded face, not 3 per triangle.
        self.assertEqual(len(self.part('shed_01')['positions']), 2 * 6 * 4)

    def test_winding_faces_outward(self):
        part = self.part('shed_01')
        triangles = part['positions'][part['indices'].reshape(-1, 3)]
        cross = np.cross(triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0])
        np.testing.assert_allclose(cross / np.linalg.norm(cross, axis=1, keepdims=True),
                                   part['normals'][part['indices'].reshape(-1, 3)[:, 0]], atol=1e-5)

    def test_guide_meshes_are_skipped(self):
        for part in ('shared', 'near', 'far'):
            self.assertLess(self.part('barn_01', part)['positions'][:, 1].max(), 25.0)

    def test_lods_store_shared_meshes_once(self):
        entry = self.entry('barn_01')
        # Shell and interior, 12 quads, are shared; near adds two boxes and far adds one.
        self.assertEqual(entry['near_triangles'], 24 + 24)
        self.assertEqual(entry['far_triangles'], 24 + 12)
        self.assertEqual(entry['stored_triangles'], 24 + 24 + 12)
        document, _ = self.glb('barn_01')
        by_name = {n['name']: i for i, n in enumerate(document['nodes'])}
        self.assertEqual(document['scenes'][0]['nodes'], [by_name['shared'], by_name['near']])
        self.assertEqual(document['scenes'][1]['nodes'], [by_name['shared'], by_name['far']])

    def test_a_building_without_lods_has_two_scenes_of_one_part(self):
        document, _ = self.glb('shed_01')
        self.assertEqual([n['name'] for n in document['nodes']], ['shared'])
        self.assertEqual(document['scenes'][0]['nodes'], document['scenes'][1]['nodes'])

    def test_one_white_double_sided_material_with_vertex_colors(self):
        document, _ = self.glb('barn_01')
        self.assertEqual(len(document['materials']), 1)
        self.assertTrue(document['materials'][0]['doubleSided'])
        for mesh in document['meshes']:
            self.assertEqual(len(mesh['primitives']), 1)
            self.assertIn('COLOR_0', mesh['primitives'][0]['attributes'])

    def test_each_triangle_takes_its_own_material_color(self):
        colors = {tuple(np.round(c, 4)) for c in self.part('shed_01')['colors']}
        self.assertEqual(colors, {tuple(np.round(np.array(eb.color_of(n), dtype='<f4'), 4)) for n in ('roof_metal_01', 'desk_wood_01')})

    def test_scene_keeps_other_entities(self):
        manifest = json.loads((self.game / 'scenes/default/scene.json').read_text())
        self.assertEqual(manifest['entities'][:2], [1, 2])
        self.assertEqual(len(manifest['entities']), 2 + len(self.placed))
        self.assertEqual(manifest['nextId'], max(manifest['entities']) + 1)

    def test_entities_use_the_lod_script(self):
        scripts = [c['fields']['scriptName'] for c in self.entity('barn_01')['components'] if c['type'] == 'ScriptRef']
        self.assertEqual(scripts, [eb.LOD_SCRIPT_NAME])
        self.assertTrue((self.game / 'assets/scripts/airfield_lod.lua').exists())

    def test_second_run_replaces_entities_with_identical_output(self):
        before = (self.game / 'assets' / self.placed['barn_01']['model']).read_bytes()
        files = sorted(p.name for p in (self.game / 'scenes/default/entities').glob('*.json'))
        eb.export(self.root, self.game)
        self.assertEqual(before, (self.game / 'assets' / self.placed['barn_01']['model']).read_bytes())
        self.assertEqual(files, sorted(p.name for p in (self.game / 'scenes/default/entities').glob('*.json')))


class Rerun(unittest.TestCase):
    """A second export over a scene someone has worked in."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'caldera'
        self.game = Path(self.temp.name) / 'game'
        build_district(self.root)
        self.scene = self.game / 'scenes/default'
        (self.scene / 'entities').mkdir(parents=True)
        (self.scene / 'scene.json').write_text(json.dumps({'entities': [1], 'name': 'default', 'nextId': 2}))
        (self.scene / 'entities/1.json').write_text(json.dumps({'components': [], 'id': 1, 'parent': 0}))
        eb.export(self.root, self.game)

    def manifest(self):
        return json.loads((self.scene / 'scene.json').read_text())

    def test_an_entity_added_after_an_export_survives_the_next(self):
        # What the editor does: a new entity takes nextId, which is above the generated ids.
        manifest = self.manifest()
        added = manifest['nextId']
        self.assertGreaterEqual(added, eb.ENTITY_BASE)
        manifest['entities'].append(added)
        manifest['nextId'] = added + 1
        (self.scene / 'scene.json').write_text(json.dumps(manifest))
        entity = {'components': [{'fields': {'name': 'Mine'}, 'type': 'Name'}], 'id': added, 'parent': 0}
        (self.scene / f'entities/{added}.json').write_text(json.dumps(entity))

        eb.export(self.root, self.game)

        manifest = self.manifest()
        self.assertIn(added, manifest['entities'])
        self.assertEqual(json.loads((self.scene / f'entities/{added}.json').read_text()), entity)
        self.assertIn(1, manifest['entities'])
        # 1, the added one and the five buildings, with no id used twice.
        self.assertEqual(len(manifest['entities']), 7)
        self.assertEqual(len(set(manifest['entities'])), 7)
        self.assertGreater(manifest['nextId'], max(manifest['entities']))
        self.assertEqual(sorted(p.stem for p in (self.scene / 'entities').glob('*.json')),
                         sorted(str(e) for e in manifest['entities']))

    def test_models_left_by_a_failed_run_do_not_reach_assets(self):
        staging = self.game / 'source/airfield_models.tmp'
        staging.mkdir(parents=True)
        (staging / 'orphan.glb').write_bytes(b'left behind')
        eb.export(self.root, self.game)
        self.assertFalse((self.game / 'assets/models/airfield/orphan.glb').exists())
        self.assertFalse(staging.exists())


class InteriorObjects(unittest.TestCase):
    def test_props_in_interior_groups_are_objects(self):
        for path in ('/geo/int_01/misc_model_5/ch_crate/geo/crateShape',
                     '/geo/side_extension_01_a/side_extension_01_a_int/crate_265/misc_model_5/ch_crate/geo/crateShape',
                     '/geo/geo_interior_01/misc_model_2/furniture_chair/geo/chairShape',
                     '/geo/int_2floor/misc_model_2/furniture_chair/geo/chairShape',
                     '/geo/INT_01/misc_model_2/furniture_chair/geo/chairShape'):
            self.assertTrue(eb.is_interior_object(path), path)

    def test_the_building_itself_stays(self):
        for path in ('/geo/ext_01/misc_model_5/ch_crate/geo/crateShape',  # outside
                     '/geo/int_01/patch_31_216',  # authored, not a prop
                     '/interior',
                     '/geo/int_01/misc_model_5/uk_metal_frame_warehouse_01/geo/frameShape',
                     '/geo/int_01/misc_model_5/mout_catwalk_support_brace/geo/braceShape',
                     '/geo/int_01/misc_model_5/building_elevator_shaft_cap/geo/capShape',
                     # A prop's name is not an interior group.
                     '/geo/ext_01/uk_stairs_int_wood_worn/misc_model_5/ch_crate/geo/crateShape',
                     '/geo/misc_model_5/int_01/geo/crateShape'):
            self.assertFalse(eb.is_interior_object(path), path)

    def test_fixtures_named_like_structure_are_objects(self):
        for asset in ('lm_light_int_wall_sconce_01_off', 'ee_clock_wall_mounted', 'ee_mirror_wall_mounted_decorative_tall'):
            self.assertTrue(eb.is_interior_object(f'/geo/int_01/misc_model_5/{asset}/geo/shape'), asset)

    def build(self, root):
        stage = build_district(root)
        barn = f'{SET}/barn_01'
        add_box(stage, f'{barn}/geo/int_01/crate_a/misc_model_1/ch_crate/geo/crateShape', (0.0, 0.0, 2.0), (2.0, 2.0, 2.0))
        add_box(stage, f'{barn}/geo/int_01/beam_a/misc_model_2/steel_beam/geo/beamShape', (0.0, 0.0, 3.0), (2.0, 2.0, 2.0))
        add_box(stage, f'{barn}/geo/ext_01/crate_b/misc_model_1/ch_crate/geo/crateShape', (0.0, 0.0, 4.0), (2.0, 2.0, 2.0))
        stage.GetRootLayer().Save()

    def export(self, keep):
        with tempfile.TemporaryDirectory() as temp:
            root, game = Path(temp) / 'caldera', Path(temp) / 'game'
            self.build(root)
            scene = game / 'scenes/default'
            (scene / 'entities').mkdir(parents=True)
            (scene / 'scene.json').write_text(json.dumps({'entities': [], 'name': 'default', 'nextId': 1}))
            report = eb.export(root, game, keep_interior_objects=keep)
            barn = next(p for p in report['placed'] if p['name'] == 'barn_01')
            return report, report['models'][barn['model']]

    def test_export_leaves_out_only_the_interior_crate(self):
        report, entry = self.export(False)
        # Shell, inner box, near LOD's two boxes, the beam and the outside crate; 12 triangles a box.
        self.assertEqual(entry['near_triangles'], 6 * 12)
        self.assertFalse(report['keep_interior_objects'])

    def test_flag_keeps_the_interior_crate(self):
        report, entry = self.export(True)
        self.assertEqual(entry['near_triangles'], 7 * 12)
        self.assertTrue(report['keep_interior_objects'])


class Parts(unittest.TestCase):
    def test_merge_splits_only_between_meshes_at_the_vertex_limit(self):
        def mesh(vertices):
            return {'positions': np.zeros((vertices, 3), dtype='<f4'), 'normals': np.zeros((vertices, 3), dtype='<f4'),
                    'colors': np.zeros((vertices, 3), dtype='<f4'), 'indices': np.array([0, vertices - 1, 1], dtype='<u4')}
        limit = eb.MAX_PART_VERTICES
        parts = eb.merge([mesh(limit - 10), mesh(5), mesh(20), None, mesh(limit + 1)])
        self.assertEqual([len(p['positions']) for p in parts], [limit - 5, 20, limit + 1])
        # Indices follow their mesh's vertices into the merged part.
        self.assertEqual(parts[0]['indices'].tolist(), [0, limit - 11, 1, limit - 10, limit - 6, limit - 9])
        self.assertEqual(eb.merge([None]), [])

    def test_buckets(self):
        names = [name for name, _, _ in eb.BUCKETS]
        self.assertEqual(names[eb.bucket_of('window_glass_01')], 'glass')
        self.assertEqual(names[eb.bucket_of('something_else')], 'concrete')

    def test_color_is_stable_and_stays_in_range(self):
        self.assertEqual(eb.color_of('brick_red_01'), eb.color_of('brick_red_01'))
        base = np.array(eb.BUCKETS[eb.bucket_of('brick_red_01')][1])
        self.assertLessEqual(float(np.abs(np.array(eb.color_of('brick_red_01')) / base - 1.0).max()), eb.SHADE_RANGE + 1e-9)

    def test_left_handed_mesh_flips_winding(self):
        with tempfile.TemporaryDirectory() as root:
            stage = Usd.Stage.CreateNew(str(Path(root) / 'a.usda'))
            UsdGeom.SetStageMetersPerUnit(stage, 0.0254)
            UsdGeom.Xform.Define(stage, '/b')
            add_box(stage, '/b/flip', (0.0, 0.0, 0.0), (2.0, 2.0, 2.0), left_handed=True)
            add_box(stage, '/b/plain', (0.0, 0.0, 0.0), (2.0, 2.0, 2.0))
            parts = eb.read_building(stage, '/b')
            flip, plain = eb.read_mesh(*parts['/flip'][1]), eb.read_mesh(*parts['/plain'][1])

            def outward(mesh):
                tri = mesh['positions'][mesh['indices'].reshape(-1, 3)]
                cross = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
                return float((cross * tri.mean(axis=1)).sum(axis=1).mean())
            self.assertGreater(outward(plain), 0.0)
            self.assertLess(outward(flip), 0.0)

    def test_authored_normals_are_used_and_rotated(self):
        with tempfile.TemporaryDirectory() as root:
            stage = Usd.Stage.CreateNew(str(Path(root) / 'a.usda'))
            UsdGeom.SetStageMetersPerUnit(stage, 0.0254)
            xform = UsdGeom.Xform.Define(stage, '/b')
            mesh = UsdGeom.Mesh.Define(stage, '/b/quad')
            mesh.GetPointsAttr().Set([Gf.Vec3f(0, 0, 0), Gf.Vec3f(100, 0, 0), Gf.Vec3f(100, 100, 0), Gf.Vec3f(0, 100, 0)])
            mesh.GetFaceVertexCountsAttr().Set([4])
            mesh.GetFaceVertexIndicesAttr().Set([0, 1, 2, 3])
            primvar = UsdGeom.PrimvarsAPI(mesh).CreatePrimvar('normals', Sdf.ValueTypeNames.Normal3fArray, UsdGeom.Tokens.faceVarying)
            primvar.Set([Gf.Vec3f(1, 0, 0)] * 4)
            part = eb.read_mesh(*eb.read_building(stage, '/b')['/quad'][1])
            np.testing.assert_allclose(part['normals'], np.tile([1.0, 0.0, 0.0], (len(part['normals']), 1)), atol=1e-6)
            self.assertEqual(len(part['positions']), 4)
            del xform

    def test_missing_layer_fails(self):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaisesRegex(et.ExportError, 'no airfield district layer'):
                eb.export(root, root)


if __name__ == '__main__':
    unittest.main()
