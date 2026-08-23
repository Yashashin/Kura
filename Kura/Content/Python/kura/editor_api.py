"""Bridge functions invoked by the C++ Slate panel (SPEC 7.3).

Business logic stays in Python; the panel calls these via
IPythonScriptPlugin::ExecPythonCommandEx (EvaluateStatement). Every function
returns its JSON result **base64-encoded** and never raises: failures come
back as {"ok": false, "errors": [...]}. Base64 because EvaluateStatement
hands C++ the repr() of the value - a raw JSON string would arrive wrapped
in quotes with \\' escapes, which no JSON parser accepts; base64 survives
repr() untouched.
"""
import base64
import json
import os
import traceback

import unreal

from . import (catalog, config, importer, ingest, library, manage, pack,
               progress, store)


def _json_api(fn):
    def wrapper(*args, **kwargs):
        try:
            config.reload()  # hand-edited config.json takes effect immediately
            result = fn(*args, **kwargs)
        except Exception as e:  # noqa: BLE001 - boundary to C++
            unreal.log_error("Kura: %s failed\n%s" % (fn.__name__,
                                                      traceback.format_exc()))
            result = {"ok": False, "errors": [str(e)]}
        payload = json.dumps(result, ensure_ascii=False).encode("utf-8")
        return base64.b64encode(payload).decode("ascii")
    wrapper.__name__ = fn.__name__
    return wrapper


def _selection():
    packages = [str(d.package_name)
                for d in unreal.EditorUtilityLibrary.get_selected_asset_data()]
    if not packages:
        raise RuntimeError("Nothing is selected in the Content Browser")
    return packages


@_json_api
def ingest_preview():
    """Analyze the Content Browser selection (F1 steps 1-5)."""
    return ingest.analyze(_selection())


@_json_api
def ingest_commit(roots_json, decisions_json="{}", deps_as_roots=False,
                  tags_csv="", collection=""):
    """Apply an ingest previously previewed (F1 steps 6-7).

    roots_json: JSON array of package paths (from the preview result, so a
    changed selection between dialog and commit cannot alter the batch).
    decisions_json: JSON object {package: "update"|"keep"} for conflicts.
    deps_as_roots: mark auto-collected dependencies as roots too (F1 option).
    tags_csv: batch tags unioned into new/updated entries (F1, 2026-07-17).
    collection: optional collection id to file the batch under (F8).
    """
    return ingest.commit(json.loads(roots_json), json.loads(decisions_json),
                         deps_as_roots=bool(deps_as_roots),
                         add_tags=str(tags_csv).split(","),
                         collection=str(collection))


@_json_api
def import_preview(packages_json, force_version=False):
    return importer.preview(json.loads(packages_json),
                            force_version=bool(force_version))


@_json_api
def import_commit(packages_json, decisions_json="{}", force_version=False):
    return importer.commit(json.loads(packages_json),
                           json.loads(decisions_json),
                           force_version=bool(force_version))


@_json_api
def library_status():
    """First-run / library-health probe for the panel (SPEC F5).

    configured: has the user saved config.json at least once (else first run).
    initialized: does the current root already hold a library skeleton + db.
    """
    return {
        "ok": True,
        "configured": config.config_file_exists(),
        "initialized": (os.path.isdir(config.TREE_DIR)
                        and os.path.isfile(config.DB_PATH)),
        "root": config.LIBRARY_ROOT,
        "default_root": config.default_library_root(),
    }


@_json_api
def init_library():
    """Create the library skeleton (folders + empty index) at the current
    root, so a brand-new install is usable immediately (SPEC F5, 2026-07-14).
    Idempotent - a root that already holds a library is adopted, not reset.
    Call after config.json was written with the chosen root.

    A failure here leaves a new user with no library and no way to guess why,
    so the reason is spelled out rather than left as the repr-mangled path
    str(OSError) produces (2026-08-23)."""
    try:
        store.ensure_library_dirs()
        entries = library.rebuild_index()
    except OSError as e:
        raise RuntimeError("Could not create the library at %s: %s"
                           % (config.LIBRARY_ROOT, e.strerror or e))
    return {"ok": True, "root": config.LIBRARY_ROOT, "entries": entries}


@_json_api
def set_roots(packages_json, is_root):
    """Mark/unmark entries as roots (SPEC 3); pure meta edit."""
    return {"ok": True,
            **manage.set_roots(json.loads(packages_json), bool(is_root))}


@_json_api
def set_tags(package, tags_csv):
    meta = manage.set_tags(package, str(tags_csv).split(","))
    return {"ok": True, "tags": meta["tags"]}


@_json_api
def set_notes(package, notes):
    manage.set_notes(package, notes)
    return {"ok": True}


@_json_api
def delete_preview(packages_json):
    """What a delete would take with it: the entries plus the dependencies
    left unused (F4). '[]' asks for the library-wide sweep."""
    result = manage.delete_preview(json.loads(packages_json))
    result["ok"] = True
    return result


@_json_api
def delete_entry(package):
    result = manage.delete_entry(package)
    result["ok"] = True
    return result


@_json_api
def delete_entries(packages_json):
    """Archive a multi-selection; in-batch referrers are deleted first."""
    with progress.Reporter("Kura: archiving entries") as report:
        result = manage.delete_entries(json.loads(packages_json),
                                       progress=report)
    result["ok"] = not result["errors"]
    return result


def _folder_move(fn, *args):
    """Shared tail of the two folder operations (F4).

    Their refusals are expected outcomes, not bugs - the referrer guard is the
    feature - so they are reported as one dialog line each instead of a single
    blob with embedded newlines. Anything unexpected still falls through to
    _json_api's traceback logging."""
    try:
        result = fn(*args)
    except (ValueError, RuntimeError) as e:
        return {"ok": False, "errors": str(e).split("\n")}
    result["ok"] = True
    return result


