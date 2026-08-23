"""SQLite index over per-asset meta json (SPEC 5.3).

Meta json files are the source of truth; this index is a rebuildable cache
holding the search surface and the dependency graph (forward + reverse).
No unreal imports here - this module must stay usable outside the editor
(future standalone browser, SPEC 2).
"""
import os
import sqlite3

from . import config, store

_SCHEMA = """
CREATE TABLE assets (
    package TEXT PRIMARY KEY,
    class TEXT NOT NULL,
    sha256 TEXT NOT NULL,
    size INTEGER NOT NULL,
    engine_version TEXT NOT NULL,
    ingested_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    source_project TEXT NOT NULL DEFAULT '',
    is_root INTEGER NOT NULL DEFAULT 0,
    notes TEXT NOT NULL DEFAULT ''
);
CREATE TABLE tags (
    package TEXT NOT NULL REFERENCES assets(package),
    tag TEXT NOT NULL
);
CREATE TABLE deps (
    src TEXT NOT NULL,
    dst TEXT NOT NULL
);
CREATE TABLE collections (
    package TEXT NOT NULL,
    cid TEXT NOT NULL
);
CREATE INDEX idx_tags_tag ON tags(tag);
CREATE INDEX idx_tags_package ON tags(package);
CREATE INDEX idx_deps_src ON deps(src);
CREATE INDEX idx_deps_dst ON deps(dst);
CREATE INDEX idx_collections_cid ON collections(cid);
CREATE INDEX idx_collections_package ON collections(package);
"""


def _connect():
    os.makedirs(config.LIBRARY_ROOT, exist_ok=True)
    con = sqlite3.connect(config.DB_PATH)
    con.row_factory = sqlite3.Row
    return con


def rebuild_index(progress=None, total=0):
    """Full rebuild from the Meta mirror. Returns the number of entries.

    progress: optional progress(done, total, label) callable; `total` is a
    hint from the caller, since the Meta walk is a generator (this module
    stays unreal-free - kura.editor_api supplies a kura.progress.Reporter)."""
    con = _connect()
    try:
        con.executescript(
            "DROP TABLE IF EXISTS collections;"
            "DROP TABLE IF EXISTS deps;"
            "DROP TABLE IF EXISTS tags;"
            "DROP TABLE IF EXISTS assets;")
        con.executescript(_SCHEMA)
        count = 0
        for m in store.iter_metas():
            _insert_meta(con, m)
            count += 1
            if progress is not None:
                progress(count, max(total, count), m["package"])
        con.commit()
        return count
    finally:
        con.close()


def _insert_meta(con, m):
    con.execute(
        "INSERT OR REPLACE INTO assets VALUES (?,?,?,?,?,?,?,?,?,?)",
        (m["package"], m["class"], m["sha256"], m["size"],
         m["engine_version"], m["ingested_at"], m["updated_at"],
         m.get("source_project", ""), int(bool(m.get("is_root"))),
         m.get("notes", "")))
    con.execute("DELETE FROM tags WHERE package=?", (m["package"],))
    con.executemany("INSERT INTO tags VALUES (?,?)",
                    [(m["package"], t) for t in m.get("tags", [])])
    con.execute("DELETE FROM deps WHERE src=?", (m["package"],))
    con.executemany("INSERT INTO deps VALUES (?,?)",
                    [(m["package"], d) for d in m.get("hard_deps", [])])
    con.execute("DELETE FROM collections WHERE package=?", (m["package"],))
    con.executemany("INSERT INTO collections VALUES (?,?)",
                    [(m["package"], c) for c in m.get("collections", [])])


def update_entry(meta):
    """Refresh one entry's index rows from its meta dict (after a meta edit)."""
    con = _ensure()
    try:
        _insert_meta(con, meta)
        con.commit()
    finally:
        con.close()


