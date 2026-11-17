"""Kura package (.kurapack) export / import (SPEC F7).

A package is a portable slice of the library: a zip holding the asset-tree
files, their Meta mirror (json + thumbnail PNG) and a manifest. Import goes
into the LIBRARY (not the project) and reuses the core identity rules with
no special cases: path = identity, hash = version, conflicts always prompt.
Meta json travels as-is - tags / notes / is_root survive the trip.

No unreal imports here - this module must stay usable outside the editor
(future standalone browser, SPEC 2). editor_api passes the editor's engine
version in for the import guard.
"""
import json
import os
import shutil
import zipfile

from . import catalog, config, library, store

FORMAT_VERSION = 1
MANIFEST_NAME = "kurapack.json"
EXTENSION = ".kurapack"

# Wire value the import dialog sends for "use the package's own collections".
# Not a collection id (ids are "c_" + 8 hex), so the two can share one field.
PACK_COLLECTIONS = "__package__"


_version_tuple = store.version_tuple


def _safe_rel(pkg):
    """Library-relative path of a MANIFEST package, or raise ValueError.

    Manifest paths are untrusted (packages are downloaded files): reject
    traversal and separator tricks before anything reaches os.path.join,
    or a malicious pack could stage files outside the library."""
    rel = store.rel_path(pkg)  # raises unless under LIB_PREFIX
    for part in rel.split("/"):
        if part in ("", ".", "..") or "\\" in part or ":" in part:
            raise ValueError("unsafe path component in %s" % pkg)
    return rel


# ---------------------------------------------------------------------------
# Archive layout: mirrors the library root, so a package is self-describing
# and can even be unzipped straight over a library by hand in a pinch.
#   Content/_AssetLib/<rel>.uasset  |  Meta/<rel>.json  |  Meta/<rel>.png
# ---------------------------------------------------------------------------

def _content_base():
    # "/Game/_AssetLib" -> "Content/_AssetLib" (SPEC 5.1 mirror)
    return "Content/" + "/".join(config.LIB_PREFIX.strip("/").split("/")[1:])


def _tree_name(rel):
    return "%s/%s.uasset" % (_content_base(), rel)


def _meta_name(rel):
    return "Meta/%s.json" % rel


def _thumb_name(rel):
    return "Meta/%s.png" % rel


# ---------------------------------------------------------------------------
# Export
# ---------------------------------------------------------------------------

def _collections_block(metas):
    """The slice of the exporter's collection tree these entries use (F7).

    Keyed by full PATH, never by id: an id only names a collection inside the
    exporter's own collections.json, which does not travel. Path is portable
    for the same reason it is in the asset tree - it is the identity.

    Only branches that actually carry a member travel, plus their ancestors:
    a package about wood must not plant the exporter's whole taxonomy in
    somebody else's library. Ancestors sort first so the importer can build
    the tree in one forward pass. Returns {} when nothing here is filed, so a
    package from a library that never used collections keeps its old shape."""
    items = catalog.load()["collections"]
    if not items:
        return {}
    byid = {c["id"]: c for c in items}
    members, used_ids = {}, set()
    for m in metas:
        paths = set()
        for cid in m.get("collections", []) or []:
            path = catalog.path_of(cid, items)
            if not path:
                continue  # membership left pointing at a deleted collection
            paths.add(path)
            current, guard = str(cid), 0
            while current in byid and guard <= len(items):
                used_ids.add(current)
                current = byid[current]["parent"]
                guard += 1
        if paths:
            members[m["package"]] = sorted(paths)
    if not members:
        return {}
    tree = [{"path": catalog.path_of(cid, items),
             "order": int(byid[cid].get("order", 0) or 0)}
            for cid in used_ids]
    tree.sort(key=lambda e: (e["path"].count("/"), e["order"], e["path"]))
    return {"tree": tree, "members": members}


