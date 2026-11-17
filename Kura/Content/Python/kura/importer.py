"""Import library entries into the current project (SPEC F3).

The dependency closure is computed live from the library graph at import
time - a master material updated after ingest brings its current deps, not
a frozen snapshot. Files are copied to their stable paths; never rewritten.
"""
import os
import shutil

import unreal

from . import deps, ingest, library, pcg, progress, store
from .ingest import engine_version, package_to_file


_version_tuple = store.version_tuple


def preview(packages, force_version=False):
    """Resolve the closure and compare each entry with the project (F3 1-3).

    Returns {"ok", "errors", "items"}; item: {package, class, selected,
    status: new|same|conflict, size, engine_version}.

    selected: the user picked this entry in the panel, as opposed to the
    closure pulling it in - the preview dialog marks those rows and counts
    the rest as dependencies (2026-08-23).
    """
    store.require_library_root()
    errors = []
    picked = {str(p) for p in packages}
    try:
        closure = library.closure(packages)
    except RuntimeError as e:
        return {"ok": False, "errors": [str(e)], "items": []}

    cur = _version_tuple(engine_version())
    needs = {}  # plugin -> entries that need it in this project (SPEC F9)
    items = []
    with progress.Reporter("Kura: checking the project against the library") as report:
        for idx, pkg in enumerate(closure, 1):
            report(idx, len(closure), pkg)
            meta = store.load_meta(pkg)
            if meta is None:
                errors.append("Library meta missing for %s (run Rescan Library)" % pkg)
                continue
            src = store.tree_file(pkg)
            if not os.path.isfile(src):
                errors.append("Library file missing for %s (run Rescan Library)" % pkg)
                continue
            for plugin in pcg.plugins_needed(meta):
                needs.setdefault(plugin, []).append(pkg)
            if _version_tuple(meta["engine_version"]) > cur and not force_version:
                errors.append(
                    "%s was saved by UE %s but this editor is %s; a newer-engine "
                    "asset cannot be loaded (enable force to override)"
                    % (pkg, meta["engine_version"], engine_version()))
                continue
            dst = package_to_file(pkg)
            if not os.path.isfile(dst):
                status = "new"
            elif store.sha256_of(dst) == meta["sha256"]:
                status = "same"
            else:
                status = "conflict"
            items.append({
                "package": pkg,
                "class": meta["class"],
                "selected": pkg in picked,
                "status": status,
                "size": meta["size"],
                "engine_version": meta["engine_version"],
            })
    # Not part of the force escape hatch: an asset whose plugin is off cannot
    # be opened at all, unlike one a little newer than this engine.
    errors += pcg.missing_plugin_errors(needs)
    if errors:
        return {"ok": False, "errors": errors, "items": []}
    return {"ok": True, "errors": [], "items": items}


def commit(packages, decisions=None, force_version=False):
    """Copy the closure into the project (F3 3-4).

    decisions: {package: "overwrite" | "keep"} for every conflict item.
    "overwrite" replaces the project file with the library version (the old
    project file is NOT archived - the library archive only guards library
    data; the UI must warn about this). Returns {"ok", "copied", "skipped",
    "kept", "errors", "roots"}.
    """
    decisions = dict(decisions or {})
    p = preview(packages, force_version=force_version)
    if not p["ok"]:
        return {"ok": False, "errors": p["errors"], "copied": [], "skipped": [],
                "kept": [], "roots": []}

    conflicts = [i["package"] for i in p["items"] if i["status"] == "conflict"]
    undecided = [c for c in conflicts
                 if decisions.get(c) not in ("overwrite", "keep")]
    if undecided:
        return {"ok": False, "copied": [], "skipped": [], "kept": [], "roots": [],
                "errors": ["No overwrite/keep decision for conflict(s): %s"
                           % undecided]}

    copied, skipped, kept, to_copy = [], [], [], []
    for item in p["items"]:
        pkg = item["package"]
        if item["status"] == "same":
            skipped.append(pkg)
        elif item["status"] == "conflict" and decisions.get(pkg) == "keep":
            kept.append(pkg)
        else:
            to_copy.append(pkg)

    # Stage first: copy every library file next to its destination, so a
    # copy failure (lock, network drive, disk full) aborts before any
    # project file is overwritten - no half-updated dependency closure.
    staged = []  # tmp paths, parallel to to_copy
    try:
        with progress.Reporter("Kura: copying assets into the project") as report:
            for idx, pkg in enumerate(to_copy, 1):
                report(idx, len(to_copy), pkg)
                dst = package_to_file(pkg)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                tmp = dst + ".kura-staging"
                staged.append(tmp)
                shutil.copy2(store.tree_file(pkg), tmp)
    except OSError as e:
        ingest._cleanup_staging(staged, stop_dir=ingest.content_dir())
        return {"ok": False, "copied": [], "skipped": skipped, "kept": kept,
                "roots": [],
                "errors": ["Staging copy failed - the project was not "
                           "modified: %s" % e]}

    # Commit: rename each staged copy into place (same volume, near-atomic).
    try:
        for pkg, tmp in zip(to_copy, staged):
            os.replace(tmp, package_to_file(pkg))
            copied.append(pkg)
    except OSError as e:
        ingest._cleanup_staging(staged, stop_dir=ingest.content_dir())
        return {"ok": False, "copied": copied, "skipped": skipped,
                "kept": kept, "roots": [],
                "errors": ["Import interrupted after %d of %d file(s): %s - "
                           "the project may hold a mixed closure; re-run the "
                           "import" % (len(copied), len(to_copy), e)]}

    # Register exactly what was written, so the Content Browser sees it. A
    # force-rescan of the whole LIB_PREFIX used to sit here and re-read every
    # package header in the project instead (2026-08-09).
    deps.refresh_files([package_to_file(pkg) for pkg in copied])

    roots = [str(pkg) for pkg in packages]
    # Content Browser sync is a Slate call and hard-asserts (uncatchable from
    # Python) in headless runs - only attempt it in a real editor session.
    cmdline = str(unreal.SystemLibrary.get_command_line()).lower()
    if "-run=" not in cmdline and "-unattended" not in cmdline:
        try:
            unreal.EditorAssetLibrary.sync_browser_to_objects(roots)
        except Exception:
            pass

    unreal.log("Kura: import done - copied %d, skipped %d (identical), kept %d"
               % (len(copied), len(skipped), len(kept)))
    return {"ok": True, "errors": [], "copied": copied, "skipped": skipped,
            "kept": kept, "roots": roots}
