"""Reduce only SF50 landing-gear topology; retain rig, materials and motion."""
import bpy,json,shutil,hashlib,collections
import numpy as np
from pathlib import Path
from datetime import datetime
from mathutils import Vector
from mathutils.bvhtree import BVHTree
ROOT=Path(__file__).resolve().parents[2];OUT=ROOT/'games'/'CalderaDemo'/'source'/'aircraft'/'sf50'
WORK=ROOT/'builds'/'sf50_gear_optimization';WORK.mkdir(parents=True,exist_ok=True)
MASTER=OUT/'cirrus_sf-50.blend'
backup=ROOT/'builds'/'sf50_backups'/('before_gear_optimization_'+datetime.now().strftime('%Y%m%d_%H%M%S')+'.blend')
backup.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(MASTER,backup)
scene=bpy.context.scene;rig=bpy.data.objects['Landing gear rig']
gear=[o for o in bpy.data.objects if o.type=='MESH' and any(m.type=='ARMATURE' for m in o.modifiers)]
assert len(gear)==3 and len(bpy.data.objects)==17
assert not any(o.get('sf50_gear_optimized',False) for o in gear),'Use the full-detail backup rather than simplifying an already optimized master again.'
static=[o for o in bpy.data.objects if o.type=='MESH' and o not in gear]
def digest_mesh(obj):
    h=hashlib.sha256()
    for prop,field,size,dtype in [('vertices','co',3,np.float32),('loops','vertex_index',1,np.int32),('polygons','material_index',1,np.int32)]:
        collection=getattr(obj.data,prop);values=np.empty(len(collection)*size,dtype=dtype);collection.foreach_get(field,values);h.update(values.tobytes())
    return h.hexdigest()
fixed={o.name:digest_mesh(o) for o in static}
def tris(mesh):return sum(len(p.vertices)-2 for p in mesh.polygons)
def evaluated_points(obj,count=14000):
    deps=bpy.context.evaluated_depsgraph_get();ev=obj.evaluated_get(deps);mesh=ev.to_mesh()
    arr=np.empty(len(mesh.vertices)*3,dtype=np.float32);mesh.vertices.foreach_get('co',arr);arr=arr.reshape(-1,3)
    selected=arr[np.linspace(0,len(arr)-1,min(count,len(arr)),dtype=np.int64)]
    ev.to_mesh_clear();return selected
def quality(source,candidate):
    results={}
    for frame in (1,28,56,84,111):
        scene.frame_set(frame);bpy.context.view_layer.update();deps=bpy.context.evaluated_depsgraph_get()
        old=BVHTree.FromObject(source,deps);new=BVHTree.FromObject(candidate,deps)
        distances=[]
        for point in evaluated_points(source):
            hit=new.find_nearest(Vector(point))
            assert hit[0] is not None
            distances.append(hit[3])
        for point in evaluated_points(candidate,4000):
            hit=old.find_nearest(Vector(point));assert hit[0] is not None;distances.append(hit[3])
        results[str(frame)]={'max_m':float(np.max(distances)),'p999_m':float(np.quantile(distances,.999)),'mean_m':float(np.mean(distances))}
    return results
