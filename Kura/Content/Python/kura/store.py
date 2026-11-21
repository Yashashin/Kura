"""Asset-tree storage layer (SPEC 5): tree files, per-asset meta json, archive.

Meta json is the source of truth; library.db is a rebuildable index on top.
No unreal imports here - this module must stay usable outside the editor
(future standalone browser, SPEC 2).
"""
import datetime
import errno
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
             is_root, hard_deps, engine_deps, required_plugins=()):
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
        # Plugins the asset needs in whatever project it is imported into
        # (SPEC F9; only PCG packages fill it). Added 2026-10-07; schema_version
        # stays 2 - an old meta reads as "needs nothing" (SPEC 5.6).
        "required_plugins": sorted(str(p) for p in required_plugins),
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
    """Meta dict for a package, or None if the entry is not in the library.

    A meta that exists but cannot be parsed (truncated by a crash or a full
    disk) is neither: it raises, naming the file, so a caller cannot mistake
    the entry for absent and the user is told where to look (D1, 2026-11-17).

    The same goes for one that exists but cannot be OPENED - a sharing
    violation, a permission or I/O error. Only "no such file" means absent:
    every other OSError used to read as None too, so an import whose asset
    file happened to be identical took the entry for an unregistered leftover
    and replaced the meta, locally edited notes and tags with it (FIX-014
    review 01, 2026-11-21)."""
    path = meta_file(package)
    try:
        with open(path, encoding="utf-8") as f:
            meta = json.load(f)
    except FileNotFoundError:
        return None
    except OSError as e:
        raise RuntimeError(
            "The metadata file for %s exists but cannot be read (%s): %s - "
            "nothing was changed. Close whatever holds it open, or check the "
            "disk and its permissions, then try again." % (package, e, path))
    except ValueError:
        meta = None
    # Parseable but unusable counts as damaged too (review 07): refusing here
    # stops every write and import at the read, before anything is changed.
    problem = "not valid JSON" if meta is None else meta_shape_error(meta)
    if problem:
        raise RuntimeError(
            "The metadata file for %s is damaged and cannot be read (%s): %s "
            "- run Verify Library to list damaged entries, then send the "
            "asset to Kura again or restore the file from a backup."
            % (package, problem, path))
    return meta


def _is_entry_meta(meta):
    return meta_shape_error(meta) is None


# Fields every reader relies on being there: the index writes each one into a
# NOT NULL column by key (library._insert_meta), import / export / Verify read
# them directly. Everything else is optional - older metas simply lack it.
_REQUIRED_STR = ("package", "class", "sha256", "engine_version",
                 "ingested_at", "updated_at")
_OPTIONAL_STR = ("source_project", "notes")
_OPTIONAL_STR_LIST = ("tags", "hard_deps", "engine_deps", "collections",
                      "required_plugins")


def meta_shape_error(meta):
    """None if a parsed meta can be read as an entry, else what is wrong.

    Valid JSON that is not a usable entry used to pass on a "package" string
    alone, then fail later with a KeyError inside Verify Library or the index
    rebuild - taking every other entry's report with it (review 07,
    2026-11-21). Checks what the readers actually use: the required strings,
    a non-negative integer size, the optional strings / string lists when
    present (a bare string where a list belongs would be iterated character
    by character) and is_root as a flag. Missing optionals are fine - that is
    every meta older than the field. schema_version is left to
    meta_schema_error()."""
    if not isinstance(meta, dict):
        return "not a JSON object"
    for key in _REQUIRED_STR:
        if not isinstance(meta.get(key), str) or not meta[key]:
            return 'missing or invalid "%s"' % key
    if not meta["package"].startswith(config.LIB_PREFIX + "/"):
        return '"package" is not under %s' % config.LIB_PREFIX
    size = meta.get("size")
    if isinstance(size, bool) or not isinstance(size, int) or size < 0:
        return 'missing or invalid "size"'
    for key in _OPTIONAL_STR:
        if key in meta and not isinstance(meta[key], str):
            return '"%s" is not a string' % key
    for key in _OPTIONAL_STR_LIST:
        value = meta.get(key, [])
        if not isinstance(value, list) or not all(
                isinstance(v, str) for v in value):
            return '"%s" is not a list of strings' % key
    if "is_root" in meta and not isinstance(meta["is_root"], (bool, int)):
        return '"is_root" is not a flag'
    return None


def save_meta(meta):
    # Last line of defence for SPEC 5.6: the callers that can produce a decent
    # batch-level message check first (ingest.analyze, manage._require_meta,
    # pack.preview), this catches whatever they miss.
    schema_error = meta_schema_error(meta)
    if schema_error:
        raise RuntimeError(schema_error)
    # Never write a meta the readers would reject (review 07).
    shape_error = meta_shape_error(meta)
    if shape_error:
        raise RuntimeError("Refusing to write metadata for %s: %s"
                           % (meta.get("package", "<unknown>"), shape_error))
    path = meta_file(meta["package"])
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=2)


def exists(package):
    return os.path.isfile(meta_file(package))


