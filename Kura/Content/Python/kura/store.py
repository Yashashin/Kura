"""Asset-tree storage layer (SPEC 5): tree files, per-asset meta json, archive.

Meta json is the source of truth; library.db is a rebuildable index on top.
No unreal imports here - this module must stay usable outside the editor
(future standalone browser, SPEC 2).
"""
import datetime
import hashlib
import json
import os
import shutil

from . import config


# ---------------------------------------------------------------------------
# Path mapping: package <-> tree file <-> meta file
# ---------------------------------------------------------------------------

def ensure_library_dirs():
    """Create the library skeleton for a fresh root (SPEC 5.1): the asset tree
    plus the Meta and _archive mirrors. Idempotent - safe to call on an
    existing library. library.db is created separately by
    library.rebuild_index().

    No type root folders since 2026-08-10 (Q24): the tree's shape is the
    user's to choose, and an empty library starts out genuinely empty."""
    os.makedirs(config.TREE_DIR, exist_ok=True)
    os.makedirs(config.META_DIR, exist_ok=True)
    os.makedirs(config.ARCHIVE_DIR, exist_ok=True)


def require_library_root():
    """Block library operations while the configured root is gone from disk.

    A moved / renamed / disconnected-drive root must surface as guidance, not
    as a fresh empty library silently recreated at the stale path (the
    sqlite/makedirs calls would otherwise do exactly that). First-run setup
    creates the root explicitly via init_library, which never hits this."""
    if not os.path.isdir(config.LIBRARY_ROOT):
        raise RuntimeError(
            "Library folder not found: %s - it may have been moved, renamed, "
            "or be on a disconnected drive. Point Kura Settings at the "
            "library's current location (or restore the folder), then retry."
            % config.LIBRARY_ROOT)


def rel_path(package):
    """Library-relative path of a package, e.g. 'Material/Master/M_A'."""
    package = str(package)
    prefix = config.LIB_PREFIX + "/"
    if not package.startswith(prefix):
        raise ValueError("Not a library package: %s" % package)
    return package[len(prefix):]


def package_of(rel):
    return "%s/%s" % (config.LIB_PREFIX, rel.replace("\\", "/").strip("/"))


def tree_file(package):
    return os.path.join(config.TREE_DIR, *rel_path(package).split("/")) + ".uasset"


def meta_file(package):
    return os.path.join(config.META_DIR, *rel_path(package).split("/")) + ".json"


def thumb_file(package):
    return os.path.join(config.META_DIR, *rel_path(package).split("/")) + ".png"


# ---------------------------------------------------------------------------
# Placement rules (SPEC 5.4)
# ---------------------------------------------------------------------------

def placement_error(package):
    """None if the package may enter the library, else a message.

    One rule since 2026-08-10 (Q24): it has to live under the stable path
    prefix. Folder layout below that is the user's call - the recommended
    convention is a root asset plus its exclusive dependencies in one folder,
    with shared masters/functions/textures parked in fixed shared folders
    (SPEC 5.4) - and classification happens in collections, not in folders
    (SPEC F8). Root class is checked separately (config.ROOT_CLASSES)."""
    package = str(package)
    if not package.startswith(config.LIB_PREFIX + "/"):
        return "%s: not under %s (move it there in the Content Browser)" % (
            package, config.LIB_PREFIX)
    return None


# ---------------------------------------------------------------------------
# Meta json I/O
# ---------------------------------------------------------------------------

def now_iso():
    return datetime.datetime.now().astimezone().isoformat(timespec="seconds")


def version_tuple(v):
    """"5.7.0" -> (5, 7) for engine-version guard comparisons (F3 / F7)."""
    try:
        parts = [int(x) for x in str(v).split(".")]
    except ValueError:
        return (0, 0)
    return tuple(parts[:2]) + (0,) * (2 - len(parts))


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def new_meta(package, cls, sha256, size, engine_version, source_project,
             is_root, hard_deps, engine_deps):
    now = now_iso()
    return {
        "schema_version": config.META_SCHEMA_VERSION,
        "package": str(package),
        "class": str(cls),
        "sha256": sha256,
        "size": size,
        "engine_version": engine_version,
        "ingested_at": now,
        "updated_at": now,
        "source_project": source_project,
        "is_root": bool(is_root),
        "tags": [],
        # Virtual classification membership (SPEC F8): collection ids defined
        # in Meta/collections.json. Added 2026-08-10; schema_version stays 2
        # because old metas simply read as "no collections" (SPEC 5.6).
        "collections": [],
        "notes": "",
        "hard_deps": sorted(str(d) for d in hard_deps),
        "engine_deps": sorted(str(d) for d in engine_deps),
    }


