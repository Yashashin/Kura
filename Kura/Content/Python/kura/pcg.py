"""PCG graph support (SPEC F9, 2026-10-07): the library takes PCG logic, never
content.

Two rules live here, both read straight from the AssetRegistry (no graph is
ever loaded):

* the clean rule - every PCG package in an upload's closure may reference
  other PCG assets (subgraphs, an instance's parent), /PCG/ and /Engine/
  content, and nothing else. A mesh set in a Static Mesh Spawner is a SOFT
  reference, so the hard-only closure would never see it: it is checked here
  and blocks, it is not collected.
* plugin requirements - the code modules a PCG package depends on, turned
  into plugin names at ingest (meta "required_plugins") and checked against
  the current project at import, because a PCG asset copied into a project
  without its plugin is just a file nothing can open.
"""
import json

import unreal

from . import config, deps

_ENGINE_CONTENT = ("/Script/", "/Engine/", config.PCG_MOUNT)

_SOFT_HINT = ("PCG graphs go into Kura without content: clear it where it is "
              "set (a Static Mesh Spawner entry, a graph parameter's default, "
              "a subgraph's parameter override), or expose it as a graph "
              "parameter with no default")
_HARD_HINT = ("a PCG graph may only reference other PCG graphs (subgraphs); "
              "remove this reference or expose it as a graph parameter")


def clean_errors(packages):
    """Clean-rule violations among the given closure packages - one line per
    offending reference, naming the package that HOLDS it, because the culprit
    is often a subgraph two levels below the graph the user picked."""
    errors = []
    for pkg in packages:
        if not deps.is_pcg_package(pkg):
            continue
        for dep in deps.hard_dependencies(pkg):
            if dep.startswith("/Game/") and not deps.is_pcg_package(dep):
                errors.append("%s references %s (%s) - %s"
                              % (pkg, dep, deps.primary_class(dep), _HARD_HINT))
        for dep in deps.soft_dependencies(pkg):
            if dep == pkg or dep.startswith(_ENGINE_CONTENT):
                continue
            errors.append("%s references %s (soft reference) - %s"
                          % (pkg, dep, _SOFT_HINT))
    return errors


_owners = {"key": None, "modules": {}}


def _module_owners():
    """{code module: plugin} over the plugins enabled in this editor, read
    from their descriptors. Cached until the enabled set changes - the
    descriptors only change with a restart, and there are a couple hundred."""
    names = sorted(str(n) for n in unreal.PluginBlueprintLibrary.get_enabled_plugin_names())
    if _owners["key"] == names:
        return _owners["modules"]
    modules = {}
    for name in names:
        path = unreal.PluginBlueprintLibrary.get_plugin_descriptor_file_path(name)
        if not path:
            continue
        path = str(unreal.Paths.convert_relative_path_to_full(path))
        try:
            with open(path, encoding="utf-8-sig") as f:
                descriptor = json.load(f)
        except (OSError, ValueError) as e:
            unreal.log_warning("Kura: could not read %s: %s" % (path, e))
            continue
        for module in descriptor.get("Modules") or []:
            if module.get("Name"):
                modules[str(module["Name"])] = name
    _owners["key"], _owners["modules"] = names, modules
    return modules


def _project_modules():
    """Code modules of the current project itself (its .uproject)."""
    path = str(unreal.Paths.convert_relative_path_to_full(
        unreal.Paths.get_project_file_path()))
    try:
        with open(path, encoding="utf-8-sig") as f:
            descriptor = json.load(f)
    except (OSError, ValueError):
        return set()
    return {str(m["Name"]) for m in descriptor.get("Modules") or [] if m.get("Name")}


def required_plugins(pkg):
    """(plugins, errors) for one PCG package.

    Each /Script/ dependency is a code module: an engine module drops out (it
    is in every installation), a plugin's module becomes that plugin's name,
    and a module of this project itself - a custom PCG node written in the
    game's C++ - blocks, because no other project will ever have it. A module
    found nowhere is refused too rather than guessed at."""
    owners = _module_owners()
    project = None
    plugins, errors = set(), []
    script = {d for d in deps.hard_dependencies(pkg) + deps.soft_dependencies(pkg)
              if d.startswith("/Script/")}
    for dep in sorted(script):
        module = dep[len("/Script/"):]
        if module in owners:
            plugins.add(owners[module])
            continue
        if project is None:
            project = _project_modules()
        if module in project:
            errors.append(
                "%s uses code from this project's own %s module (a custom PCG "
                "node?) - other projects will never have it, so it cannot go "
                "into Kura" % (pkg, module))
        elif unreal.find_object(None, dep) is None:
            errors.append("%s uses code module %s, which is not loaded in this "
                          "editor" % (pkg, module))
    return sorted(plugins), errors


def plugins_needed(meta):
    """Plugins an entry needs in the project it is imported into. PCG roots
    always need PCG, whatever the meta says - the class is the safety net
    behind the field (SPEC F9)."""
    need = set(meta.get("required_plugins") or [])
    if meta.get("class") in config.PCG_ROOT_CLASSES:
        need.add(config.PCG_PLUGIN)
    return need


def missing_plugin_errors(needs):
    """needs: {plugin: [packages]}. One error per plugin this project does not
    have enabled. There is no force: unlike a newer-engine asset there is no
    case where the copy would open."""
    if not needs:
        return []
    enabled = {str(n) for n in unreal.PluginBlueprintLibrary.get_enabled_plugin_names()}
    errors = []
    for plugin in sorted(needs):
        if plugin in enabled:
            continue
        packages = sorted(needs[plugin])
        shown = ", ".join(p.rsplit("/", 1)[-1] for p in packages[:5])
        if len(packages) > 5:
            shown += ", and %d more" % (len(packages) - 5)
        errors.append(
            "The %s plugin is not enabled in this project, and %d asset(s) "
            "need it: %s. Enable it in Edit > Plugins, restart the editor, "
            "then import again" % (plugin, len(packages), shown))
    return errors


def project_has(plugin):
    return plugin in {str(n) for n in unreal.PluginBlueprintLibrary.get_enabled_plugin_names()}
