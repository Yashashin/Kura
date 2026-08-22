"""Headless self-test drivers: asset-tree round-trip + master uniqueness
(the SPEC 9 acceptance chain), run across two throwaway projects.

In the host project:   import kura.selftest as s; s.run_host_test()
In the target project: import kura.selftest as s; s.run_target_test()

Test chain (created procedurally, laid out by the SPEC 5.4 convention:
a root plus its exclusive deps per folder, shared assets parked centrally):
SM_KuraTest -> MI_AA -> M_A -> T_KuraTest ; MI_AB -> M_A (shared master)
"""
import os

import unreal

from . import catalog, config, importer, ingest, library, manage, store

TEX = config.LIB_PREFIX + "/Shared/Textures/T_KuraTest"
MA = config.LIB_PREFIX + "/Shared/Master/M_A"
AA = config.LIB_PREFIX + "/KuraTest/Chair/MI_AA"
SM = config.LIB_PREFIX + "/KuraTest/Chair/SM_KuraTest"
AB = config.LIB_PREFIX + "/KuraTest/Lamp/MI_AB"
# A texture sitting next to the mesh that uses it: blocked before 2026-08-10
# (wrong type root), now the recommended layout (Q24).
COLOCATED = config.LIB_PREFIX + "/KuraTest/Chair/T_ChairOnly"
OUTSIDE = "/Game/TestAssets/T_Outside"


def _check(cond, msg):
    if not cond:
        raise RuntimeError("Kura selftest FAILED: %s" % msg)


def _asset_sub():
    return unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)


def _statuses(result):
    return {i["package"]: i["status"] for i in result["items"]}


def _reset_library():
    """Archive every existing entry so the run starts from an empty tree."""
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


def run_host_test():
    """Ingest side (F1) in the host project."""
    sub = _asset_sub()
    _reset_library()
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
    _check(library.rebuild_index() == 5, "library should have 5 entries")

    _run_collection_checks(furniture)

    unreal.log_warning("Kura selftest HOST PASS (5 entries, master unique, "
                       "conflict flow + collections verified)")


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

    unreal.log_warning("Kura selftest TARGET PASS (round-trip intact, master "
                       "unique, conflict flow verified)")
