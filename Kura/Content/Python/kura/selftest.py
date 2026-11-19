"""Headless self-test drivers: asset-tree round-trip + master uniqueness
(the SPEC 9 acceptance chain), run across two throwaway projects.

In the host project:   import kura.selftest as s; s.run_host_test()
In the target project: import kura.selftest as s; s.run_target_test()

Test chain (created procedurally, laid out by the SPEC 5.4 convention:
a root plus its exclusive deps per folder, shared assets parked centrally):
SM_KuraTest -> MI_AA -> M_A -> T_KuraTest ; MI_AB -> M_A (shared master)
"""
import base64
import json
import os
import shutil
import tempfile

import unreal

from . import (catalog, config, deps, editor_api, importer, ingest, library,
               manage, pack, pcg, store)

TEX = config.LIB_PREFIX + "/Shared/Textures/T_KuraTest"
MA = config.LIB_PREFIX + "/Shared/Master/M_A"
AA = config.LIB_PREFIX + "/KuraTest/Chair/MI_AA"
SM = config.LIB_PREFIX + "/KuraTest/Chair/SM_KuraTest"
AB = config.LIB_PREFIX + "/KuraTest/Lamp/MI_AB"
# A texture sitting next to the mesh that uses it: blocked before 2026-08-10
# (wrong type root), now the recommended layout (Q24).
COLOCATED = config.LIB_PREFIX + "/KuraTest/Chair/T_ChairOnly"
OUTSIDE = "/Game/TestAssets/T_Outside"

# PCG graphs (SPEC F9): two graphs sharing one subgraph, an instance, and two
# that must be refused - content two levels down, and a hard reference to a BP.
PCG_SUB = config.LIB_PREFIX + "/Shared/PCG/PCGSG_Scatter"
PCG_FOREST = config.LIB_PREFIX + "/KuraTest/PCG/Forest/PCG_Forest"
PCG_ROCKS = config.LIB_PREFIX + "/KuraTest/PCG/Rocks/PCG_Rocks"
PCG_ROCKS_I = config.LIB_PREFIX + "/KuraTest/PCG/Rocks/PCGI_Rocks"
PCG_DIRTY_SUB = config.LIB_PREFIX + "/KuraTest/PCG/Dirty/PCGSG_Dirty"
PCG_DIRTY = config.LIB_PREFIX + "/KuraTest/PCG/Dirty/PCG_Dirty"
PCG_ACTOR = config.LIB_PREFIX + "/KuraTest/PCG/Actor/PCG_SpawnsActor"
PCG_BP = config.LIB_PREFIX + "/KuraTest/PCG/Actor/BP_Spawned"
PCG_BUILTIN = "/PCG/Subgraphs/ElevationIsolines"
ENGINE_SPHERE = "/Engine/BasicShapes/Sphere"


def _check(cond, msg):
    if not cond:
        raise RuntimeError("Kura selftest FAILED: %s" % msg)


def _asset_sub():
    return unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)


def _statuses(result):
    return {i["package"]: i["status"] for i in result["items"]}


def _index_snapshot():
    """Everything library.db holds, as comparable data: assets + tags from
    search(), the deps table through referrers_of(), the collections table
    through collection_counts().

    The commit paths stopped rebuilding the whole index per operation
    (2026-08-23), so a row the incremental update forgets would otherwise only
    surface much later as a wrong search result."""
    rows = sorted((r["package"], r["class"], r["is_root"], r["notes"],
                   tuple(sorted(r["tags"])))
                  for r in library.search(sort="name"))
    refs = sorted((r[0], tuple(library.referrers_of(r[0]))) for r in rows)
    return rows, refs, sorted(library.collection_counts().items())


def _selected(result):
    """The packages a preview says the user picked, as opposed to the ones
    the closure pulled in (the preview dialog's second axis, 2026-08-23)."""
    return {i["package"] for i in result["items"] if i.get("selected")}


def _init_library():
    """editor_api.init_library() decoded - the C++ bridge hands C++ base64 of
    the JSON, and the first-run flow is worth testing through the entry point
    it actually calls (2026-08-27)."""
    return json.loads(base64.b64decode(editor_api.init_library()).decode("utf-8"))


def _run_seed_check():
    """A brand-new library opens with the starting collection tree, in the
    order DEFAULT_TREE lists it (2026-08-27). Runs on the reset library,
    before anything is filed, and hands back an empty tree so the collection
    checks downstream still start from nothing."""
    _check(not os.path.isfile(config.COLLECTIONS_FILE),
           "seed check must start with no collections.json")
    r = _init_library()
    _check(r["ok"] and r["entries"] == 0, "init_library on a fresh root: %s" % r)
    _check(r["collections"] == len(catalog.DEFAULT_TREE),
           "a new library must be seeded with the whole tree: %s" % r)

    rows = catalog.list_tree()["collections"]
    items = catalog.load()["collections"]
    _check([catalog.path_of(c["id"], items) for c in rows]
           == list(catalog.DEFAULT_TREE),
           "the seeded tree must read back in DEFAULT_TREE order")
    _check(all(c["count"] == 0 for c in rows),
           "a seeded tree files nothing by itself")

    _check(_init_library()["collections"] == 0, "seeding must not run twice")
    _check(len(catalog.list_tree()["collections"]) == len(catalog.DEFAULT_TREE),
           "the second init must leave the tree exactly as it was")

    # The guard is the file, not an empty tree: emptying the tree is a
    # decision, and finding it back after the next restart would make that
    # decision unmakeable.
    for top in [c for c in catalog.load()["collections"] if not c["parent"]]:
        catalog.delete(top["id"])
    _check(catalog.list_tree()["collections"] == [],
           "deleting every top-level collection must empty the tree")
    _check(_init_library()["collections"] == 0,
           "an emptied tree must never be re-seeded")
    _check(catalog.list_tree()["collections"] == [],
           "re-opening an emptied library must leave it empty")


