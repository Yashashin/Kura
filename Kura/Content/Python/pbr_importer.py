"""PBR Material Importer (Kura SPEC F6) - standalone companion tool.

Turns a folder of downloaded PBR texture sets into ready-to-use Material
Instances: one disk subfolder = one set, textures are matched to material
parameters by filename keyword, imported with the right settings (sRGB /
compression / normal flip) and wired into a new MI under the user's master
material.

Deliberately independent of the kura package: no kura imports, usable in any
project by calling scan()/commit() from Python. The Kura panel drives it via
kura.editor_api's thin pbr_* wrappers. Editor-only (asset import + MI creation
need the editor; headless import crashes in commandlets - Phase 0 finding).
"""
import json
import os
import re

try:
    import unreal
except ImportError:  # offline tests exercise scan()'s matching logic only
    unreal = None

CONFIG_FILE = os.path.join(os.environ.get("APPDATA", ""), "Kura",
                           "pbr_importer.json")
KURA_CONFIG_FILE = os.path.join(os.environ.get("APPDATA", ""), "Kura",
                                "config.json")

IMAGE_EXTS = (".png", ".jpg", ".jpeg", ".tga", ".exr", ".tif", ".tiff",
              ".bmp", ".hdr")

# Filename keyword -> master material parameter + import settings (SPEC F6).
# Keywords are matched against whole filename tokens (split on non-alnum),
# never as substrings - "arm" must not match "farmhouse". Editable via
# pbr_importer.json {"rules": [...]}.
DEFAULT_RULES = [
    {"type": "BaseColor",
     "keywords": ["basecolor", "albedo", "diffuse", "diff", "col", "color"],
     "param": "Base Color Texture", "switch": "Use Base Color Texture",
     "srgb": True, "compression": "default"},
    {"type": "Normal",
     "keywords": ["normal", "nor", "nrm", "normalgl", "normaldx"],
     "param": "Normal Texture", "switch": "Use Normal Texture",
     "srgb": False, "compression": "normalmap"},
    {"type": "ORM",
     "keywords": ["orm", "arm"],
     "param": "ORM Texture", "switch": "Use ORM Texture",
     "srgb": False, "compression": "masks"},
    {"type": "Roughness",
     "keywords": ["roughness", "rough"],
     "param": "Roughness Texture", "switch": "Use Roughness Texture",
     "srgb": False, "compression": "default"},
    {"type": "Metallic",
     "keywords": ["metallic", "metalness", "metal"],
     "param": "Metallic Texture", "switch": "Use Metallic Texture",
     "srgb": False, "compression": "default"},
    {"type": "AO",
     "keywords": ["ao", "ambientocclusion", "occlusion"],
     "param": "AO Texture", "switch": "Use AO Texture",
     "srgb": False, "compression": "default"},
]

# Packed ORM wins over separate maps (SPEC F6); the loose ones are reported.
SEPARATE_TYPES = ("Roughness", "Metallic", "AO")


def _load_config():
    cfg = {"master_material": "", "last_category": "", "flip_green": False,
           "rules": DEFAULT_RULES}
    try:
        with open(CONFIG_FILE, encoding="utf-8-sig") as f:  # BOM-tolerant
            cfg.update(json.load(f))
    except (OSError, ValueError):
        pass
    return cfg


def _save_config(cfg):
    try:
        os.makedirs(os.path.dirname(CONFIG_FILE), exist_ok=True)
        with open(CONFIG_FILE, "w", encoding="utf-8") as f:
            json.dump(cfg, f, ensure_ascii=False, indent=2)
    except OSError:
        pass  # remembering defaults is best-effort


def _lib_prefix():
    """Stable path prefix, read straight from Kura's config.json (a plain
    file read - still no kura import)."""
    try:
        with open(KURA_CONFIG_FILE, encoding="utf-8-sig") as f:  # BOM-tolerant
            return json.load(f).get("lib_prefix", "/Game/_AssetLib")
    except (OSError, ValueError):
        return "/Game/_AssetLib"


def defaults():
    cfg = _load_config()
    return {"master_material": cfg["master_material"],
            "category": cfg["last_category"],
            "flip_green": bool(cfg["flip_green"])}


def _clean_name(name):
    """UE asset names: keep alnum/underscore/hyphen, collapse the rest."""
    return re.sub(r"_+", "_", re.sub(r"[^A-Za-z0-9_\-]", "_", name)).strip("_")