def meta_schema_error(meta):
    """None if this Kura may WRITE this entry, else why not (SPEC 5.6).

    The "read new" half of the schema policy: a meta written by a newer Kura
    can carry fields this build knows nothing about, and every write path here
    rewrites the whole json - so saving it back would silently drop them.
    Reading stays allowed (browsing a newer library is fine); only changes are
    refused, with the upgrade as the way out. An unreadable schema_version is
    NOT treated as newer: that is drift for Verify Library to report, not a
    reason to lock the library."""
    try:
        version = int(float(meta.get("schema_version", 1)))
    except (TypeError, ValueError):
        return None
    if version > config.META_SCHEMA_VERSION:
        return ("%s: written by a newer Kura (meta schema %d, this Kura writes "
                "%d) - update Kura to change this entry"
                % (meta.get("package", "<unknown>"), version,
                   config.META_SCHEMA_VERSION))
    return None


def load_meta(package):
    """Meta dict for a package, or None if the entry is not in the library."""
    try:
        with open(meta_file(package), encoding="utf-8") as f:
            return json.load(f)
    except OSError:
        return None


def save_meta(meta):
    # Last line of defence for SPEC 5.6: the callers that can produce a decent
    # batch-level message check first (ingest.analyze, manage._require_meta,
    # pack.preview), this catches whatever they miss.
    schema_error = meta_schema_error(meta)
    if schema_error:
        raise RuntimeError(schema_error)
    path = meta_file(meta["package"])
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=2)


def exists(package):
    return os.path.isfile(meta_file(package))


def iter_metas():
    """Yield every per-asset meta dict in the library (Meta mirror walk).

    Skips the reserved files the mirror also hosts - collections.json sits at
    the top of Meta/ but describes the collection tree, not an asset."""
    for dirpath, _dirs, files in os.walk(config.META_DIR):
        for name in sorted(files):
            if not name.endswith(".json") or name in config.RESERVED_META_FILES:
                continue
            with open(os.path.join(dirpath, name), encoding="utf-8") as f:
                yield json.load(f)


# ---------------------------------------------------------------------------
# Archive (SPEC 5.1: never hard-delete)
# ---------------------------------------------------------------------------

def archive(packages, batch_dir=None):
    """Move entries' tree files + meta (+ thumbnail) into a timestamped
    _archive batch folder, preserving relative paths. Returns the batch dir.
    """
    if batch_dir is None:
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        batch_dir = os.path.join(config.ARCHIVE_DIR, stamp)
        n = 0
        while os.path.isdir(batch_dir):  # same-second batches
            n += 1
            batch_dir = os.path.join(config.ARCHIVE_DIR, "%s_%d" % (stamp, n))
    for package in packages:
        rel = rel_path(package)
        for src, sub in ((tree_file(package), "Content"),
                         (meta_file(package), "Meta"),
                         (thumb_file(package), "Meta")):
            if os.path.isfile(src):
                dst = os.path.join(batch_dir, sub, *rel.split("/")) \
                    + os.path.splitext(src)[1]
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.move(src, dst)
        _prune_empty_dirs(os.path.dirname(tree_file(package)), config.TREE_DIR)
        _prune_empty_dirs(os.path.dirname(meta_file(package)), config.META_DIR)
    return batch_dir


def clear_archive():
    """Manual maintenance (F4): wipe _archive. Returns freed byte count."""
    freed = 0
    if os.path.isdir(config.ARCHIVE_DIR):
        for dirpath, _dirs, files in os.walk(config.ARCHIVE_DIR):
            for name in files:
                freed += os.path.getsize(os.path.join(dirpath, name))
        shutil.rmtree(config.ARCHIVE_DIR)
    return freed


def _prune_empty_dirs(path, stop):
    """Remove now-empty folders up to (not including) `stop`."""
    stop = os.path.normpath(stop)
    path = os.path.normpath(path)
    while path.startswith(stop) and path != stop:
        try:
            os.rmdir(path)  # fails on non-empty, which ends the walk
        except OSError:
            return
        path = os.path.dirname(path)
