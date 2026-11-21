"""Virtual classification layer - "collections" (SPEC F8).

The asset tree is frozen as identity + storage: a package path IS the asset's
identity, so moving a file in the library would mint a new asset and strand
every project that already imported the old path (SPEC 5.5). Organisation
therefore lives here instead - creating, renaming, nesting and re-filing are
pure metadata edits that never touch a .uasset.

Two halves, both under Meta/ so the data layer stays self-describing and the
index stays rebuildable (SPEC 5.1 / 5.3):

    Meta/collections.json   the tree: id, display name, parent, sort order
    <asset>.json            membership: "collections": [<id>, ...]

Membership rides in each asset's own meta so it survives archive/restore for
free, and so renaming a collection never rewrites N asset files. Ids are
opaque and permanent; names are not.

No unreal imports here - this module must stay usable outside the editor
(future standalone browser, SPEC 2).
"""
import json
import os
import tempfile
import uuid

from . import config, library, store

# Bumped only if the shape of collections.json itself changes. Independent of
# the per-asset meta schema (SPEC 5.6), which stays at 2 - "collections" is a
# new field with a default, not a breaking change.
CATALOG_SCHEMA_VERSION = 1

# The tree a brand-new library starts with (2026-08-27, reversing the
# 2026-08-22 veto). An empty library used to open with no collections at all,
# which left the ingest dialog's Collection dropdown offering nothing but
# "None (Unsorted)": the one moment a user knows what an asset IS, is the one
# moment Kura gave them no way to say so. A starting tree costs nothing to
# ignore - rename, delete or nest anything, and only newly created libraries
# are seeded - but it turns filing into picking from a list.
#
# Depth-first in display order: ensure_paths() numbers siblings in the order
# it first meets them, so this listing IS the order the panel shows. Every
# node is spelled out rather than leaves only, so the shape stays readable
# and a branch that should exist empty (a place to file into later) does.
DEFAULT_TREE = (
    "Surfaces",
    "Surfaces/Wood",
    "Surfaces/Wood/Planks & Boards",
    "Surfaces/Wood/Flooring",
    "Surfaces/Wood/Raw & Rough",
    "Surfaces/Wood/Painted & Treated",
    "Surfaces/Wood/Bark",
    "Surfaces/Metal",
    "Surfaces/Metal/Clean & Brushed",
    "Surfaces/Metal/Painted & Coated",
    "Surfaces/Metal/Rusted & Weathered",
    "Surfaces/Metal/Plates & Panels",
    "Surfaces/Metal/Mesh & Perforated",
    "Surfaces/Stone",
    "Surfaces/Stone/Rock & Cliff",
    "Surfaces/Stone/Marble",
    "Surfaces/Stone/Granite & Slate",
    "Surfaces/Stone/Cut & Polished",
    "Surfaces/Stone/Masonry",
    "Surfaces/Brick",
    "Surfaces/Brick/Modern",
    "Surfaces/Brick/Old & Weathered",
    "Surfaces/Brick/Painted",
    "Surfaces/Concrete",
    "Surfaces/Concrete/Smooth & Polished",
    "Surfaces/Concrete/Rough",
    "Surfaces/Concrete/Damaged & Stained",
    "Surfaces/Plaster & Paint",
    "Surfaces/Plaster & Paint/Plaster",
    "Surfaces/Plaster & Paint/Painted Wall",
    "Surfaces/Tiles",
    "Surfaces/Tiles/Ceramic",
    "Surfaces/Tiles/Mosaic",
    "Surfaces/Tiles/Stone Tiles",
    "Surfaces/Tiles/Terracotta",
    "Surfaces/Ground",
    "Surfaces/Ground/Soil & Mud",
    "Surfaces/Ground/Sand",
    "Surfaces/Ground/Gravel",
    "Surfaces/Ground/Grass",
    "Surfaces/Ground/Moss",
    "Surfaces/Ground/Forest Floor",
    "Surfaces/Ground/Snow & Ice",
    "Surfaces/Fabric",
    "Surfaces/Fabric/Plain",
    "Surfaces/Fabric/Patterned",
    "Surfaces/Fabric/Knit & Wool",
    "Surfaces/Fabric/Carpet & Rug",
    "Surfaces/Leather",
    "Surfaces/Leather/Smooth",
    "Surfaces/Leather/Worn & Suede",
    "Surfaces/Roads & Paving",
    "Surfaces/Roads & Paving/Asphalt",
    "Surfaces/Roads & Paving/Paving Stones",
    "Surfaces/Roads & Paving/Concrete Paving",
    "Surfaces/Roofing",
    "Surfaces/Roofing/Shingles & Tiles",
    "Surfaces/Roofing/Metal Roofing",
    "Surfaces/Roofing/Thatch",
    "Surfaces/Synthetic",
    "Surfaces/Synthetic/Plastic",
    "Surfaces/Synthetic/Rubber",
    "Surfaces/Synthetic/Paper & Cardboard",
    "Surfaces/Synthetic/Rope & Wicker",
    "Surfaces/Custom",

    "Props",
    "Props/Furniture",
    "Props/Furniture/Seating",
    "Props/Furniture/Tables & Desks",
    "Props/Furniture/Storage",
    "Props/Furniture/Beds",
    "Props/Lighting",
    "Props/Lighting/Ceiling",
    "Props/Lighting/Wall",
    "Props/Lighting/Floor & Table",
    "Props/Lighting/Outdoor",
    "Props/Decor",
    "Props/Decor/Vases & Pottery",
    "Props/Decor/Frames & Art",
    "Props/Decor/Books & Stationery",
    "Props/Decor/Soft Decor",
    "Props/Decor/Potted Plants",
    "Props/Kitchen & Tableware",
    "Props/Kitchen & Tableware/Cookware",
    "Props/Kitchen & Tableware/Tableware",
    "Props/Kitchen & Tableware/Appliances",
    "Props/Kitchen & Tableware/Containers",
    "Props/Electronics",
    "Props/Electronics/Computers & Displays",
    "Props/Electronics/Audio & Video",
    "Props/Electronics/Household",
    "Props/Tools & Hardware",
    "Props/Tools & Hardware/Hand Tools",
    "Props/Tools & Hardware/Power Tools",
    "Props/Tools & Hardware/Hardware & Parts",
    "Props/Food",
    "Props/Food/Fruits & Vegetables",
    "Props/Food/Baked & Packaged",
    "Props/Food/Drinks",
    "Props/Outdoor & Street",
    "Props/Outdoor & Street/Street Furniture",
    "Props/Outdoor & Street/Signage",
    "Props/Outdoor & Street/Garden",
    "Props/Outdoor & Street/Construction",
    "Props/Industrial",
    "Props/Industrial/Machinery",
    "Props/Industrial/Pipes & Ducts",
    "Props/Industrial/Crates & Barrels",
    "Props/Misc",

    "Nature",
    "Nature/Trees",
    "Nature/Bushes & Shrubs",
    "Nature/Grass & Flowers",
    "Nature/Rocks & Boulders",
    "Nature/Cliffs & Formations",
    "Nature/Debris",

    "Architecture",
    "Architecture/Doors",
    "Architecture/Windows",
    "Architecture/Walls & Panels",
    "Architecture/Trim & Molding",
    "Architecture/Stairs",
    "Architecture/Railings & Fences",
    "Architecture/Columns & Beams",
    "Architecture/Modular Kits",

    "Utility",
    "Utility/Decals",
    "Utility/Decals/Dirt & Leaks",
    "Utility/Decals/Damage & Cracks",
    "Utility/Decals/Graffiti & Posters",
    "Utility/Decals/Road Markings",
    "Utility/Imperfections",
    "Utility/Noise & Patterns",
    "Utility/Gradients & LUTs",
    "Utility/Test & Calibration",
)


