<p align="center">
  <img src="Kura/Resources/Icon128.png" width="96" alt="Kura logo" />
</p>

<h1 align="center">Kura</h1>

<p align="center"><b>A local asset library for Unreal Engine.</b><br/>
Send assets to your library, browse and organise them with thumbnails, tags and your own collections, then import them into any project with one click.</p>

<p align="center">
  <img src="https://img.shields.io/badge/Unreal_Engine-5.7_%7C_5.8-0E1128?logo=unrealengine&logoColor=white" alt="Unreal Engine 5.7 and 5.8" />
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="MIT license" /></a>
  <a href="https://github.com/Yashashin/Kura/releases"><img src="https://img.shields.io/github/v/release/Yashashin/Kura?color=success&label=release" alt="Latest release" /></a>
  <a href="https://ko-fi.com/yukio1024"><img src="https://img.shields.io/badge/Ko--fi-support-FF5E5B?logo=kofi&logoColor=white" alt="Support on Ko-fi" /></a>
</p>

---

![The Kura library panel](Docs/media/panel.png)

## 💡 Why Kura

Every UE artist ends up with a pile of old projects kept alive only because "the good chair is in there somewhere." Kura replaces that with one library folder on disk that every project can reach.

- **Send to Kura** — right-click anything in the Content Browser. It goes in *with its whole dependency chain*: materials, textures, material functions, all of it.
- **Browse it like a library** — thumbnails, folders and tag filters, with search across names, paths, tags and notes.
- **Organise it your way** — **Collections** are your own nested categories, stored as metadata only. File one asset into several of them, rename or reshuffle the whole tree whenever you change your mind — nothing on disk moves, no references break.
- **Import in one click** — the asset lands in your project at the path it always had, references intact. No FBX round-trip, no retexturing, no redirector cleanup, no hunting for the one material that didn't come along.
- **Pack it up and hand it over** — export any selection as a single `.kurapack` file; another Kura library imports it whole, tags and thumbnails included.
- **Nothing gets lost in the library** — deletes and overwrites inside the library go to a restorable archive, never straight off the disk.

## ⚙️ How it works

Every library asset keeps one stable path — `/Game/_AssetLib/...` — identical in the library and in every project. Since `.uasset` files reference each other by path, ingest and import are pure file copies: no reference rewriting, nothing to fix up afterwards.

**Path is identity, hash is version.** Same hash: skipped as a duplicate. Different hash: a conflict dialog, never a silent overwrite. Overwrites in the library go to a restorable archive; an import that replaces a file in your project does not.

## ✨ Features