def export_package(packages, out_path, name="", description="", author="",
                   progress=None):
    """Write the entries' live dependency closure into a .kurapack (F7).

    The closure is computed from the library graph at export time (same rule
    as F3 import), so a package is always self-contained; /Engine/ deps are
    recorded in each entry's meta and never packaged.
    progress: optional progress(done, total, label) - this module stays
    unreal-free, so kura.editor_api passes a kura.progress.Reporter.
    Returns {"ok", "errors", "path", "entries", "bytes"}.
    """
    store.require_library_root()
    roots = [str(p) for p in packages]
    if not roots:
        return {"ok": False, "errors": ["Nothing selected to export"]}
    try:
        closure = library.closure(roots)
    except RuntimeError as e:
        return {"ok": False, "errors": [str(e)]}

    metas, errors = [], []
    for pkg in closure:
        meta = store.load_meta(pkg)
        if meta is None:
            errors.append("Library meta missing for %s (run Verify Library)" % pkg)
        elif not os.path.isfile(store.tree_file(pkg)):
            errors.append("Library file missing for %s (run Verify Library)" % pkg)
        else:
            metas.append(meta)
    if errors:
        return {"ok": False, "errors": errors}

    out_path = str(out_path)
    if not out_path.lower().endswith(EXTENSION):
        out_path += EXTENSION
    manifest = {
        "format_version": FORMAT_VERSION,
        "name": str(name) or os.path.splitext(os.path.basename(out_path))[0],
        "description": str(description),
        "author": str(author),
        "created_at": store.now_iso(),
        # Highest engine version inside; the import guard compares this first
        # so an incompatible package is refused before anything is extracted.
        "engine_version": max((m["engine_version"] for m in metas),
                              key=_version_tuple),
        "meta_schema_version": config.META_SCHEMA_VERSION,
        "lib_prefix": config.LIB_PREFIX,
        "entries": [{
            "package": m["package"],
            "class": m["class"],
            "sha256": m["sha256"],
            "size": m["size"],
            "is_root": bool(m.get("is_root")),
        } for m in metas],
    }
    # Filing rides along keyed by path (SPEC F7). Added only when there IS
    # any, and deliberately WITHOUT bumping format_version: an older Kura
    # hard-refuses a higher format_version but silently ignores a manifest
    # key it does not know, so a package stays importable either way - it
    # just lands in Unsorted there.
    collections = _collections_block(metas)
    if collections:
        manifest["collections"] = collections

    tmp = out_path + ".kura-staging"
    try:
        os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
        # strict_timestamps=False: a file dated before 1980 (zip's epoch) -
        # common after extracting some archives - is stored as 1980 instead
        # of raising ValueError and blocking the whole export (2026-11-04).
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED,
                             strict_timestamps=False) as z:
            z.writestr(MANIFEST_NAME,
                       json.dumps(manifest, ensure_ascii=False, indent=2))
            for idx, m in enumerate(metas, 1):
                if progress is not None:
                    progress(idx, len(metas), m["package"])
                rel = store.rel_path(m["package"])
                # .uasset payloads are mostly pre-compressed textures; store
                # them raw so multi-GB packs write at copy speed. The small
                # json/png sidecars keep the archive's default deflate.
                z.write(store.tree_file(m["package"]), _tree_name(rel),
                        zipfile.ZIP_STORED)
                # Meta travels as-is except for the collection membership,
                # whose ids only mean anything in THIS library's
                # collections.json - which does not travel (SPEC F8). The
                # filing itself does travel, by path, in the manifest's
                # "collections" block; the ids are stripped here as well as on
                # import so a hand-unzipped package never shows a reader ids
                # they cannot resolve.
                shipped = dict(m)
                shipped["collections"] = []
                z.writestr(_meta_name(rel),
                           json.dumps(shipped, ensure_ascii=False, indent=2))
                thumb = store.thumb_file(m["package"])
                if os.path.isfile(thumb):
                    z.write(thumb, _thumb_name(rel))
        os.replace(tmp, out_path)
    except Exception as e:  # noqa: BLE001 - the half-written file must go
        # Broader than OSError for the same reason as commit()'s staging: any
        # other exception escaping here left a *.kura-staging next to the
        # user's chosen path (2026-11-04).
        try:
            os.remove(tmp)
        except OSError:
            pass
        return {"ok": False, "errors": ["Package write failed: %s" % e]}
    return {"ok": True, "errors": [], "path": out_path,
            "entries": len(metas), "bytes": os.path.getsize(out_path)}


# ---------------------------------------------------------------------------
# Import
# ---------------------------------------------------------------------------

