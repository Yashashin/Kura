"""Library management (SPEC F4). No unreal imports - pure data-layer ops
that must also work from a future standalone browser.
"""
import filecmp
import json
import os
import shutil
import tempfile

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
    """set_root() for a multi-selection. Every entry is checked before the
    first write, so an unknown or newer-Kura entry (SPEC 5.6) refuses the
    batch instead of stopping halfway through it, and the index is updated
    in one transaction (2026-11-04)."""
    metas = [_require_meta(pkg) for pkg in packages]
    touched = []
    try:
        for meta in metas:
            meta["is_root"] = bool(is_root)
            store.save_meta(meta)
            touched.append(meta)
    finally:
        library.update_entries(touched)
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

    progress: optional progress(done, total, label) callable (see rescan).

    The batch is judged as a whole, by the same rule as delete_preview()
    (library.deletable_subset, review 09, 2026-11-21). Entry-by-entry
    deletion with the single-entry guard could never start on a cycle - two
    entries referencing only each other each still had a referrer - so the
    preview offered both and the delete archived neither. An entry the batch
    cannot take is reported the way delete_entry() words it."""
    requested = list(dict.fromkeys(str(p) for p in packages))
    total = len(requested)
    errors, valid = {}, []
    for pkg in requested:
        try:
            _require_meta(pkg)
        except RuntimeError as e:
            errors[pkg] = str(e)
            continue
        valid.append(pkg)
    verdict = library.deletable_subset(valid)
    for pkg, refs in verdict["blocked"].items():
        errors[pkg] = ("Cannot delete %s - still referenced by: %s"
                       % (pkg, refs))
    # Each group connected by references is archived all or nothing
    # (store.archive rolls a failed one back): a locked Z.uasset used to stop
    # the loop after the A that Z uses had already gone, leaving a live Z
    # pointing at a missing A (review 09). The index is only told once a
    # group is entirely in the batch.
    archived, batch, stranded = [], None, False
    for group in verdict["groups"]:
        try:
            batch = store.archive(group, batch)
        except store.ArchiveRolledBack as e:
            stranded = stranded or bool(e.stuck)
            linked = ("" if len(group) == 1 else
                      ", together with the %d entr%s it is linked to in this "
                      "delete" % (len(group) - 1,
                                  "y" if len(group) == 2 else "ies"))
            for pkg in group:
                errors[pkg] = "%s: not deleted%s - %s" % (pkg, linked, e)
            if e.stuck and batch is None:
                batch = e.batch_dir  # what is stuck there restores from it
            continue
        for pkg in group:
            library.remove_entry(pkg)
            archived.append(pkg)
            if progress is not None:
                progress(len(archived), total, pkg)
    if stranded:
        # Files that could not be put back are in the batch, not where the
        # index says - make it say what is actually on disk.
        library.rebuild_index()
    return {"archived": archived,
            "archived_to": batch,
            "errors": [errors[p] for p in requested if p in errors]}


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
    # Every segment becomes part of a package path, i.e. an identity. "." and
    # ".." would be resolved by the filesystem but kept verbatim in the meta,
    # minting an entry whose recorded package names no file (2026-11-04).
    for part in dst_rel.split("/"):
        if part in ("", ".", ".."):
            raise ValueError("Invalid folder path: %s" % dst_rel)
    if dst_rel == src_rel:
        raise RuntimeError("%s is already there" % src_rel)
    if dst_rel.startswith(src_rel + "/"):
        raise RuntimeError("Cannot move %s into itself" % src_rel)

    src_prefix = store.package_of(src_rel) + "/"
    unreadable = []
    moving = [m for m in store.iter_metas(unreadable)
              if m["package"].startswith(src_prefix)]
    # A damaged meta cannot be rewritten for the new path; moving the rest
    # would strand its asset file behind (D1, 2026-11-17).
    damaged = [r for r in unreadable if r.startswith(src_rel + "/")]
    if damaged:
        raise RuntimeError(
            "%s holds metadata that cannot be read (%s) - run Verify Library "
            "and repair it before moving the folder"
            % (src_rel, ", ".join(damaged)))
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

    # Any library file at the destination counts, not only a meta: shutil.move
    # replaces an existing file, so an asset file sitting there without meta
    # (or a stray thumbnail) used to be overwritten by the move (review 01,
    # 2026-11-21).
    collisions = []
    for m in moving:
        new_pkg = store.package_of(
            dst_rel + "/" + store.rel_path(m["package"])[len(src_rel) + 1:])
        taken = store.occupied_files(new_pkg)
        if taken:
            collisions.append("%s  (%s)" % (new_pkg, ", ".join(taken)))
    if collisions:
        raise RuntimeError("\n".join(
            ["Cannot move %s: the destination already holds files at these "
             "paths (path is identity, so they cannot be merged; nothing was "
             "moved):" % src_rel]
            + ["    %s" % p for p in collisions]))

    # A moving entry whose own asset file is gone would land at the new path
    # as a meta without file - the move cannot make it whole, so say so first.
    missing = [m["package"] for m in moving
               if not os.path.isfile(store.tree_file(m["package"]))]
    if missing:
        raise RuntimeError("\n".join(
            ["Cannot move %s: these entries have metadata but no asset file "
             "(Verify Library lists them under Meta without file); nothing "
             "was moved:" % src_rel]
            + ["    %s" % p for p in missing]))

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
    if new_name in (".", ".."):
        raise ValueError("A folder name cannot be %s" % new_name)
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
    unreadable = []
    for m in store.iter_metas(unreadable):
        rel = store.rel_path(m["package"])
        meta_rels.add(rel)
        metas[rel] = m

    # A file whose meta is damaged HAS a meta - it is reported once, under
    # "unreadable", not a second time as a file without one.
    unregistered = sorted(store.package_of(r)
                          for r in tree_rels - meta_rels - set(unreadable))
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
    # naming a problem the user has no way to act on. Only when the tree can
    # be read, though: judged against a damaged collections.json every id
    # looks stale, and clearing them would unfile the whole library for good
    # (review 02, 2026-11-21). Then the file is reported and nothing pruned.
    try:
        pruned_collections, collections_error = catalog.prune_unknown(), []
    except catalog.CatalogError as e:
        pruned_collections, collections_error = [], [str(e)]

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
        # Why the pruning above was skipped - collections.json unreadable or
        # gone while entries are filed. Empty when it ran (review 02).
        "collections_error": collections_error,
        # Entries this Kura may read but not modify (SPEC 5.6).
        "newer_schema": newer_schema,
        # Meta files that could not be parsed (truncated by a crash or a full
        # disk). Left out of the index and hidden from the panel; reported so
        # they can be re-sent or restored (D1, 2026-11-17).
        "unreadable": sorted(store.package_of(r) for r in unreadable),
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
            entries, metas, size = [], set(), 0
            meta_root = os.path.join(path, "Meta")
            for dirpath, _dirs, files in os.walk(path):
                for f in files:
                    full = os.path.join(dirpath, f)
                    size += os.path.getsize(full)
                    if f.endswith(".uasset"):
                        rel = os.path.relpath(full, os.path.join(path, "Content"))
                        entries.append(rel[:-len(".uasset")].replace(os.sep, "/"))
                    elif f.endswith((".json", ".png")) and \
                            os.path.normpath(dirpath).startswith(
                                os.path.normpath(meta_root)):
                        rel = os.path.relpath(
                            os.path.splitext(full)[0], meta_root)
                        metas.add(rel.replace(os.sep, "/"))
            # Meta / thumbnails archived without their asset file: nothing to
            # restore, kept until the user deletes the batch. Counted so such
            # a batch does not read as "0 entries" with a size (R3).
            batches.append({"batch": name, "entries": sorted(entries),
                            "orphans": len(metas - set(entries)),
                            "bytes": size})
    return {"batches": batches}


def _same_bytes(a, b):
    return os.path.isfile(a) and os.path.isfile(b) and filecmp.cmp(
        a, b, shallow=False)


def _discard_archived(path):
    """Remove one archived copy once the library holds its twin. A seam for
    tests: a sharing lock on Windows is what makes this fail in practice."""
    os.remove(path)


# The restore journal (review 08, second round, 2026-11-21). Equal bytes in a
# library slot do not prove THIS restore put them there - a copy made by hand
# or an independent restore looks exactly the same, and SPEC F4 says an
# occupied slot blocks. So before a restore renames anything into the
# library it records, per entry, which slots it has taken, in
# <batch>/Restoring/<rel>.json. A later restore of the same batch treats a
# slot as its own only when that record names it, and finishes the job from
# there - whichever of the cleanup steps was the one that failed.
#
# Two lists, because the moment between "about to place" and "placed" is a
# gap another writer can fall into (review 08, third round): "placing" is
# written before a slot is filled, "placed" only after the file system has
# confirmed the slot was free and is now ours. A retry that finds a file in
# a slot still marked "placing" cannot know whose it is, and stops.
JOURNAL_DIR = "Restoring"
_KINDS = ("tree", "thumb", "meta")  # placement order: meta last


def _journal_file(batch_path, rel):
    return os.path.join(batch_path, JOURNAL_DIR, *rel.split("/")) + ".json"


def _unique_temp(path):
    """A temporary file of this attempt's own beside `path`: created
    exclusively, so no other operation's staging file is ever reused or
    overwritten (review 08, third round)."""
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path),
                               prefix=os.path.basename(path) + ".",
                               suffix=".kura-staging")
    os.close(fd)
    return tmp


def _read_journal(batch_path, rel):
    """(placed, placing) slot kinds an earlier restore of this entry
    recorded, or None if none did. Raises ValueError for a record that cannot
    be read - an entry whose ownership is unknown must not be guessed at."""
    path = _journal_file(batch_path, rel)
    try:
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
    except FileNotFoundError:
        return None
    except (OSError, ValueError) as e:
        raise ValueError("its restore record %s cannot be read (%s)" % (path, e))
    if not isinstance(doc, dict):
        raise ValueError("its restore record %s is damaged" % path)
    lists = [doc.get("placed", []), doc.get("placing", [])]
    if not all(isinstance(v, list) and set(v) <= set(_KINDS) for v in lists):
        raise ValueError("its restore record %s is damaged" % path)
    return set(lists[0]), set(lists[1])


def _write_journal(batch_path, rel, pkg, placed, placing=()):
    path = _journal_file(batch_path, rel)
    if not placed and not placing:
        try:
            os.remove(path)
        except FileNotFoundError:
            pass
        store._prune_empty_dirs(os.path.dirname(path), batch_path)
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = _unique_temp(path)
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({"package": pkg, "placed": sorted(placed),
                       "placing": sorted(placing)}, f)
        os.replace(tmp, path)  # the record is this batch's own file
    except BaseException:
        _remove_quietly([tmp])
        raise


def _restore_entry(batch_path, rel, pkg, sources, journal=None, allowed=()):
    """Put one archived entry back - copy, place, then discard the archived
    copies. Returns (restored, message): message is the line restore_archive()
    reports, None when there is nothing to say.

    sources: {kind: archived path} for "tree" / "meta" / "thumb" (missing
    files are skipped). journal: (placed, placing) from this batch's record
    of an earlier restore, if any. allowed: library files proven to belong
    some other way (the legacy case - see restore_archive).

    Until 2026-11-21 each file was shutil.move()d in turn: a lock on the
    archived .json after the .uasset had gone back stranded the entry
    between library and batch (review 08). Now:

    - every library slot is checked first. A slot is free, or the journal
      claims it and it still holds the archived bytes. Anything else - an
      independent copy with identical bytes included - blocks the entry and
      leaves both sides untouched;
    - the archived files are COPIED next to their slots, so a lock or a full
      disk fails before anything in the library changes;
    - each slot is marked "placing", filled with an atomic no-overwrite
      rename (store.rename_no_replace - a slot another writer filled after
      the check above fails the rename and is left alone), then marked
      "placed", meta last. A failure takes back what this call placed - only
      while it still holds the archived bytes, so a file someone replaced it
      with stays - and drops those claims; the originals are still archived;
    - only then are the archived copies removed. Any that cannot be (a lock
      that lets them be read but not deleted) stay in the batch with the
      journal, and the next restore recognises its own files and finishes."""
    claimed, placing_before = journal or (set(), set())
    claimed = set(claimed)
    library_slot = {"tree": store.tree_file(pkg), "meta": store.meta_file(pkg),
                    "thumb": store.thumb_file(pkg)}
    slot_kind = {os.path.normcase(p): k for k, p in library_slot.items()}
    sources = {k: p for k, p in sources.items() if os.path.isfile(p)}
    allowed = {os.path.normcase(p) for p in allowed}
    unknown = [library_slot[k] for k in sorted(placing_before - claimed)
               if os.path.lexists(library_slot[k])]
    if unknown:
        return False, ("%s: an earlier restore stopped while placing %s, and "
                       "the library now holds a file there - Kura cannot tell "
                       "whether it is the restored copy or someone else's, so "
                       "nothing was changed. Check it, move it out of the "
                       "library if it is not wanted, then restore again"
                       % (pkg, ", ".join(unknown)))
    stray, pending = [], []
    try:
        for live in store.occupied_files(pkg):
            kind = slot_kind[os.path.normcase(live)]
            if os.path.normcase(live) in allowed:
                continue
            if kind not in claimed:
                stray.append(live)  # not placed by this restore
            elif kind in sources and not _same_bytes(sources[kind], live):
                stray.append(live)  # ours once, changed since
        for kind in _KINDS:
            if kind in sources and not os.path.lexists(library_slot[kind]):
                pending.append(kind)
    except OSError as e:
        return False, ("%s: could not compare with the library (%s) - nothing "
                       "was changed; try again" % (pkg, e))
    if stray:
        return False, ("%s: the library already has an entry or files at this "
                       "path (%s) - nothing was overwritten, and the archived "
                       "copy stays restorable" % (pkg, ", ".join(sorted(stray))))

    staged = {}
    try:
        for kind in pending:
            dst = library_slot[kind]
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            staged[kind] = _unique_temp(dst)
            shutil.copy2(sources[kind], staged[kind])
    except OSError as e:
        _remove_quietly(staged.values())
        return False, ("%s: could not be restored (%s) - the library was left "
                       "as it was and the archived copy is intact; try again"
                       % (pkg, e))

    placed, current = [], None
    try:
        for kind in pending:
            current = kind
            _write_journal(batch_path, rel, pkg, claimed, {kind})
            store.rename_no_replace(staged[kind], library_slot[kind])
            del staged[kind]
            claimed.add(kind)
            placed.append(kind)
            _write_journal(batch_path, rel, pkg, claimed)
    except OSError as e:
        _remove_quietly(staged.values())
        for kind in reversed(placed):
            slot = library_slot[kind]
            try:
                if _same_bytes(sources[kind], slot):
                    os.remove(slot)
                # else: replaced by someone else since - theirs now, left be
                claimed.discard(kind)
            except OSError:
                pass  # still ours, and the journal keeps saying so
            store._prune_empty_dirs(os.path.dirname(slot),
                                    config.TREE_DIR if kind == "tree"
                                    else config.META_DIR)
        try:
            _write_journal(batch_path, rel, pkg, claimed)
        except OSError:
            pass  # an over-claim or a stale "placing" only blocks a retry
        if isinstance(e, FileExistsError):
            return False, ("%s: another program put a file at %s while this "
                           "restore was running - it was left as it is, what "
                           "this attempt had placed was taken back, and the "
                           "archived copy is intact"
                           % (pkg, library_slot[current]))
        return False, ("%s: could not be restored (%s) - what this attempt "
                       "had placed was taken back and the archived copy is "
                       "intact; try again" % (pkg, e))

    left = []
    for kind in _KINDS:
        if kind in sources:
            try:
                _discard_archived(sources[kind])
            except OSError as e:
                left.append("%s (%s)" % (sources[kind], e))
    if left:
        return True, ("%s: restored, but the archived copy could not be "
                      "removed: %s - restore this batch again to tidy it up"
                      % (pkg, "; ".join(left)))
    try:
        _write_journal(batch_path, rel, pkg, set())
    except OSError as e:
        return True, ("%s: restored, but its restore record could not be "
                      "removed (%s) - restore this batch again to tidy it up"
                      % (pkg, e))
    return True, None


def _remove_quietly(paths):
    for p in list(paths):
        try:
            os.remove(p)
        except OSError:
            pass


def _legacy_half_restore(pkg, meta_src):
    """True for what the move-based restore before 2026-11-21 left when it
    stopped after the asset: the asset back in the library with no meta,
    its meta still in the batch - recognised by the archived meta's hash
    matching the live file, the one proof available without a journal."""
    tree = store.tree_file(pkg)
    try:
        if os.path.lexists(store.meta_file(pkg)) or not os.path.isfile(tree) \
                or not os.path.isfile(meta_src):
            return False
        with open(meta_src, encoding="utf-8") as f:
            archived = json.load(f)
        return (isinstance(archived, dict)
                and archived.get("sha256") == store.sha256_of(tree))
    except (OSError, ValueError):
        return False


def restore_archive(batch):
    """Move a batch's entries back into the library (reverse of store.archive).

    Per entry: blocked when the library already holds that package path (the
    stable-path slot is taken - path = identity, never overwrite silently);
    otherwise tree file + meta + thumbnail move back. Fully-restored batches
    are removed; a partially-blocked batch keeps the blocked leftovers.

    A meta / thumbnail with no .uasset beside it (an entry deleted while its
    file was already missing - Verify Library's "missing") is blocked too,
    and stays where it is: there is nothing to restore, and never
    hard-deleting (SPEC 5.1) means only Delete / Clear All may discard it.
    Before 2026-11-04 the walk only looked at .uasset files, so such a batch
    reported nothing and could never be emptied.

    Each entry goes back through _restore_entry (copy, claim in the batch's
    Restoring/ journal, place, discard), so a lock or a full disk leaves the
    library and the batch consistent, and a retry finishes exactly what an
    earlier attempt recorded as its own - never a slot that merely holds the
    same bytes (review 08). Without a journal, the one interrupted state
    still finished is the move-based version's (before 2026-11-21): asset
    back, meta in the batch, hashes matching."""
    path = _archive_batch_dir(batch)
    restored, blocked = [], []
    content_root = os.path.join(path, "Content")
    meta_root = os.path.join(path, "Meta")
    journal_root = os.path.join(path, JOURNAL_DIR)

    def rels_under(root, exts):
        out = set()
        for dirpath, _dirs, files in os.walk(root):
            for f in files:
                stem, ext = os.path.splitext(f)
                if ext in exts:
                    out.add(os.path.relpath(os.path.join(dirpath, stem), root)
                            .replace(os.sep, "/"))
        return out

    asset_rels = rels_under(content_root, (".uasset",))
    meta_rels = rels_under(meta_root, (".json", ".png"))
    journal_rels = rels_under(journal_root, (".json",))
    for rel in sorted(asset_rels | meta_rels | journal_rels):
        pkg = store.package_of(rel)
        sources = {
            "tree": os.path.join(content_root, *rel.split("/")) + ".uasset",
            "meta": os.path.join(meta_root, *rel.split("/")) + ".json",
            "thumb": os.path.join(meta_root, *rel.split("/")) + ".png",
        }
        try:
            claimed = _read_journal(path, rel)
        except ValueError as e:
            blocked.append("%s: not restored - %s; the archived copy is "
                           "intact" % (pkg, e))
            continue
        if claimed is not None:
            # An earlier restore of this batch stopped partway - placing, or
            # cleaning up after itself. Its journal says which slots it took.
            done, message = _restore_entry(path, rel, pkg, sources, claimed)
        elif rel in asset_rels:
            done, message = _restore_entry(path, rel, pkg, sources)
        elif _legacy_half_restore(pkg, sources["meta"]):
            # The move-based restore before 2026-11-21 stopped after the
            # asset: finish it with the meta (and thumbnail) it left here.
            done, message = _restore_entry(
                path, rel, pkg, {k: sources[k] for k in ("meta", "thumb")},
                allowed=(store.tree_file(pkg),))
        else:
            blocked.append("%s: archived without its asset file (only the "
                           "meta or thumbnail is in this batch), so there is "
                           "nothing to restore - delete the batch to discard "
                           "it" % pkg)
            continue
        if done:
            restored.append(pkg)
        if message:
            blocked.append(message)
    # Drop now-empty dirs; remove the batch folder entirely once emptied.
    for sub in ("Content", "Meta", JOURNAL_DIR):
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
