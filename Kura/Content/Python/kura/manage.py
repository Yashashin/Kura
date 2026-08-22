"""Library management (SPEC F4). No unreal imports - pure data-layer ops
that must also work from a future standalone browser.
"""
import os
import shutil

from . import catalog, config, library, store


def set_tags(package, tags):
    meta = _require_meta(package)
    meta["tags"] = sorted({str(t).strip() for t in tags if str(t).strip()})
    store.save_meta(meta)
    library.update_entry(meta)
    return meta


def set_notes(package, notes):
    meta = _require_meta(package)
    meta["notes"] = str(notes)
    store.save_meta(meta)
    library.update_entry(meta)
    return meta


def set_root(package, is_root):
    """Flip an entry's is_root flag (SPEC 3). Root entries show in the default
    browse view; dependencies (is_root False) only surface with Show
    Dependencies, alongside whatever uses them.
    Pure meta edit - never touches the tree file."""
    meta = _require_meta(package)
    meta["is_root"] = bool(is_root)
    store.save_meta(meta)
    library.update_entry(meta)
    return meta


def set_roots(packages, is_root):
    for pkg in packages:
        set_root(pkg, is_root)
    return {"count": len(packages)}


def _require_meta(package):
    """The entry's meta, or a refusal. Every edit in this module goes through
    here, so this is also where a library written by a newer Kura is turned
    away (SPEC 5.6) - one clear message before anything is touched."""
    meta = store.load_meta(package)
    if meta is None:
        raise RuntimeError("Not a library entry: %s" % package)
    schema_error = store.meta_schema_error(meta)
    if schema_error:
        raise RuntimeError(schema_error)
    return meta


def delete_preview(packages):
    """What a delete would actually take with it (F4, 2026-08-02).

    Reports the requested entries plus the dependencies that would be left
    unused - the UI offers those as an opt-in so a material never strands its
    textures in the library. An empty list asks for the library-wide sweep
    (Clean Unused Dependencies)."""
    targets = list(dict.fromkeys(str(p) for p in packages))
    for pkg in targets:
        _require_meta(pkg)
    return library.deletable_closure(targets)


def delete_entry(package, batch_dir=None):
    """Archive an entry (F4). Blocked while anything still references it.

    batch_dir: append to an existing _archive batch instead of opening a new
    one, so everything deleted in one action restores together."""
    package = str(package)
    _require_meta(package)
    referrers = library.referrers_of(package)
    if referrers:
        raise RuntimeError(
            "Cannot delete %s - still referenced by: %s" % (package, referrers))
    batch = store.archive([package], batch_dir)
    library.remove_entry(package)
    return {"archived_to": batch}


def delete_entries(packages, progress=None):
    """Archive several entries (F4; multi-select delete, 2026-07-12).

    Entries referenced by other entries in the same batch must go first, so
    loop until a pass makes no progress: each successful delete removes its
    dep edges from the index, unblocking its dependencies for the next pass.
    Entries still referenced from outside the batch stay blocked and are
    reported with their referrer list.

    One action = one _archive batch (2026-08-02): a material archived together
    with its unused textures must come back as a set, not as N batches the
    user has to restore one by one.

    progress: optional progress(done, total, label) callable (see rescan)."""
    remaining = list(dict.fromkeys(str(p) for p in packages))
    total = len(remaining)
    archived, last_error, batch = [], {}, None
    moved = True
    while remaining and moved:
        moved = False
        for pkg in list(remaining):
            try:
                batch = delete_entry(pkg, batch)["archived_to"]
            except RuntimeError as e:
                last_error[pkg] = str(e)
                continue
            archived.append(pkg)
            remaining.remove(pkg)
            moved = True
            if progress is not None:
                progress(len(archived), total, pkg)
    return {"archived": archived,
            "archived_to": batch,
            "errors": [last_error.get(p, "%s: delete failed" % p)
                       for p in remaining]}