def _reset_library():
    """Archive every existing entry so the run starts from an empty tree."""
    # The run may point at a library that does not exist yet. Create it the
    # way first-run setup does - the index no longer makes the root on its
    # own (2026-11-17).
    store.ensure_library_dirs()
    packages = [m["package"] for m in store.iter_metas()]
    if packages:
        batch = store.archive(packages)
        unreal.log_warning("Kura selftest: archived %d pre-existing entr(ies) "
                           "to %s" % (len(packages), batch))
    if os.path.isfile(config.COLLECTIONS_FILE):
        os.remove(config.COLLECTIONS_FILE)  # F8 tree is per-run state too
    library.rebuild_index()


def _make_assets():
    sub = _asset_sub()
    at = unreal.AssetToolsHelpers.get_asset_tools()
    mel = unreal.MaterialEditingLibrary

    for folder in (config.LIB_PREFIX, "/Game/TestAssets"):
        if sub.does_directory_exist(folder):
            unreal.log_warning("Kura selftest: deleting leftover %s" % folder)
            sub.delete_directory(folder)

    # texture: duplicate an engine texture (file import asserts headless)
    src = None
    for cand in ("/Engine/EngineResources/DefaultTexture",
                 "/Engine/EngineResources/WhiteSquareTexture",
                 "/Engine/EngineResources/GradientTexture0"):
        if sub.does_asset_exist(cand):
            src = cand
            break
    _check(src, "no engine source texture found")
    _check(sub.duplicate_asset(src, TEX), "duplicate texture")
    tex = sub.load_asset(TEX)

    folder, name = MA.rsplit("/", 1)
    mat = at.create_asset(name, folder, unreal.Material, unreal.MaterialFactoryNew())
    _check(mat, "create M_A")
    node = mel.create_material_expression(
        mat, unreal.MaterialExpressionTextureSampleParameter2D, -384, 0)
    node.set_editor_property("parameter_name", "BaseTex")
    node.set_editor_property("texture", tex)
    mel.connect_material_property(node, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.recompile_material(mat)

    for mi_pkg in (AA, AB):
        folder, name = mi_pkg.rsplit("/", 1)
        mi = at.create_asset(name, folder, unreal.MaterialInstanceConstant,
                             unreal.MaterialInstanceConstantFactoryNew())
        _check(mi, "create %s" % name)
        mel.set_material_instance_parent(mi, mat)
        mel.set_material_instance_texture_parameter_value(mi, "BaseTex", tex)

    _check(sub.duplicate_asset("/Engine/BasicShapes/Cube", SM), "duplicate cube")
    sm = sub.load_asset(SM)
    sm.set_material(0, sub.load_asset(AA))

    for pkg in (TEX, MA, AA, AB, SM):
        _check(sub.save_asset(pkg, False), "save %s" % pkg)
    unreal.log("Kura selftest: test chain created")


def _modify_master(two_sided):
    sub = _asset_sub()
    mat = sub.load_asset(MA)
    mat.set_editor_property("two_sided", two_sided)
    unreal.MaterialEditingLibrary.recompile_material(mat)
    _check(sub.save_asset(MA, False), "save modified M_A")


def _new_pcg_graph(pkg):
    folder, name = pkg.rsplit("/", 1)
    graph = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        name, folder, unreal.PCGGraph, unreal.PCGGraphFactory())
    _check(graph, "create %s" % name)
    return graph


def _add_spawner(graph, meshes):
    """A Static Mesh Spawner whose weighted entries point at the given meshes -
    soft references, which is exactly what the clean rule is about."""
    _node, settings = graph.add_node_of_type(unreal.PCGStaticMeshSpawnerSettings)
    selector = settings.get_editor_property("mesh_selector_parameters")
    entries = []
    for mesh in meshes:
        descriptor = unreal.PCGSoftISMComponentDescriptor()
        descriptor.set_editor_property("static_mesh", mesh)
        entry = unreal.PCGMeshSelectorWeightedEntry()
        entry.set_editor_property("descriptor", descriptor)
        entries.append(entry)
    selector.set_editor_property("mesh_entries", entries)


def _add_subgraph(graph, subgraph):
    _node, settings = graph.add_node_of_type(unreal.PCGSubgraphSettings)
    settings.get_editor_property("subgraph_instance").set_editor_property("graph", subgraph)


