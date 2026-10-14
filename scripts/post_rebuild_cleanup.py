#!/usr/bin/env python
"""
Post-rebuild cleanup script for graphify knowledge graphs.

Called by git hooks after _rebuild_code() completes. Transforms the raw
(noisy) graph.json into a clean, clustered, community-labelled artifact.

Usage:
    python scripts/post_rebuild_cleanup.py

Must be run from the project root.
"""

import json
import os
import subprocess
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path

# Deterministic clustering: networkx louvain iterates string-keyed sets whose
# order is randomized per-process by PYTHONHASHSEED. Pinning it makes graphify-out
# reproducible across rebuilds.
os.environ['PYTHONHASHSEED'] = '0'

# ── Paths ──────────────────────────────────────────────────────────────────
# This script lives in scripts/ but operates on graphify's output directory,
# which is a sibling. GRAPHIFY_OUT honours the same env override graphify itself
# uses, so worktree and shared-output setups stay consistent; an absolute value
# replaces the project-relative default.
_SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = _SCRIPT_DIR.parent  # scripts/ is a child of the project root
GRAPHIFY_OUT = PROJECT_ROOT / os.environ.get('GRAPHIFY_OUT', 'graphify-out')
GRAPH_JSON = GRAPHIFY_OUT / 'graph.json'
REPORT_PATH = GRAPHIFY_OUT / 'GRAPH_REPORT.md'
LABELS_PATH = GRAPHIFY_OUT / '.graphify_labels.json'
HTML_PATH = GRAPHIFY_OUT / 'graph.html'

# ── Granularity ────────────────────────────────────────────────────────────
# The graph is kept at type and function level. graphify's own `_callable` flag
# is true for classes, structs, enums and functions and false for their members,
# which separates the API surface from its fields almost exactly. Dropping the
# falsy side removes member variables, enumerators and the external-include
# "concept" nodes - together about 44% of nodes - and roughly halves the page
# weight of graph.html. No call, inheritance or containment relation between
# symbols is lost; the only relation that disappears entirely is `defines`,
# which only ever linked a type to its own members.
#
# Set GRAPHIFY_KEEP_MEMBERS=1 to keep the detailed graph instead.
KEEP_MEMBER_NODES = os.environ.get('GRAPHIFY_KEEP_MEMBERS', '').lower() in ('1', 'true', 'yes')

# ── Noise label sets ───────────────────────────────────────────────────────

# Step 1: Generic AST noise — labels that represent C++ keywords, STL types,
# built-in primitives, and other AST-internal symbols with no topical value.
_NOISE_LABELS = {
    'namespace', 'neurus()', 'neurus', 'string', 'unordered_map',
    'vector', 'shared_ptr', 'unique_ptr', 'pair',
    'class', 'struct', 'void', 'int', 'bool', 'float', 'double', 'char',
    'auto', 'size_t', 'uint32_t', 'uint64_t', 'int32_t', 'int64_t',
    'nullptr', 'true', 'false',
    'return', 'if', 'else', 'for', 'while', 'switch', 'case', 'break',
    'continue', 'default',
    'public', 'private', 'protected',
    'virtual', 'override', 'static', 'const', 'mutable', 'inline',
    'explicit', 'friend',
    'typedef', 'using', 'template', 'typename',
    'this', 'new', 'delete', 'operator',
    'enum', 'union', 'extern', 'volatile',
    'unsigned', 'signed', 'long', 'short',
    'constexpr', 'decltype', 'noexcept',
    'static_cast', 'dynamic_cast', 'const_cast', 'reinterpret_cast',
    'main()', 'main',
    'span', 'sstream', 'ifstream', 'ofstream', 'fstream',
    'vk', 'vk()', 'glm', 'std',
    'array', 'map', 'set', 'list', 'deque', 'queue', 'stack',
    'tuple', 'function', 'bind', 'move', 'forward',
}