def move_folder(src_rel, dst_rel):
    """Move a library folder (F4 level 2): files + meta together.

    Only allowed when NO entry inside has any referrer - a referrer's import
    table stores the old package path, and Kura never rewrites .uasset files
    (this includes references between two entries that would move together).
    File names never change (SPEC 5.5). Since the type roots went away
    (2026-08-10, Q24) the destination is unconstrained; re-filing a whole
    group is what collections are for (SPEC F8), this stays the rare
    physical-layout fix.
    """
    store.require_library_root()
    src_rel = src_rel.replace("\\", "/").strip("/")
    dst_rel = dst_rel.replace("\\", "/").strip("/")
    if not src_rel or not dst_rel:
        raise ValueError("Empty folder path")
    if dst_rel == src_rel:
        raise RuntimeError("%s is already there" % src_rel)
    if dst_rel.startswith(src_rel + "/"):
        raise RuntimeError("Cannot move %s into itself" % src_rel)

    src_prefix = store.package_of(src_rel) + "/"
    moving = [m for m in store.iter_metas()
              if m["package"].startswith(src_prefix)]
    if not moving:
        raise RuntimeError("No library entries under %s" % src_rel)

    schema_errors = [err for err in (store.meta_schema_error(m) for m in moving)
                     if err]
    if schema_errors:
        raise RuntimeError("\n".join(schema_errors))

    blocked = {}
    for m in moving:
        refs = library.referrers_of(m["package"])
        if refs:
            blocked[m["package"]] = refs
    if blocked:
        # One line per blocked entry: the guard is the whole story here, and
        # "which asset, referenced by what" is what makes it actionable.
        lines = ["Cannot move %s: %s still referenced by library entries, and "
                 "Kura never rewrites .uasset files - the references would "
                 "break." % (src_rel, "1 entry is" if len(blocked) == 1
                             else "%d entries are" % len(blocked))]
        for pkg in sorted(blocked):
            lines.append("    %s  <-  %s" % (pkg, ", ".join(sorted(blocked[pkg]))))
        lines.append(
            "Most library folders hold something referenced: a root asset uses "
            "the textures stored beside it, a shared master is used by its "
            "instances. To relocate one anyway: move it in the project's "
            "Content Browser (Unreal fixes the references), send it to Kura "
            "again, then delete the old entries. To re-file assets for "
            "browsing, use collections - they change nothing on disk.")
        raise RuntimeError("\n".join(lines))

    collisions = []
    for m in moving:
        new_pkg = store.package_of(
            dst_rel + "/" + store.rel_path(m["package"])[len(src_rel) + 1:])
        if store.exists(new_pkg):
            collisions.append(new_pkg)
    if collisions:
        raise RuntimeError("\n".join(
            ["Cannot move %s: the destination already holds these entries "
             "(path is identity, so they cannot be merged):" % src_rel]
            + ["    %s" % p for p in collisions]))

    moved = []
    try:
        for m in moving:
            old_pkg = m["package"]
            new_pkg = store.package_of(
                dst_rel + "/" + store.rel_path(old_pkg)[len(src_rel) + 1:])
            for src, dst in ((store.tree_file(old_pkg), store.tree_file(new_pkg)),
                             (store.meta_file(old_pkg), store.meta_file(new_pkg)),
                             (store.thumb_file(old_pkg), store.thumb_file(new_pkg))):
                if os.path.isfile(src):
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    shutil.move(src, dst)
            m["package"] = new_pkg
            store.save_meta(m)
            store._prune_empty_dirs(os.path.dirname(store.tree_file(old_pkg)),
                                    config.TREE_DIR)
            store._prune_empty_dirs(os.path.dirname(store.meta_file(old_pkg)),
                                    config.META_DIR)
            moved.append(new_pkg)
    except OSError as e:
        # Entry-by-entry moves, so a locked file or a permission error can stop
        # halfway. Nothing is lost - both halves are real entries - but the
        # index has to be told before the user sees the message.
        library.rebuild_index()
        raise RuntimeError(
            "Move interrupted after %d of %d entries: %s\nThe folder is now "
            "split between %s and %s; move the remaining entries by hand (or "
            "the moved ones back), then run Verify Library."
            % (len(moved), len(moving), e, src_rel, dst_rel))
    library.rebuild_index()
    return {"moved": moved}


