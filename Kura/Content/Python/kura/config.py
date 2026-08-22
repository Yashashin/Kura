"""Kura configuration (SPEC F5).

Defaults can be overridden by %APPDATA%/Kura/config.json and by the
KURA_LIBRARY_ROOT environment variable (used by tests). No unreal imports
here - this module must stay usable outside the editor.
"""
import json
import os


def default_library_root():
    """Where a fresh install puts the library: <user>\\Documents\\Kura Library.
    The user picks/confirms this on first run (no config.json yet) and can
    change it later in Settings."""
    home = os.environ.get("USERPROFILE") or os.path.expanduser("~")
    return os.path.join(home, "Documents", "Kura Library")


def config_file_exists():
    """True once the user has saved settings at least once. Absence of this
    file is how the panel detects a first run (SPEC F5)."""
    return os.path.isfile(CONFIG_FILE)


_DEFAULTS = {
    "library_root": default_library_root(),
    "lib_prefix": "/Game/_AssetLib",
    # Empty = render material thumbnails with UE's own default (the sphere),
    # matching the Content Browser. Set a mesh path to preview on that mesh
    # instead (SPEC 7.2).
    "material_preview_mesh": "",
    # Library-PNG camera for material thumbnails (SPEC 7.2). UE's scene
    # thumbnail defaults; tweak here and re-ingest to re-render.
    "thumb_orbit_pitch": -11.25,
    "thumb_orbit_yaw": -157.5,
    "thumb_orbit_zoom": 0.0,
    "thumb_resolution": 256,
    # Browser defaults (SPEC F5, 2026-07-12). tile_size is written back by the
    # panel's tile-size slider; show_dependencies seeds the toolbar toggle
    # (off = only the assets you sent to Kura yourself, SPEC Q10).
    "show_dependencies": 0,
    "tile_size": 150,
    # sort_mode 0 = Name, 1 = Date Added, 2 = Size; sort_ascending applies to
    # whichever key is active (A-Z / oldest / smallest first).
    "sort_mode": 0,
    "sort_ascending": 1,
}

CONFIG_FILE = os.path.join(os.environ.get("APPDATA", ""), "Kura", "config.json")


def _load():
    cfg = dict(_DEFAULTS)
    try:
        with open(CONFIG_FILE, encoding="utf-8") as f:
            cfg.update(json.load(f))
    except (OSError, ValueError):
        pass
    if os.environ.get("KURA_LIBRARY_ROOT"):
        cfg["library_root"] = os.environ["KURA_LIBRARY_ROOT"]
    return cfg


def _apply(cfg):
    """Set the module-level constants from a config dict. Other kura modules
    read these as attributes at call time, so reload() takes effect without
    restarting the editor."""
    g = globals()
    g["_CFG"] = cfg
    g["LIBRARY_ROOT"] = cfg["library_root"]
    # Stable path prefix (SPEC 5.5). Library assets live under this path in
    # EVERY project, so ingest/import are plain file copies with no fixup.
    g["LIB_PREFIX"] = cfg["lib_prefix"]
    # Single asset tree (SPEC 5.1): Library\Content\_AssetLib mirror.
    g["TREE_DIR"] = os.path.join(
        cfg["library_root"], "Content", *cfg["lib_prefix"].strip("/").split("/")[1:])
    # Per-asset meta json + thumbnail PNG mirror, parallel to the tree (SPEC 5.2).
    g["META_DIR"] = os.path.join(cfg["library_root"], "Meta")
    # Virtual classification tree (SPEC F8). Membership itself lives in each
    # asset's meta json; this file only defines the collections.
    g["COLLECTIONS_FILE"] = os.path.join(cfg["library_root"], "Meta",
                                         "collections.json")
    # Safety net for overwritten/deleted entries (SPEC 5.1); cleared manually.
    g["ARCHIVE_DIR"] = os.path.join(cfg["library_root"], "_archive")
    g["DB_PATH"] = os.path.join(cfg["library_root"], "library.db")
    # Preview mesh for material thumbnails in the LIBRARY PNGs only; project
    # assets keep their default sphere thumbnails (SPEC 7.2, 2026-07-12).
    g["MATERIAL_PREVIEW_MESH"] = cfg["material_preview_mesh"]
    g["THUMB_ORBIT_PITCH"] = float(cfg["thumb_orbit_pitch"])
    g["THUMB_ORBIT_YAW"] = float(cfg["thumb_orbit_yaw"])
    g["THUMB_ORBIT_ZOOM"] = float(cfg["thumb_orbit_zoom"])
    g["THUMB_RESOLUTION"] = int(cfg["thumb_resolution"])


def reload():
    """Re-read config.json (after the F5 settings dialog saved it)."""
    _apply(_load())


_apply(_load())

META_SCHEMA_VERSION = 2

# Files under Meta/ that are NOT per-asset meta and must be skipped by the
# mirror walk. Keep in sync with KuraLibrary::LoadEntries.
RESERVED_META_FILES = ("collections.json",)

# Classes the user may actively ingest as roots (SPEC 5.4). Everything else
# may still enter as a collected dependency (Q13).
#
# Folder placement is otherwise unconstrained since 2026-08-10 (Q24): the old
# fixed type roots (StaticMeshes/Materials/Textures/Others) are gone, the
# browser groups by class instead, and organisation lives in collections
# (SPEC F8). Ingest validates only "under LIB_PREFIX" + this whitelist.
ROOT_CLASSES = {
    "StaticMesh",
    "Material",
    "MaterialInstanceConstant",
    "Texture2D",
    "TextureCube",
    "Texture2DArray",
}
