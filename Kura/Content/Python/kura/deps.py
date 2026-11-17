"""Dependency collection & classification (SPEC 7.1)."""
import unreal

from . import config


def _registry():
    return unreal.AssetRegistryHelpers.get_asset_registry()


def _hard_only_options():
    opts = unreal.AssetRegistryDependencyOptions()
    opts.set_editor_property("include_soft_package_references", False)
    opts.set_editor_property("include_hard_package_references", True)
    opts.set_editor_property("include_searchable_names", False)
    opts.set_editor_property("include_soft_management_references", False)
    opts.set_editor_property("include_hard_management_references", False)
    return opts


def _soft_only_options():
    """Soft package references only (SPEC F9). Never followed when collecting -
    a PCG graph is checked against them, so content it points at blocks the
    upload instead of riding along."""
    opts = _hard_only_options()
    opts.set_editor_property("include_soft_package_references", True)
    opts.set_editor_property("include_hard_package_references", False)
    return opts


def hard_dependencies(pkg):
    return [str(d) for d in _registry().get_dependencies(pkg, _hard_only_options()) or []]


def soft_dependencies(pkg):
    return [str(d) for d in _registry().get_dependencies(pkg, _soft_only_options()) or []]


def refresh_files(files):
    """Force-rescan the given package FILES so the registry's dependency graph
    reflects freshly saved assets. Newly created/saved packages otherwise
    report empty dependencies (observed in commandlet runs).

    Files, not folders: a folder rescan is O(assets under the folder), which on
    a real staging project meant re-reading 10,635 package headers on every
    Send to Kura (7.3 s, measured 2026-08-09) no matter how few were selected.
    """
    files = [str(f) for f in files if f]
    if files:
        _registry().scan_files_synchronous(files, True)


def collect(root_packages):
    """Recursively collect hard package dependencies of the given roots.

    Returns {"game": [...], "engine": [...], "other": [...]} of package names
    (roots included). /Script/ class references are ignored; /Engine/ packages
    are recorded but not recursed into (they exist in every installation).

    A PCG package's /PCG/ references count as engine ones (SPEC F9): that is
    the PCG plugin's own content, and a PCG graph cannot be opened without the
    plugin anyway. Decided per edge, so the same /PCG/ package reached from
    anything else still lands in "other" and blocks.
    """
    ar = _registry()
    opts = _hard_only_options()
    seen = set()
    queue = [str(p) for p in root_packages]
    game, engine, other = set(), set(), set()
    while queue:
        pkg = queue.pop()
        if pkg in seen:
            continue
        seen.add(pkg)
        if pkg.startswith("/Script/"):
            continue
        if pkg.startswith("/Engine/"):
            engine.add(pkg)
            continue
        if not pkg.startswith("/Game/"):
            other.add(pkg)
            continue
        game.add(pkg)
        pcg = None  # looked up on the first /PCG/ edge only - nothing else pays
        for dep in ar.get_dependencies(pkg, opts) or []:
            dep = str(dep)
            if dep.startswith(config.PCG_MOUNT):
                if pcg is None:
                    pcg = is_pcg_package(pkg)
                if pcg:
                    engine.add(dep)
                    continue
            queue.append(dep)
    return {"game": sorted(game), "engine": sorted(engine), "other": sorted(other)}


def collect_fresh(root_packages, to_file):
    """collect() over a registry view force-refreshed for the closure ONLY.

    The graph must be re-scanned before collecting (see refresh_files), but the
    closure is exactly what the scan is needed to find. So iterate: refresh what
    is known, re-collect, refresh whatever that turned up, until a round adds
    nothing new. Two or three rounds in practice - roots, their deps, done.

    to_file: package -> project-side .uasset path, or None when there is no
    file to scan; passed in so this module keeps no path helpers of its own.
    """
    roots = [str(p) for p in root_packages]
    refreshed, pending = set(), roots
    while True:
        refresh_files([to_file(p) for p in pending])
        refreshed.update(pending)
        d = collect(roots)
        pending = [p for p in d["game"] if p not in refreshed]
        if not pending:
            return d


def _class_name(asset_data):
    # str(TopLevelAssetPath) is a struct repr in UE 5.7 - read asset_name.
    path = asset_data.get_editor_property("asset_class_path")
    return str(path.get_editor_property("asset_name"))


def class_path(pkg):
    """(module package, class name) of the primary asset in a package, e.g.
    ('/Script/Engine', 'StaticMesh'); ('', 'Unknown') when there is none."""
    for ad in _registry().get_assets_by_package_name(pkg) or []:
        path = ad.get_editor_property("asset_class_path")
        name = str(path.get_editor_property("asset_name"))
        if name != "ObjectRedirector":
            return str(path.get_editor_property("package_name")), name
    return "", "Unknown"


def primary_class(pkg):
    """Class name of the primary asset in a package (e.g. 'StaticMesh')."""
    return class_path(pkg)[1]


def is_pcg_package(pkg):
    """Is the primary asset a PCG one - a graph, an instance, or anything else
    of the PCG module (SPEC F9)?"""
    return class_path(pkg)[0] == config.PCG_SCRIPT_PACKAGE


def is_redirector(pkg):
    for ad in _registry().get_assets_by_package_name(pkg) or []:
        if _class_name(ad) == "ObjectRedirector":
            return True
    return False
