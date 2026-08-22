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
import uuid

from . import config, library, store

# Bumped only if the shape of collections.json itself changes. Independent of
# the per-asset meta schema (SPEC 5.6), which stays at 2 - "collections" is a
# new field with a default, not a breaking change.
CATALOG_SCHEMA_VERSION = 1


# ---------------------------------------------------------------------------
# collections.json I/O
# ---------------------------------------------------------------------------

def load():
    """The collection tree as {"schema_version", "collections": [...]}.

    A missing / unreadable file reads as an empty tree: a library that has
    never filed anything is a valid library, not a broken one."""
    try:
        with open(config.COLLECTIONS_FILE, encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, ValueError):
        doc = {}
    items = []
    for raw in doc.get("collections", []) or []:
        cid = str(raw.get("id", "")).strip()
        name = str(raw.get("name", "")).strip()
        if not cid or not name:
            continue
        items.append({
            "id": cid,
            "name": name,
            "parent": str(raw.get("parent", "")).strip(),
            "order": int(raw.get("order", 0) or 0),
        })
    _reparent_orphans(items)
    return {"schema_version": CATALOG_SCHEMA_VERSION, "collections": items}


def save(doc):
    os.makedirs(os.path.dirname(config.COLLECTIONS_FILE), exist_ok=True)
    payload = {
        "schema_version": CATALOG_SCHEMA_VERSION,
        "collections": sorted(doc["collections"],
                              key=lambda c: (c["parent"], c["order"],
                                             c["name"].lower())),
    }
    with open(config.COLLECTIONS_FILE, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)


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
    leaves (2026-08-13)."""
    items = load()["collections"]
    counts = library.collection_counts()
    ordered = []

    def walk(parent, depth):
        total = 0
        for c in _children_of(items, parent):
            row = {"id": c["id"], "name": c["name"],
                   "parent": c["parent"], "order": c["order"],
                   "depth": depth, "count": counts.get(c["id"], 0)}
            ordered.append(row)
            row["count"] += walk(c["id"], depth + 1)
            total += row["count"]
        return total

    walk("", 0)
    return {"ok": True, "collections": ordered, "unsorted": counts.get("", 0)}


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
    doc = load()
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


def rename(cid, name):
    doc = load()
    items = doc["collections"]
    c = _require(items, cid)
    c["name"] = _check_name(items, name, c["parent"], ignore_id=c["id"])
    save(doc)
    return {"ok": True, "id": c["id"], "name": c["name"]}


def move(cid, parent="", order=None):
    """Re-parent and/or re-order a collection. Moving a node into its own
    subtree would detach that subtree from the root, so it is refused."""
    doc = load()
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
    doc = load()
    items = doc["collections"]
    _require(items, cid)
    doomed = set(_descendants(items, str(cid)))
    doc["collections"] = [c for c in items if c["id"] not in doomed]
    save(doc)
    unfiled = _strip_ids(doomed)
    return {"ok": True, "removed": sorted(doomed), "unfiled": unfiled}


def _strip_ids(cids):
    """Drop the given collection ids from every entry that carries one."""
    touched = []
    for cid in cids:
        for pkg in library.packages_in_collection(cid):
            if pkg not in touched:
                touched.append(pkg)
    for pkg in touched:
        meta = store.load_meta(pkg)
        if meta is None:
            continue
        kept = [c for c in meta.get("collections", []) if c not in cids]
        meta["collections"] = kept
        store.save_meta(meta)
        library.update_entry(meta)
    return sorted(touched)


# ---------------------------------------------------------------------------
# Membership
# ---------------------------------------------------------------------------

def assign(packages, cid, add=True):
    """Add or remove entries' membership in one collection (SPEC F8).

    Pure meta edit, batch-friendly, idempotent - re-adding an entry that is
    already filed is a no-op, and an entry may sit in several collections."""
    cid = str(cid)
    if add:
        _require(load()["collections"], cid)
    changed = []
    for pkg in dict.fromkeys(str(p) for p in packages):
        meta = store.load_meta(pkg)
        if meta is None:
            raise RuntimeError("Not a library entry: %s" % pkg)
        current = list(meta.get("collections", []))
        if add and cid not in current:
            current.append(cid)
        elif not add and cid in current:
            current.remove(cid)
        else:
            continue
        meta["collections"] = current
        store.save_meta(meta)
        library.update_entry(meta)
        changed.append(pkg)
    return {"ok": True, "changed": changed}


def members(cid):
    """Packages filed under a collection, nested collections included - what
    "Import this collection" has to gather (SPEC F8)."""
    items = load()["collections"]
    _require(items, cid)
    out = []
    for one in _descendants(items, str(cid)):
        for pkg in library.packages_in_collection(one):
            if pkg not in out:
                out.append(pkg)
    return sorted(out)


def prune_unknown():
    """Drop membership ids that no longer name a collection (F4 Verify
    Library). Cheap self-heal for hand-edited files and interrupted deletes."""
    known = {c["id"] for c in load()["collections"]}
    fixed = []
    for meta in list(store.iter_metas()):
        if store.meta_schema_error(meta):
            continue  # written by a newer Kura - not ours to rewrite (SPEC 5.6)
        current = meta.get("collections", [])
        kept = [c for c in current if c in known]
        if len(kept) != len(current):
            meta["collections"] = kept
            store.save_meta(meta)
            fixed.append(meta["package"])
    return fixed