def rename_folder(src_rel, new_name):
    """Rename a library folder (F4). Same operation as move_folder - a
    folder's path IS its identity, so renaming and re-parenting are one move -
    and it owns just the one extra rule: a name is a single path segment."""
    new_name = str(new_name).strip().strip("/")
    if not new_name:
        raise ValueError("Empty folder name")
    bad = [c for c in '/\\:*?"<>|' if c in new_name]
    if bad:
        raise ValueError(
            "A folder name cannot contain %s" % " ".join(sorted(set(bad))))
    src_rel = str(src_rel).replace("\\", "/").strip("/")
    parent = src_rel.rsplit("/", 1)[0] if "/" in src_rel else ""
    return move_folder(src_rel, "%s/%s" % (parent, new_name) if parent else new_name)


def rescan(progress=None):
    """Full reconciliation (F4): report drift between disk and Meta, then
    rebuild the index. A rescue tool: it reports what it finds and repairs
    nothing that involves a judgement call. The one exception is memberships
    naming a deleted collection - there is nothing to preserve and nothing to
    decide, and the index is being rewritten anyway (F8).

    progress: optional progress(done, total, label) callable - hashing every
    file is the long part, so it reports per file (kura.progress.Reporter
    supplies one; this module stays unreal-free)."""
    store.require_library_root()
    tree_rels, meta_rels = set(), set()
    for dirpath, _dirs, files in os.walk(config.TREE_DIR):
        for name in files:
            if name.endswith(".uasset"):
                full = os.path.join(dirpath, name)
                rel = os.path.relpath(full, config.TREE_DIR)
                tree_rels.add(rel[:-len(".uasset")].replace(os.sep, "/"))
    metas = {}
    for m in store.iter_metas():
        rel = store.rel_path(m["package"])
        meta_rels.add(rel)
        metas[rel] = m

    unregistered = sorted(store.package_of(r) for r in tree_rels - meta_rels)
    missing = sorted(metas[r]["package"] for r in meta_rels - tree_rels)
    modified = []
    shared = sorted(tree_rels & meta_rels)
    for idx, rel in enumerate(shared, 1):
        m = metas[rel]
        if progress is not None:
            progress(idx, len(shared), m["package"])
        if store.sha256_of(store.tree_file(m["package"])) != m["sha256"]:
            modified.append(m["package"])
    broken_deps = {}
    for rel in sorted(meta_rels):
        m = metas[rel]
        bad = [d for d in m.get("hard_deps", [])
               if not d.startswith(config.LIB_PREFIX + "/")
               or store.rel_path(d) not in meta_rels]
        if bad:
            broken_deps[m["package"]] = bad

    # Stale memberships are pure noise - the collection they name is gone, so
    # there is nothing to preserve and nothing to decide. Verify Library
    # already rebuilds the index, so clearing them here keeps the report from
    # naming a problem the user has no way to act on.
    pruned_collections = catalog.prune_unknown()

    # Entries a newer Kura wrote (SPEC 5.6): still readable and still indexed,
    # but every write path refuses them. Reported so the reason for those
    # refusals is discoverable from the library itself.
    newer_schema = sorted(m["package"] for m in metas.values()
                          if store.meta_schema_error(m))

    count = library.rebuild_index(progress=progress, total=len(meta_rels))
    return {
        "entries": count,
        "unregistered": unregistered,   # file on disk, no meta
        "missing": missing,             # meta without file
        "modified": modified,           # externally changed (hash mismatch)
        "broken_deps": broken_deps,     # hard_deps pointing at no entry
        # Memberships that named a collection which no longer exists (F8) -
        # hand-edited collections.json, or an interrupted delete. Already
        # cleared by the time this is reported (see above).
        "pruned_collections": pruned_collections,
        # Entries this Kura may read but not modify (SPEC 5.6).
        "newer_schema": newer_schema,
        # Dependencies nobody references any more - dead weight from deletes
        # done before the cascade existed, or from external edits. Reported
        # only; Clean Unused Dependencies is what archives them.
        "orphans": library.deletable_closure([])["unused_deps"],
    }


