import neurus

mesh = neurus.data.meshes["Ground"]
print(mesh.position)
mesh.position = (2, 0, 0)
print(mesh.position)
mesh.scale = (2, 1, 1)
print(mesh.scale)
mesh.rotation = (0, 0, 45)