def _load_manifest(z):
    """Manifest dict of an open zip, or raise ValueError with a user message."""
    try:
        manifest = json.loads(z.read(MANIFEST_NAME).decode("utf-8"))
    except KeyError:
        raise ValueError("Not a Kura package: %s is missing" % MANIFEST_NAME)
    except ValueError:
        raise ValueError("Not a Kura package: %s is unreadable" % MANIFEST_NAME)
    # Shape-check untrusted input so a malformed manifest fails with one clear
    # message instead of leaking a Python traceback into the error dialog.
    try:
        if (not isinstance(manifest, dict)
                or not isinstance(manifest.get("entries", []), list)
                or not isinstance(manifest.get("collections", {}), dict)):
            raise TypeError
        int(manifest.get("format_version", 0))
        int(manifest.get("meta_schema_version", 0))
    except (TypeError, ValueError):
        raise ValueError("Not a Kura package: malformed %s" % MANIFEST_NAME)
    if int(manifest.get("format_version", 0)) > FORMAT_VERSION:
        raise ValueError(
            "This package was created by a newer Kura (package format %s, "
            "this Kura reads up to %s) - update Kura to import it"
            % (manifest.get("format_version"), FORMAT_VERSION))
    if int(manifest.get("meta_schema_version", 0)) > config.META_SCHEMA_VERSION:
        raise ValueError(
            "This package carries meta schema %s but this Kura writes schema "
            "%s - update Kura to import it"
            % (manifest.get("meta_schema_version"), config.META_SCHEMA_VERSION))
    return manifest


def _tree_paths(block):
    """Collection paths a manifest's "collections" block ships, ancestors
    first. Tolerates a hand-edited manifest: anything unusable is dropped."""
    out = []
    tree = (block or {}).get("tree")
    for entry in tree if isinstance(tree, list) else []:
        if isinstance(entry, dict):
            path = str(entry.get("path", "")).strip()
            if path and path not in out:
                out.append(path)
    return out


def _members(block):
    """{package: [path, ...]} from a manifest's "collections" block, with
    anything that is not that shape dropped - the block is untrusted input
    read AFTER the files are in place, so a malformed one must cost the
    filing, never the import. A bare string is refused rather than iterated,
    which would plant one collection per character."""
    raw = (block or {}).get("members")
    if not isinstance(raw, dict):
        return {}
    return {str(pkg): [str(p).strip() for p in paths if str(p).strip()]
            for pkg, paths in raw.items() if isinstance(paths, list)}


def _collections_summary(block):
    """What the import dialog needs to offer "use the package's collections"
    (SPEC F7 import step 5): whether any filing rides along, how many
    branches, and how many of them this library does not have yet."""
    paths = _tree_paths(block)
    if not paths:
        return {"total": 0, "new": 0, "entries": 0}
    return {"total": len(paths),
            "new": len(catalog.ensure_paths(paths, dry_run=True)["created"]),
            "entries": len(_members(block))}