def clear_archive():
    return {"freed_bytes": store.clear_archive()}


# ---------------------------------------------------------------------------
# Recently Deleted (F4, 2026-07-15): browse/restore/delete _archive batches
# ---------------------------------------------------------------------------

def _archive_batch_dir(batch):
    """Resolve a batch name to its folder, refusing anything that is not a
    direct child of _archive (no separators, no traversal)."""
    batch = str(batch)
    if not batch or os.path.basename(batch) != batch or batch in (".", ".."):
        raise ValueError("Invalid archive batch name: %s" % batch)
    path = os.path.join(config.ARCHIVE_DIR, batch)
    if not os.path.isdir(path):
        raise RuntimeError("No such archive batch: %s" % batch)
    return path


def list_archive():
    """All _archive batches, newest first: name, entry rel-paths, byte size."""
    batches = []
    if os.path.isdir(config.ARCHIVE_DIR):
        for name in sorted(os.listdir(config.ARCHIVE_DIR), reverse=True):
            path = os.path.join(config.ARCHIVE_DIR, name)
            if not os.path.isdir(path):
                continue
            entries, size = [], 0
            for dirpath, _dirs, files in os.walk(path):
                for f in files:
                    full = os.path.join(dirpath, f)
                    size += os.path.getsize(full)
                    if f.endswith(".uasset"):
                        rel = os.path.relpath(full, os.path.join(path, "Content"))
                        entries.append(rel[:-len(".uasset")].replace(os.sep, "/"))
            batches.append({"batch": name, "entries": sorted(entries),
                            "bytes": size})
    return {"batches": batches}


def restore_archive(batch):
    """Move a batch's entries back into the library (reverse of store.archive).

    Per entry: blocked when the library already holds that package path (the
    stable-path slot is taken - path = identity, never overwrite silently);
    otherwise tree file + meta + thumbnail move back. Fully-restored batches
    are removed; a partially-blocked batch keeps the blocked leftovers."""
    path = _archive_batch_dir(batch)
    restored, blocked = [], []
    content_root = os.path.join(path, "Content")
    for dirpath, _dirs, files in os.walk(content_root):
        for f in sorted(files):
            if not f.endswith(".uasset"):
                continue
            full = os.path.join(dirpath, f)
            rel = os.path.relpath(full, content_root)[:-len(".uasset")] \
                .replace(os.sep, "/")
            pkg = store.package_of(rel)
            if store.exists(pkg) or os.path.isfile(store.tree_file(pkg)):
                blocked.append("%s: the library already has an entry at this "
                               "path" % pkg)
                continue
            meta_src = os.path.join(path, "Meta", *rel.split("/")) + ".json"
            thumb_src = os.path.join(path, "Meta", *rel.split("/")) + ".png"
            for src, dst in ((full, store.tree_file(pkg)),
                             (meta_src, store.meta_file(pkg)),
                             (thumb_src, store.thumb_file(pkg))):
                if os.path.isfile(src):
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    shutil.move(src, dst)
            restored.append(pkg)
    # Drop now-empty dirs; remove the batch folder entirely once emptied.
    for sub in ("Content", "Meta"):
        for dirpath, _dirs, _files in os.walk(os.path.join(path, sub),
                                              topdown=False):
            try:
                os.rmdir(dirpath)
            except OSError:
                pass
    try:
        os.rmdir(path)
    except OSError:
        pass
    library.rebuild_index()
    return {"restored": restored, "blocked": blocked}


def delete_archive_batch(batch):
    """Permanently delete one _archive batch. Returns freed byte count."""
    path = _archive_batch_dir(batch)
    freed = 0
    for dirpath, _dirs, files in os.walk(path):
        for f in files:
            freed += os.path.getsize(os.path.join(dirpath, f))
    shutil.rmtree(path)
    return {"freed_bytes": freed}