@_json_api
def move_folder(src_rel, dst_rel):
    """Re-parent a library folder; dst_rel is its new full library-relative
    path (F4). Blocked while anything inside is referenced."""
    return _folder_move(manage.move_folder, src_rel, dst_rel)


@_json_api
def rename_folder(src_rel, new_name):
    """Rename a library folder in place (F4) - the same move, with the new
    name as a single path segment."""
    return _folder_move(manage.rename_folder, src_rel, new_name)


@_json_api
def rescan():
    # manage/pack stay unreal-free, so the slow-task dialog is injected here.
    with progress.Reporter("Kura: verifying library") as report:
        result = manage.rescan(progress=report)
    result["ok"] = True
    return result


@_json_api
def rebuild_index():
    return {"ok": True, "entries": library.rebuild_index()}


@_json_api
def clear_archive():
    result = manage.clear_archive()
    result["ok"] = True
    return result


@_json_api
def list_archive():
    """Recently Deleted (F4): _archive batches, newest first."""
    result = manage.list_archive()
    result["ok"] = True
    return result


@_json_api
def restore_archive(batch):
    """Move a batch back into the library; occupied paths stay blocked."""
    result = manage.restore_archive(batch)
    result["ok"] = True
    return result


@_json_api
def delete_archive_batch(batch):
    """Permanently delete one _archive batch (confirmed in the UI)."""
    result = manage.delete_archive_batch(batch)
    result["ok"] = True
    return result


# --- Collections (SPEC F8) -------------------------------------------------
# The virtual classification layer: every call is a pure meta edit, so the
# panel can refresh straight afterwards without touching the asset tree.

@_json_api
def collection_list():
    """The collection tree in display order + the Unsorted count (F8)."""
    return catalog.list_tree()


@_json_api
def collection_create(name, parent=""):
    return catalog.create(name, parent)


@_json_api
def collection_rename(cid, name):
    return catalog.rename(cid, name)


@_json_api
def collection_move(cid, parent="", order=-1):
    """Re-parent / re-order a collection; order < 0 means "append"."""
    return catalog.move(cid, parent, None if int(order) < 0 else int(order))


@_json_api
def collection_delete(cid):
    """Remove a collection and its children; assets only lose the membership."""
    return catalog.delete(cid)


@_json_api
def collection_assign(packages_json, cid, add=True):
    return catalog.assign(json.loads(packages_json), cid, add=bool(add))


@_json_api
def collection_members(cid):
    """Everything filed under a collection, nested collections included -
    the batch behind "Import this collection" (F8)."""
    return {"ok": True, "packages": catalog.members(cid)}


# --- Package export / import (SPEC F7) ------------------------------------

@_json_api
def package_export(packages_json, out_path, name=""):
    """Export the entries (plus their live closure) into a .kurapack."""
    with progress.Reporter("Kura: writing package") as report:
        return pack.export_package(json.loads(packages_json), out_path,
                                   name=name, progress=report)


@_json_api
def package_preview(pack_path, force_version=False):
    """Compare a .kurapack against the library (F7 import steps 1-2)."""
    with progress.Reporter("Kura: reading package") as report:
        return pack.preview(pack_path, current_engine=ingest.engine_version(),
                            force_version=bool(force_version), progress=report)


@_json_api
def package_commit(pack_path, decisions_json="{}", force_version=False,
                   collection=""):
    """Import a previewed .kurapack into the library (F7 import step 3).

    collection is what the dialog's picker returned: pack.PACK_COLLECTIONS
    ("__package__") to rebuild the package's own filing, "" for Unsorted, or
    an existing collection id to file the roots flat into that one."""
    with progress.Reporter("Kura: importing package") as report:
        return pack.commit(pack_path, json.loads(decisions_json),
                           current_engine=ingest.engine_version(),
                           force_version=bool(force_version), progress=report,
                           collection=str(collection))


# --- PBR Material Importer (SPEC F6) --------------------------------------
# Thin pass-throughs: the tool lives in the standalone pbr_importer module
# (no kura imports there); these exist so the C++ bridge can reach it.

@_json_api
def pbr_defaults():
    import pbr_importer
    return {"ok": True, **pbr_importer.defaults()}


@_json_api
def pbr_scan(root_dir, category):
    import pbr_importer
    return pbr_importer.scan(root_dir, category)


@_json_api
def pbr_commit(root_dir, category, master_path, flip_green=False):
    import pbr_importer
    return pbr_importer.commit(root_dir, category, master_path,
                               bool(flip_green))


@_json_api
def create_project_folders():
    """Create /Game/_AssetLib in the CURRENT project (F1 onboarding,
    2026-07-15). Folders only - Kura never moves assets.

    Just the one folder since 2026-08-10 (Q24): there are no fixed type roots
    to seed any more, and the layout underneath is the user's to design."""
    created = []
    if not unreal.EditorAssetLibrary.does_directory_exist(config.LIB_PREFIX):
        unreal.EditorAssetLibrary.make_directory(config.LIB_PREFIX)
        created.append(config.LIB_PREFIX)
    return {"ok": True, "created": created}


@_json_api
def rerender_thumbnails(packages_json):
    """Re-render library thumbnails for entries present in this project."""
    return ingest.rerender_thumbnails(json.loads(packages_json))


@_json_api
def reload_config():
    """Re-read config.json after the F5 settings dialog saved it."""
    config.reload()
    return {"ok": True, "library_root": config.LIBRARY_ROOT,
            "lib_prefix": config.LIB_PREFIX,
            "material_preview_mesh": config.MATERIAL_PREVIEW_MESH}
