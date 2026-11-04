"""Export Caldera's airfield runway as a static glTF model.

Usage:
    python export_runway.py --caldera-root <dir> --game-dir games/CalderaDemo

Writes <game>/assets/models/runway.glb and <game>/source/runway_export_report.json, whose
entity_position is where the scene must place the model. The concrete texture is read from
<game>/source/textures/concrete_color.jpg, outside assets/ so AssetCooker skips it.
Same input and pinned environment give identical bytes.

Run without -I: this imports export_terrain from its own folder.
"""
import argparse
import json
from pathlib import Path
import struct
import sys

import numpy as np

import export_terrain as et

UV_PERIOD = (8.0, 4.0)  # meters per texture repeat; the concrete image is 2:1
# Material name to (base color factor, uses the concrete texture).
LOOKS = {
    'concrete_brushed_spotty_01_rvl_bot': ((0.55, 0.55, 0.55, 1.0), True),
    'cement_ground_patchwork_cracked': ((0.45, 0.45, 0.45, 1.0), True),
    'concrete_edge_crease_seam_02': ((0.35, 0.35, 0.35, 1.0), True),
    'asphalt_painted_line_02': ((0.9, 0.9, 0.88, 1.0), False),
}
GL_ARRAY_BUFFER = 34962
GL_FLOAT = 5126
GL_REPEAT = 10497
GL_LINEAR = 9729
GL_LINEAR_MIPMAP_LINEAR = 9987


def build_primitives(triangles):
    """(entity position, primitives, stats). Each primitive holds unshared vertices with flat normals.

    Vertices are local to the entity position, the footprint center at the highest surface.
    """
    everything = np.concatenate([t for t in triangles.values() if len(t)]).reshape(-1, 3)
    low, high = everything.min(axis=0), everything.max(axis=0)
    origin = np.round(np.array([(low[0] + high[0]) / 2, high[1], (low[2] + high[2]) / 2]), 3)
    primitives = []
    stats = {'degenerate_dropped': 0, 'down_facing_dropped': 0, 'triangles': {}}
    for name, tris in triangles.items():
        if not len(tris):
            continue
        cross = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
        length = np.linalg.norm(cross, axis=1)
        solid = length > 1e-9
        stats['degenerate_dropped'] += int((~solid).sum())
        up = cross[:, 1] > 0
        stats['down_facing_dropped'] += int((solid & ~up).sum())
        keep = solid & up
        if not keep.any():
            continue
        normals = cross[keep] / length[keep, None]
        positions = tris[keep].reshape(-1, 3) - origin
        positions[:, 1] += et.RUNWAY_KEPT[name]
        stats['triangles'][name] = int(keep.sum())
        primitives.append({
            'name': name,
            'positions': positions.astype('<f4'),
            'normals': np.repeat(normals, 3, axis=0).astype('<f4'),
            # glTF's V runs down the image; a tiling texture only needs the sign to stay fixed.
            'uvs': (np.stack((positions[:, 0], -positions[:, 2]), axis=1) / np.array(UV_PERIOD)).astype('<f4'),
        })
    if not primitives:
        raise et.ExportError('no runway triangles left after dropping degenerate and down-facing ones')
    return origin, primitives, stats