# Step 2: Third-party Vulkan-HPP type nodes
_VULKAN_HPP_LABELS = {
    'Device', 'PhysicalDevice', 'Queue', 'Format', 'Extent2D', 'DeviceSize',
    'CommandBuffer', 'Pipeline', 'DescriptorSetLayout', 'ImageView',
    'Sampler', 'SurfaceKHR', 'ShaderModule', 'ImageSubresourceRange',
    'ImageUsageFlags', 'MemoryPropertyFlags', 'BufferUsageFlags',
    'DescriptorBufferInfo', 'DescriptorImageInfo', 'DescriptorType',
    'DescriptorSetLayoutBinding', 'DescriptorBindingFlags',
    'DescriptorPoolSize', 'Semaphore', 'Fence', 'ClearValue',
    'ImageAspectFlags', 'ImageType', 'PipelineShaderStageCreateInfo',
    'ShaderStageFlagBits', 'AttachmentLoadOp', 'AttachmentStoreOp',
    'CommandPool', 'Instance', 'PipelineLayout', 'PresentModeKHR',
    'SurfaceFormatKHR', 'SwapchainKHR', 'VertexInputBindingDescription',
    'VKAPI_ATTR', 'VkDebugUtilsMessageSeverityFlagBitsEXT',
    'VkDebugUtilsMessageTypeFlagsEXT',
    'VkDebugUtilsMessengerCallbackDataEXT', 'VkCommandBuffer',
}

_GLM_LABELS = {'vec3', 'vec2', 'vec4', 'mat4', 'mat3', 'mat2', 'quat'}
_QT_LABELS = {
    'QString', 'QWidget', 'QObject', 'QWindow', 'QVulkanInstance',
    'QComboBox', 'QFormLayout', 'QCheckBox', 'QSlider', 'QSpinBox',
    'QStringList', 'QGroupBox', 'QPointF', 'QRectF', 'QSizeF', 'QVector3D', 'QMatrix4x4',
    'QModelIndex', 'QDoubleSpinBox', 'QKeyEvent', 'QMouseEvent',
    'QPaintEvent', 'QResizeEvent', 'QWheelEvent', 'HWND',
}
_SHADERC_LABELS = {'shaderc_shader_kind', 'shaderc_optimization_level'}

# Quick lookup helper: compile regex patterns for label prefix matching
def _is_std_label(label: str) -> bool:
    return label.startswith('std::') or label.startswith('std ')

def _is_vk_label(label: str) -> bool:
    return label.startswith('Vk') or label.startswith('vk::') or label.startswith('VK_')

def is_noise_label(label: str) -> bool:
    """Return True if label should be removed as noise or third-party."""
    if label in _NOISE_LABELS:
        return True
    if _is_std_label(label):
        return True
    if label in _VULKAN_HPP_LABELS:
        return True
    if _is_vk_label(label):
        return True
    if label in _GLM_LABELS:
        return True
    if label in _QT_LABELS:
        return True
    if label in _SHADERC_LABELS:
        return True
    return False


# ── Community-labeling rules ───────────────────────────────────────────────
# Each rule is (keywords_or_labels, label_name).
# keywords_or_labels: a tuple of strings — if ANY member matches ANY label in
# the community, the rule fires.
# When multiple rules match, the first wins.
_COMMUNITY_LABEL_RULES = [
    (('Barrier', 'ImageState'), 'Image & Barrier System'),
    (('EditorContext', 'SelectionManager', 'Property', 'Section', 'SpinBox'),
     'Editor & Property UI'),
    (('EditorContext', 'SelectionManager', 'UIEvents'), 'Editor & Context'),
    (('FromImageData', 'createImage', 'GenerateMipmaps'), 'GPU Image Core'),
    (('ShaderCompiler', 'CompileGlslToSpv'), 'Shader Compilation'),
    (('RenderShader', 'ShaderLibrary', 'ComputeShader'), 'Shader Compilation'),
    (('Application', 'Run()', 'InitVulkan'), 'Application Entry'),
    (('Swapchain', 'AcquireNextImage'), 'Swapchain'),
    (('VulkanContext', 'debugCallback'), 'Vulkan Context'),
    (('NeurusMainWindow', 'CreateDocks'), 'Main Window & UI'),
    (('ShaderStruct', 'ParseType'), 'Shader Struct Parsing'),
    (('SelectionController', 'RaycastSelect', 'CameraPushEvent'),
     'Scene & Camera Controllers'),
    (('RecordMouse', 'RecordKey', 'Edit()', 'GetInputState'), 'Input System'),
    (('LightType', 'ParseLightName'), 'Debug Primitives & Light'),
    (('DebugLine', 'PushDebugLine', 'PushDebugPoint'), 'Debug Primitives & Transform'),
    (('ShadowDepthPass', 'MeshGPU'), 'Descriptor & Shadow Pipeline'),
    (('CreatePipeline', 'BuildLayout', 'BuildComputePipeline'), 'Render Pipeline & SSAO'),
    (('GPUBuffer', 'StagingBuffer', 'UniformBuffer'), 'Buffer Hierarchy'),
    (('RenderCache', 'AttachmentName', 'Screenshot'), 'Render Cache & Screenshots'),
    (('createSampler', 'SaveTexture', 'Texture'), 'Texture & Material'),
    (('MeshData', 'LoadObj', 'LoadObjFromString'), 'Mesh Data Loading'),
    (('DescriptorManager', 'Allocate'), 'Descriptor & Buffer Layout'),
    (('PassType', 'PresetClearValues'), 'Pass System'),
    (('ComputeModelMatrix', 'GetDirection'), 'Transform Math'),
    (('HalfToFloat', 'SavePNG'), 'Image Data IO'),
    (('RecomputeMatrices',), 'Camera'),
    (('DeterministicRNG', 'rand01'), 'Deterministic RNG'),
    (('CreateDefault', 'Project'), 'Project Management'),
    (('DeferredRenderer', 'DrawFrame'), 'Deferred Renderer'),
    (('SocketOutT', 'SocketInT'), 'Node Graph'),
    (('IBLPushConstants',), 'IBL Push Constants'),
    (('SunShadowEval',), 'Shadow Eval Constants'),
    (('BufferLayout', 'GetBindingDescription'), 'Buffer Layout'),
    (('Dispatch', 'SetViewport'), 'Command Buffer'),
]