# ---------------------------------------------------------------------------
# collections.json I/O
# ---------------------------------------------------------------------------

class CatalogError(RuntimeError):
    """collections.json exists but cannot be read as a collection tree."""


def _damaged(reason):
    return CatalogError(
        "The collections file cannot be read (%s): %s - Kura left it and every "
        "asset's collection membership as they are. Restore the file from a "
        "backup (or fix it by hand), then try again."
        % (reason, config.COLLECTIONS_FILE))


def load(strict=False):
    """The collection tree as {"schema_version", "collections": [...]}.

    A missing file reads as an empty tree: a library that has never filed
    anything is a valid library, not a broken one. An unreadable one (I/O
    error, truncated JSON, wrong shape) reads as empty too for display - but
    with strict=True it raises CatalogError instead, and every path that
    writes the tree back or drops memberships based on it passes strict:
    treating damage as "no collections" let Verify Library unfile the whole
    library and an edit overwrite the damaged file (review 02, 2026-11-21).

    Strict means the document as written, not what is left after the lenient
    clean-up: a "collections" key that is missing or not a list (an empty
    object included - falsy is not empty), or any node that the lenient pass
    would drop or re-type (not an object, id / name missing, empty or not a
    string, parent neither a string nor null) is damage, because dropping a
    node drops its id, and every membership naming that id would then look
    stale. What Kura itself has always written stays valid: a list (empty
    or not), with or without schema_version, nodes without parent / order,
    and an order that is not a number - it only sorts, and reads as 0."""
    try:
        with open(config.COLLECTIONS_FILE, encoding="utf-8") as f:
            doc = json.load(f)
    except FileNotFoundError:
        doc = {"collections": []}  # never created: a valid, empty tree
    except (OSError, ValueError) as e:
        if strict:
            raise _damaged(e.__class__.__name__ + ": " + str(e))
        doc = {}
    if strict:
        problem = _structure_error(doc)
        if problem:
            raise _damaged(problem)
    raw_items = doc.get("collections") if isinstance(doc, dict) else None
    if not isinstance(raw_items, list):
        raw_items = []
    items = []
    for raw in raw_items:
        if not isinstance(raw, dict):
            continue
        cid = str(raw.get("id", "")).strip()
        name = str(raw.get("name", "")).strip()
        if not cid or not name:
            continue
        items.append({
            "id": cid,
            "name": name,
            "parent": str(raw.get("parent") or "").strip(),
            "order": _order(raw.get("order")),
        })
    _reparent_orphans(items)
    return {"schema_version": CATALOG_SCHEMA_VERSION, "collections": items}


