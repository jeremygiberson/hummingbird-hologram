"""GLB model loader — loads hummingbird.glb via pygltflib, extracts geometry
and skeletal data, and builds ModernGL buffers."""

import struct
import numpy as np
from pygltflib import GLTF2

from config import MODEL_PATH


def _accessor_data(gltf, accessor_index):
    """Read raw accessor data into a numpy array."""
    accessor = gltf.accessors[accessor_index]
    buffer_view = gltf.bufferViews[accessor.bufferView]
    data = gltf.binary_blob()

    offset = (buffer_view.byteOffset or 0) + (accessor.byteOffset or 0)
    count = accessor.count

    # Map component type to numpy dtype
    comp_map = {
        5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16,
        5125: np.uint32, 5126: np.float32,
    }
    dtype = comp_map[accessor.componentType]

    # Map type to number of components
    type_map = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
    num_components = type_map[accessor.type]

    byte_stride = buffer_view.byteStride
    element_size = np.dtype(dtype).itemsize * num_components

    if byte_stride and byte_stride != element_size:
        # Strided access — read element-by-element
        arr = np.empty((count, num_components), dtype=dtype)
        for i in range(count):
            start = offset + i * byte_stride
            arr[i] = np.frombuffer(data, dtype=dtype, count=num_components, offset=start)
    else:
        arr = np.frombuffer(data, dtype=dtype, count=count * num_components, offset=offset)
        if num_components > 1:
            arr = arr.reshape((count, num_components))

    return arr


class MeshGroup:
    """A named subset of the model's geometry (e.g., body, left_wing, right_wing)."""
    __slots__ = ("name", "positions", "normals", "texcoords", "indices",
                 "joints", "weights", "face_count")

    def __init__(self, name):
        self.name = name
        self.positions = None
        self.normals = None
        self.texcoords = None
        self.indices = None
        self.joints = None
        self.weights = None
        self.face_count = 0


def _classify_vertices_by_bone(joints, weights):
    """Return a per-vertex label: 'left_wing', 'right_wing', or 'body'
    based on which bone has the highest weight.

    From CLAUDE.md joint order:
    [0=rootJoint, 1=Bone_12(body), 2=Bone.001_6(neck), 3=Bone.002_5(head),
     4=Bone.003_7(tail), 5=Wing1.R_9, 6=Wing2.R_8, 7=Wing1.L_11, 8=Wing2.L_10]
    """
    RIGHT_WING_JOINTS = {5, 6}
    LEFT_WING_JOINTS = {7, 8}

    n = len(joints)
    labels = []
    for i in range(n):
        # Find dominant joint
        max_idx = 0
        max_w = 0.0
        for j in range(4):
            if weights[i][j] > max_w:
                max_w = weights[i][j]
                max_idx = int(joints[i][j])
        if max_idx in RIGHT_WING_JOINTS:
            labels.append("right_wing")
        elif max_idx in LEFT_WING_JOINTS:
            labels.append("left_wing")
        else:
            labels.append("body")
    return labels


def load_model(path=None):
    """Load the GLB and return a dict of MeshGroup by name, plus skin data.

    Returns:
        groups: dict[str, MeshGroup] with keys 'body', 'left_wing', 'right_wing'
        skin_data: dict with 'joint_names', 'inverse_bind_matrices', 'joint_order'
        all_positions: the raw position array for bounding box / camera setup
    """
    if path is None:
        path = MODEL_PATH

    gltf = GLTF2.load(path)

    # Collect geometry from all mesh primitives
    all_positions = []
    all_normals = []
    all_texcoords = []
    all_indices = []
    all_joints = []
    all_weights = []
    vertex_offset = 0

    for mesh in gltf.meshes:
        for prim in mesh.primitives:
            attrs = prim.attributes

            positions = _accessor_data(gltf, attrs.POSITION).astype(np.float32)
            normals = _accessor_data(gltf, attrs.NORMAL).astype(np.float32) if attrs.NORMAL is not None else np.zeros_like(positions)
            texcoords = _accessor_data(gltf, attrs.TEXCOORD_0).astype(np.float32) if attrs.TEXCOORD_0 is not None else np.zeros((len(positions), 2), dtype=np.float32)

            indices = _accessor_data(gltf, prim.indices).flatten().astype(np.uint32)

            has_skin = attrs.JOINTS_0 is not None and attrs.WEIGHTS_0 is not None
            if has_skin:
                joints = _accessor_data(gltf, attrs.JOINTS_0).astype(np.float32)
                weights = _accessor_data(gltf, attrs.WEIGHTS_0).astype(np.float32)
            else:
                joints = np.zeros((len(positions), 4), dtype=np.float32)
                weights = np.zeros((len(positions), 4), dtype=np.float32)
                weights[:, 0] = 1.0  # bind everything to root

            all_positions.append(positions)
            all_normals.append(normals)
            all_texcoords.append(texcoords)
            all_indices.append(indices + vertex_offset)
            all_joints.append(joints)
            all_weights.append(weights)
            vertex_offset += len(positions)

    positions = np.concatenate(all_positions)
    normals = np.concatenate(all_normals)
    texcoords = np.concatenate(all_texcoords)
    indices = np.concatenate(all_indices)
    joints = np.concatenate(all_joints)
    weights = np.concatenate(all_weights)

    # Classify vertices into body / left_wing / right_wing
    labels = _classify_vertices_by_bone(joints, weights)

    # Build per-face groups: a face belongs to a group if the majority of its
    # vertices belong to that group
    group_names = ("body", "left_wing", "right_wing")
    group_indices = {name: [] for name in group_names}

    for i in range(0, len(indices), 3):
        face_labels = [labels[indices[i]], labels[indices[i + 1]], labels[indices[i + 2]]]
        # majority vote
        from collections import Counter
        dominant = Counter(face_labels).most_common(1)[0][0]
        group_indices[dominant].extend([indices[i], indices[i + 1], indices[i + 2]])

    groups = {}
    for name in group_names:
        g = MeshGroup(name)
        idx = np.array(group_indices[name], dtype=np.uint32)
        g.indices = idx
        g.face_count = len(idx) // 3
        # Store references to the shared vertex arrays — the indices select into them
        g.positions = positions
        g.normals = normals
        g.texcoords = texcoords
        g.joints = joints
        g.weights = weights
        groups[name] = g

    # Extract skin / joint info
    skin_data = None
    if gltf.skins:
        skin = gltf.skins[0]
        ibm = _accessor_data(gltf, skin.inverseBindMatrices).astype(np.float32)
        joint_node_indices = skin.joints
        joint_names = [gltf.nodes[j].name for j in joint_node_indices]
        skin_data = {
            "joint_names": joint_names,
            "inverse_bind_matrices": ibm,
            "joint_node_indices": joint_node_indices,
        }

    print(f"[model] Loaded {len(positions)} vertices, {len(indices) // 3} faces")
    for name, g in groups.items():
        print(f"  {name}: {g.face_count} faces")

    return groups, skin_data, positions