def _community_member_labels(G, community_node_ids):
    """Return set of labels for all non-file nodes in a community."""
    labels = set()
    for nid in community_node_ids:
        attrs = G.nodes.get(nid, {})
        label = attrs.get('label', '')
        file_type = attrs.get('file_type', '')
        if label and file_type != 'file':
            labels.add(label)
    return labels


def label_community(G, community_id, community_node_ids):
    """Assign a human-readable label to a community using content-based rules."""
    member_labels = _community_member_labels(G, community_node_ids)

    for keywords, label_name in _COMMUNITY_LABEL_RULES:
        # Check if any keyword matches any member label (exact or substring)
        # Use exact match first, then substring
        for kw in keywords:
            for ml in member_labels:
                if kw == ml or kw in ml:
                    return label_name

    # Fallback: use the first file node (strip extension)
    for nid in community_node_ids:
        label = G.nodes[nid].get('label', '')
        if label.endswith('.cpp') or label.endswith('.h') or label.endswith('.hpp'):
            return Path(label).stem
        # Also check source_file
        sf = G.nodes[nid].get('source_file', '')
        if sf:
            stem = Path(sf).stem
            if stem:
                return stem

    return f'Community {community_id}'


def deduplicate_labels(labels_dict):
    """Append ' (alt)' to repeated labels."""
    seen = Counter()
    result = {}
    for cid, label in sorted(labels_dict.items()):
        count = seen[label]
        if count > 0:
            result[cid] = f'{label} (alt)'
        else:
            result[cid] = label
        seen[label] += 1
    return result


# ── Graph loading helpers ──────────────────────────────────────────────────

def load_extraction(path):
    """Load graph.json and return the extraction dict."""
    with open(path, 'r', encoding='utf-8') as f:
        data = json.load(f)
    return data


def save_extraction(data, path):
    """Write the extraction dict back to graph.json."""
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(data, f, indent=2)


# ── Step 1: Remove generic AST noise nodes ────────────────────────────────

def remove_noise_nodes(data):
    """Remove nodes whose label is generic AST noise or third-party types."""
    nodes = data.get('nodes', [])
    node_ids_before = {n['id'] for n in nodes}

    keep = []
    removed = []
    for node in nodes:
        label = node.get('label', '')
        if is_noise_label(label):
            removed.append(node['id'])
        else:
            keep.append(node)

    removed_set = set(removed)
    data['nodes'] = keep

    # Remove edges whose source or target was removed
    links = data.get('links', [])
    data['links'] = [
        link for link in links
        if link.get('source') not in removed_set
        and link.get('target') not in removed_set
    ]

    n_removed = len(removed)
    if n_removed > 0:
        print(f'[cleanup] Removed {n_removed} noise nodes ({len(node_ids_before)} -> {len(keep)})')
    return data