def _make_pcg_assets():
    sub = _asset_sub()
    at = unreal.AssetToolsHelpers.get_asset_tools()
    # Plugin content is not in a commandlet's registry until asked for - the
    # fixture's own need, not something Kura's code paths ever do.
    unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous(
        [PCG_BUILTIN.rsplit("/", 1)[0]], True)
    builtin = sub.load_asset(PCG_BUILTIN)
    _check(isinstance(builtin, unreal.PCGGraphInterface),
           "built-in subgraph %s missing" % PCG_BUILTIN)

    shared = _new_pcg_graph(PCG_SUB)
    _add_spawner(shared, [])  # a spawner with no mesh set is the clean form

    forest = _new_pcg_graph(PCG_FOREST)
    _add_subgraph(forest, shared)
    _add_subgraph(forest, builtin)
    _add_spawner(forest, [sub.load_asset(ENGINE_SPHERE)])  # a placeholder

    rocks = _new_pcg_graph(PCG_ROCKS)
    _add_subgraph(rocks, shared)

    folder, name = PCG_ROCKS_I.rsplit("/", 1)
    instance = at.create_asset(name, folder, unreal.PCGGraphInstance,
                               unreal.PCGGraphInstanceFactory())
    _check(instance, "create %s" % name)
    instance.set_editor_property("graph", rocks)

    dirty_sub = _new_pcg_graph(PCG_DIRTY_SUB)
    _add_spawner(dirty_sub, [sub.load_asset(SM)])
    _add_subgraph(_new_pcg_graph(PCG_DIRTY), dirty_sub)

    folder, name = PCG_BP.rsplit("/", 1)
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", unreal.Actor)
    _check(at.create_asset(name, folder, unreal.Blueprint, factory), "create %s" % name)
    _node, settings = _new_pcg_graph(PCG_ACTOR).add_node_of_type(
        unreal.PCGSpawnActorSettings)
    settings.set_editor_property(
        "template_actor_class", unreal.EditorAssetLibrary.load_blueprint_class(PCG_BP))

    for pkg in (PCG_SUB, PCG_FOREST, PCG_ROCKS, PCG_ROCKS_I, PCG_DIRTY_SUB,
                PCG_DIRTY, PCG_BP, PCG_ACTOR):
        _check(sub.save_asset(pkg, False), "save %s" % pkg)


def _run_pcg_checks():
    """PCG graphs (SPEC F9) on the host: the clean rule, subgraphs as shared
    dependencies, /PCG/ and /Engine/ content recorded but never packed."""
    _check(pcg.project_has(config.PCG_PLUGIN),
           "the host project needs the PCG plugin enabled for the F9 checks")
    _make_pcg_assets()

    # A clean graph: its subgraph rides along, the built-in subgraph and the
    # engine placeholder mesh are recorded and stay out of the library.
    r = ingest.analyze([PCG_FOREST])
    _check(r["ok"], "clean PCG graph must be accepted: %s" % r["errors"])
    _check(_statuses(r) == {PCG_FOREST: "new", PCG_SUB: "new"},
           "PCG closure must be the graph + its /Game subgraph only: %s"
           % _statuses(r))
    item = [i for i in r["items"] if i["package"] == PCG_FOREST][0]
    _check(item["pcg"] and item["required_plugins"] == [config.PCG_PLUGIN],
           "a PCG graph must need the PCG plugin: %s" % item)
    r = ingest.commit([PCG_FOREST])
    _check(r["ok"] and sorted(r["added"]) == sorted([PCG_FOREST, PCG_SUB]),
           "PCG commit: %s" % r)
    meta = store.load_meta(PCG_FOREST)
    _check(meta["required_plugins"] == [config.PCG_PLUGIN],
           "required_plugins must reach the meta: %s" % meta["required_plugins"])
    _check(meta["hard_deps"] == [PCG_SUB], "hard_deps: %s" % meta["hard_deps"])
    _check(PCG_BUILTIN in meta["engine_deps"] and ENGINE_SPHERE in meta["engine_deps"],
           "/PCG/ and /Engine/ references must be recorded: %s" % meta["engine_deps"])

    # A second graph on the same subgraph: one copy in the library.
    r = ingest.commit([PCG_ROCKS])
    _check(r["ok"] and r["added"] == [PCG_ROCKS] and r["skipped"] == [PCG_SUB],
           "shared subgraph must be skipped the second time: %s" % r)
    r = ingest.commit([PCG_ROCKS_I])
    _check(r["ok"] and r["added"] == [PCG_ROCKS_I]
           and sorted(r["skipped"]) == sorted([PCG_ROCKS, PCG_SUB]),
           "an instance root must bring its parent graph: %s" % r)

    # Content blocks - also when it sits in a subgraph, and the message names
    # the package that holds it.
    r = ingest.analyze([PCG_DIRTY])
    _check(not r["ok"] and any(e.startswith(PCG_DIRTY_SUB) and SM in e
                               and "soft" in e for e in r["errors"]),
           "a mesh two levels down must block, naming the subgraph: %s"
           % r["errors"])
    r = ingest.analyze([PCG_ACTOR])
    _check(not r["ok"] and any(e.startswith(PCG_ACTOR) and PCG_BP in e
                               for e in r["errors"]),
           "a hard reference to a Blueprint must block: %s" % r["errors"])

    # Editing the shared subgraph conflicts and lists both graphs using it.
    sub = _asset_sub()
    sub.load_asset(PCG_SUB).add_node_of_type(unreal.PCGDensityFilterSettings)
    _check(sub.save_asset(PCG_SUB, False), "save modified subgraph")
    r = ingest.analyze([PCG_FOREST])
    _check(r["ok"], "analyze after subgraph edit: %s" % r["errors"])
    conflicts = {i["package"]: i for i in r["items"] if i["status"] == "conflict"}
    _check(PCG_SUB in conflicts, "edited subgraph must conflict: %s" % _statuses(r))
    _check(conflicts[PCG_SUB]["referrers"] == sorted([PCG_FOREST, PCG_ROCKS]),
           "conflict must list both graphs: %s" % conflicts[PCG_SUB]["referrers"])
    r = ingest.commit([PCG_FOREST], {p: "update" for p in conflicts})
    _check(r["ok"] and PCG_SUB in r["updated"], "subgraph update: %s" % r)

    incremental = _index_snapshot()
    library.rebuild_index()
    _check(incremental == _index_snapshot(),
           "the index PCG ingests built incrementally must equal a full rebuild")


