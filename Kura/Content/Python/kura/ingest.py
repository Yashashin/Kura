"""Ingest (SPEC F1): validate placement, collect deps, preview, commit.

Kura validates but never moves assets (SPEC 5.4): the user arranges them
under /Game/_AssetLib in the Content Browser before sending. Preview and
commit share one analysis pass; commit re-runs it so decisions always apply
to the current on-disk state.
"""
import os
import shutil

import unreal

from . import catalog, config, deps, library, pcg, progress, store


def content_dir():
    return os.path.normpath(unreal.SystemLibrary.get_project_content_directory())


def package_to_file(pkg):
    """Project-side .uasset path of a /Game package."""
    pkg = str(pkg)
    if not pkg.startswith("/Game/"):
        raise ValueError("Not a /Game/ package: %s" % pkg)
    rel = pkg[len("/Game/"):]
    return os.path.join(content_dir(), *rel.split("/")) + ".uasset"


def package_file_or_none(pkg):
    """package_to_file() for callers that want "nothing to scan here" instead
    of an exception: None for a non-/Game package or one with no file yet."""
    try:
        path = package_to_file(pkg)
    except ValueError:
        return None
    return path if os.path.isfile(path) else None


def engine_version():
    # "5.7.0-12345678+++UE5+Release-5.7" -> "5.7.0"
    return str(unreal.SystemLibrary.get_engine_version()).split("-")[0]


def is_headless():
    """Commandlet / unattended runs: thumbnail rendering and other Slate-adjacent
    calls are off-limits. Canonical check lives in kura.progress - slow-task
    dialogs are off-limits for the same reason."""
    return progress.is_headless()


_MATERIAL_CLASSES = ("Material", "MaterialInstanceConstant")


def _render_thumbnail(pkg, cls):
    """Render one library PNG from the current project's asset (SPEC 7.2).

    Material-family assets render with the configured preview mesh applied
    transiently - the project asset itself keeps its default sphere
    thumbnail (decision 2026-07-12)."""
    lib = unreal.KuraThumbnailLibrary
    png = store.thumb_file(pkg)
    if cls in _MATERIAL_CLASSES and config.MATERIAL_PREVIEW_MESH:
        return lib.export_material_thumbnail_png(
            pkg, png, config.MATERIAL_PREVIEW_MESH,
            config.THUMB_ORBIT_PITCH, config.THUMB_ORBIT_YAW,
            config.THUMB_ORBIT_ZOOM, config.THUMB_RESOLUTION)
    # No preview mesh configured -> UE's own default thumbnail (the sphere for
    # materials), same as the Content Browser (SPEC 7.2).
    return lib.export_thumbnail_png(pkg, png, config.THUMB_RESOLUTION)


def _export_thumbnails(items):
    """Write thumbnail PNGs into the Meta mirror (SPEC 7.2) for EVERY entry
    in the batch - re-sending an asset re-renders after a camera/config
    tweak, so no stale-PNG skipping. PCG assets are left out: no thumbnail
    renderer exists for them, so a render only logs a warning (SPEC F9)."""
    if is_headless():
        return
    items = [i for i in items if not i.get("pcg")]
    # The slowest phase of an upload by far - one render per asset.
    with progress.Reporter("Kura: rendering thumbnails") as report:
        for idx, item in enumerate(items, 1):
            report(idx, len(items), item["package"])
            if not _render_thumbnail(item["package"], item["class"]):
                unreal.log_warning("Kura: no thumbnail written for %s"
                                   % item["package"])


def rerender_thumbnails(packages):
    """Re-render library PNGs without a re-ingest (SPEC 7.2, 2026-07-12).

    The render can only see assets in the CURRENT project, and the library
    thumbnail must show the library version - so each package must exist in
    the project at its stable path with the same hash as its library entry.
    """
    if is_headless():
        return {"ok": False, "rendered": [],
                "errors": ["Thumbnail rendering is unavailable in headless runs"]}
    errors, rendered = [], []
    todo = [str(p) for p in packages]
    with progress.Reporter("Kura: rendering thumbnails") as report:
        for idx, pkg in enumerate(todo, 1):
            report(idx, len(todo), pkg)
            meta = store.load_meta(pkg)
            if meta is None:
                errors.append("%s: not a library entry" % pkg)
                continue
            if meta.get("class") in config.PCG_ROOT_CLASSES:
                errors.append("%s: PCG graphs have no thumbnail - the panel "
                              "shows a PCG tile for them instead" % pkg)
                continue
            src = package_to_file(pkg)
            if not os.path.isfile(src):
                errors.append("%s: not present in the current project - "
                              "import it first" % pkg)
                continue
            if store.sha256_of(src) != meta["sha256"]:
                # The usual cause is a deliberate edit - most often a new
                # thumbnail angle, which lives inside the .uasset and so
                # changes the hash. Leading with "overwrite" would tell the
                # user to throw that work away; re-sending is what they want.
                errors.append("%s: the project file differs from the library "
                              "version, and the library thumbnail must show "
                              "the library version. If you changed this asset "
                              "on purpose (a new thumbnail angle counts - it "
                              "is stored in the .uasset), send it to Kura "
                              "again to update the library. Only import "
                              "(overwrite) if you want to discard the project "
                              "copy's changes." % pkg)
                continue
            if _render_thumbnail(pkg, meta["class"]):
                rendered.append(pkg)
            else:
                errors.append("%s: render failed - see Output Log" % pkg)
    return {"ok": not errors, "errors": errors, "rendered": rendered}


