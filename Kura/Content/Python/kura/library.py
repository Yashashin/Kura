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
    # Never makedirs the root here: a moved / disconnected library must
    # surface as guidance, not come back as an empty one at the stale path
    # (R2, 2026-11-17). First-run setup creates the folders explicitly.
    store.require_library_root()
    con = sqlite3.connect(config.DB_PATH)
    con.row_factory = sqlite3.Row
    return con


def rebuild_index(progress=None, total=0):
    """Full rebuild from the Meta mirror. Returns the number of entries.

    progress: optional progress(done, total, label) callable; `total` is a
    hint from the caller, since the Meta walk is a generator (this module
    stays unreal-free - kura.editor_api supplies a kura.progress.Reporter).

    Drop, recreate and refill run as ONE transaction (review 04, 2026-11-21).
    executescript() commits on its own, so the tables used to be gone and
    empty the moment the rebuild began: a meta read failing halfway left an
    index with no rows, which _ensure() then kept using - and with it the
    delete guard, which reads referrers from here, let referenced entries go.
    Now any exception rolls back to the index as it was and propagates."""
    con = _connect()
    # Manual transaction control: sqlite3's implicit transactions do not
    # cover DDL in every Python version, and executescript() would COMMIT.
    con.isolation_level = None
    try:
        con.execute("BEGIN IMMEDIATE")
        try:
            for table in ("collections", "deps", "tags", "assets"):
                con.execute("DROP TABLE IF EXISTS %s" % table)
            for statement in _SCHEMA.split(";"):
                if statement.strip():
                    con.execute(statement)
            count = 0
            for m in store.iter_metas():
                _insert_meta(con, m)
                count += 1
                if progress is not None:
                    progress(count, max(total, count), m["package"])
            con.execute("COMMIT")
        except BaseException:
            if con.in_transaction:  # SQLite may have rolled back by itself
                con.execute("ROLLBACK")
            raise
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


# A collection holds ROOTS (SPEC F8: "only roots need filing"). An entry's
# membership ids stay in its meta when it stops being a root - Unset Root is
# a flag edit, and setting it again should bring the filing back - but while
# it is not a root it is not a member: not counted, not gathered by "Import
# this collection", and not shown by the panel's collection grid, which has
# always listed roots only. Until 2026-11-21 the counts and the import still
# included such entries, so a collection could read 1 and show nothing
# (review 13). The same holds for memberships older metas already carry.
_ROOT_MEMBERS = ("SELECT c.cid, c.package FROM collections c JOIN assets a "
                 "ON a.package = c.package WHERE a.is_root = 1")


def packages_in_collection(cid, roots_only=False):
    """Entries assigned to a collection (SPEC F8). Lets catalog.py touch only
    the metas that actually carry the id instead of walking the whole mirror.

    roots_only: just the members in the F8 sense (see _ROOT_MEMBERS) - what
    a collection holds. Without it, every entry carrying the id, which is
    what a meta edit (deleting the collection) has to reach."""
    sql = ("SELECT package FROM (%s) WHERE cid=? ORDER BY package"
           % _ROOT_MEMBERS) if roots_only else \
        "SELECT package FROM collections WHERE cid=? ORDER BY package"
    con = _ensure()
    try:
        return [r[0] for r in con.execute(sql, (str(cid),))]
    finally:
        con.close()


def collection_memberships():
    """{cid: set of member packages} for every collection that has any - lets
    catalog.list_tree() roll counts up a subtree without counting an entry
    filed in two of its branches twice. Root members only (F8)."""
    con = _ensure()
    try:
        out = {}
        for cid, package in con.execute(_ROOT_MEMBERS):
            out.setdefault(cid, set()).add(package)
        return out
    finally:
        con.close()


def collection_counts():
    """{cid: member count} plus "" -> number of unfiled roots (F8 Unsorted).
    Root members only, like everything that reads a collection (F8)."""
    con = _ensure()
    try:
        counts = {r[0]: r[1] for r in con.execute(
            "SELECT cid, COUNT(*) FROM (%s) GROUP BY cid" % _ROOT_MEMBERS)}
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

    keep = _self_contained(set(targets) | candidates, referrers)

    return {
        "deletable": sorted(keep),
        "unused_deps": sorted(keep - set(targets)),
        # Requested entries that the guard will refuse, with the referrers
        # that hold them (same reason delete_entry raises).
        "blocked": {p: sorted(referrers.get(p, set()) - keep)
                    for p in targets if p not in keep},
    }


def _self_contained(keep, referrers):
    """Shrink a set to the largest subset whose every referrer lies inside
    it - the one rule behind both the delete preview and the delete itself."""
    keep = set(keep)
    changed = True
    while changed:
        changed = False
        for pkg in sorted(keep):
            if not referrers.get(pkg, set()) <= keep:
                keep.discard(pkg)
                changed = True
    return keep


def deletable_subset(packages):
    """Which of exactly these entries one delete may archive together
    (review 09, 2026-11-21): the largest subset nothing outside it refers
    to - the rule deletable_closure() previews, applied to what the user
    finally ticked. Entries referencing only each other (a cycle) go
    together; anything held from outside stays, and so does whatever it
    holds. Returns {"deletable": [...], "blocked": {package: [outside
    referrers, ...]}, "groups": [[...], ...]}.

    groups splits "deletable" into sets connected by references (either
    direction). Each has to leave the library whole or not at all - archiving
    a dependency and then failing on the entry using it would leave a live
    entry pointing at nothing - while unrelated groups need not wait on each
    other (review 09)."""
    wanted = list(dict.fromkeys(str(p) for p in packages))
    con = _ensure()
    try:
        referrers, deps = {}, {}
        for src, dst in con.execute("SELECT src, dst FROM deps"):
            referrers.setdefault(dst, set()).add(src)
            deps.setdefault(src, set()).add(dst)
    finally:
        con.close()
    keep = _self_contained(wanted, referrers)
    deletable = [p for p in wanted if p in keep]
    groups, seen = [], set()
    for start in deletable:
        if start in seen:
            continue
        group, queue = [], [start]
        seen.add(start)
        while queue:
            pkg = queue.pop()
            group.append(pkg)
            for other in (referrers.get(pkg, set()) | deps.get(pkg, set())):
                if other in keep and other not in seen:
                    seen.add(other)
                    queue.append(other)
        groups.append([p for p in deletable if p in set(group)])
    return {"deletable": deletable,
            "blocked": {p: sorted(referrers.get(p, set()) - keep)
                        for p in wanted if p not in keep},
            "groups": groups}


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