def run_host_test():
    """Ingest side (F1) in the host project."""
    sub = _asset_sub()
    _reset_library()
    _run_seed_check()
    _make_assets()

    # --- placement validation (SPEC 5.4): only "under the prefix" remains ---
    _check(sub.duplicate_asset(TEX, COLOCATED), "duplicate co-located texture")
    _check(sub.save_asset(COLOCATED, False), "save co-located texture")
    r = ingest.analyze([COLOCATED])
    _check(r["ok"], "a texture beside its mesh must be accepted now: %s"
           % r["errors"])
    sub.delete_asset(COLOCATED)

    _check(sub.duplicate_asset(TEX, OUTSIDE), "duplicate outside texture")
    _check(sub.save_asset(OUTSIDE, False), "save outside texture")
    r = ingest.analyze([OUTSIDE])
    _check(not r["ok"] and any("not under" in e for e in r["errors"]),
           "outside-lib asset not blocked: %s" % r["errors"])
    sub.delete_asset(OUTSIDE)

    # --- first ingest: MI_AA pulls M_A + T (all new) ---
    r = ingest.analyze([AA])
    _check(r["ok"], "analyze MI_AA: %s" % r["errors"])
    _check(_statuses(r) == {AA: "new", MA: "new", TEX: "new"},
           "MI_AA preview statuses: %s" % _statuses(r))
    _check(_selected(r) == {AA},
           "only the picked asset is selected; the closure rides along: %s"
           % _selected(r))
    # deps_as_roots makes every item a root, and must not blur "what did I
    # pick" - that is what the preview dialog dims the other rows by.
    ra = ingest.analyze([AA], deps_as_roots=True)
    _check(all(i["is_root"] for i in ra["items"]),
           "deps_as_roots must mark the whole closure as roots")
    _check(_selected(ra) == {AA},
           "deps_as_roots must not widen selected: %s" % _selected(ra))
    r = ingest.commit([AA])
    _check(r["ok"] and sorted(r["added"]) == sorted([AA, MA, TEX]),
           "MI_AA commit: %s" % r)
    _check(store.load_meta(AA)["is_root"] is True, "MI_AA is_root")
    _check(store.load_meta(MA)["is_root"] is False, "M_A must not be root yet")
    manage.set_tags(MA, ["master"])

    # --- master uniqueness: MI_AB shares M_A, only MI_AB is new ---
    r = ingest.analyze([AB])
    _check(r["ok"] and _statuses(r) == {AB: "new", MA: "same", TEX: "same"},
           "MI_AB preview statuses: %s" % r)
    r = ingest.commit([AB])
    _check(r["ok"] and r["added"] == [AB] and len(r["skipped"]) == 2,
           "MI_AB commit: %s" % r)
    ma_files = []
    for dirpath, _d, files in os.walk(config.TREE_DIR):
        ma_files += [f for f in files if f == "M_A.uasset"]
    _check(len(ma_files) == 1, "library must hold exactly one M_A, found %d"
           % len(ma_files))
    _check(library.rebuild_index() == 4, "library should have 4 entries")

    # --- conflict: modified master must prompt, never silently overwrite ---
    _modify_master(True)
    r = ingest.analyze([AB])
    _check(r["ok"], "analyze modified M_A: %s" % r["errors"])
    st = _statuses(r)
    _check(st[MA] == "conflict" and st[AB] == "same",
           "modified M_A must conflict: %s" % st)
    conflict = [i for i in r["items"] if i["package"] == MA][0]
    _check(conflict["referrers"] == [AA, AB],
           "conflict referrer list: %s" % conflict["referrers"])
    r = ingest.commit([AB])  # no decision -> refuse
    _check(not r["ok"], "undecided conflict must fail the commit")

    old_meta = store.load_meta(MA)
    r = ingest.commit([AB], {MA: "keep"})
    _check(r["ok"] and not r["added"] and not r["updated"],
           "keep decision must not upload: %s" % r)
    _check(store.load_meta(MA)["sha256"] == old_meta["sha256"],
           "keep decision must leave library M_A untouched")

    r = ingest.commit([AB], {MA: "update"})
    _check(r["ok"] and r["updated"] == [MA], "update decision: %s" % r)
    new_meta = store.load_meta(MA)
    _check(new_meta["sha256"] != old_meta["sha256"], "M_A sha must change")
    _check(new_meta["ingested_at"] == old_meta["ingested_at"],
           "update must keep first-ingest time")
    _check(new_meta["tags"] == ["master"], "update must keep tags")
    batches = os.listdir(config.ARCHIVE_DIR)
    _check(any(os.path.isfile(os.path.join(config.ARCHIVE_DIR, b, "Content",
                                           "Shared", "Master", "M_A.uasset"))
               for b in batches), "old M_A must land in _archive")

    # --- mesh ingest + root promotion of an existing dependency ---
    furniture = catalog.create("Furniture")["id"]
    r = ingest.commit([SM], collection=furniture)
    _check(r["ok"] and r["added"] == [SM], "SM commit: %s" % r)
    _check(store.load_meta(SM)["collections"] == [furniture],
           "ingest must file the selected root into the chosen collection")
    _check(store.load_meta(AA)["collections"] == [],
           "auto-collected deps must not be filed")

    r = ingest.commit([MA])
    _check(r["ok"] and set(r["skipped"]) == {MA, TEX},
           "M_A re-ingest should skip its closure: %s" % r)
    _check(store.load_meta(MA)["is_root"] is True,
           "active ingest must promote M_A to root")
    incremental = _index_snapshot()
    _check(library.rebuild_index() == 5, "library should have 5 entries")
    _check(incremental == _index_snapshot(),
           "the index ingest built incrementally must equal a full rebuild")

    _run_collection_checks(furniture)
    _run_pack_collection_checks()
    _run_update_keeps_filing_check()

    # Adopting a root that already holds assets must leave its filing alone
    # (2026-08-27) - "Use This Folder" on an existing library comes through
    # this same call, and seeding it would be filing someone else's library.
    before = catalog.list_tree()["collections"]
    r = _init_library()
    _check(r["ok"] and r["entries"] > 0 and r["collections"] == 0,
           "adopting a filled library must not seed: %s" % r)
    _check(catalog.list_tree()["collections"] == before,
           "adopting must leave the collection tree exactly as it was")

    _run_reserved_name_check()
    _run_pcg_checks()
    _run_redirector_check()

    unreal.log_warning("Kura selftest HOST PASS (master unique, conflict flow "
                       "+ collections + pack filing + PCG clean rule verified)")