# ── Step 2: Remove third-party type nodes ──────────────────────────────────
# Already handled by remove_noise_nodes above (third-party labels are in
# _NOISE_LABELS / _VULKAN_HPP_LABELS / etc.). This step is now integrated.


# ── Step 3: Rename file nodes (strip .cpp/.h suffix) and merge ────────────

def merge_file_nodes(data):
    """
    Rename file nodes (strip .cpp/.h/.hpp suffix) and merge into existing
    class nodes with the same name.
    """
    nodes = data.get('nodes', [])
    links = data.get('links', [])

    # Index nodes by ID
    node_by_id = {n['id']: n for n in nodes}

    # Find file-suffixed nodes
    file_suffixes = ('.cpp', '.h', '.hpp', '.cxx', '.hxx', '.cc', '.hh')
    suffixed_nodes = []
    for n in nodes:
        label = n.get('label', '')
        if any(label.endswith(s) for s in file_suffixes):
            suffixed_nodes.append(n)

    if not suffixed_nodes:
        return data

    # Build rename map: old_id -> (new_label, new_id_or_merge_target_id)
    # Also track which suffixed nodes to remove (merged into class node)
    rename_map = {}   # old_id -> new_id (for renaming)
    remove_ids = set()  # IDs of suffixed nodes to remove (merged)

    for sn in suffixed_nodes:
        old_label = sn.get('label', '')
        stem = Path(old_label).stem  # strip extension
        old_id = sn['id']

        # Check if a class node with the same label already exists
        target_node = None
        target_id = None
        for n in nodes:
            if n['id'] != old_id and n.get('label') == stem:
                target_node = n
                target_id = n['id']
                break

        if target_node:
            # Merge: redirect edges, remove suffixed node
            remove_ids.add(old_id)
            rename_map[old_id] = target_id
        else:
            # Rename: just change label and generate a new ID
            new_id = old_id + '_file'  # simpler: keep old id but rename label
            # Actually, simpler to keep the ID and just change the label
            sn['label'] = stem
            print(f'[cleanup] Renamed file node "{old_label}" -> "{stem}" (id={old_id})')

    if not rename_map:
        return data

    # Redirect edges: for edges pointing to removed nodes, rewire to target
    new_links = []
    redirected = 0
    for link in links:
        src = link.get('source', '')
        tgt = link.get('target', '')

        if src in rename_map:
            link['source'] = rename_map[src]
            redirected += 1
        if tgt in rename_map:
            link['target'] = rename_map[tgt]
            redirected += 1
        new_links.append(link)

    # Remove merged nodes
    data['nodes'] = [n for n in nodes if n['id'] not in remove_ids]
    data['links'] = new_links

    print(f'[cleanup] Merged {len(remove_ids)} file nodes into class nodes ({redirected} edges redirected)')
    return data


# ── Step 4: Merge duplicate labels ─────────────────────────────────────────

def merge_duplicate_labels(data):
    """
    Merge nodes with the same label: keep the one with the most edges,
    redirect all edges to the survivor, deduplicate edges, skip self-loops.
    """
    nodes = data.get('nodes', [])
    links = data.get('links', [])

    # Index nodes by ID
    node_by_id = {n['id']: n for n in nodes}

    # Count edges per node
    edge_counts = Counter()
    for link in links:
        edge_counts[link.get('source', '')] += 1
        edge_counts[link.get('target', '')] += 1

    # Group nodes by label
    label_to_nodes = defaultdict(list)
    for n in nodes:
        label_to_nodes[n.get('label', '')].append(n)

    duplicates_found = 0
    remove_ids = set()
    id_remap = {}  # old_id -> survivor_id

    for label, group in label_to_nodes.items():
        if len(group) <= 1:
            continue
        duplicates_found += 1

        # Sort by edge count descending — survivor is the most connected
        group.sort(key=lambda n: edge_counts.get(n['id'], 0), reverse=True)
        survivor = group[0]

        for dupe in group[1:]:
            remove_ids.add(dupe['id'])
            id_remap[dupe['id']] = survivor['id']

    if not id_remap:
        return data

    # Redirect edges
    new_links = []
    redirected = 0
    for link in links:
        src = link.get('source', '')
        tgt = link.get('target', '')

        if src in id_remap:
            link['source'] = id_remap[src]
        if tgt in id_remap:
            link['target'] = id_remap[tgt]

        new_src = link['source']
        new_tgt = link['target']

        # Skip self-loops
        if new_src == new_tgt:
            continue

        new_links.append(link)

    # Deduplicate edges: use set of (source, target, relation)
    seen_edges = set()
    deduped_links = []
    for link in new_links:
        key = (link.get('source', ''), link.get('target', ''), link.get('relation', ''))
        if key in seen_edges:
            continue
        seen_edges.add(key)
        deduped_links.append(link)

    # Remove merged nodes
    data['nodes'] = [n for n in nodes if n['id'] not in remove_ids]
    data['links'] = deduped_links

    print(f'[cleanup] Merged {len(remove_ids)} duplicate-label nodes ({duplicates_found} labels affected, '
          f'{redirected} edges redirected, {len(new_links) - len(deduped_links)} duplicate edges removed)')
    return data