def _structure_error(doc):
    """Why a parsed collections.json is not a collection tree, or None.
    The strict half of load() - see there for what counts."""
    if not isinstance(doc, dict):
        return "the top level is not an object"
    if "collections" not in doc:
        return 'the "collections" list is missing'
    nodes = doc["collections"]
    if not isinstance(nodes, list):
        return '"collections" is a %s, not a list' % type(nodes).__name__
    for idx, raw in enumerate(nodes):
        if not isinstance(raw, dict):
            return "collection #%d is not an object" % idx
        for key in ("id", "name"):
            value = raw.get(key)
            if not isinstance(value, str) or not value.strip():
                return 'collection #%d has no usable "%s"' % (idx, key)
        parent = raw.get("parent")
        if parent is not None and not isinstance(parent, str):
            return 'collection #%d has a "parent" that is not a string' % idx
    return None


def _order(value):
    """Sort order of a hand-editable node; anything non-numeric reads as 0
    rather than failing load() - and with it the whole tree (2026-11-04)."""
    try:
        return int(value or 0)
    except (TypeError, ValueError):
        return 0


def save(doc):
    os.makedirs(os.path.dirname(config.COLLECTIONS_FILE), exist_ok=True)
    payload = {
        "schema_version": CATALOG_SCHEMA_VERSION,
        "collections": sorted(doc["collections"],
                              key=lambda c: (c["parent"], c["order"],
                                             c["name"].lower())),
    }
    # Write beside, then swap in: opening the real file for "w" truncated it
    # first, so a crash or a full disk mid-write left the tree half there -
    # the damage load(strict=True) now has to refuse (review 02, 2026-11-21).
    # The temporary name is unique per writer (mkstemp creates it
    # exclusively), so two editors saving at once never write into - or
    # clean up - each other's copy, and a failure removes only its own.
    folder, base = os.path.split(config.COLLECTIONS_FILE)
    fd, tmp = tempfile.mkstemp(prefix=base + ".", suffix=".kura-staging",
                               dir=folder)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(payload, f, ensure_ascii=False, indent=2)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, config.COLLECTIONS_FILE)
    except BaseException:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise


def _reparent_orphans(items):
    """Hand-edited file or a half-applied delete can leave a node pointing at
    a parent that is gone. Show it at the top level rather than hiding it."""
    known = {c["id"] for c in items}
    for c in items:
        if c["parent"] and c["parent"] not in known:
            c["parent"] = ""


# ---------------------------------------------------------------------------
# Tree queries
# ---------------------------------------------------------------------------

def _by_id(items):
    return {c["id"]: c for c in items}