RD_OLD = config.LIB_PREFIX + "/KuraTest/Redirect/T_RedirOld"
RD_NEW = config.LIB_PREFIX + "/KuraTest/Redirect/T_RedirNew"
RD_MI = config.LIB_PREFIX + "/KuraTest/Redirect/MI_Redir"


def _run_redirector_check():
    """Redirectors under the prefix (SPEC F1 step 2). Until 2026-11-19 ingest
    called an AssetTools.fixup_referencers that Python never had, so any
    redirector blocked every upload with an AttributeError - and no check
    here ever made one.

    The fixup itself ends in the engine's modal report window, which a
    commandlet cannot show, so headless it is skipped by design; what this
    checks is that the upload then stops with guidance naming the
    redirector instead of crashing or carrying it into the library. The
    editor path needs a hand check (PROJECT_STATUS UI-01).

    UE leaves a redirector on rename only when something still needs the old
    path. A Content Browser collection holding the asset is the case that
    works headless (a level referencer crashes the commandlet; a read-only
    referencer makes the rename refuse)."""
    sub = _asset_sub()
    at = unreal.AssetToolsHelpers.get_asset_tools()
    mel = unreal.MaterialEditingLibrary
    _check(sub.duplicate_asset(TEX, RD_OLD), "duplicate redirect texture")
    folder, name = RD_MI.rsplit("/", 1)
    mi = at.create_asset(name, folder, unreal.MaterialInstanceConstant,
                         unreal.MaterialInstanceConstantFactoryNew())
    _check(mi, "create MI_Redir")
    mel.set_material_instance_parent(mi, sub.load_asset(MA))
    mel.set_material_instance_texture_parameter_value(
        mi, "BaseTex", sub.load_asset(RD_OLD))
    for pkg in (RD_OLD, RD_MI):
        _check(sub.save_asset(pkg, False), "save %s" % pkg)
    # Just-saved assets have an empty dependency graph until rescanned.
    deps.refresh_files([ingest.package_to_file(p) for p in (RD_MI, RD_OLD)])
    cm = unreal.get_editor_subsystem(unreal.CollectionManagerSubsystem)
    res = cm.create_or_empty_collection(
        cm.get_collection_containers()[0], "KuraSelftestRedirect",
        unreal.CollectionShareType.LOCAL)
    coll = res[-1] if isinstance(res, tuple) else res
    _check(cm.add_asset_to_collection(coll, unreal.SoftObjectPath(
        RD_OLD + "." + RD_OLD.rsplit("/", 1)[1])),
        "add the texture to a collection")
    try:
        _check(sub.rename_asset(RD_OLD, RD_NEW), "rename the texture")
    finally:
        cm.destroy_collection(coll)
    unreal.EditorLoadingAndSavingUtils.save_dirty_packages(False, True)
    deps.refresh_files([ingest.package_to_file(RD_OLD)])
    _check(ingest._redirectors_under_prefix() == [RD_OLD],
           "renaming must leave a redirector to test against: %s"
           % ingest._redirectors_under_prefix())

    r = ingest.analyze([RD_MI])
    _check(not r["ok"] and any("Fix Up Redirectors" in e and RD_OLD in e
                               for e in r["errors"]),
           "a redirector left in place must stop the upload with guidance: %s"
           % r["errors"])
    _check(not any(f.endswith("T_RedirOld.uasset")
                   for _d, _s, fs in os.walk(config.TREE_DIR) for f in fs),
           "the redirector must not reach the library")
    for pkg in (RD_MI, RD_NEW, RD_OLD):
        if sub.does_asset_exist(pkg):
            sub.delete_asset(pkg)