def _clean_category(category):
    # Segments that clean to nothing ("..", ".", "!!!") are dropped rather
    # than joined in as empty path segments ("a/../b" used to give "a//b").
    parts = (_clean_name(p) for p in re.split(r"[\\/]+", str(category).strip()))
    return "/".join(p for p in parts if p)


def _tokens(filename):
    return [t for t in re.split(r"[^a-z0-9]+",
                                os.path.splitext(filename)[0].lower()) if t]


def _match_rule(filename, rules):
    toks = set(_tokens(filename))
    for rule in rules:
        if toks & set(k.lower() for k in rule["keywords"]):
            return rule
    return None


def _asset_exists(path):
    return bool(unreal) and unreal.EditorAssetLibrary.does_asset_exist(path)


def scan(root_dir, category):
    """Dry-run (SPEC F6): group subfolders into sets, match files to rules,
    flag ambiguities and occupied targets. Never touches assets.

    Returns {"ok", "errors", "sets": [{name, mi, textures: [{file, type,
    param, dest}], unmatched, errors}]}; a set with errors is skipped by
    commit().
    """
    root_dir = str(root_dir)
    category = _clean_category(category)
    if not os.path.isdir(root_dir):
        return {"ok": False, "errors": ["Not a folder: %s" % root_dir],
                "sets": []}
    if not category:
        return {"ok": False, "errors": ["Category is required (the subfolder "
                                        "the sets are created under)"],
                "sets": []}

    rules = _load_config()["rules"]
    prefix = _lib_prefix()
    sets = []
    for sub in sorted(os.listdir(root_dir)):
        set_dir = os.path.join(root_dir, sub)
        if not os.path.isdir(set_dir):
            continue
        set_name = _clean_name(sub)
        # One folder per set, MI and its textures together (SPEC 5.4 co-locate
        # convention, 2026-08-10) - the textures belong to this MI alone.
        set_folder = "%s/%s/%s" % (prefix, category, set_name)
        mi_name = "MI_%s" % set_name
        info = {"name": set_name, "mi": "%s/%s" % (set_folder, mi_name),
                "textures": [], "unmatched": [], "errors": []}

        by_type = {}
        for dirpath, _dirs, files in os.walk(set_dir):
            for fname in sorted(files):
                if os.path.splitext(fname)[1].lower() not in IMAGE_EXTS:
                    continue
                rule = _match_rule(fname, rules)
                if rule is None:
                    info["unmatched"].append(fname)
                    continue
                by_type.setdefault(rule["type"], []).append(
                    (os.path.join(dirpath, fname), rule))

        # Ambiguity: two files for one slot (e.g. 2K + 4K, or nor_gl + nor_dx)
        for map_type, matches in sorted(by_type.items()):
            if len(matches) > 1:
                info["errors"].append(
                    "Multiple files match %s: %s - keep one and rescan"
                    % (map_type, ", ".join(os.path.basename(m[0])
                                           for m in matches)))
        # ORM beats separate maps; demote the losers to the unmatched list
        if "ORM" in by_type:
            for sep in SEPARATE_TYPES:
                if sep in by_type:
                    info["unmatched"].extend(
                        os.path.basename(m[0]) for m in by_type.pop(sep))
        if not by_type:
            info["errors"].append("No recognized textures in this folder")

        for map_type, matches in sorted(by_type.items()):
            src, rule = matches[0]
            dest = "T_%s_%s" % (set_name, map_type)
            info["textures"].append({"file": os.path.basename(src),
                                     "path": src, "type": map_type,
                                     "param": rule["param"], "dest": dest})
            if _asset_exists("%s/%s" % (set_folder, dest)):
                info["errors"].append("Target already exists: %s/%s"
                                      % (set_folder, dest))
        if _asset_exists(info["mi"]):
            info["errors"].append("Target already exists: %s" % info["mi"])

        sets.append(info)

    # Folder names are cleaned into asset names, so two folders can land on
    # one set folder ("Wood 01" / "Wood_01") or on none at all ("!!!"). The
    # existence checks above cannot see that - neither target exists yet -
    # and commit() would mix the two sets' textures into one MI folder
    # (2026-11-04).
    seen = {}
    for info in sets:
        seen.setdefault(info["name"], []).append(info)
    for name, group in seen.items():
        for info in group:
            if not name:
                info["errors"].append("The folder name has no usable "
                                      "characters for an asset name - "
                                      "rename the folder")
            elif len(group) > 1:
                info["errors"].append("%d folders become the same set name "
                                      "%s - rename all but one"
                                      % (len(group), name))

    if not sets:
        return {"ok": False, "sets": [],
                "errors": ["No subfolders found - each texture set must be "
                           "one subfolder of the chosen folder"]}
    return {"ok": True, "errors": [], "sets": sets}