def update_entries(metas):
    """Refresh several entries' index rows in ONE transaction, after a batch
    write. Returns the number of entries written.

    update_entry() per package opens, commits and closes its own connection;
    at batch scale that costs as much as the rebuild_index() it was meant to
    replace (6 ms x 200 entries, measured 2026-08-23). One connection and one
    commit brings the same 200 entries down to a few milliseconds.

    Only ever adds or replaces rows - a caller that removed entries wants
    remove_entry() or a full rebuild_index()."""
    metas = [m for m in metas if m]
    if not metas:
        return 0
    con = _ensure()
    try:
        for m in metas:
            _insert_meta(con, m)
        con.commit()
    finally:
        con.close()
    return len(metas)


def remove_entry(package):
    con = _ensure()
    try:
        con.execute("DELETE FROM collections WHERE package=?", (package,))
        con.execute("DELETE FROM deps WHERE src=?", (package,))
        con.execute("DELETE FROM tags WHERE package=?", (package,))
        con.execute("DELETE FROM assets WHERE package=?", (package,))
        con.commit()
    finally:
        con.close()


def _ensure():
    if not os.path.isfile(config.DB_PATH):
        rebuild_index()
    return _connect()


# ---------------------------------------------------------------------------
# Queries
# ---------------------------------------------------------------------------

def search(text=None, tags=None, folder=None, roots_only=False, sort="updated",
           collection=None, unsorted=False):
    """Search entries; returns a list of dicts (each with its "tags" list).

    text: case-insensitive substring on package path / notes
    tags: entry must carry ALL given tags (AND)
    folder: library-relative folder prefix, e.g. "Master"
    roots_only: only entries the user actively ingested (SPEC Q10)
    sort: "updated" (newest first) or "name"
    collection: only entries in this collection id (SPEC F8)
    unsorted: only ROOT entries in no collection at all - the F8 to-file queue
    """
    where, params = [], []
    if text:
        like = "%" + str(text).lower() + "%"
        where.append("(lower(package) LIKE ? OR lower(notes) LIKE ?)")
        params += [like, like]
    if folder:
        prefix = store.package_of(folder)
        where.append("(package LIKE ? OR package LIKE ?)")
        params += [prefix + "/%", prefix]
    if roots_only:
        where.append("is_root = 1")
    if collection:
        where.append("package IN (SELECT package FROM collections WHERE cid=?)")
        params.append(str(collection))
    if unsorted:
        where.append("is_root = 1 AND package NOT IN "
                     "(SELECT package FROM collections)")
    tags = list(tags or [])
    if tags:
        where.append(
            "package IN (SELECT package FROM tags WHERE tag IN (%s) "
            "GROUP BY package HAVING COUNT(DISTINCT tag) = ?)"
            % ",".join("?" * len(tags)))
        params += tags + [len(tags)]
    sql = "SELECT * FROM assets"
    if where:
        sql += " WHERE " + " AND ".join(where)
    sql += (" ORDER BY updated_at DESC" if sort == "updated"
            else " ORDER BY package COLLATE NOCASE")
    con = _ensure()
    try:
        rows = [dict(r) for r in con.execute(sql, params)]
        for r in rows:
            r["tags"] = [t[0] for t in con.execute(
                "SELECT tag FROM tags WHERE package=? ORDER BY tag",
                (r["package"],))]
        return rows
    finally:
        con.close()


def packages_in_collection(cid):
    """Entries assigned to a collection (SPEC F8). Lets catalog.py touch only
    the metas that actually carry the id instead of walking the whole mirror."""
    con = _ensure()
    try:
        return [r[0] for r in con.execute(
            "SELECT package FROM collections WHERE cid=? ORDER BY package",
            (str(cid),))]
    finally:
        con.close()


def collection_counts():
    """{cid: member count} plus "" -> number of unfiled roots (F8 Unsorted)."""
    con = _ensure()
    try:
        counts = {r[0]: r[1] for r in con.execute(
            "SELECT cid, COUNT(*) FROM collections GROUP BY cid")}
        counts[""] = con.execute(
            "SELECT COUNT(*) FROM assets WHERE is_root = 1 AND package NOT IN "
            "(SELECT package FROM collections)").fetchone()[0]
        return counts
    finally:
        con.close()


def all_tags():
    con = _ensure()
    try:
        return [r[0] for r in con.execute(
            "SELECT DISTINCT tag FROM tags ORDER BY tag")]
    finally:
        con.close()