def glb_bytes(primitives, texture):
    """A one-mesh binary glTF with the texture embedded."""
    body = bytearray()
    views, accessors = [], []

    def add_view(data, target=None):
        views.append({'buffer': 0, 'byteOffset': len(body), 'byteLength': len(data)})
        if target:
            views[-1]['target'] = target
        body.extend(data)
        body.extend(b'\0' * (-len(body) % 4))
        return len(views) - 1

    def add_accessor(array, kind, bounds=False):
        accessors.append({'bufferView': add_view(array.tobytes(), GL_ARRAY_BUFFER), 'componentType': GL_FLOAT,
                          'count': len(array), 'type': kind})
        if bounds:
            accessors[-1]['min'] = [float(v) for v in array.min(axis=0)]
            accessors[-1]['max'] = [float(v) for v in array.max(axis=0)]
        return len(accessors) - 1

    materials, mesh_primitives = [], []
    for p in primitives:
        factor, textured = LOOKS[p['name']]
        pbr = {'baseColorFactor': list(factor), 'metallicFactor': 0.0, 'roughnessFactor': 1.0}
        if textured:
            pbr['baseColorTexture'] = {'index': 0}
        materials.append({'name': p['name'], 'pbrMetallicRoughness': pbr})
        mesh_primitives.append({
            'attributes': {
                'POSITION': add_accessor(p['positions'], 'VEC3', bounds=True),
                'NORMAL': add_accessor(p['normals'], 'VEC3'),
                'TEXCOORD_0': add_accessor(p['uvs'], 'VEC2'),
            },
            'material': len(materials) - 1,
        })
    image_view = add_view(texture)
    document = {
        'asset': {'version': '2.0', 'generator': 'tools/caldera_export/export_runway.py'},
        'scene': 0,
        'scenes': [{'nodes': [0]}],
        'nodes': [{'name': 'runway', 'mesh': 0}],
        'meshes': [{'name': 'runway', 'primitives': mesh_primitives}],
        'materials': materials,
        'textures': [{'sampler': 0, 'source': 0}],
        'images': [{'bufferView': image_view, 'mimeType': 'image/jpeg'}],
        'samplers': [{'magFilter': GL_LINEAR, 'minFilter': GL_LINEAR_MIPMAP_LINEAR, 'wrapS': GL_REPEAT, 'wrapT': GL_REPEAT}],
        'accessors': accessors,
        'bufferViews': views,
        'buffers': [{'byteLength': len(body)}],
    }
    text = json.dumps(document, separators=(',', ':'), sort_keys=True).encode()
    text += b' ' * (-len(text) % 4)
    total = 12 + 8 + len(text) + 8 + len(body)
    return (struct.pack('<4sII', b'glTF', 2, total) + struct.pack('<I4s', len(text), b'JSON') + text +
            struct.pack('<I4s', len(body), b'BIN\0') + bytes(body))


def export(root, game_dir):
    """Run the export; returns the report. Raises ExportError, writing nothing, on failure."""
    texture_path = Path(game_dir) / 'source' / 'textures' / 'concrete_color.jpg'
    if not texture_path.exists():
        raise et.ExportError(f'no concrete texture at {texture_path}')
    origin, primitives, stats = build_primitives(et.read_runway(root))
    data = glb_bytes(primitives, texture_path.read_bytes())
    commit = et.caldera_commit(root)
    report = {
        'caldera_commit': commit,
        'caldera_commit_is_pinned': commit == et.PINNED_COMMIT,
        'entity_position': [float(v) for v in origin],
        **stats,
    }
    et.write_atomic(Path(game_dir) / 'assets' / 'models' / 'runway.glb', data)
    et.write_atomic(Path(game_dir) / 'source' / 'runway_export_report.json', (json.dumps(report, indent=2, sort_keys=True) + '\n').encode())
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--caldera-root', required=True)
    parser.add_argument('--game-dir', required=True)
    args = parser.parse_args()
    try:
        report = export(args.caldera_root, args.game_dir)
    except et.ExportError as e:
        print(f'export failed: {e}', file=sys.stderr)
        return 1
    if not report['caldera_commit_is_pinned']:
        print(f"warning: Caldera is at {report['caldera_commit']}, expected {et.PINNED_COMMIT}", file=sys.stderr)
    print(f"entity position {report['entity_position']}, triangles {report['triangles']}, "
          f"dropped {report['degenerate_dropped']} degenerate and {report['down_facing_dropped']} down-facing")
    return 0


if __name__ == '__main__':
    sys.exit(main())
