import bpy,json,hashlib,sys,numpy as np
from pathlib import Path
from mathutils import Vector
out=Path(__file__).resolve().parents[2]/'games'/'CalderaDemo'/'source'/'aircraft'/'sf50'
scene=bpy.context.scene
rig=bpy.data.objects['Landing gear rig']
assert len(bpy.data.armatures)==1 and len(bpy.data.actions)==1
assert len(bpy.data.objects)==17 and not any(o.type not in ('MESH','ARMATURE') for o in bpy.data.objects)
moving=[o for o in bpy.data.objects if o.modifiers]
static=[o for o in bpy.data.objects if o.type=='MESH' and not o.modifiers]
assert len(moving)==3 and len(static)==13
assert all(not o.animation_data and not o.parent for o in static)
assert all(len(o.modifiers)==1 and o.modifiers[0].object==rig for o in moving)
assert all(i.packed_file or Path(bpy.path.abspath(i.filepath)).is_file() for i in bpy.data.images if i.source=='FILE')
manifest_path=out/'reports'/'sf50_texture_manifest.json'
if manifest_path.exists():
    manifest=json.loads(manifest_path.read_text())
    by_name={i.name:i for i in bpy.data.images}
    assert len([i for i in bpy.data.images if i.source=='FILE'])==manifest['image_datablocks']
    for item in manifest['images']:
        img=by_name[item['image']]
        path=Path(bpy.path.abspath(img.filepath))
        assert path.resolve()==(out/item['file']).resolve()
        assert hashlib.sha256(path.read_bytes()).hexdigest()==item['sha256']
        assert img.colorspace_settings.name==item['color_space'] and img.alpha_mode==item['alpha_mode']
def points(obj):
    ev=obj.evaluated_get(bpy.context.evaluated_depsgraph_get());mesh=ev.to_mesh()
    arr=np.empty(len(mesh.vertices)*3,dtype=np.float32);mesh.vertices.foreach_get('co',arr)
    ev.to_mesh_clear();return arr
scene.frame_set(1);bpy.context.view_layer.update()
fixed={o.name:hashlib.sha256(points(o).tobytes()).hexdigest() for o in static}
up={o.name:points(o) for o in moving}
scene.frame_set(111);bpy.context.view_layer.update()
assert all(hashlib.sha256(points(o).tobytes()).hexdigest()==fixed[o.name] for o in static)
travel={o.name:float(np.max(np.abs(points(o)-up[o.name]))) for o in moving}
assert all(v>.1 for v in travel.values()),travel
report={'passed':True,'objects':len(bpy.data.objects),'static_meshes':len(static),'rigged_meshes':len(moving),'armatures':len(bpy.data.armatures),'actions':len(bpy.data.actions),'gear_travel_coordinate_m':travel,'static_geometry_unchanged':True,'missing_textures':[]}
(out/'reports'/'sf50_gear_validation.json').write_text(json.dumps(report,indent=2)+'\n');print('GEAR VALIDATION PASSED',json.dumps(report),flush=True)
if '--render' not in sys.argv or '--no-render' in sys.argv:sys.exit(0)
data=bpy.data.cameras.new('Temporary preview camera');cam=bpy.data.objects.new(data.name,data);scene.collection.objects.link(cam)
cam.location=(13,-18,5);target=Vector((0,-.2,.65));cam.rotation_euler=(target-cam.location).to_track_quat('-Z','Y').to_euler();data.lens=48;scene.camera=cam
center=Vector((0,.12796,.78094))
for name,loc,power,size in [('Key',(4,-6,10),2800,8),('Fill',(-7,-1,6),2000,7),('Rim',(2,8,9),3200,6)]:
    data=bpy.data.lights.new('Temporary '+name,'AREA');data.energy=power;data.shape='DISK';data.size=size
    obj=bpy.data.objects.new(data.name,data);scene.collection.objects.link(obj);obj.location=center+Vector(loc);obj.rotation_euler=(center-obj.location).to_track_quat('-Z','Y').to_euler()
scene.cycles.samples=16;scene.render.resolution_percentage=75
for frame,name in [(1,'sf50_gear_retracted.png'),(111,'sf50_exterior.png')]:
    scene.frame_set(frame);scene.render.filepath=str(out/'previews'/name);bpy.ops.render.render(write_still=True)