report={'backup':str(backup),'method':'Preserved low-polygon panels/decals verbatim; applied quadric collapse only to dense hardware before armature; UVs/material assignments/weights retained; hardware normals transferred from source bind mesh','meshes':[]}
for obj in gear:
    before=tris(obj.data);before_vertices=len(obj.data.vertices);accepted=None
    # Preserve a bind-space reference for transferring authored normals.
    reference=obj.copy();reference.data=obj.data.copy();reference.modifiers.clear();scene.collection.objects.link(reference)
    reference.name='TEMP normal reference';reference.hide_render=True
    for ratio in (.10,.16,.25):
        scene.frame_set(111)
        candidate=obj.copy();candidate.data=obj.data.copy();scene.collection.objects.link(candidate)
        candidate.name='TEMP optimized '+obj.name
        bpy.ops.object.select_all(action='DESELECT');candidate.select_set(True);bpy.context.view_layer.objects.active=candidate
        material_counts=collections.Counter()
        for face in candidate.data.polygons:material_counts[face.material_index]+=len(face.vertices)-2
        protected_materials={index for index,count in material_counts.items() if count<5000}
        # Do not simplify layered paint/decals independently: small offsets can
        # otherwise produce z-fighting even when their surface error is tiny.
        scene.tool_settings.mesh_select_mode=(False,False,True)
        bpy.ops.object.mode_set(mode='EDIT');bpy.ops.mesh.select_all(action='DESELECT');bpy.ops.object.mode_set(mode='OBJECT')
        for face in candidate.data.polygons:face.select=face.material_index in protected_materials
        previous_objects=set(bpy.data.objects)
        bpy.ops.object.mode_set(mode='EDIT');bpy.ops.mesh.separate(type='SELECTED');bpy.ops.object.mode_set(mode='OBJECT')
        protected=list(set(bpy.data.objects)-previous_objects)
        bpy.ops.object.select_all(action='DESELECT');candidate.select_set(True);bpy.context.view_layer.objects.active=candidate
        dec=candidate.modifiers.new('Gear topology reduction','DECIMATE');dec.decimate_type='COLLAPSE';dec.ratio=ratio;dec.use_collapse_triangulate=True
        bpy.ops.object.modifier_move_up(modifier=dec.name)
        bpy.ops.object.modifier_apply(modifier=dec.name)
        # Reconstruct corner normals from the full-detail bind mesh without
        # affecting UV coordinates, materials or vertex-group skinning weights.
        normal=candidate.modifiers.new('Preserve source normals','DATA_TRANSFER');normal.object=reference
        normal.use_loop_data=True;normal.data_types_loops={'CUSTOM_NORMAL'};normal.loop_mapping='POLYINTERP_NEAREST'
        bpy.ops.object.modifier_move_up(modifier=normal.name)
        bpy.ops.object.modifier_apply(modifier=normal.name)
        for part in protected:part.select_set(True)
        if protected:bpy.ops.object.join()
        checks=quality(obj,candidate)
        worst=max(v['max_m'] for v in checks.values());p999=max(v['p999_m'] for v in checks.values())
        print('GEAR CANDIDATE',obj.name,'ratio',ratio,'triangles',tris(candidate.data),'max',worst,'p999',p999,flush=True)
        # Conservative surface-distance limits across all sampled gear poses.
        if worst<=.018 and p999<=.004:
            accepted=candidate;break
        mesh=candidate.data;bpy.data.objects.remove(candidate,do_unlink=True)
        if mesh.users==0:bpy.data.meshes.remove(mesh)
    assert accepted is not None,'No reduction passed the surface checks for '+obj.name
    old_mesh=obj.data;obj.data=accepted.data
    bpy.data.objects.remove(accepted,do_unlink=True)
    if old_mesh.users==0:bpy.data.meshes.remove(old_mesh)
    ref_mesh=reference.data;bpy.data.objects.remove(reference,do_unlink=True)
    if ref_mesh.users==0:bpy.data.meshes.remove(ref_mesh)
    # Deform group order/names live on the copied mesh in Blender 5.2.
    assert all(g.name in rig.data.bones or g.name.startswith('Unused ') for g in obj.vertex_groups)
    obj.data.name=obj.name+' mesh'
    obj['sf50_gear_optimized']=True
    report['meshes'].append({'name':obj.name,'triangles_before':before,'triangles_after':tris(obj.data),'vertices_before':before_vertices,'vertices_after':len(obj.data.vertices),'ratio':ratio,'surface_checks':checks})
assert all(digest_mesh(o)==fixed[o.name] for o in static),'Static aircraft geometry changed'
assert len(bpy.data.objects)==17 and len(bpy.data.armatures)==1 and len(bpy.data.actions)==1
assert all(len(o.modifiers)==1 and o.modifiers[0].type=='ARMATURE' and o.modifiers[0].object==rig for o in gear)
assert all(i.packed_file or Path(bpy.path.abspath(i.filepath)).is_file() for i in bpy.data.images if i.source=='FILE')
report['triangles_before']=sum(m['triangles_before'] for m in report['meshes'])
report['triangles_after']=sum(m['triangles_after'] for m in report['meshes'])
report['reduction_percent']=100*(1-report['triangles_after']/report['triangles_before'])
report['vertices_before']=sum(m['vertices_before'] for m in report['meshes'])
report['vertices_after']=sum(m['vertices_after'] for m in report['meshes'])
report['vertex_reduction_percent']=100*(1-report['vertices_after']/report['vertices_before'])
report['static_geometry_unchanged']=True;report['rig_bones']=len(rig.data.bones)
scene.frame_set(111);scene['sf50_stage']='Gear topology optimized; gear-only rig; static aircraft unchanged'
notes=bpy.data.texts.get('SF50_README')
if notes:notes.write('\nLanding gear topology optimized; see sf50_gear_optimization_report.json. Full-detail backup: '+str(backup)+'\n')
bpy.ops.object.select_all(action='DESELECT');rig.select_set(True);bpy.context.view_layer.objects.active=rig
bpy.context.preferences.filepaths.save_version=0
# Save a candidate for visual inspection; the master is promoted after review.
bpy.ops.wm.save_as_mainfile(filepath=str(WORK/'optimized_candidate.blend'),compress=True,relative_remap=False)
# Relative texture paths need to resolve beside the master even during preview.
for img in bpy.data.images:
    if img.source=='FILE':img.filepath=str(OUT/Path(img.filepath.removeprefix('//')))
bpy.ops.wm.save_as_mainfile(filepath=str(WORK/'optimized_preview.blend'),compress=True,relative_remap=False)
(WORK/'optimization_report.json').write_text(json.dumps(report,indent=2))
print('GEAR OPTIMIZATION PASSED',json.dumps(report),flush=True)
