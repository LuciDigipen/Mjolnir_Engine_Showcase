"""Validate the exported GLB and author a parked SF50 in the Caldera scene."""
import json,struct,math
from pathlib import Path
import numpy as np
ROOT=Path(__file__).resolve().parents[2];GAME=ROOT/'games'/'CalderaDemo'
def glb(path):
    data=path.read_bytes();magic,version,length=struct.unpack_from('<III',data)
    assert magic==0x46546c67 and version==2 and length==len(data)
    offset=12;doc=None;binary=b''
    while offset<len(data):
        size,kind=struct.unpack_from('<II',data,offset);offset+=8
        chunk=data[offset:offset+size];offset+=size
        if kind==0x4e4f534a:doc=json.loads(chunk)
        elif kind==0x004e4942:binary=chunk
    return doc,binary
def accessor(doc,binary,index):
    a=doc['accessors'][index];v=doc['bufferViews'][a['bufferView']]
    types={5126:'<f4',5125:'<u4',5123:'<u2',5121:'u1',5122:'<i2',5120:'i1'}
    size={'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4,'MAT4':16}[a['type']]
    dtype=np.dtype(types[a['componentType']]);start=v.get('byteOffset',0)+a.get('byteOffset',0)
    return np.ndarray((a['count'],size),dtype=dtype,buffer=binary,offset=start,strides=(v.get('byteStride',dtype.itemsize*size),dtype.itemsize)).copy()
doc,binary=glb(GAME/'assets'/'models'/'sf50'/'sf50.glb')
assert len(doc.get('animations',[]))==1
assert len(doc.get('skins',[]))==1
assert len([n for n in doc['nodes'] if 'skin' in n])==3
assert len(doc['meshes'])==16
assert not doc.get('extensionsRequired'),doc.get('extensionsRequired')
animation=doc['animations'][0]
duration=max(float(accessor(doc,binary,s['input']).max()) for s in animation['samplers'])
assert duration>0
for channel in animation['channels']:
    target=doc['nodes'][channel['target']['node']]
    assert 'mesh' not in target,'Static mesh animated: '+target.get('name','')
for material in doc['materials']:
    assert all(math.isfinite(x) for x in material.get('pbrMetallicRoughness',{}).get('baseColorFactor',[1]*4))
    if 'Fuselage_1'==material['name'].removeprefix('SF50_'):
        color=material['pbrMetallicRoughness']['baseColorFactor'];assert color[0]<.05,color
runway,rbin=glb(GAME/'assets'/'models'/'runway.glb')
primitives=runway['meshes'][0]['primitives']
base=next((p for p in primitives if 'concrete_brushed' in runway['materials'][p['material']].get('name','')),primitives[0])
positions=accessor(runway,rbin,base['attributes']['POSITION'])
indices=accessor(runway,rbin,base['indices']).reshape(-1) if 'indices' in base else np.arange(len(positions))
triangles=positions[indices].reshape(-1,3,3)
points=positions[:,[0,2]];eigenvalues,eigenvectors=np.linalg.eigh(np.cov(points.T));direction=eigenvectors[:,-1]
if direction[1]<0:direction=-direction
def ground(xz):
    p=triangles[:,:,[0,2]];a=p[:,0];b=p[:,1]-a;c=p[:,2]-a;q=np.asarray(xz)-a
    det=b[:,0]*c[:,1]-c[:,0]*b[:,1];valid=np.abs(det)>1e-10
    u=np.zeros(len(det));v=np.zeros(len(det))
    u[valid]=(q[valid,0]*c[valid,1]-c[valid,0]*q[valid,1])/det[valid]
    v[valid]=(b[valid,0]*q[valid,1]-q[valid,0]*b[valid,1])/det[valid]
    hit=valid&(u>=-1e-5)&(v>=-1e-5)&(u+v<=1+1e-5)
    if not hit.any():return None
    return float(np.max(triangles[hit,0,1]+u[hit]*(triangles[hit,1,1]-triangles[hit,0,1])+v[hit]*(triangles[hit,2,1]-triangles[hit,0,1])))
offset=direction*-40
height=ground(offset)
if height is None:offset=np.array([0.,0.]);height=ground(offset)
assert height is not None,'No runway surface under aircraft'
scene_dir=GAME/'scenes'/'default';runway_entity=json.loads((scene_dir/'entities'/'4.json').read_text())
origin=next(c['fields']['position'] for c in runway_entity['components'] if c['type']=='Transform')
export=json.loads((GAME/'source'/'sf50_export_report.json').read_text())
position=[origin[0]+float(offset[0]),origin[1]+height-export['blender_bounds_min'][2]+.015,origin[2]+float(offset[1])]
yaw=math.atan2(float(direction[0]),float(direction[1]));rotation=[0,math.sin(yaw/2),0,math.cos(yaw/2)]
scene_path=scene_dir/'scene.json';scene=json.loads(scene_path.read_text())
existing=[]
for path in (scene_dir/'entities').glob('*.json'):
    ent=json.loads(path.read_text())
    if any(c['type']=='Name' and c['fields'].get('name')=='Cirrus SF50 - Arrivee Stellar' for c in ent.get('components',[])):existing.append(ent['id'])
identifier=existing[0] if existing else scene['nextId']
assert existing or not (scene_dir/'entities'/f'{identifier}.json').exists()
entity={'id':identifier,'parent':0,'components':[{'type':'Transform','fields':{'position':position,'rotation':rotation,'scale':[1,1,1]}},{'type':'WorldTransform','fields':{}},{'type':'Name','fields':{'name':'Cirrus SF50 - Arrivee Stellar'}},{'type':'ModelPlayer','fields':{'modelName':'models/sf50/sf50.glb','modelAsset':'','animationAsset':'','scene':-1,'clip':0,'time':duration,'playing':False,'loop':False,'speed':1.0}}]}
(scene_dir/'entities'/f'{identifier}.json').write_text(json.dumps(entity,indent=2)+'\n')
if identifier not in scene['entities']:scene['entities'].append(identifier)
scene['nextId']=max(scene['nextId'],identifier+1);scene_path.write_text(json.dumps(scene,indent=2)+'\n')
report={'entity':identifier,'position':position,'rotation':rotation,'gear_clip':animation.get('name'),'gear_extended_time_seconds':duration,'meshes':len(doc['meshes']),'materials':len(doc['materials']),'images':len(doc.get('images',[])),'skins':len(doc['skins']),'animation_channels':len(animation['channels']),'runway_local_surface_y':height,'runway_heading_degrees':math.degrees(yaw),'export_bounds_min':export['blender_bounds_min']}
(GAME/'source'/'sf50_scene_report.json').write_text(json.dumps(report,indent=2)+'\n')
print('SF50 SCENE AUTHORED',json.dumps(report),flush=True)