# ---------------------------------------------------------------------------
# File / meta agreement at one stable path (2026-11-21, review 01)
# ---------------------------------------------------------------------------
# Whether a path is taken used to be read off the meta alone. An asset file
# with no meta beside it then counted as a free slot - Import Package and Send
# to Kura replaced it as "new" without archiving it, and Move Folder moved
# over it - while a meta whose file was gone counted as a whole entry, so a
# re-import reported it identical and left the file missing. Every write path
# that decides "new / same / conflict" or "is the destination free" asks
# these first.

def unregistered_file_error(package):
    """The slot holds an asset file but no meta: Kura cannot know what the
    file is, so it must not be replaced or moved over."""
    return ("%s: the library folder already holds an asset file at this path, "
            "but no metadata for it (Verify Library lists it under Files "
            "without meta), so Kura cannot tell what it would replace. The "
            "file was left as it is - move it out of the library folder or "
            "restore its .json, then try again: %s"
            % (package, tree_file(package)))


def missing_file_error(package):
    """The slot holds a meta but no asset file: treating it as a complete
    entry would report a re-import as identical and leave the file missing."""
    return ("%s: the library has metadata for this entry but its asset file is "
            "missing (Verify Library lists it under Meta without file). "
            "Nothing was changed - put the .uasset back, or delete the entry "
            "(it goes to Recently Deleted), then try again: %s"
            % (package, tree_file(package)))


def new_slot_error(package, sha256):
    """None if a package planned as NEW may still be published, else why not.

    Checked again right before anything is renamed into place, after staging:
    the preview's verdict is only as current as its read, and something can
    take the slot in between - a meta written by another editor, an asset
    file that is not the one being placed. Anything there at all (even an
    entry that cannot be opened) counts as taken; only an asset file whose
    bytes are exactly the incoming ones may stay, which is the interrupted-
    import recovery (FIX-014 review 01)."""
    if os.path.lexists(meta_file(package)):
        return ("%s: metadata for this entry appeared in the library while "
                "the operation was running, so nothing was replaced: %s"
                % (package, meta_file(package)))
    tree = tree_file(package)
    if os.path.lexists(tree):
        try:
            same = os.path.isfile(tree) and sha256_of(tree) == sha256
        except OSError:
            same = False
        if not same:
            return unregistered_file_error(package)
    return None


def occupied_files(package):
    """Library files present at a package's stable path - asset, meta and
    thumbnail. A destination is free only when this is empty."""
    return [p for p in (tree_file(package), meta_file(package),
                        thumb_file(package)) if os.path.isfile(p)]


def iter_metas(unreadable=None):
    """Yield every per-asset meta dict in the library (Meta mirror walk).

    A file that cannot be parsed, or that parses to something other than an
    entry, is skipped rather than raised: one truncated meta used to take
    down the index rebuild and Verify Library - the rescue tool itself (D1,
    2026-11-17). Pass a list as `unreadable` to collect those files as
    Meta-relative paths without the extension ("Props/SM_Chair").

    Skips the reserved files the mirror also hosts - collections.json sits at
    the top of Meta/ but describes the collection tree, not an asset. Matched
    at the top level only: an asset named "collections" further down owns a
    meta file with exactly that name, and skipping it by name anywhere would
    drop that entry from the index without a word (2026-08-23)."""
    top = os.path.normpath(config.META_DIR)
    for dirpath, _dirs, files in os.walk(config.META_DIR):
        at_top = os.path.normpath(dirpath) == top
        for name in sorted(files):
            if not name.endswith(".json"):
                continue
            if at_top and name in config.RESERVED_META_FILES:
                continue
            full = os.path.join(dirpath, name)
            try:
                with open(full, encoding="utf-8") as f:
                    meta = json.load(f)
            except ValueError:  # JSONDecodeError / UnicodeDecodeError
                meta = None
            if not _is_entry_meta(meta):
                if unreadable is not None:
                    rel = os.path.relpath(full, config.META_DIR)
                    unreadable.append(
                        rel[:-len(".json")].replace(os.sep, "/"))
                continue
            yield meta


# ---------------------------------------------------------------------------
# Archive (SPEC 5.1: never hard-delete)
# ---------------------------------------------------------------------------

class ArchiveRolledBack(OSError):
    """archive() could not move every file and put back what it had moved.

    `stuck` lists (live path, archived path) pairs it could NOT put back -
    empty when the library is exactly as it was. Anything stuck is still in
    the batch, restorable; the message names each one."""

    def __init__(self, cause, stuck, batch_dir):
        self.cause, self.stuck, self.batch_dir = cause, list(stuck), batch_dir
        if self.stuck:
            text = ("Archiving stopped (%s) and %d file(s) could not be put "
                    "back - they are safe in %s and restorable from Recently "
                    "Deleted: %s" % (cause, len(self.stuck), batch_dir,
                                     ", ".join(a for _l, a in self.stuck)))
        else:
            text = ("Archiving stopped (%s) - every file was put back, nothing "
                    "was deleted" % cause)
        super().__init__(text)