def preview(pack_path, current_engine=None, force_version=False, progress=None):
    """Compare a package against the library (F7 import steps 1-2).

    Returns {"ok", "errors", "manifest", "items"}; item: {package, class,
    status: new|same|conflict, size, sha256, referrers}. The wording of the
    engine-guard error must keep "newer-engine" - the C++ flow keys its
    force-retry prompt on it (same contract as F3 import).
    """
    store.require_library_root()
    pack_path = str(pack_path)
    try:
        z = zipfile.ZipFile(pack_path)
    except (OSError, zipfile.BadZipFile) as e:
        return {"ok": False, "errors": ["Cannot open package: %s" % e],
                "manifest": {}, "items": []}
    with z:
        try:
            manifest = _load_manifest(z)
        except ValueError as e:
            return {"ok": False, "errors": [str(e)], "manifest": {}, "items": []}

        info = {k: manifest.get(k, "") for k in
                ("name", "description", "author", "created_at",
                 "engine_version", "format_version")}
        info["entries"] = len(manifest.get("entries", []))
        info["collections"] = _collections_summary(manifest.get("collections"))
        errors = []

        prefix = manifest.get("lib_prefix", "")
        if prefix != config.LIB_PREFIX:
            errors.append(
                "This package was built for stable path prefix %s but this "
                "library uses %s - the paths are incompatible"
                % (prefix or "<unknown>", config.LIB_PREFIX))
        if (current_engine and not force_version
                and _version_tuple(manifest.get("engine_version"))
                > _version_tuple(current_engine)):
            errors.append(
                "This package was saved by UE %s but this editor is %s; a "
                "newer-engine asset cannot be loaded (enable force to override)"
                % (manifest.get("engine_version"), current_engine))
        if errors:
            return {"ok": False, "errors": errors, "manifest": info, "items": []}

        names = set(z.namelist())
        items, seen = [], set()
        entries = manifest.get("entries", [])
        for idx, entry in enumerate(entries, 1):
            if progress is not None:
                progress(idx, len(entries), str(entry.get("package", ""))
                         if isinstance(entry, dict) else "")
            if not isinstance(entry, dict):
                errors.append("Malformed manifest entry: %r" % (entry,))
                continue
            pkg = str(entry.get("package", ""))
            if pkg in seen:
                # A duplicate would archive-then-clobber itself on commit.
                errors.append("Duplicate entry in manifest: %s" % pkg)
                continue
            seen.add(pkg)
            try:
                rel = _safe_rel(pkg)
            except ValueError:
                errors.append("Package entry outside %s or with an unsafe "
                              "path: %s" % (config.LIB_PREFIX, pkg))
                continue
            for arc in (_tree_name(rel), _meta_name(rel)):
                if arc not in names:
                    errors.append("Package is incomplete: %s is missing" % arc)
            meta = store.load_meta(pkg)
            # Newer-Kura entry on THIS side: the manifest check above only
            # covers the schema the package itself carries (SPEC 5.6).
            local_schema_error = (store.meta_schema_error(meta)
                                  if meta is not None else None)
            if local_schema_error:
                errors.append(local_schema_error)
                continue
            if meta is None:
                status, referrers = "new", []
            elif meta["sha256"] == entry.get("sha256"):
                status, referrers = "same", []
            else:
                status, referrers = "conflict", library.referrers_of(pkg)
            items.append({
                "package": pkg,
                "class": entry.get("class", ""),
                # No "selected" key: the user picked the whole file, so the
                # preview dialog's "you picked this / Kura collected it"
                # split does not apply and it drops that line entirely.
                "status": status,
                "size": entry.get("size", 0),
                "sha256": entry.get("sha256", ""),
                "referrers": referrers,
            })
        if errors:
            return {"ok": False, "errors": errors, "manifest": info, "items": []}
        return {"ok": True, "errors": [], "manifest": info, "items": items}


def _cleanup_staged(staged):
    """Best-effort removal of leftover staging copies (+ now-empty dirs)."""
    for _item, files in staged:
        for tmp, _final in files:
            try:
                os.remove(tmp)
            except OSError:
                pass
            d = os.path.dirname(tmp)
            store._prune_empty_dirs(d, config.TREE_DIR)
            store._prune_empty_dirs(d, config.META_DIR)


def _stage_meta(z, arc, tmp, pkg, sha256):
    """Write an incoming meta into staging with the exporter's filing
    dropped and the recipient's own kept (SPEC F7 import step 5).

    Collection ids name collections in the EXPORTER's Meta/collections.json,
    which does not travel, so the incoming ids are always dropped (SPEC F8).
    What takes their place is the recipient's filing of the entry this
    updates, if any: an update brings a new version, not a reason to unfile
    it - until 2026-10-07 it came out Unsorted, while an identical entry
    (never restaged) kept its collections. The package's own filing is added
    on top afterwards from the manifest's path-keyed "collections" block, by
    _file_imported, once the entries are in place - so a new entry is
    briefly Unsorted between staging and filing, and stays that way if the
    user picked "None (Unsorted)". Everything else rides along untouched -
    tags, notes and is_root describe the asset.

    The meta must describe the entry it ships with: its "package" is what
    every later write derives the meta's path from (store.save_meta), and its
    "sha256" is what Verify Library checks the file against. Both were taken
    on trust until 2026-11-04, so a crafted package could pass _safe_rel on
    the manifest and still name a different - even a "../" - package inside
    the meta. A mismatch fails staging like a bad hash does."""
    with z.open(arc) as src:
        meta = json.loads(src.read().decode("utf-8"))
    if not isinstance(meta, dict):
        raise ValueError("%s: the meta in the package is not an object" % pkg)
    if meta.get("package") != pkg:
        raise ValueError("%s: the meta in the package describes %r instead"
                         % (pkg, meta.get("package")))
    if meta.get("sha256") != sha256:
        raise ValueError("%s: the meta in the package disagrees with the "
                         "manifest about the file's hash" % pkg)
    local = store.load_meta(pkg)
    meta["collections"] = list((local or {}).get("collections") or [])
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=2)