def _run_reserved_name_check():
    """Meta/collections.json is reserved at the TOP of the mirror only
    (2026-08-23). Skipping it by name anywhere dropped a library asset called
    "collections" out of the index - and out of the panel - without a word.

    Meta-only fixture: the point is the mirror walk, and a tree file would
    only add drift for rescan to report."""
    pkg = config.LIB_PREFIX + "/KuraTest/Chair/collections"
    meta_path = store.meta_file(pkg)
    os.makedirs(os.path.dirname(meta_path), exist_ok=True)
    store.save_meta(store.new_meta(pkg, "Texture2D", "0" * 64, 1,
                                   ingest.engine_version(), "selftest",
                                   False, [], []))
    try:
        # Walks the whole mirror. The reserved file at the top carries no
        # "package" key, so reaching the assert at all proves it was skipped.
        packages = {m["package"] for m in store.iter_metas()}
        _check(pkg in packages,
               "an asset named 'collections' must still be indexed")
    finally:
        os.remove(meta_path)
        library.rebuild_index()


def _run_collection_checks(furniture):
    """Virtual classification layer (SPEC F8), on the library the host test
    just built: 5 entries, 4 of them roots (SM, MI_AA, MI_AB, M_A), SM filed."""
    counts = library.collection_counts()
    _check(counts.get(furniture) == 1, "Furniture should hold 1 entry: %s" % counts)
    _check(counts.get("") == 3,
           "unfiled roots should be MI_AA + MI_AB + M_A: %s" % counts)

    # nesting, and a name clash among siblings is refused
    chairs = catalog.create("Chairs", furniture)["id"]
    try:
        catalog.create("Chairs", furniture)
        _check(False, "duplicate sibling name must be refused")
    except RuntimeError:
        pass

    # a collection cannot be moved inside its own subtree
    try:
        catalog.move(furniture, chairs)
        _check(False, "moving a collection into its own child must be refused")
    except RuntimeError:
        pass

    # multi-membership, and members() reaches into nested collections
    catalog.assign([SM], chairs)
    _check(sorted(store.load_meta(SM)["collections"]) == sorted([furniture, chairs]),
           "an entry may sit in several collections")
    _check(catalog.members(furniture) == [SM],
           "members() must include nested collections")

    # membership survives a full index rebuild (Meta is the source of truth)
    library.rebuild_index()
    _check(library.collection_counts().get(chairs) == 1,
           "collections index must rebuild from Meta")

    # archive + restore keeps the filing (membership rides in the asset meta)
    manage.delete_entries([SM])
    _check(library.collection_counts().get(chairs, 0) == 0, "delete must unfile")
    batch = manage.list_archive()["batches"][0]["batch"]
    _check(manage.restore_archive(batch)["restored"] == [SM], "restore SM")
    _check(sorted(store.load_meta(SM)["collections"]) == sorted([furniture, chairs]),
           "restore must bring the filing back")

    # deleting a collection removes the children too, and only unfiles assets
    r = catalog.delete(furniture)
    _check(sorted(r["removed"]) == sorted([furniture, chairs]),
           "delete must take nested collections with it: %s" % r)
    _check(store.load_meta(SM)["collections"] == [],
           "deleting a collection must unfile its entries")
    _check(store.exists(SM), "deleting a collection must never delete assets")
    _check(catalog.list_tree()["collections"] == [], "tree should be empty again")