def _children_of(items, parent):
    return sorted((c for c in items if c["parent"] == parent),
                  key=lambda c: (c["order"], c["name"].lower()))


def _descendants(items, cid):
    """cid plus everything nested under it."""
    out, queue = [], [cid]
    while queue:
        current = queue.pop()
        out.append(current)
        queue += [c["id"] for c in items if c["parent"] == current]
    return out


def list_tree():
    """Flat collection list for the panel, in display order (depth-first),
    each with its `depth` and member `count`. Also reports how many roots sit
    in no collection at all (F8 Unsorted).

    `count` includes nested collections, so it answers the question the row
    actually poses - "how much would selecting this show me" - and matches
    what members() / "Import this collection" would gather. Without the
    roll-up every branch of a deep tree reads 0, since assets are filed in
    leaves (2026-08-13).

    The roll-up is a union of member sets, not a sum of child counts: an
    entry filed in two branches of one subtree is one asset to the parent,
    as it is to members(). Until 2026-11-04 it was summed and counted twice
    (the C++ panel's LoadCollectionTree still sums - see Docs/review_2026-11)."""
    items = load()["collections"]
    memberships = library.collection_memberships()
    unsorted = library.collection_counts().get("", 0)
    ordered = []

    def walk(parent, depth):
        under = set()
        for c in _children_of(items, parent):
            row = {"id": c["id"], "name": c["name"],
                   "parent": c["parent"], "order": c["order"],
                   "depth": depth, "count": 0}
            ordered.append(row)
            members = set(memberships.get(c["id"], ())) | walk(c["id"],
                                                              depth + 1)
            row["count"] = len(members)
            under |= members
        return under

    walk("", 0)
    return {"ok": True, "collections": ordered, "unsorted": unsorted}


def path_of(cid, items=None):
    """Full display path of a collection - "Surfaces/Wood/Flooring".

    The portable key for a collection (SPEC F7): an id only names a
    collection inside ONE library's collections.json, a path means the same
    thing everywhere - the asset tree's "path = identity" rule (SPEC 5.5)
    applied one layer up. Names cannot contain slashes (_check_name), so
    splitting a path back into segments is unambiguous.

    Returns "" for an unknown id. `items` lets a caller walking many ids
    read collections.json once."""
    if items is None:
        items = load()["collections"]
    byid = _by_id(items)
    parts, seen = [], set()
    current = str(cid)
    while current in byid and current not in seen:
        seen.add(current)  # a hand-edited parent cycle must not hang an export
        parts.append(byid[current]["name"])
        current = byid[current]["parent"]
    return "/".join(reversed(parts))


# ---------------------------------------------------------------------------
# Tree edits
# ---------------------------------------------------------------------------

def _check_name(items, name, parent, ignore_id=None):
    name = str(name).strip()
    if not name:
        raise ValueError("Collection name cannot be empty")
    if "/" in name or "\\" in name:
        raise ValueError("Collection name cannot contain slashes")
    for c in items:
        if (c["parent"] == parent and c["id"] != ignore_id
                and c["name"].lower() == name.lower()):
            raise RuntimeError(
                "A collection named \"%s\" already exists here" % name)
    return name


def _require(items, cid):
    c = _by_id(items).get(str(cid))
    if c is None:
        raise RuntimeError("No such collection: %s" % cid)
    return c


def create(name, parent=""):
    """Add a collection under `parent` ("" = top level). Returns its new id."""
    store.require_library_root()
    doc = load(strict=True)
    items = doc["collections"]
    parent = str(parent or "")
    if parent:
        _require(items, parent)
    name = _check_name(items, name, parent)
    known = {c["id"] for c in items}
    cid = "c_" + uuid.uuid4().hex[:8]
    while cid in known:
        cid = "c_" + uuid.uuid4().hex[:8]
    siblings = _children_of(items, parent)
    items.append({"id": cid, "name": name, "parent": parent,
                  "order": (siblings[-1]["order"] + 1) if siblings else 0})
    save(doc)
    return {"ok": True, "id": cid, "name": name, "parent": parent}