def _file_imported(packages, choice, block):
    """Apply the import dialog's collection choice (SPEC F7 import step 5).

    choice: PACK_COLLECTIONS to rebuild the package's own filing by path,
    "" to file nothing (the entries stay in Unsorted), or an existing
    collection id to drop every imported ROOT into that one node, flat - the
    same semantics as the ingest dialog's picker, so a user only learns the
    control once. In that last case the package's own paths are deliberately
    NOT planted: the user just said where these belong.

    Never raises. By the time this runs the assets are already in place, so a
    filing problem is something to report, not something to fail an import
    over. Returns {"choice", "filed", "created", "error"}."""
    out = {"choice": str(choice or ""), "filed": [], "created": [], "error": ""}
    if not out["choice"] or not packages:
        return out
    metas = {}
    for pkg in packages:
        meta = store.load_meta(pkg)
        if meta is not None:
            metas[pkg] = meta
    try:
        if out["choice"] != PACK_COLLECTIONS:
            roots = sorted(p for p, m in metas.items() if m.get("is_root"))
            if roots:
                catalog.assign(roots, out["choice"], add=True)
                out["filed"] = roots
            return out

        wanted = {}  # collection path -> [package, ...]
        members = _members(block)
        for pkg in sorted(metas):
            for path in members.get(pkg, []):
                wanted.setdefault(path, []).append(pkg)
        if not wanted:
            return out
        # Create the shipped branch set in one pass, ancestors first, so
        # branches that only hold sub-collections exist too and the tree
        # reads the way it did on the exporting side. _tree_paths is already
        # ancestor-ordered; sorted() is the fallback for a manifest with
        # members but no tree.
        made = catalog.ensure_paths(_tree_paths(block) or sorted(wanted))
        out["created"] = made["created"]
        ids = made["ids"]
        missing = sorted(p for p in wanted if p not in ids)
        if missing:
            extra = catalog.ensure_paths(missing)
            ids.update(extra["ids"])
            out["created"] += extra["created"]
        filed = []
        for path, pkgs in sorted(wanted.items()):
            cid = ids.get(path)
            if cid:
                catalog.assign(pkgs, cid, add=True)
                filed += pkgs
        out["filed"] = sorted(set(filed))
    except Exception as e:  # noqa: BLE001 - see "Never raises" above
        out["error"] = str(e)
    return out