def dependencies_of(package):
    """Direct hard deps of an entry (library packages only)."""
    con = _ensure()
    try:
        return [r[0] for r in con.execute(
            "SELECT dst FROM deps WHERE src=? ORDER BY dst", (str(package),))]
    finally:
        con.close()


def referrers_of(package):
    """Entries that directly depend on the given package (reverse edges);
    delete guard / conflict impact list / detail panel (SPEC 5.3)."""
    con = _ensure()
    try:
        return [r[0] for r in con.execute(
            "SELECT src FROM deps WHERE dst=? ORDER BY src", (str(package),))]
    finally:
        con.close()


def deletable_closure(packages):
    """Resolve a delete request into what would actually leave the library
    (SPEC F4 unused dependencies, 2026-08-02).

    Deleting a material must not strand the textures it dragged in: they are
    non-root entries (they only ever entered as auto-collected dependencies,
    SPEC 3) and nothing else references them once their referrer is gone.

    Start from the requested entries plus every non-root entry reachable
    through their dependency edges, then shrink: anything still referenced
    from OUTSIDE the set survives - and a survivor keeps its own dependencies
    alive in turn. The fixpoint is the largest set whose referrers all live
    inside it, so whole chains (MI -> M -> T) collapse together while a shared
    master and its textures stay put.

    An empty `packages` asks for the library-wide sweep: every dependency
    nobody references any more (F4 Clean Unused Dependencies).

    Roots are never candidates - a texture the user ingested on purpose stays
    even with no referrer; Set as Root is how you pin one (SPEC F4).
    """
    targets = list(dict.fromkeys(str(p) for p in packages))
    con = _ensure()
    try:
        known = {r[0] for r in con.execute("SELECT package FROM assets")}
        non_roots = {r[0] for r in con.execute(
            "SELECT package FROM assets WHERE is_root = 0")}
        deps, referrers = {}, {}
        for src, dst in con.execute("SELECT src, dst FROM deps"):
            deps.setdefault(src, set()).add(dst)
            referrers.setdefault(dst, set()).add(src)
    finally:
        con.close()

    if targets:
        candidates, seen, queue = set(), set(), list(targets)
        while queue:
            pkg = queue.pop()
            if pkg in seen:
                continue
            seen.add(pkg)
            for dep in deps.get(pkg, ()):
                if dep not in known:
                    continue  # broken edge; Verify Library reports it
                queue.append(dep)
                if dep in non_roots:
                    candidates.add(dep)
        candidates -= set(targets)
    else:
        candidates = set(non_roots)

    keep = set(targets) | candidates
    changed = True
    while changed:
        changed = False
        for pkg in sorted(keep):
            if not referrers.get(pkg, set()) <= keep:
                keep.discard(pkg)
                changed = True

    return {
        "deletable": sorted(keep),
        "unused_deps": sorted(keep - set(targets)),
        # Requested entries that the guard will refuse, with the referrers
        # that hold them (same reason delete_entry raises).
        "blocked": {p: sorted(referrers.get(p, set()) - keep)
                    for p in targets if p not in keep},
    }


def closure(packages):
    """Transitive hard-dep closure over the library graph (roots included).
    Computed live at import time (SPEC F3): a master updated after ingest
    pulls in its current deps, not a frozen snapshot. Raises on missing
    entries (broken library reference)."""
    con = _ensure()
    try:
        seen, queue, missing = set(), [str(p) for p in packages], []
        while queue:
            pkg = queue.pop()
            if pkg in seen:
                continue
            seen.add(pkg)
            row = con.execute(
                "SELECT 1 FROM assets WHERE package=?", (pkg,)).fetchone()
            if row is None:
                missing.append(pkg)
                continue
            queue += [r[0] for r in con.execute(
                "SELECT dst FROM deps WHERE src=?", (pkg,))]
        if missing:
            raise RuntimeError(
                "Library dependency graph is broken; missing entries: %s "
                "(run Rescan Library)" % sorted(missing))
        return sorted(seen)
    finally:
        con.close()