def ensure_paths(paths, dry_run=False):
    """Find-or-create every collection named by a full path (SPEC F7 import).

    Returns {"ids": {path: id}, "created": [path, ...]}. Reads and writes
    collections.json ONCE for the whole batch - a package plants a handful of
    branches at a time, and create() per node would rewrite the file per node.

    Matching is per segment and case-insensitive, the same rule _check_name
    uses to refuse two siblings with one name: an incoming "Surfaces/Wood"
    files into the recipient's existing "surfaces/wood" instead of growing a
    second one beside it. Backslashes are read as separators too, so a
    hand-edited manifest cannot mint a name that _check_name would later
    refuse to rename. dry_run reports what WOULD be created and writes
    nothing - that is what the import dialog counts, so only a real write
    insists on a readable file."""
    store.require_library_root()
    doc = load(strict=not dry_run)
    items = doc["collections"]
    known = {c["id"] for c in items}
    # (parent id, lowercased name) -> id, kept in step with `items` below.
    index = {(c["parent"], c["name"].lower()): c["id"] for c in items}
    ids, created = {}, []

    for raw in paths:
        parent, walked = "", []
        for segment in str(raw).replace("\\", "/").split("/"):
            name = segment.strip()
            if not name:
                continue  # tolerate "a//b" and leading / trailing separators
            walked.append(name)
            key = (parent, name.lower())
            cid = index.get(key)
            if cid is None:
                cid = "c_" + uuid.uuid4().hex[:8]
                while cid in known:
                    cid = "c_" + uuid.uuid4().hex[:8]
                known.add(cid)
                siblings = _children_of(items, parent)
                items.append({
                    "id": cid, "name": name, "parent": parent,
                    "order": (siblings[-1]["order"] + 1) if siblings else 0,
                })
                index[key] = cid
                created.append("/".join(walked))
            parent = cid
        if walked:
            ids[str(raw)] = parent
    if created and not dry_run:
        save(doc)
    return {"ids": ids, "created": created}


def seed_default_tree():
    """Plant DEFAULT_TREE in a library that has never had a collection.

    Guarded on the FILE, not on the tree being empty: a user who deleted every
    collection on purpose has a collections.json saying so, and re-seeding it
    behind their back would make the delete undoable only by deleting twice.

    Idempotent and cheap - one read, one write, and nothing at all on the
    second call. Returns how many nodes were planted (0 = already had a
    tree)."""
    if os.path.exists(config.COLLECTIONS_FILE):
        return {"ok": True, "seeded": False, "created": 0}
    result = ensure_paths(DEFAULT_TREE)
    return {"ok": True, "seeded": True, "created": len(result["created"])}


def rename(cid, name):
    doc = load(strict=True)
    items = doc["collections"]
    c = _require(items, cid)
    c["name"] = _check_name(items, name, c["parent"], ignore_id=c["id"])
    save(doc)
    return {"ok": True, "id": c["id"], "name": c["name"]}


def move(cid, parent="", order=None):
    """Re-parent and/or re-order a collection. Moving a node into its own
    subtree would detach that subtree from the root, so it is refused."""
    doc = load(strict=True)
    items = doc["collections"]
    c = _require(items, cid)
    parent = str(parent or "")
    if parent:
        _require(items, parent)
        if parent in _descendants(items, c["id"]):
            raise RuntimeError("Cannot move a collection into itself")
    if parent != c["parent"]:
        _check_name(items, c["name"], parent, ignore_id=c["id"])
    c["parent"] = parent
    siblings = [s for s in _children_of(items, parent) if s["id"] != c["id"]]
    if order is None:
        c["order"] = (siblings[-1]["order"] + 1) if siblings else 0
    else:
        # Renumber the whole sibling row so the requested slot is exact and
        # no two siblings share an order value.
        siblings.insert(max(0, min(int(order), len(siblings))), c)
        for idx, s in enumerate(siblings):
            s["order"] = idx
    save(doc)
    return {"ok": True, "id": c["id"], "parent": parent}


def delete(cid):
    """Remove a collection and everything nested under it.

    Assets are never touched beyond losing the membership line - deleting a
    collection is a filing decision, not a delete (SPEC F8). Entries left with
    no collection reappear under Unsorted."""
    doc = load(strict=True)
    items = doc["collections"]
    _require(items, cid)
    doomed = set(_descendants(items, str(cid)))
    # Every member meta is read and checked BEFORE the tree changes: one
    # written by a newer Kura cannot be rewritten (SPEC 5.6), and finding
    # that out halfway left the node gone but some members still filed under
    # it (2026-11-04).
    metas = _member_metas(doomed)
    _refuse_newer(metas)
    doc["collections"] = [c for c in items if c["id"] not in doomed]
    save(doc)
    unfiled = _strip_ids(doomed, metas)
    return {"ok": True, "removed": sorted(doomed), "unfiled": unfiled}