- **Library browser** — class tabs, a Collections / Folders tree and a thumbnail grid with multi-select. The tab picks *what kind*, the tree picks *which ones*, **Show Dependencies** shows *what they are made of*.
- **Collections** — nested categories held in metadata only: file one asset into several, rename or reorganise them any time, nothing on disk moves. A new library starts with a ready-made tree (Surfaces, Props, Nature, Architecture, Utility) so there is something to file into from your very first upload — rename, delete or nest it into whatever suits your work. Unfiled uploads wait under **Unsorted**.
- **Shared Dependencies** — a built-in view of everything two or more assets reference: masters, material functions, shared textures. Computed from the reference graph, so there is nothing to file and nothing to maintain.
- **Search and tags** — search names, paths, tags and notes, filter by tag, and follow clickable dependency / referenced-by links.
- **Preview before every write** — uploads and imports list each asset as New, Identical or Conflict; conflicts offer Update or Ignore, per item or in bulk. **Mark Dependencies as Roots** promotes collected dependencies to entries of their own.
- **Batch tagging** — tag a whole upload at once. Unchanged assets are skipped and keep their own tags, so a shared master never collects the tag of every pack that uses it.
- **Shared masters stored once** — two material instances that share a master keep one copy of it in the library, and one in the project you import them into.
- **Version guard** — assets saved by a newer engine are blocked from importing into an older one, with the reason named.
- **Safe deletes** — nothing is hard-deleted: entries go to a restorable archive, and dependencies nothing else uses can be swept in the same batch or later.
- **`.kurapack` packages** — export any selection or folder, dependency closure included, as one file another Kura library can import.
- **Portable data layer** — per-asset JSON and thumbnails on disk; the database is only a cache and rebuilds from them.
- **PCG graphs** — PCG Graphs and Graph Instances are library entries with a tab of their own; subgraphs come along as dependencies and are stored once, like a master material. See [PCG graphs](#-pcg-graphs) for the one rule they follow.
- **PBR Material Importer** — point it at a folder of PBR texture sets: textures imported with the right sRGB and compression settings, one Material Instance built per set. Current project only, nothing sent to the library.

**Sending assets to the library** — right-click in the Content Browser, review what comes along, confirm.

![Send to Kura](Docs/media/send-to-kura.gif)

**Importing into a project** — find it in the panel, one click, dependencies and paths intact.

![Importing from the library](Docs/media/import.gif)

## 📥 Installation

| Requirement | |
|---|---|
| Engine | Unreal Engine 5.7 or 5.8 (Launcher build) |
| OS | Windows 10 / 11 |
| Projects | Blueprint-only and C++ projects both work |

1. Download the zip for your engine version from [Releases](https://github.com/Yashashin/Kura/releases) — and `Kura_Starter.kurapack` while you are there, for the quick start below. Plugin binaries are engine-specific; the wrong zip will not load.
2. Extract it into that engine's plugin folder, so that `...\UE_5.7\Engine\Plugins\Marketplace\Kura\Kura.uplugin` exists.
3. Start the editor, enable *Kura* in **Edit → Plugins** (its Python dependencies come along automatically) and restart when prompted.

## 🚀 Quick start

1. **Open the panel** — **Tools → Kura Library**. On first run Kura asks where to keep your library, creates it for you and fills the Collections tree with a starting set of categories.
2. **Import the sample pack** — load `Kura_Starter.kurapack` with **⋮ → Library → Import Package** (the empty-library screen offers the same button). You get a filled library to click around in before you commit anything of your own.
3. **Import an asset into your project** — right-click a tile → **Import**. The asset and its whole dependency chain land at the paths they had in the library.
4. **Send in assets of your own** — **⋮ → This Project → Create _AssetLib Folder** adds `/Game/_AssetLib` to the project. Move what you want to keep into it in the Content Browser, select it, right-click → **Send to Kura**, then review the preview, add a tag or two, pick a collection, confirm.

A few things worth knowing early:

- Library entries can be Static Meshes, Materials, Material Instances, Textures and PCG Graphs. Whatever they reference — material functions, curves, textures, subgraphs — comes along on its own.
- **Folders are storage; collections are organisation.** A library folder is also the asset's path, so it is fixed at upload time — **keep categories out of it**, since categories are exactly what you change your mind about. A layout that works: one folder per asset with its own textures, plus a `Shared/` folder for anything reused.
- Never rename files inside the library folder by hand — that breaks references by definition. Rename in a project, re-upload, delete the old entry.

## 📦 Sharing assets: `.kurapack`

Right-click tiles → **Export as Package**, or a folder in the tree → **Export Folder as Package**. The dependency closure comes along, so a package is never a broken subset.

The result is one self-contained file — assets, metadata, tags and thumbnails — that another Kura library imports with **⋮ → Library → Import Package**. Identical entries are skipped, changed ones prompt, and every file is hash-verified. Packages carry their collections too: the import dialog lets you keep them, file everything into one collection of your own, or skip filing entirely.

### More assets!

Kura ships empty, so I put my own material library up for anyone to take: **[2,680 CC0 materials across 33 packs](Docs/packs.md)** from [Poly Haven](https://polyhaven.com) and [ambientCG](https://ambientcg.com), plus the sample pack attached to every [release](https://github.com/Yashashin/Kura/releases). Import the sample pack first and every pack after it will skip the assets they share.

## 🌲 PCG graphs

A PCG Graph or Graph Instance goes into the library as **pure logic**: the graph and its subgraphs, with no meshes, materials or Blueprints attached.

- **What comes along:** subgraphs and a Graph Instance's parent graph. PCG's built-in content (`/PCG/...`) and engine content are recorded but not copied — every project with PCG enabled already has them.
- **What blocks an upload:** a graph that points at project content — a mesh set on a Static Mesh Spawner, a Graph Parameter with a default value, a parameter override on a subgraph node, a Spawn Actor pointing at a Blueprint. The message names the graph holding the reference, so one buried three subgraphs deep is still findable. Clear the reference, or expose it as a Graph Parameter with no default.
- **How to use an imported graph:** create a Graph Instance of it in your project, *outside* `_AssetLib`, and set the meshes there through Graph Parameters. Editing the imported graph's Spawner nodes works too, but you are then editing your project's copy of the library version — importing that graph again, or another graph sharing one of its subgraphs, raises a conflict, and choosing the library version overwrites your settings.
- **The PCG plugin has to be on** in the project you import into; otherwise the import is refused and tells you to enable it in **Edit → Plugins**. Kura itself does not need PCG and never enables it for you. Graphs using nodes from another plugin record that plugin and check for it the same way; graphs using custom nodes from a project's own C++ module cannot be uploaded.
- PCG graphs have no thumbnail — tiles show a `PCG` / `PCGI` type block instead.

## 🛠️ Configuration

Most of this lives in the panel (**⋮ → Settings**); the file behind it is `%APPDATA%\Kura\config.json`, hot-reloaded on every operation.

| Key | Default | Meaning |
|---|---|---|
| `library_root` | `<user>\Documents\Kura Library` | Where the library lives |
| `lib_prefix` | `/Game/_AssetLib` | Stable path prefix — set once, do not change on a populated library |
| `material_preview_mesh` | *(empty)* | Empty = UE's default material sphere; set a mesh path (e.g. `/Engine/EngineMeshes/SM_MatPreviewMesh_01`) to preview materials on that instead |
| `thumb_orbit_pitch` | `-11.25` | Thumbnail camera pitch |
| `thumb_orbit_yaw` | `-157.5` | Thumbnail camera yaw |
| `thumb_orbit_zoom` | `0` | Thumbnail camera distance offset (negative = closer) |
| `thumb_resolution` | `256` | Thumbnail PNG size, 32–2048 |

Thumbnail settings apply from then on; the ones already in your library keep the look they were made with until you refresh them (tile right-click → **Refresh Thumbnail**). The three orbit values steer **material** thumbnails only, and only while a preview mesh is set — without one the Settings dialog greys them out. Static meshes keep the angle you gave them in the Content Browser.

The panel keeps its own state in the same file. The first two are also in **Settings**; the rest just remember what you last did:

| Key | Default | Meaning |
|---|---|---|
| `show_dependencies` | `0` | Start with dependency-only entries hidden |
| `tile_size` | `150` | Tile size, 90–260, saved from the View Options slider |
| `sort_mode` | `0` | 0 = Name, 1 = Date Added, 2 = Size |
| `sort_ascending` | `1` | 1 = ascending (A-Z / oldest / smallest first), 0 = descending |
| `ingest_deps_as_roots` | `0` | Remembers the "Mark Dependencies as Roots" checkbox |
| `ingest_collection` | *(empty)* | Remembers the collection last picked in the upload preview. Uploads of a single kind keep their own (`ingest_collection.static_meshes`, `.materials`, …), so a mesh upload does not offer the collection you last filed textures into |
| `collections_view` | `1` | 1 = left column shows Collections, 0 = Folders |

## 📂 Where Kura keeps things

| Path | Contents |
|---|---|
| `<library root>\Content\_AssetLib\` | the assets themselves — a mirror of the `/Game/_AssetLib` tree (the folder name follows `lib_prefix`) |
| `<library root>\Meta\` | per-asset JSON + PNG thumbnail, parallel to the tree — the source of truth |
| `<library root>\Meta\collections.json` | the collection tree (names and nesting); which assets are in them lives in each asset's own JSON |
| `<library root>\library.db` | SQLite index; a cache, rebuildable any time with **Verify Library** |
| `<library root>\_archive\` | overwritten and deleted versions; it only grows, until you clear entries for good from **Recently Deleted** |
| `%APPDATA%\Kura\config.json` | settings |
| `%APPDATA%\Kura\pbr_importer.json` | PBR importer keyword table — editable |

Back up the library root and you have backed up everything that matters — the two `%APPDATA%` files only hold preferences.

Kura writes into your *project* only when you ask: **Import**, **Create _AssetLib Folder**, the PBR Material Importer. Preparing an upload adds two small ones — Fix Up Redirectors over `/Game/_AssetLib`, and saving any selected asset with unsaved edits, so what lands in the library is what is on disk. Nothing else: browsing never writes, and the thumbnail camera override is undone, dirty flag included, the moment the PNG exists.

## 🔄 Updating and uninstalling

- **Update:** close the editor, then replace the `Kura` plugin folder with the contents of the new zip, wherever you installed it. A newer Kura reads your existing library as it is — there is nothing to migrate (next section).
- **Uninstall:** delete the `Kura` plugin folder. Your library folder and `%APPDATA%\Kura` are left alone; remove them by hand if you want the data gone too.

## 🎮 Engine versions & library format

- **Supported engines: UE 5.7 and 5.8.** The policy is the current engine plus the previous one. Each release ships one zip per engine version — plugin binaries are never interchangeable between engine versions.
- **Assets only travel upward.** The sample pack and the asset packs are built with UE 5.7, so they import into 5.7 and 5.8 alike. That direction is an engine rule rather than a Kura one: a `.uasset` records the version that saved it, an older engine refuses to load a newer one, and Unreal has no "save as an older version". Kura's version guard catches this at import time and tells you why, instead of leaving you with an editor that fails to open the file.
- **Working across both versions?** The engine that uploads an asset sets that asset's floor — send it from 5.8 and only 5.8 can import it back, send it from 5.7 and either engine can. A mixed library is fine: the guard is per asset, and an import is blocked only when something in its dependency closure is too new. The simple rule is to upload from the older engine.
- **Library format:** each asset's metadata carries a `schema_version` (currently 2). New fields always arrive with a default and existing ones are never renamed or repurposed, so a newer Kura reads an older library as it is and upgrades entries one at a time as it rewrites them — never a bulk rewrite of a library it did not create. A `.kurapack` built by a newer Kura than yours is refused on import, with a message telling you to update. The same holds for a whole library: if you open one written by a newer Kura, it stays readable — browse it, import from it — but any change to it is refused rather than risk dropping fields this build does not know about. *Verify Library* lists the entries this applies to.

## ⚠️ Known limitations

- **Windows only.** There is no macOS or Linux build.
- **Uploadable types:** Static Mesh, Material, Material Instance, Texture (2D / Cube / Array), PCG Graph, PCG Graph Instance. Skeletal Meshes, Blueprints, Niagara systems, levels and animation assets cannot be library entries. Dependency-only classes that a supported asset references (material functions, curves, …) come along fine and show up under the **All Assets** tab.
- **Content from other plugins is blocked.** Only `/Game` (copied into the library) and `/Engine` (recorded in metadata, not copied) are supported. Anything depending on `/Fab/` or another plugin mount — Fab / Quixel Bridge content, plugins that ship content — is rejected with the offending references listed. Move what you need into `/Game` first. The one exception is the PCG plugin's own content, which PCG graphs may reference.
- **One library at a time.** Kura points at a single `library_root`; switching means changing the setting.
- **No multi-user locking.** A library on a NAS or a synced folder can be shared, but Kura assumes a single writer — concurrent uploads from two machines are not protected against.
- **Not version control.** `_archive` keeps whatever you overwrite or delete **in the library**, one batch per action. That is a safety net you can restore from, not a history you can navigate.
- **Renaming inside the library folder breaks it by design** — the path *is* the identity, so never rearrange the library in Explorer. Kura can rename or move a library folder for you (right-click it in the **Folders** tree), but only while nothing inside it is referenced by another entry — for an asset stored next to its own textures, that means almost never. To relocate something that *is* referenced: move it in a project, send it to Kura again, then delete the old entries. To re-file assets for browsing, use collections; they change nothing on disk.

## ❓ FAQ

**Why does everything have to live under `/Game/_AssetLib`?**
Because that is what makes import a plain file copy. A package path is baked into every `.uasset` that references it, so holding one path — the same in the library as in every project — is what lets Kura move files in and out without rewriting references or leaving redirectors behind. You can rename the prefix (`lib_prefix`) before you put anything in the library, but not after: it is the identity of everything already in there.

**Can I upload assets I bought on Fab or the Marketplace?**
If they live under `/Game`, Kura will take them — it does not inspect licences. Whether you may keep them in a personal library, and whether you may put them in a `.kurapack` you hand to someone else, is between you and that asset's licence. Most store licences allow reuse across your own projects; essentially none allow redistribution. Content that lives in a plugin mount is blocked outright (see limitations).

**Can I move my library to another drive, or to another machine?**
Move the folder, then point **⋮ → Settings → Library Root** at where it went. Nothing inside records an absolute path — assets, metadata and the index are all keyed by their `/Game/_AssetLib` path — so the same library works from any drive, and copying the library root is a complete backup.

**Two of my projects have a different `M_Wood` at the same path. Now what?**
Kura reports a version conflict, because the path is the identity. If they really are different materials, rename one of them in its project and upload again — you'll end up with both. If one is simply the newer version, choose *Update*; the old one goes to `_archive`.

## ❤️ Support

Bugs and feature requests are welcome in [Issues](https://github.com/Yashashin/Kura/issues).

Kura is free, and there is no paid version planned. If it saves you time and you feel like it, you can [buy me a coffee](https://ko-fi.com/yukio1024) — knowing that this is useful to someone is what keeps me building it.

## 📄 License

[MIT](LICENSE) — © 2026 Yukio.