def _run_pack_collection_checks():
    """A .kurapack carries its filing by path (SPEC F7 import step 5,
    2026-08-22). Runs on the library the host test built, with the F8 tree
    left empty by _run_collection_checks."""
    surfaces = catalog.create("Surfaces")["id"]
    wood = catalog.create("Wood", surfaces)["id"]
    flooring = catalog.create("Flooring", wood)["id"]
    catalog.create("Metal", surfaces)  # no members: must not travel
    catalog.assign([SM], flooring)
    catalog.assign([SM], wood)                       # multi-membership
    catalog.assign([AB], flooring)

    tmp = tempfile.mkdtemp(prefix="kura_selftest_pack_")
    try:
        out = os.path.join(tmp, "filing.kurapack")
        r = pack.export_package([SM, AB], out, name="Filing")
        _check(r["ok"], "pack export: %s" % r.get("errors"))

        # Wipe the tree; the entries fall back to Unsorted.
        catalog.delete(surfaces)
        _check(catalog.list_tree()["collections"] == [], "tree must be empty")
        _check(store.load_meta(SM)["collections"] == [], "SM must be unfiled")

        pv = pack.preview(out, current_engine=ingest.engine_version())
        _check(pv["ok"], "pack preview: %s" % pv.get("errors"))
        summary = pv["manifest"]["collections"]
        _check(summary == {"total": 3, "new": 3, "entries": 2},
               "preview must count the shipped branches: %s" % summary)

        r = pack.commit(out, {}, current_engine=ingest.engine_version(),
                        collection=pack.PACK_COLLECTIONS)
        _check(r["ok"], "pack commit: %s" % r.get("errors"))
        _check(r["collections"]["error"] == "",
               "filing must not error: %s" % r["collections"]["error"])
        items = catalog.load()["collections"]
        paths = sorted(catalog.path_of(c["id"], items) for c in items)
        _check(paths == ["Surfaces", "Surfaces/Wood", "Surfaces/Wood/Flooring"],
               "only branches with members travel: %s" % paths)
        byid = {catalog.path_of(c["id"], items): c["id"] for c in items}
        _check(sorted(store.load_meta(SM)["collections"])
               == sorted([byid["Surfaces/Wood"], byid["Surfaces/Wood/Flooring"]]),
               "multi-membership must survive the round trip")
        _check(store.load_meta(MA)["collections"] == [],
               "a dependency must not be filed by the package")
        _check(sorted(library.packages_in_collection(byid["Surfaces/Wood/Flooring"]))
               == sorted([AB, SM]), "db index must agree with the meta")
        incremental = _index_snapshot()
        library.rebuild_index()
        _check(incremental == _index_snapshot(),
               "the index a package import built incrementally must equal a "
               "full rebuild")

        # Same package again, filed into one of the user's own collections:
        # roots only, flat, and none of the package's own branches planted.
        mine = catalog.create("MyStuff")["id"]
        r = pack.commit(out, {}, current_engine=ingest.engine_version(),
                        collection=mine)
        _check(r["ok"], "flat commit: %s" % r.get("errors"))
        # Every root in the package, not just the two that were filed: the
        # closure carries M_A and MI_AA, which this library promoted earlier.
        want = sorted(p for p in (i["package"] for i in pv["items"])
                      if store.load_meta(p)["is_root"])
        _check(sorted(library.packages_in_collection(mine)) == want,
               "a chosen collection takes every root in the package, flat")
        _check(store.load_meta(TEX)["collections"] == [],
               "a non-root must stay unfiled even for an explicit choice")
        _check(len(catalog.load()["collections"]) == 4,
               "a chosen collection must not plant the package's branches")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    for top in [c["id"] for c in catalog.load()["collections"] if not c["parent"]]:
        catalog.delete(top)
    _check(catalog.list_tree()["collections"] == [], "tree should be empty again")


def _run_update_keeps_filing_check():
    """An update replaces an entry's version, never the user's filing
    (2026-10-07). Both update paths rebuild the meta - ingest from scratch,
    a package import from the package's copy - and both used to come out of
    it Unsorted, while an identical re-send kept its collections.

    Runs on M_A, a filed root here, and leaves the library version where the
    target test expects it (two-sided), since that test edits M_A to
    one-sided and needs that to conflict."""
    keep = catalog.create("KeepMe")["id"]
    catalog.assign([MA], keep)
    tmp = tempfile.mkdtemp(prefix="kura_selftest_filing_")
    try:
        # A package holding the CURRENT library M_A, to update back to later.
        out = os.path.join(tmp, "master.kurapack")
        _check(pack.export_package([MA], out, name="Master")["ok"], "export M_A")

        _modify_master(False)
        r = ingest.commit([MA], {MA: "update"})
        _check(r["ok"] and r["updated"] == [MA], "M_A ingest update: %s" % r)
        _check(store.load_meta(MA)["collections"] == [keep],
               "an ingest update must keep the entry's collections: %s"
               % store.load_meta(MA)["collections"])
        _check(library.collection_counts().get(keep) == 1,
               "the index must still count the updated entry as filed")

        # "None (Unsorted)" on purpose: the package's own filing would put
        # M_A back into KeepMe by path and hide a dropped membership.
        r = pack.commit(out, {MA: "update"}, current_engine=ingest.engine_version(),
                        collection="")
        _check(r["ok"] and r["updated"] == [MA], "M_A package update: %s" % r)
        _check(store.load_meta(MA)["collections"] == [keep],
               "a package update must keep the entry's local collections: %s"
               % store.load_meta(MA)["collections"])
        _check(library.collection_counts().get(keep) == 1,
               "the index must still count the package-updated entry as filed")

        incremental = _index_snapshot()
        library.rebuild_index()
        _check(incremental == _index_snapshot(),
               "the index both updates built incrementally must equal a full rebuild")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    catalog.delete(keep)
    _check(catalog.list_tree()["collections"] == [], "tree should be empty again")