def _member_metas(cids):
    """Metas of every entry filed under any of the given ids, read once."""
    metas = []
    seen = set()
    for cid in cids:
        for pkg in library.packages_in_collection(cid):
            if pkg in seen:
                continue
            seen.add(pkg)
            meta = store.load_meta(pkg)
            if meta is not None:
                metas.append(meta)
    return metas


def _refuse_newer(metas):
    errors = [e for e in (store.meta_schema_error(m) for m in metas) if e]
    if errors:
        raise RuntimeError("\n".join(errors))


def _strip_ids(cids, metas):
    """Drop the given collection ids from the given entries' metas."""
    touched = []
    try:
        for meta in metas:
            meta["collections"] = [c for c in meta.get("collections", [])
                                   if c not in cids]
            store.save_meta(meta)
            touched.append(meta)
    finally:
        library.update_entries(touched)  # see assign() for why in a finally
    return sorted(m["package"] for m in touched)


# ---------------------------------------------------------------------------
# Membership
# ---------------------------------------------------------------------------

def assign(packages, cid, add=True):
    """Add or remove entries' membership in one collection (SPEC F8).

    Pure meta edit, batch-friendly, idempotent - re-adding an entry that is
    already filed is a no-op, and an entry may sit in several collections.

    The whole batch is read and checked before the first write: an unknown
    package or one written by a newer Kura (SPEC 5.6) refuses the batch
    instead of leaving it half filed (2026-11-04)."""
    cid = str(cid)
    if add:
        _require(load(strict=True)["collections"], cid)
    metas = []
    for pkg in dict.fromkeys(str(p) for p in packages):
        meta = store.load_meta(pkg)
        if meta is None:
            raise RuntimeError("Not a library entry: %s" % pkg)
        metas.append(meta)
    _refuse_newer(metas)
    changed, touched = [], []
    try:
        for meta in metas:
            pkg = meta["package"]
            current = list(meta.get("collections", []))
            if add and cid not in current:
                current.append(cid)
            elif not add and cid in current:
                current.remove(cid)
            else:
                continue
            meta["collections"] = current
            store.save_meta(meta)
            touched.append(meta)
            changed.append(pkg)
    finally:
        # One transaction for the batch: filing a package import's 661 roots
        # one update_entry() at a time spent seconds in sqlite commits
        # (2026-08-23). In a finally because the metas are already on disk -
        # a write failing later in the batch (disk full, a locked file) must
        # not leave the entries written before it missing from the index.
        library.update_entries(touched)
    return {"ok": True, "changed": changed}


def members(cid):
    """Packages filed under a collection, nested collections included - what
    "Import this collection" has to gather (SPEC F8). Root members only, the
    same set list_tree() counts and the panel's grid shows (review 13): an
    entry whose root flag was unset keeps its ids, but is not a member."""
    items = load()["collections"]
    _require(items, cid)
    out = []
    for one in _descendants(items, str(cid)):
        for pkg in library.packages_in_collection(one, roots_only=True):
            if pkg not in out:
                out.append(pkg)
    return sorted(out)


def prune_unknown():
    """Drop membership ids that no longer name a collection (F4 Verify
    Library). Cheap self-heal for hand-edited files and interrupted deletes.

    Raises CatalogError, before any meta is touched, when the tree it would
    judge against cannot be read - or is missing while entries still carry
    memberships, which is the same loss seen from the other side: the ids in
    the metas are then the only record of the filing, and clearing them made
    restoring collections.json from a backup useless (review 02,
    2026-11-21)."""
    known = {c["id"] for c in load(strict=True)["collections"]}
    metas = list(store.iter_metas())
    if not os.path.isfile(config.COLLECTIONS_FILE) and any(
            m.get("collections") for m in metas):
        raise _damaged("the file is missing, but entries are still filed")
    fixed = []
    for meta in metas:
        if store.meta_schema_error(meta):
            continue  # written by a newer Kura - not ours to rewrite (SPEC 5.6)
        current = meta.get("collections", [])
        kept = [c for c in current if c in known]
        if len(kept) != len(current):
            meta["collections"] = kept
            store.save_meta(meta)
            fixed.append(meta["package"])
    return fixed