COMPRESSION = {}
if unreal:
    COMPRESSION = {
        "default": unreal.TextureCompressionSettings.TC_DEFAULT,
        "normalmap": unreal.TextureCompressionSettings.TC_NORMALMAP,
        "masks": unreal.TextureCompressionSettings.TC_MASKS,
    }


def _import_texture(src, folder, dest, rule, flip_green):
    """Import one texture and save it. Returns (texture, problem): problem is
    None on success, else what went wrong, naming the asset path.

    The save result counts (review 12, 2026-11-21): save_loaded_asset() was
    called and ignored, so a texture that never reached disk still went into
    an MI reported as created - and was gone after an editor restart."""
    task = unreal.AssetImportTask()
    task.filename = src
    task.destination_path = folder
    task.destination_name = dest
    task.automated = True
    task.save = False
    task.replace_existing = False
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    path = "%s/%s" % (folder, dest)
    tex = unreal.load_asset("%s.%s" % (path, dest))
    if not tex:
        return None, "failed to import %s as %s" % (os.path.basename(src), path)
    # Import settings are the easy-to-forget half of the manual workflow.
    tex.set_editor_property("srgb", bool(rule["srgb"]))
    tex.set_editor_property("compression_settings",
                            COMPRESSION.get(str(rule["compression"]).lower(),
                                            COMPRESSION["default"]))
    if rule["type"] == "Normal" and flip_green:
        tex.set_editor_property("flip_green_channel", True)
    if not unreal.EditorAssetLibrary.save_loaded_asset(tex, False):
        return tex, "failed to save %s" % path
    return tex, None


def _remove_created(paths, set_name):
    """Delete the assets THIS attempt created for a set that did not finish,
    newest first (the MI before the textures it references). Returns error
    lines for any that could not be removed.

    Only paths this attempt made are ever passed in: scan() blocks a set
    whose targets already exist, and commit() re-checks each one right
    before creating it, so an asset that was there before is never touched
    (review 11, 2026-11-21). Leaving the made ones behind is what used to
    block the retry: the next Scan found the textures and called the whole
    set taken."""
    problems = []
    for path in reversed(paths):
        try:
            if not _asset_exists(path):
                continue
            if not unreal.EditorAssetLibrary.delete_asset(path) \
                    or _asset_exists(path):
                raise RuntimeError("the editor refused to delete it")
        except Exception as e:  # noqa: BLE001 - report, then try the rest
            problems.append("%s: could not remove %s, created by this attempt "
                            "(%s) - delete it by hand, then import again"
                            % (set_name, path, e))
    return problems


def _exists_quietly(path):
    """_asset_exists() for bookkeeping after a failure: an answer that cannot
    be had counts as "not ours", so it is never deleted."""
    try:
        return _asset_exists(path)
    except Exception:  # noqa: BLE001
        return False


def _build_set(info, set_folder, rules_by_type, master, flip_green,
               can_switch, tools, mel, made):
    """Create one set's textures and MI, each saved. Returns None on success,
    else what failed, naming the target path.

    `made` collects every asset path THIS attempt created, in creation order,
    for the caller to roll back - also one whose creating call then raised:
    each target is checked absent right before its call, and present again
    in a finally, whatever the call did. Until the correction to 11 / 12
    (2026-11-21) only False results were handled; a RuntimeError from an
    import, a property setter or a save escaped commit() with the set half
    made, and the leftovers blocked the retry."""
    textures, target = [], None
    try:
        for item in info["textures"]:
            rule = rules_by_type[item["type"]]
            target = "%s/%s" % (set_folder, item["dest"])
            if _asset_exists(target):
                return ("%s already exists (it appeared after the scan) - "
                        "left as it is" % target)
            try:
                tex, problem = _import_texture(item["path"], set_folder,
                                               item["dest"], rule, flip_green)
            finally:
                if _exists_quietly(target):
                    made.append(target)  # ours: it was not there a moment ago
            if problem:
                return problem
            textures.append((rule, tex))

        target = info["mi"]
        if _asset_exists(target):
            return ("%s already exists (it appeared after the scan) - left as "
                    "it is" % target)
        mi = None
        try:
            mi = tools.create_asset("MI_%s" % info["name"], set_folder,
                                    unreal.MaterialInstanceConstant,
                                    unreal.MaterialInstanceConstantFactoryNew())
            if not mi:
                return "failed to create material instance %s" % target
            mel.set_material_instance_parent(mi, master)
            for rule, tex in textures:
                if can_switch:
                    mel.set_material_instance_static_switch_parameter_value(
                        mi, rule["switch"], True)
                mel.set_material_instance_texture_parameter_value(
                    mi, rule["param"], tex)
            mel.update_material_instance(mi)
            if not unreal.EditorAssetLibrary.save_loaded_asset(mi, False):
                return "failed to save %s" % target
        finally:
            mi = None  # no reference may keep it alive for the rollback
            if _exists_quietly(target):
                made.append(target)
        return None
    except Exception as e:  # noqa: BLE001 - one set must not stop the batch
        return ("the editor raised an error while creating %s (%s: %s)"
                % (target or set_folder, e.__class__.__name__, e))
    finally:
        del textures[:]  # let go of the textures before they are deleted