def _cleanup_staging(paths, stop_dir=None):
    """Best-effort removal of leftover staging copies (+ now-empty dirs)."""
    for tmp in paths:
        try:
            os.remove(tmp)
        except OSError:
            pass
        if stop_dir is None:
            stop_dir = config.TREE_DIR
        store._prune_empty_dirs(os.path.dirname(tmp), stop_dir)


def _redirectors_under_prefix():
    ar = unreal.AssetRegistryHelpers.get_asset_registry()
    f = unreal.ARFilter(
        package_paths=[config.LIB_PREFIX], recursive_paths=True,
        class_paths=[unreal.TopLevelAssetPath("/Script/CoreUObject", "ObjectRedirector")])
    return sorted({str(ad.package_name) for ad in ar.get_assets(f) or []})


def fixup_redirectors():
    """Fix Up Redirectors on /Game/_AssetLib (SPEC F1 step 2). Returns the
    redirectors still there afterwards - normally none.

    Goes through the plugin's C++ library: IAssetTools::FixupReferencers is
    not exposed to Python, and the AssetTools.fixup_referencers this used to
    call does not exist on 5.7 / 5.8, so any redirector under the prefix
    blocked the whole upload with an AttributeError (2026-11-19)."""
    redirectors = _redirectors_under_prefix()
    if not redirectors:
        return []
    # Package names, not loaded objects: a reference held here would keep a
    # redirector alive and stop the engine deleting it. Ends with the
    # engine's Redirector Update Report window, as the Content Browser
    # command does; returns False (and does nothing) headless.
    unreal.KuraThumbnailLibrary.fixup_redirectors(redirectors)
    left = _redirectors_under_prefix()
    unreal.log("Kura: fixed up redirectors under %s (%d left)"
               % (config.LIB_PREFIX, len(left)))
    return left


def _save_dirty(packages):
    """Save the batch members that hold unsaved edits, so the comparison runs
    against what is on disk (SPEC 8). Returns the packages actually saved.

    Only a package already loaded in memory can be dirty, so ask the editor for
    its dirty list instead of calling save_asset(pkg, only_if_dirty) per
    package: that helper load()s the asset BEFORE it looks at the dirty flag
    (UE 5.7 UEditorAssetSubsystem::SaveAsset), which pulled every texture in the
    closure into memory for nothing - 1.15 s vs 0.00 s (measured 2026-08-09).
    Both paths end in the same UEditorLoadingAndSavingUtils::SavePackages.
    """
    wanted = set(packages)
    dirty, saved = [], []
    utils = unreal.EditorLoadingAndSavingUtils
    for package in utils.get_dirty_content_packages():
        # A UPackage's own name IS its package path; get_path_name() agrees for
        # a top-level object, and checking both keeps this honest either way.
        for name in (str(package.get_name()), str(package.get_path_name())):
            if name in wanted:
                dirty.append(package)
                saved.append(name)
                break
    if dirty:
        utils.save_packages(dirty, True)
    return saved


def _direct_deps(pkg, is_pcg=False):
    """Direct hard deps of one package, split into (game, engine).

    For a PCG package "engine" also holds its /PCG/ references and the soft
    ones into /Engine/ and /PCG/ - everything a clean graph may still point
    at, recorded so the detail panel can show it (SPEC F9)."""
    engine_like = ("/Engine/", config.PCG_MOUNT) if is_pcg else ("/Engine/",)
    game, engine = [], set()
    for d in deps.hard_dependencies(pkg):
        if d.startswith(engine_like):
            engine.add(d)
        elif d.startswith("/Game/"):
            game.append(d)
        # /Script/ ignored; other mounts already blocked by collect()
    if is_pcg:
        engine.update(d for d in deps.soft_dependencies(pkg)
                      if d.startswith(engine_like))
    return sorted(game), sorted(engine)