# ── Step 5: Drop member-level nodes ────────────────────────────────────────

def remove_member_nodes(data):
    """
    Reduce the graph to types and functions, dropping their members.

    Runs last so the file-node and duplicate-label merges above have already
    redirected edges onto the surviving symbols; dropping members earlier would
    throw those edges away instead of rehoming them.
    """
    if KEEP_MEMBER_NODES:
        print('[cleanup] GRAPHIFY_KEEP_MEMBERS set - keeping member-level nodes')
        return data

    nodes = data.get('nodes', [])
    links = data.get('links', [])

    # A handful of enumerators carry _callable, so match them by edge role too:
    # the target of a `case_of` edge is by definition an enum member.
    enumerators = {
        link.get('target') for link in links
        if link.get('relation') == 'case_of'
    }

    keep, counts = [], Counter()
    remove_ids = set()
    for node in nodes:
        if node.get('file_type') == 'concept':
            counts['external include'] += 1
        elif node['id'] in enumerators:
            counts['enumerator'] += 1
        elif not node.get('_callable'):
            counts['member'] += 1
        else:
            keep.append(node)
            continue
        remove_ids.add(node['id'])

    if not remove_ids:
        return data

    links_before = len(links)
    data['nodes'] = keep
    data['links'] = [
        link for link in links
        if link.get('source') not in remove_ids
        and link.get('target') not in remove_ids
    ]

    detail = ', '.join(f'{v} {k}' for k, v in counts.most_common())
    print(f'[cleanup] Dropped {len(remove_ids)} member-level nodes ({detail}) '
          f'({len(nodes)} -> {len(keep)} nodes, '
          f'{links_before} -> {len(data["links"])} edges)')
    return data


# ── Cleanup pipeline ───────────────────────────────────────────────────────

def apply_cleanups(data):
    """Apply all cleanup steps to the extraction dict."""
    data = remove_noise_nodes(data)
    data = merge_file_nodes(data)
    data = merge_duplicate_labels(data)
    data = remove_member_nodes(data)
    return data


# ── Main ───────────────────────────────────────────────────────────────────