def run_target_test():
    """Import side (F3) in the target project."""
    sub = _asset_sub()
    if sub.does_directory_exist(config.LIB_PREFIX):
        unreal.log_warning("Kura selftest: deleting leftover %s" % config.LIB_PREFIX)
        sub.delete_directory(config.LIB_PREFIX)

    # --- import MI_AA: closure copies exactly MI_AA + M_A + T ---
    r = importer.commit([AA])
    _check(r["ok"] and sorted(r["copied"]) == sorted([AA, MA, TEX]),
           "import MI_AA: %s" % r)
    mi = sub.load_asset(AA)
    _check(mi is not None, "load MI_AA after import")
    _check(str(mi.get_editor_property("parent").get_path_name())
           .startswith(MA + "."), "MI_AA parent must be library M_A")

    # --- master uniqueness in the project: MI_AB copies only itself ---
    r = importer.commit([AB])
    _check(r["ok"] and r["copied"] == [AB] and len(r["skipped"]) == 2,
           "import MI_AB: %s" % r)

    # --- repeat import: everything identical -> all skipped ---
    r = importer.commit([AB])
    _check(r["ok"] and not r["copied"] and len(r["skipped"]) == 3,
           "re-import MI_AB: %s" % r)

    # --- mesh round-trip fidelity ---
    r = importer.commit([SM])
    _check(r["ok"] and r["copied"] == [SM], "import SM: %s" % r)
    sm = sub.load_asset(SM)
    _check(isinstance(sm, unreal.StaticMesh), "load SM after import")
    slot = sm.get_material(0)
    _check(slot is not None, "SM material slot 0")
    tex = unreal.MaterialEditingLibrary.get_material_instance_texture_parameter_value(
        slot, "BaseTex")
    _check(tex is not None, "texture parameter did not resolve")

    # --- import conflict: local edit -> prompt, keep or overwrite ---
    _modify_master(False)
    p = importer.preview([AA])
    _check(p["ok"], "preview after local edit: %s" % p["errors"])
    _check(_selected(p) == {AA},
           "import preview must mark only the picked entry: %s" % _selected(p))
    st = _statuses(p)
    _check(st[MA] == "conflict", "locally edited M_A must conflict: %s" % st)
    r = importer.commit([AA])
    _check(not r["ok"], "undecided import conflict must fail")
    r = importer.commit([AA], {MA: "keep"})
    _check(r["ok"] and r["kept"] == [MA], "keep decision: %s" % r)
    _check(store.sha256_of(ingest.package_to_file(MA))
           != store.load_meta(MA)["sha256"], "keep must not touch project M_A")
    r = importer.commit([AA], {MA: "overwrite"})
    _check(r["ok"] and r["copied"] == [MA], "overwrite decision: %s" % r)
    _check(store.sha256_of(ingest.package_to_file(MA))
           == store.load_meta(MA)["sha256"], "overwrite must restore library M_A")

    pcg_result = _run_pcg_import_checks()
    unreal.log_warning("Kura selftest TARGET PASS (round-trip intact, master "
                       "unique, conflict flow verified; %s)" % pcg_result)


def _run_pcg_import_checks():
    """PCG import (SPEC F9). Both outcomes are tested, one per run: without
    the PCG plugin the import must refuse and copy nothing, with it the graph
    and its shared subgraph arrive linked. Returns the PASS-line label."""
    sub = _asset_sub()
    if not pcg.project_has(config.PCG_PLUGIN):
        p = importer.preview([PCG_FOREST])
        _check(not p["ok"] and any("PCG plugin is not enabled" in e
                                   for e in p["errors"]),
               "import without PCG must be refused: %s" % p["errors"])
        r = importer.commit([PCG_FOREST], force_version=True)
        _check(not r["ok"] and not r["copied"],
               "force must not get past the plugin check: %s" % r)
        _check(not os.path.isfile(ingest.package_to_file(PCG_FOREST)),
               "a refused import must copy nothing")
        return "PCG off: import refused"

    r = importer.commit([PCG_FOREST])
    _check(r["ok"] and sorted(r["copied"]) == sorted([PCG_FOREST, PCG_SUB]),
           "import PCG graph: %s" % r)
    _check(isinstance(sub.load_asset(PCG_FOREST), unreal.PCGGraph),
           "load PCG graph after import")
    _check(PCG_SUB in deps.hard_dependencies(PCG_FOREST),
           "imported graph must still point at its subgraph")
    r = importer.commit([PCG_ROCKS_I])
    _check(r["ok"] and sorted(r["copied"]) == sorted([PCG_ROCKS_I, PCG_ROCKS])
           and r["skipped"] == [PCG_SUB],
           "instance import must reuse the subgraph already here: %s" % r)
    return "PCG on: graphs + shared subgraph imported"