def rename_no_replace(src, dst):
    """Rename src to dst only if nothing is at dst - decided atomically by
    the file system, never by a check followed by a rename.

    A check-then-replace lets another writer finish a file at dst in between
    and then destroys it (FIX-014 review 08, second round). Windows - the
    platform Kura ships on - gives this for free: os.rename is MoveFileEx
    without MOVEFILE_REPLACE_EXISTING and fails with FileExistsError when dst
    exists. POSIX rename() silently replaces, so there the file is hard-linked
    into place (link() fails on an existing name) and the old name dropped.
    A file system without hard links refuses with an OSError - the safe way
    to fail. Raises FileExistsError when dst was taken; src is then
    untouched."""
    if os.name == "nt":
        os.rename(src, dst)
        return
    os.link(src, dst)
    try:
        os.remove(src)
    except OSError:
        os.remove(dst)  # ours: link() just made it - back to one name
        raise


def _move_file(src, dst):
    """Move one file without ever replacing what is at dst.

    A rename where possible - the archive lives in the library root, so on
    one volume - which either happens whole or not at all: a file another
    program holds without delete sharing simply stays where it is.
    shutil.move() fell back to copy + delete there, which can leave a copy
    behind when the delete is refused. Across volumes (an _archive junction
    to another drive) it copies into a file it creates exclusively, and
    takes the copy back if the original cannot be removed."""
    try:
        rename_no_replace(src, dst)
        return
    except OSError as e:
        if e.errno != errno.EXDEV and getattr(e, "winerror", None) != 17:
            raise
    with open(src, "rb") as fsrc, open(dst, "xb") as fdst:
        try:
            shutil.copyfileobj(fsrc, fdst, 1 << 20)
        except BaseException:
            fdst.close()
            os.remove(dst)  # created above, so ours
            raise
    try:
        shutil.copystat(src, dst)
        os.remove(src)
    except OSError:
        try:
            os.remove(dst)
        except OSError:
            pass
        raise


def archive(packages, batch_dir=None):
    """Move entries' tree files + meta (+ thumbnail) into a timestamped
    _archive batch folder, preserving relative paths. Returns the batch dir.

    All or nothing for the packages given (FIX-014 review 09, 2026-11-21):
    file by file, a locked .uasset or .json used to stop the call with some
    entries - or half of one - already gone, while the index and the live
    referrers still pointed at them. Now every file moved is moved back on
    the first failure, and ArchiveRolledBack (an OSError, so every caller's
    existing handling applies) says whether that fully worked. Callers pass
    the entries that must go together - a delete group, one conflict."""
    created = batch_dir is None
    if created:
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        batch_dir = os.path.join(config.ARCHIVE_DIR, stamp)
        n = 0
        while os.path.isdir(batch_dir):  # same-second batches
            n += 1
            batch_dir = os.path.join(config.ARCHIVE_DIR, "%s_%d" % (stamp, n))
    moved = []    # (live, archived), in move order
    folders = []  # every batch folder a destination needed - the failed one too
    try:
        for package in packages:
            rel = rel_path(package)
            for src, sub in ((tree_file(package), "Content"),
                             (meta_file(package), "Meta"),
                             (thumb_file(package), "Meta")):
                if os.path.isfile(src):
                    dst = os.path.join(batch_dir, sub, *rel.split("/")) \
                        + os.path.splitext(src)[1]
                    folders.append(os.path.dirname(dst))
                    os.makedirs(folders[-1], exist_ok=True)
                    _move_file(src, dst)
                    moved.append((src, dst))
    except OSError as e:
        stuck = []
        for live, archived in reversed(moved):
            try:
                os.makedirs(os.path.dirname(live), exist_ok=True)
                _move_file(archived, live)
            except OSError:
                stuck.append((live, archived))
        # Including the folder made for the file that refused to move: it
        # held nothing, and left behind it kept an empty batch alive in
        # Recently Deleted (review 09, second round). Only empty folders go,
        # so a batch this call was appending to keeps everything in it.
        for folder in folders:
            _prune_empty_dirs(folder, batch_dir)
        if created and not stuck:
            try:
                os.rmdir(batch_dir)
            except OSError:
                pass
        raise ArchiveRolledBack(e, stuck, batch_dir)
    for package in packages:
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


def _is_strictly_inside(path, stop):
    """True when `path` lies below `stop` (never `stop` itself).

    Compared per path component, not as strings: "C:\\Lib\\Content2" starts
    with "C:\\Lib\\Content" but is not inside it. normcase folds Windows case
    and separators; commonpath raises across drives, which is "not inside"."""
    path = os.path.normcase(os.path.abspath(path))
    stop = os.path.normcase(os.path.abspath(stop))
    try:
        return path != stop and os.path.commonpath([path, stop]) == stop
    except ValueError:
        return False


def _prune_empty_dirs(path, stop):
    """Remove now-empty folders up to (not including) `stop`. Anything not
    inside `stop` is left alone - this deletes folders, so it does not rely
    on its callers handing it a path that happens to be under `stop`."""
    path = os.path.normpath(path)
    while _is_strictly_inside(path, stop):
        try:
            os.rmdir(path)  # fails on non-empty, which ends the walk
        except OSError:
            return
        path = os.path.dirname(path)