class _Progress:
    """Slow-task dialog for the import loop (2026-08-02).

    Deliberately a local copy of kura.progress.Reporter: this module must not
    import kura (module docstring). Skipped when unreal is missing (offline
    tests) or in a commandlet, where Slate calls crash past Python's reach.
    """

    def __init__(self, title):
        self._title = str(title)
        self._task = None
        self._enabled = bool(unreal) and not (
            "-run=" in str(unreal.SystemLibrary.get_command_line()).lower()
            or "-unattended" in str(unreal.SystemLibrary.get_command_line()).lower())

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        if self._task is not None:
            self._task.__exit__(None, None, None)
            self._task = None
        return False

    def __call__(self, done, total, label=""):
        if not self._enabled:
            return
        if self._task is None:
            self._task = unreal.ScopedSlowTask(float(max(int(total), 1)),
                                               self._title)
            self._task.__enter__()
            self._task.make_dialog_delayed(0.5)
        self._task.enter_progress_frame(1.0, str(label) or self._title)


def commit(root_dir, category, master_path, flip_green=False):
    """Import every non-blocked set and build its MI (SPEC F6). Re-scans so
    the batch always reflects the disk right now."""
    if not unreal:
        return {"ok": False, "errors": ["Editor only - run inside UE"],
                "created": [], "skipped": []}
    master = unreal.load_asset(str(master_path))
    if not master or not isinstance(master, unreal.MaterialInterface):
        return {"ok": False, "created": [], "skipped": [],
                "errors": ["Master material not found: %s" % master_path]}

    result = scan(root_dir, category)
    if not result["ok"]:
        return {"ok": False, "created": [], "skipped": [],
                "errors": result["errors"]}

    rules_by_type = {r["type"]: r for r in _load_config()["rules"]}
    mel = unreal.MaterialEditingLibrary
    can_switch = hasattr(mel, "set_material_instance_static_switch_parameter_value")
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    prefix = _lib_prefix()
    category = _clean_category(category)
    created, skipped, errors = [], [], []
    if not can_switch:
        errors.append("MaterialEditingLibrary has no static-switch setter in "
                      "this engine version - Use * Texture switches were NOT "
                      "set; enable them by hand in each MI")

    sets = result["sets"]
    with _Progress("PBR Importer: building material instances") as report:
        for idx, info in enumerate(sets, 1):
            report(idx, len(sets), info["name"])
            if info["errors"]:
                skipped.append(info["name"])
                continue
            set_folder = "%s/%s/%s" % (prefix, category, info["name"])

            # A set is all or nothing (reviews 11 / 12): every texture and
            # the MI must be created AND saved, or what this attempt made is
            # removed again and the set is reported skipped with the path
            # that failed - so fixing the source and importing again works.
            made = []
            failure = _build_set(info, set_folder, rules_by_type, master,
                                 flip_green, can_switch, tools, mel, made)
            if failure:
                errors.append("%s: %s - the set was not imported, and what "
                              "this attempt had created for it is removed"
                              % (info["name"], failure))
                errors.extend(_remove_created(made, info["name"]))
                skipped.append(info["name"])
                continue
            created.append(info["mi"])

    cfg = _load_config()
    cfg.update({"master_material": str(master_path),
                "last_category": category, "flip_green": bool(flip_green)})
    _save_config(cfg)

    unreal.log("PBR Importer: created %d MI(s), skipped %d set(s)"
               % (len(created), len(skipped)))
    return {"ok": not errors, "created": created, "skipped": skipped,
            "errors": errors}