def analyze(root_packages, deps_as_roots=False):
    """Shared preview/commit pass (SPEC F1 steps 1-5).

    deps_as_roots: when True, every auto-collected dependency is marked as a
    root too, so the whole closure shows in the default browse view without a
    second manual upload (SPEC F1 option, 2026-07-14).

    Returns {"ok", "errors", "roots", "items"}; items only when ok.
    Item: {package, class, is_root, selected, status: new|same|conflict, size,
           sha256 (only when this pass computed one - commit() reuses it),
           referrers (library entries affected, conflict only),
           pcg, required_plugins (SPEC F9; empty for anything not PCG)}.

    selected: the user picked this one in the Content Browser, as opposed to
    Kura collecting it around them. Separate from is_root because
    deps_as_roots turns is_root on for the whole closure - the preview dialog
    needs "what did I actually ask for" and would lose it (2026-08-23).
    """
    store.require_library_root()
    roots = [str(p) for p in root_packages]
    picked = set(roots)  # membership test per closure entry, not per root
    errors = []

    # 1. root type whitelist
    for pkg in roots:
        cls = deps.primary_class(pkg)
        if cls not in config.ROOT_CLASSES:
            errors.append(
                "%s: class %s cannot be sent to Kura as a root asset. This "
                "version collects StaticMesh / Material / Material Instance / "
                "Texture / PCG Graph; other types (SkeletalMesh, Blueprint, "
                "Niagara, ...) are on the roadmap - see the project page"
                % (pkg, cls))
    if errors:
        return {"ok": False, "errors": errors, "roots": roots, "items": []}

    # 2-3. redirector fixup, collect the closure over a registry view refreshed
    #      for THIS batch, save any member with unsaved edits, then re-collect
    #      so the graph reflects what is on disk (SPEC 8: save before compare).
    #      Everything here used to run outside a Reporter, so the editor simply
    #      froze until the compare loop started (2026-08-09).
    with progress.Reporter("Kura: reading the selection") as report:
        report(1, 4, "Fixing up redirectors")
        left = fixup_redirectors()
        report(2, 4, "Collecting dependencies")
        d = deps.collect_fresh(roots, package_file_or_none)
        report(3, 4, "Saving modified assets")
        saved = _save_dirty(d["game"])
        if saved:
            report(4, 4, "Re-reading saved assets")
            deps.refresh_files([package_file_or_none(p) for p in saved])
            d = deps.collect(roots)

    # A redirector that survived the fixup (the report window was cancelled,
    # a referencer failed to save, or no editor UI to run it in) would
    # otherwise travel into the library as an entry of its own. Stop and say
    # what to do instead.
    if left:
        errors.append(
            "Fix Up Redirectors could not clear %d redirector(s) under %s: %s"
            " - right-click the folder in the Content Browser, choose Fix Up"
            " Redirectors, check the Output Log for why any remain, then"
            " send again" % (len(left), config.LIB_PREFIX, ", ".join(left)))

    if d["other"]:
        errors.append(
            "Dependencies outside /Game and /Engine are not supported: %s"
            % d["other"])

    class_paths = {pkg: deps.class_path(pkg) for pkg in d["game"]}
    classes = {pkg: cp[1] for pkg, cp in class_paths.items()}

    # PCG graphs go in without content (SPEC F9): every PCG package in the
    # closure - subgraphs included - is held to the clean rule, and the code
    # modules it uses become the plugins an import will require.
    pcg_packages = sorted(p for p, cp in class_paths.items()
                          if cp[0] == config.PCG_SCRIPT_PACKAGE)
    plugins = {}
    if pcg_packages:
        errors += pcg.clean_errors(pcg_packages)
        for pkg in pcg_packages:
            plugins[pkg], plugin_errors = pcg.required_plugins(pkg)
            errors += plugin_errors

    # 4. placement validation (SPEC 5.4): block, never move. Since 2026-08-10
    #    that is only "under the stable prefix" - folder layout is the user's
    #    (Q24), and classification lives in collections (F8).
    for pkg in d["game"]:
        err = store.placement_error(pkg)
        if err:
            if deps.is_redirector(pkg):
                err += " [redirector]"
            errors.append(err)
    if errors:
        return {"ok": False, "errors": errors, "roots": roots, "items": []}

    # 5. compare with the library (hashes every incoming file the library
    #    already holds - the slow part of a big upload, so it reports per
    #    asset). A size mismatch settles the comparison without reading the
    #    file at all; the hash it did compute rides along in the item so
    #    commit() does not read the same file a second time.
    items = []
    game = sorted(d["game"])
    with progress.Reporter("Kura: checking assets against the library") as report:
        for idx, pkg in enumerate(game, 1):
            report(idx, len(game), pkg)
            src = package_to_file(pkg)
            if not os.path.isfile(src):
                errors.append("Package file missing on disk: %s (%s)" % (src, pkg))
                continue
            size = os.path.getsize(src)
            meta = store.load_meta(pkg)
            if meta is not None:
                # A library written by a newer Kura is read-only to this one
                # (SPEC 5.6): overwriting the entry would drop whatever fields
                # this build does not know about. Blocks the batch here rather
                # than failing halfway through commit().
                schema_error = store.meta_schema_error(meta)
                if schema_error:
                    errors.append(schema_error)
                    continue
            sha = None
            if meta is None:
                status, referrers = "new", []
            else:
                if "size" in meta and meta["size"] != size:
                    same = False  # different size -> different content
                else:
                    sha = store.sha256_of(src)
                    same = meta["sha256"] == sha
                if same:
                    status, referrers = "same", []
                else:
                    status, referrers = "conflict", library.referrers_of(pkg)
            items.append({
                "package": pkg,
                "class": classes[pkg],
                "is_root": deps_as_roots or pkg in picked,
                "selected": pkg in picked,
                "status": status,
                "size": size,
                "sha256": sha,  # None = not computed here; commit() will
                "referrers": referrers,
                "pcg": pkg in plugins,
                "required_plugins": plugins.get(pkg, []),
            })
    if errors:
        return {"ok": False, "errors": errors, "roots": roots, "items": []}
    return {"ok": True, "errors": [], "roots": roots, "items": items}


