"""Export a Caldera-compatible SF50 without changing the editable blend."""
import bpy,json,math,struct
from pathlib import Path
from mathutils import Vector
ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'games'/'CalderaDemo'/'assets'/'models'/'sf50'
OUT.mkdir(parents=True,exist_ok=True)
STAGE=ROOT/'builds'/'sf50_export';STAGE.mkdir(parents=True,exist_ok=True)
scene=bpy.context.scene
assert len(bpy.data.objects)==17 and len(bpy.data.armatures)==1
scene.frame_start=1;scene.frame_end=111;scene.frame_set(111)
report={'source':'games/CalderaDemo/source/aircraft/sf50/cirrus_sf-50.blend','materials':[],'texture_resizes':[],'material_limitations':'Core glTF base color, vertex color and alpha; runtime model shader does not implement Blender clear coat, normal maps or Fresnel glass.'}

def constant(socket,seen=None):
    seen=set() if seen is None else seen.copy()
    if not socket.is_linked:
        value=socket.default_value
        return list(value) if hasattr(value,'__len__') else float(value)
    node=socket.links[0].from_node
    if node.as_pointer() in seen:return [1,1,1,1]
    seen.add(node.as_pointer())
    if node.type in ('TEX_IMAGE','VERTEX_COLOR','ATTRIBUTE'):return [1,1,1,1]
    if node.type in ('MIX','MIX_RGB'):
        inputs=[i for i in node.inputs if i.type=='RGBA']
        if len(inputs)>=2:
            a=constant(inputs[0],seen);b=constant(inputs[1],seen)
            a=a if isinstance(a,list) else [a]*4;b=b if isinstance(b,list) else [b]*4
            if node.blend_type=='MULTIPLY':return [x*y for x,y in zip(a,b)]
    return [1,1,1,1]

used_images=set()
for mat in bpy.data.materials:
    if not mat.node_tree:continue
    old=list(mat.node_tree.nodes)
    bsdf=next((n for n in old if n.type=='BSDF_PRINCIPLED'),None)
    glass=bsdf is None and any(n.type=='FRESNEL' for n in old)
    texture=next((n for n in old if n.type=='TEX_IMAGE' and n.image and 'base_color' in n.image.filepath.lower()),None)
    img=texture.image if texture else None
    vertex=next((n for n in old if n.type=='VERTEX_COLOR' and n.outputs[0].is_linked),None)
    vertex_name=vertex.layer_name if vertex else None
    factor=constant(bsdf.inputs['Base Color']) if bsdf else [.72,.82,.9,1]
    if not isinstance(factor,list):factor=[factor]*3+[1]
    alpha=float(bsdf.inputs['Alpha'].default_value) if bsdf else .18
    if glass:
        source=mat.get('sf50_source_material',mat.name)
        if 'SunVisor' in source:factor=[.35,.29,.18,1];alpha=.4
        if 'Red_Glass' in source:factor=[.75,.008,.004,1];alpha=.45
    use_alpha=glass or (bsdf and bsdf.inputs['Alpha'].is_linked) or alpha<.999
    mat.node_tree.nodes.clear();nodes=mat.node_tree.nodes;links=mat.node_tree.links
    shader=nodes.new('ShaderNodeBsdfPrincipled');shader.inputs['Base Color'].default_value=factor;shader.inputs['Alpha'].default_value=alpha
    shader.inputs['Metallic'].default_value=0;shader.inputs['Roughness'].default_value=.5
    output=nodes.new('ShaderNodeOutputMaterial');links.new(shader.outputs['BSDF'],output.inputs['Surface'])
    color_output=None
    if img:
        tex=nodes.new('ShaderNodeTexImage');tex.image=img;used_images.add(img)
        color_output=tex.outputs['Color']
        mult=nodes.new('ShaderNodeMix');mult.data_type='RGBA';mult.blend_type='MULTIPLY';mult.inputs[0].default_value=1;mult.inputs[7].default_value=factor
        links.new(color_output,mult.inputs[6]);color_output=mult.outputs[2]
        if use_alpha:
            alpha_node=nodes.new('ShaderNodeMath');alpha_node.operation='MULTIPLY';alpha_node.inputs[1].default_value=alpha
            links.new(tex.outputs['Alpha'],alpha_node.inputs[0]);links.new(alpha_node.outputs[0],shader.inputs['Alpha'])
    if vertex_name is not None:
        attr=nodes.new('ShaderNodeVertexColor');attr.layer_name=vertex_name
        mult=nodes.new('ShaderNodeMix');mult.data_type='RGBA';mult.blend_type='MULTIPLY';mult.inputs[0].default_value=1
        if color_output:links.new(color_output,mult.inputs[6])
        else:mult.inputs[6].default_value=factor
        links.new(attr.outputs['Color'],mult.inputs[7]);color_output=mult.outputs[2]
    if color_output:links.new(color_output,shader.inputs['Base Color'])
    if use_alpha:mat.surface_render_method='BLENDED'
    report['materials'].append({'name':mat.name,'base_color':factor,'alpha':alpha,'texture':img.name if img else None,'vertex_color':vertex_name,'glass_approximation':glass})
for img in used_images:
    width,height=img.size
    limit=2048 if any(s in img.name for s in ('fuselage','wings','livery')) else 1024
    if max(width,height)>limit:
        scale=limit/max(width,height);size=(max(1,round(width*scale)),max(1,round(height*scale)))
        img.scale(*size);report['texture_resizes'].append({'image':img.name,'from':[width,height],'to':list(size)})
rig=bpy.data.objects['Landing gear rig'];rig.animation_data.action.name='Gear_Retract_Extend'
points=[o.matrix_world@Vector(c) for o in bpy.data.objects if o.type=='MESH' for c in o.evaluated_get(bpy.context.evaluated_depsgraph_get()).bound_box]
report['blender_bounds_min']=[min(p[i] for p in points) for i in range(3)]
report['blender_bounds_max']=[max(p[i] for p in points) for i in range(3)]
report['source_fps']=scene.render.fps/scene.render.fps_base
report['expected_clip_duration_seconds']=110/report['source_fps']
bpy.ops.object.select_all(action='SELECT')
bpy.ops.export_scene.gltf(filepath=str(STAGE/'sf50.glb'),export_format='GLB',use_selection=True,export_yup=True,export_animations=True,export_animation_mode='ACTIONS',export_force_sampling=True,export_frame_range=True,export_frame_step=1,export_def_bones=True,export_leaf_bone=False,export_all_influences=True,export_all_vertex_colors=False,export_cameras=False,export_lights=False,export_extras=False)
with (STAGE/'sf50.glb').open('rb') as stream:
    header=stream.read(20);length=struct.unpack_from('<I',header,12)[0];document=json.loads(stream.read(length))
assert len(document['meshes'])==16 and len(document['skins'])==1 and len(document['animations'])==1
# Publish only complete exports so an editor watcher cannot cook a partial GLB.
(STAGE/'sf50.glb').replace(OUT/'sf50.glb')
report['glb_bytes']=(OUT/'sf50.glb').stat().st_size
dest=ROOT/'games'/'CalderaDemo'/'source'/'sf50_export_report.json';dest.write_text(json.dumps(report,indent=2))
print('SF50 EXPORTED',report['glb_bytes'],'bytes; duration',report['expected_clip_duration_seconds'],flush=True)