def commit(pack_path, decisions=None, current_engine=None, force_version=False,
           progress=None, collection=""):
    """Import a package into the library (F7 import steps 2-3).

    decisions: {package: "update" | "keep"} for every conflict item.
    collection: where the imported entries get filed - PACK_COLLECTIONS,
    "" (Unsorted), or an existing collection id. Filing runs last, after the
    index update, because it is a meta write plus an index update; it
    covers everything except entries the user explicitly chose to Ignore
    (status "same" included, so re-importing a package fixes filing that
    predates this feature), and it never fails the import.
    "update" archives the current library version first (restorable from
    Recently Deleted); the package's meta then replaces the old meta wholesale
    - meta travels as-is, so local tag edits live on in the archived copy -
    except the filing, which is the recipient's and stays (_stage_meta).
    Every staged .uasset is hash-verified against the manifest before anything
    is placed, so a corrupted download aborts with the library untouched.
    Returns {"ok", "errors", "added", "updated", "skipped"}, plus
    "collections" (see _file_imported) when the import went through.
    """
    decisions = dict(decisions or {})
    p = preview(pack_path, current_engine=current_engine,
                force_version=force_version)
    if not p["ok"]:
        return {"ok": False, "errors": p["errors"],
                "added": [], "updated": [], "skipped": []}

    conflicts = [i["package"] for i in p["items"] if i["status"] == "conflict"]
    undecided = [c for c in conflicts
                 if decisions.get(c) not in ("update", "keep")]
    if undecided:
        return {"ok": False, "added": [], "updated": [], "skipped": [],
                "errors": ["No update/keep decision for conflict(s): %s"
                           % undecided]}

    skipped, plan = [], []
    for item in p["items"]:
        if item["status"] == "same" or decisions.get(item["package"]) == "keep":
            skipped.append(item["package"])
        else:
            plan.append(item)

    # Stage: extract every incoming file next to its destination and verify
    # its hash, so extraction/verification failure (corrupt zip, disk full)
    # aborts the whole batch before anything is archived (same discipline as
    # ingest/import staging).
    store.ensure_library_dirs()
    staged = []  # (item, [(tmp, final), ...])
    pack_collections = {}
    try:
        with zipfile.ZipFile(pack_path) as z:
            names = set(z.namelist())
            # Re-read rather than thread it out of preview(): the manifest is
            # a few KB and this zip is open anyway, even when plan is empty.
            # preview() already validated it, so a failure here can only mean
            # the file changed underneath us - filing is not worth aborting a
            # staged import for.
            try:
                pack_collections = _load_manifest(z).get("collections") or {}
            except ValueError:
                pack_collections = {}
            for idx, item in enumerate(plan, 1):
                pkg = item["package"]
                if progress is not None:
                    progress(idx, len(plan), pkg)
                rel = _safe_rel(pkg)  # defense in depth; preview validated too
                tree_arc, meta_arc = _tree_name(rel), _meta_name(rel)
                if tree_arc not in names or meta_arc not in names:
                    # preview() checked both are in the zip; getting here
                    # means the file changed underneath us.
                    raise ValueError(
                        "%s is missing from the package - it changed while "
                        "the import was running" % pkg)
                # Registered for cleanup BEFORE anything is written: this
                # item's own files must go too when one of them fails - a
                # corrupt meta used to leave the .uasset copy staged before
                # it behind (2026-11-04; R2 had covered earlier items only).
                files = []
                staged.append((item, files))
                for arc, final in ((tree_arc, store.tree_file(pkg)),
                                   (meta_arc, store.meta_file(pkg)),
                                   (_thumb_name(rel), store.thumb_file(pkg))):
                    if arc not in names:
                        continue  # thumbnails are optional
                    os.makedirs(os.path.dirname(final), exist_ok=True)
                    tmp = final + ".kura-staging"
                    files.append((tmp, final))
                    if arc == meta_arc:
                        _stage_meta(z, arc, tmp, pkg, item["sha256"])
                    else:
                        with z.open(arc) as src, open(tmp, "wb") as dst:
                            shutil.copyfileobj(src, dst, 1 << 20)
                tree_tmp = files[0][0]
                if store.sha256_of(tree_tmp) != item["sha256"]:
                    raise OSError(
                        "%s failed hash verification - the package file is "
                        "corrupted (re-download it), or the library it was "
                        "exported from had unverified changes (run Verify "
                        "Library there and re-export)" % pkg)
    except Exception as e:  # noqa: BLE001 - staging must always clean up
        # Broader than OSError on purpose: a corrupt meta inside the package
        # raises JSONDecodeError out of _stage_meta, and anything else that
        # escapes here would leave *.kura-staging files behind for the user to
        # find and delete by hand (2026-08-23). Nothing is committed yet, so
        # there is no state to salvage - only files to remove.
        _cleanup_staged(staged)
        return {"ok": False, "added": [], "updated": [], "skipped": skipped,
                "errors": ["Package staging failed - the library was not "
                           "modified: %s" % e]}

    # Commit: archive the old version of each conflict, then rename the
    # staged copies into place (same volume, near-atomic).
    added, updated = [], []
    archive_batch = None
    try:
        for item, files in staged:
            pkg = item["package"]
            if item["status"] == "conflict":
                archive_batch = store.archive([pkg], batch_dir=archive_batch)
            for tmp, final in files:
                os.replace(tmp, final)
            (updated if item["status"] == "conflict" else added).append(pkg)
    except OSError as e:
        _cleanup_staged(staged[len(added) + len(updated):])
        library.rebuild_index()
        errors = ["Package import interrupted after %d of %d entr%s: %s"
                  % (len(added) + len(updated), len(staged),
                     "y" if len(staged) == 1 else "ies", e)]
        if archive_batch:
            errors.append("Archived library versions are in: %s" % archive_batch)
        return {"ok": False, "errors": errors, "added": added,
                "updated": updated, "skipped": skipped}

    # Re-reading every meta file in the library to register this import cost
    # 1.31 s at 10,833 entries regardless of package size (2026-08-23). The
    # meta files just landed on disk, so read back only those; an import never
    # removes an entry, so no row can go stale.
    library.update_entries(store.load_meta(p) for p in added + updated)
    # Everything that came in OR was already identical, minus what the user
    # chose to Ignore: filing a "same" entry is what makes re-importing a
    # package repair filing done before packages carried any.
    filable = [i["package"] for i in p["items"]
               if decisions.get(i["package"]) != "keep"]
    return {"ok": True, "errors": [], "added": added, "updated": updated,
            "skipped": skipped,
            "collections": _file_imported(filable, collection,
                                          pack_collections)}