def commit(root_packages, decisions=None, deps_as_roots=False, add_tags=None,
           collection=""):
    """Apply an ingest (SPEC F1 steps 6-7).

    decisions: {package: "update" | "keep"} for every conflict item
    ("update" archives the library version first; "keep" skips the upload).
    deps_as_roots: mark auto-collected dependencies as roots too (SPEC F1).
    add_tags: batch tags unioned into NEW and UPDATED entries only - "same"
    entries keep their tags, so a shared master in the closure never picks up
    a source tag by accident (SPEC F1, 2026-07-17).
    collection: optional collection id to file this batch under (SPEC F8).
    Applies to the packages the user actually selected, whatever their status
    - re-sending an entry that is already identical is still a filing
    instruction - and never to auto-collected dependencies, which would drag
    a shared master into an unrelated collection.
    Returns {"ok", "added", "updated", "skipped", "errors"}.
    """
    decisions = dict(decisions or {})
    batch_tags = {str(t).strip() for t in (add_tags or []) if str(t).strip()}
    a = analyze(root_packages, deps_as_roots)
    if not a["ok"]:
        return {"ok": False, "errors": a["errors"],
                "added": [], "updated": [], "skipped": []}

    conflicts = [i["package"] for i in a["items"] if i["status"] == "conflict"]
    undecided = [p for p in conflicts if decisions.get(p) not in ("update", "keep")]
    if undecided:
        return {"ok": False, "added": [], "updated": [], "skipped": [],
                "errors": ["No update/keep decision for conflict(s): %s" % undecided]}

    ever = engine_version()
    project = str(unreal.SystemLibrary.get_game_name())
    added, updated, skipped = [], [], []

    # Plan: build the new meta for every incoming item without touching the
    # library yet ("keep"/"same" only get their root promotion, which never
    # rewrites a tree file).
    plan = []  # (item, meta)
    promoted = []  # metas changed without a file write (root promotion)
    for item in a["items"]:
        pkg = item["package"]
        src = package_to_file(pkg)

        if item["status"] == "same" or decisions.get(pkg) == "keep":
            skipped.append(pkg)
            # actively ingesting an existing entry promotes it to root (SPEC 3)
            if item["is_root"]:
                meta = store.load_meta(pkg)
                if meta and not meta.get("is_root"):
                    meta["is_root"] = True
                    store.save_meta(meta)
                    promoted.append(meta)
            continue

        hard, engine = _direct_deps(pkg, item["pcg"])
        # analyze() just hashed this file unless it settled the comparison on
        # size alone; either way the file is read at most once per commit.
        sha = item.get("sha256") or store.sha256_of(src)
        if item["status"] == "conflict":  # decision == "update"
            old = store.load_meta(pkg)
            meta = store.new_meta(pkg, item["class"], sha,
                                  item["size"], ever, project,
                                  item["is_root"] or old.get("is_root"),
                                  hard, engine, item["required_plugins"])
            # identity survives an update: keep first-ingest time, tags, notes,
            # and the filing - an update replaces the version, not where the
            # user put it (collections were dropped here until 2026-10-07)
            meta["ingested_at"] = old.get("ingested_at", meta["ingested_at"])
            meta["tags"] = old.get("tags", [])
            meta["notes"] = old.get("notes", "")
            meta["collections"] = old.get("collections", [])
        else:  # new
            meta = store.new_meta(pkg, item["class"], sha,
                                  item["size"], ever, project,
                                  item["is_root"], hard, engine,
                                  item["required_plugins"])
        if batch_tags:
            meta["tags"] = sorted(set(meta["tags"]) | batch_tags)
        plan.append((item, meta))

    # Stage: copy every incoming file next to its destination first, so a
    # copy failure (file lock, network drive, disk full) aborts the whole
    # batch before anything is archived - the library stays untouched.
    staged = []  # tmp paths, parallel to plan
    try:
        with progress.Reporter("Kura: copying assets into the library") as report:
            for idx, (item, _meta) in enumerate(plan, 1):
                report(idx, len(plan), item["package"])
                dst = store.tree_file(item["package"])
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                tmp = dst + ".kura-staging"
                staged.append(tmp)
                shutil.copy2(package_to_file(item["package"]), tmp)
    except OSError as e:
        _cleanup_staging(staged)
        # The root promotions above are already in their metas; without this
        # the index kept calling them dependencies until the next full
        # rebuild (2026-11-04). The library's FILES were not modified.
        library.update_entries(promoted)
        return {"ok": False, "added": [], "updated": [], "skipped": skipped,
                "errors": ["Staging copy failed - the library was not "
                           "modified: %s" % e]}

    # Commit: archive the old version, then rename the staged copy into
    # place (same volume, so os.replace is cheap and near-atomic).
    archive_batch = None
    try:
        for (item, meta), tmp in zip(plan, staged):
            pkg = item["package"]
            if item["status"] == "conflict":
                archive_batch = store.archive([pkg], batch_dir=archive_batch)
            os.replace(tmp, store.tree_file(pkg))
            store.save_meta(meta)
            (updated if item["status"] == "conflict" else added).append(pkg)
    except OSError as e:
        _cleanup_staging(staged)
        library.rebuild_index()
        errors = ["Ingest interrupted after %d of %d file(s): %s"
                  % (len(added) + len(updated), len(plan), e)]
        if archive_batch:
            errors.append("Archived library versions are in: %s" % archive_batch)
        return {"ok": False, "errors": errors, "added": added,
                "updated": updated, "skipped": skipped}

    # Only the rows this batch wrote. A full rebuild_index() here re-read
    # every meta file in the library and recreated four tables to register one
    # upload - 1.31 s at 10,833 entries whether the batch held one asset or a
    # thousand (measured 2026-08-23). An ingest only ever adds or replaces
    # entries, so nothing can be left stale by updating just these; the full
    # rebuild stays on the interrupted path above, where what landed is not
    # known.
    library.update_entries([meta for _item, meta in plan] + promoted)

    # File the batch (F8). After the index update, so membership lands on
    # entries that are already registered. A filing problem must not fail an
    # upload whose files are already safely in the library - report and move on.
    filed, filing_error = [], ""
    if collection:
        try:
            filed = catalog.assign(
                [p for p in a["roots"] if store.exists(p)], collection)["changed"]
        except (RuntimeError, ValueError) as e:
            # Returned as well as logged: the panel shows it next to the
            # success notice, the way a .kurapack import reports
            # collections.error (R1, 2026-11-17).
            filing_error = str(e)
            unreal.log_warning("Kura: assets were uploaded but could not be "
                               "added to the collection: %s" % e)

    # Never render a thumbnail from a project file the user chose to KEEP out
    # of the library - it differs from the library version by definition.
    kept = {i["package"] for i in a["items"]
            if i["status"] == "conflict" and decisions.get(i["package"]) == "keep"}
    _export_thumbnails([i for i in a["items"] if i["package"] not in kept])
    unreal.log("Kura: ingest done - added %d, updated %d, skipped %d"
               % (len(added), len(updated), len(skipped)))
    return {"ok": True, "errors": [], "added": added, "updated": updated,
            "skipped": skipped, "filed": filed,
            "filing_error": filing_error}