def main():
    t0 = time.time()

    if not GRAPH_JSON.exists():
        print(f'[cleanup] graph.json not found at {GRAPH_JSON} — nothing to clean')
        return

    print(f'[cleanup] Loading graph from {GRAPH_JSON}...')
    data = load_extraction(GRAPH_JSON)

    nodes_before = len(data.get('nodes', []))
    links_before = len(data.get('links', []))
    print(f'[cleanup] Before: {nodes_before} nodes, {links_before} edges')

    # ── Clean ───────────────────────────────────────────────────────────
    data = apply_cleanups(data)
    nodes_after_clean = len(data.get('nodes', []))
    links_after_clean = len(data.get('links', []))
    print(f'[cleanup] After cleanup: {nodes_after_clean} nodes, {links_after_clean} edges')

    # Early exit if nothing to build
    if nodes_after_clean == 0:
        print('[cleanup] No nodes remaining after cleanup — skipping cluster/report')
        return

    # ── Build NetworkX graph ─────────────────────────────────────────────
    print('[cleanup] Building NetworkX graph...')
    from graphify.build import build_from_json
    G = build_from_json(data, root=str(PROJECT_ROOT))
    print(f'[cleanup] Graph built: {G.number_of_nodes()} nodes, {G.number_of_edges()} edges')

    if G.number_of_nodes() == 0:
        print('[cleanup] Empty graph after build — nothing to cluster')
        return

    # ── Cluster ──────────────────────────────────────────────────────────
    print('[cleanup] Running community detection...')
    from graphify.cluster import cluster, score_all
    communities = cluster(G)
    print(f'[cleanup] Found {len(communities)} communities')

    # ── Prune small communities (<=3 nodes) ──────────────────────────────
    # Identify which node IDs belong to small communities
    small_community_ids = {
        cid for cid, members in communities.items()
        if len(members) <= 3
    }
    if small_community_ids:
        nodes_to_prune = set()
        for cid in small_community_ids:
            nodes_to_prune.update(communities[cid])

        # Remove those nodes from the graph
        G.remove_nodes_from(nodes_to_prune)
        print(f'[cleanup] Pruned {len(nodes_to_prune)} nodes from {len(small_community_ids)} small communities')

        # Re-cluster
        print('[cleanup] Re-clustering after pruning...')
        communities = cluster(G)
        print(f'[cleanup] After re-cluster: {len(communities)} communities')

    # ── Label communities ────────────────────────────────────────────────
    print('[cleanup] Labelling communities...')
    raw_labels = {}
    for cid, members in communities.items():
        raw_labels[cid] = label_community(G, cid, members)
    community_labels = deduplicate_labels(raw_labels)

    # Save labels
    with open(LABELS_PATH, 'w', encoding='utf-8') as f:
        json.dump(community_labels, f, indent=2)
    print(f'[cleanup] Saved {len(community_labels)} community labels to {LABELS_PATH}')

    # ── Compute analysis data ────────────────────────────────────────────
    print('[cleanup] Computing analysis data...')
    from graphify.analyze import god_nodes, surprising_connections, suggest_questions
    gods = god_nodes(G)
    surprises = surprising_connections(G, communities)
    questions = suggest_questions(G, communities, community_labels)

    cohesion_scores = score_all(G, communities)

    # Build a minimal detection_result from node source_files
    source_files = set()
    for nid in G.nodes():
        sf = G.nodes[nid].get('source_file', '')
        if sf:
            source_files.add(sf)
    detection_result = {
        'total_files': len(source_files),
        'total_words': G.number_of_nodes() * 5,  # rough estimate
        'warning': None,
    }

    token_cost = {'input': 0, 'output': 0}

    # ── Generate GRAPH_REPORT.md ─────────────────────────────────────────
    print('[cleanup] Generating GRAPH_REPORT.md...')
    from graphify.report import generate
    report_md = generate(
        G=G,
        communities=communities,
        cohesion_scores=cohesion_scores,
        community_labels=community_labels,
        god_node_list=gods,
        surprise_list=surprises,
        detection_result=detection_result,
        token_cost=token_cost,
        root=str(PROJECT_ROOT),
        suggested_questions=questions,
    )
    with open(REPORT_PATH, 'w', encoding='utf-8') as f:
        f.write(report_md)
    print(f'[cleanup] Saved {REPORT_PATH}')

    # ── Save graph.json ─────────────────────────────────────────────────
    print('[cleanup] Saving graph.json (force=True)...')
    from graphify.export import to_json
    ok = to_json(
        G=G,
        communities=communities,
        output_path=str(GRAPH_JSON),
        force=True,
        community_labels=community_labels,
    )
    if ok:
        print(f'[cleanup] Saved {GRAPH_JSON}')
    else:
        print(f'[cleanup] WARNING: to_json returned False for {GRAPH_JSON}, may not have been written')

    # ── Export HTML ───────────────────────────────────────────────────────
    print('[cleanup] Exporting HTML...')
    try:
        result = subprocess.run(
            [sys.executable, '-m', 'graphify', 'export', 'html'],
            cwd=str(PROJECT_ROOT),
            timeout=120,
            capture_output=True,
            text=True,
        )
        if result.returncode == 0:
            print('[cleanup] HTML export successful')
        else:
            print(f'[cleanup] HTML export stderr: {result.stderr.strip()}')
    except subprocess.TimeoutExpired:
        print('[cleanup] HTML export timed out')
    except Exception as exc:
        print(f'[cleanup] HTML export failed: {exc}')

    elapsed = time.time() - t0
    print(f'[cleanup] Done in {elapsed:.1f}s — '
          f'{G.number_of_nodes()} nodes, {G.number_of_edges()} edges, {len(communities)} communities')


if __name__ == '__main__':
    main()
