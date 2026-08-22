#pragma once

#include "CoreMinimal.h"

/** One library entry, parsed from its Meta json (SPEC 5.2). */
struct FKuraEntry
{
	FString Package;        // /Game/_AssetLib/Master/M_A
	FString RelPath;        // Master/M_A
	FString Folder;         // Master
	FString Name;           // M_A
	FString Class;
	FString Sha256;
	int64 Size = 0;
	FString EngineVersion;
	FString IngestedAt;
	FString UpdatedAt;
	FString SourceProject;
	FString Notes;
	bool bIsRoot = false;
	TArray<FString> Tags;
	TArray<FString> Collections; // virtual classification ids (SPEC F8)
	TArray<FString> HardDeps;
	TArray<FString> EngineDeps;
	TArray<FString> Referrers; // reverse edges, built in-memory over all metas
	FString ThumbPath;         // Meta-mirror PNG (may not exist yet, Phase 2)

	/** Lowercased once at load for the search filter. Lowercasing 10k packages
	 *  per keystroke was the filter's whole cost, and it was paid twice over -
	 *  FString::Contains is case-insensitive by default, so the lowercase copy
	 *  bought nothing (2026-08-20). */
	FString PackageLower;
	FString NotesLower;
	TArray<FString> TagsLower;

	/** The Meta json this was parsed from, with the stat it was parsed at:
	 *  a refresh re-reads only the files that actually changed (2026-08-20). */
	FString MetaFile;
	FDateTime MetaStamp = FDateTime::MinValue();
	int64 MetaSize = 0;
};

/** One node of the on-disk folder tree under the asset tree (SPEC F2). */
struct FKuraFolderNode
{
	FString Name;    // display segment, e.g. "Master"
	FString RelPath; // "" for the synthetic All node, else "Props/Chair01"
	TArray<TSharedPtr<FKuraFolderNode>> Children;
};

/** One node of the virtual classification tree (SPEC F8), read from
 *  Meta/collections.json. Ids are opaque and permanent; names are not. */
struct FKuraCollectionNode
{
	FString Id;
	FString Name;
	int32 Count = 0;   // entries filed here, nested collections included
	/** The same entries split by asset class, so a class tab can report (and
	 *  filter on) what IT would show without a second pass over the library. */
	TMap<FString, int32> ClassCounts;
	bool bUnsorted = false; // the synthetic "no collection yet" queue
	bool bShared = false;   // the synthetic "used by more than one asset" view
	TArray<TSharedPtr<FKuraCollectionNode>> Children;

	/** Saved queries (Unsorted, Shared Dependencies) live in the tree but are
	 *  not collections: nothing can be filed into them, and they cannot be
	 *  renamed, nested or deleted. Membership is computed, never stored. */
	bool IsQuery() const { return bUnsorted || bShared; }

	/** How much of Count the given class tab would actually list. */
	int32 CountForTab(int32 Tab) const;
};

namespace KuraLibrary
{
	/** Browser tab = a CLASS FILTER over ROOT entries since 2026-08-10
	 *  (Q24/Q16-revised); the library's folder layout no longer encodes type.
	 *  A per-class "Others" tab would be permanently empty - nothing outside
	 *  the root whitelist can be a root (SPEC 5.4).
	 *
	 *  All comes FIRST and is the default (2026-08-13): with collections as
	 *  the main view the library reads as one cross-class thing, and All is
	 *  also the management view - the only tab that shows every collection,
	 *  including ones nothing is filed in yet, plus the only place
	 *  dependency-only classes (material functions, parameter collections)
	 *  can be audited. Nothing persists a tab index, so the order is free to
	 *  change; code must find All by its empty class list, never by position. */
	struct FKuraClassTab
	{
		const TCHAR* Label;
		const TCHAR* Classes; // "|"-separated; empty = matches every class
		const TCHAR* Icon;    // FAppStyle brush for the tab button
	};
	/** The single source of truth for the tab strip - the widget is built by
	 *  walking this, so adding or reordering a tab is a one-line edit here. */
	inline const FKuraClassTab ClassTabs[] = {
		// Icons.BrowseContent, not ClassIcon.Default: the latter is a legacy
		// PNG (a pale grey ring) next to three Starship SVGs, which is what
		// made the strip look misaligned (2026-08-14).
		// "All Assets", not "All": the strip is a SUniformGridPanel, so every
		// segment is as wide as the widest ("Static Meshes") and a two-letter
		// label floats in the middle of a lot of nothing. The longer label
		// also answers "all what?" (2026-08-14).
		{ TEXT("All Assets"), TEXT(""), TEXT("Icons.BrowseContent") },
		{ TEXT("Static Meshes"), TEXT("StaticMesh"), TEXT("ClassIcon.StaticMesh") },
		{ TEXT("Materials"), TEXT("Material|MaterialInstanceConstant|MaterialFunction"),
			TEXT("ClassIcon.Material") },
		{ TEXT("Textures"), TEXT("Texture2D|TextureCube|Texture2DArray"),
			TEXT("ClassIcon.Texture2D") },
	};
	constexpr int32 NumClassTabs = UE_ARRAY_COUNT(ClassTabs);

	/** Would this tab show an entry of that class? Always true for All. */
	bool ClassMatchesTab(const FString& ClassName, int32 Tab);

	/** Index of the All tab (the one with no class list). */
	int32 AllTabIndex();

	/** True for the tab that filters nothing - the management view (F8). */
	bool IsAllTab(int32 Tab);

	/** Tab to jump to for a class: its own, or All when it has no named tab. */
	int32 TabIndexForClass(const FString& ClassName);

	/** Selection sentinels for the saved queries (SPEC F8) - not real ids, so
	 *  they can never collide with one (collection ids are "c_" + hex). */
	inline const TCHAR* const UnsortedId = TEXT("@unsorted");
	inline const TCHAR* const SharedId = TEXT("@shared");

	/** An entry is shared infrastructure once more than one library asset
	 *  references it (2026-08-14, F8). A FACT read off the reference graph,
	 *  never a filing decision - which is exactly why it is a query and not a
	 *  collection: uploading a second user of a master would otherwise have to
	 *  silently rewrite someone's classification. */
	constexpr int32 SharedReferrerThreshold = 2;

	/** Fresh-install default library root: <user>\Documents\Kura Library.
	 *  Matches kura/config.py default_library_root(). */
	FString GetDefaultLibraryRoot();

	/** False on a first run (no %APPDATA%/Kura/config.json yet); the panel
	 *  then offers to create the library (SPEC F5). */
	bool IsConfigured();

	/** Library root, resolved the same way as kura/config.py (keep in sync):
	 *  %APPDATA%/Kura/config.json override, then KURA_LIBRARY_ROOT env. */
	FString GetLibraryRoot();

	/** Stable path prefix, default /Game/_AssetLib (config lib_prefix). */
	FString GetLibPrefix();

	/** Material thumbnail preview mesh (config material_preview_mesh, SPEC 7.2). */
	FString GetMaterialPreviewMesh();

	/** Numeric config value (thumb camera, tile_size, thumb_resolution,
	 *  show_dependencies, ... - SPEC F5). */
	double GetConfigNumber(const TCHAR* Key, double Default);

	/** String config value; empty when unset (e.g. ingest_collection). */
	FString GetConfigString(const TCHAR* Key);

	/** Merge the given fields into %APPDATA%/Kura/config.json (F5). */
	bool SaveConfig(const TMap<FString, FString>& StringFields,
		const TMap<FString, double>& NumberFields);

	/** %APPDATA%/Kura/config.json (for the gear menu's Open config.json). */
	FString GetConfigFilePath();

	/** Native folder-picker for the Library Root field (F5 / first-run). Seeds
	 *  at InitialDir; returns false if the user cancelled. */
	bool PickDirectory(const FString& Title, const FString& InitialDir, FString& OutDir);

	/** Asset tree folder on disk: <root>/Content/_AssetLib (SPEC 5.1). */
	FString GetTreeDir();

	/** Parse every Meta/**.json and wire up reverse dependencies.
	 *
	 *  Pass the previous result to make it INCREMENTAL (2026-08-20): entries
	 *  whose meta file has the same timestamp and size are carried over
	 *  untouched, so editing one tag no longer re-reads and re-parses the whole
	 *  library (10,828 files, seconds of it on a spinning disk). The walk still
	 *  happens - that is what notices additions and deletions - but it is one
	 *  stat-carrying pass, not one open per entry. Nullptr = full reload, which
	 *  is what the toolbar's Refresh does. */
	TArray<TSharedPtr<FKuraEntry>> LoadEntries(
		const TArray<TSharedPtr<FKuraEntry>>* Previous = nullptr);

	/** Folder tree read straight from disk: the top-level folders of the asset
	 *  tree and everything under them. One tree for the whole library since
	 *  2026-08-10 - the tabs filter by class, not by folder (Q24). */
	TArray<TSharedPtr<FKuraFolderNode>> LoadFolderTree();

	/** Virtual classification tree from Meta/collections.json, with member
	 *  counts taken from the given entries (SPEC F8). The Unsorted node is
	 *  appended last and only when something is actually unfiled. */
	TArray<TSharedPtr<FKuraCollectionNode>> LoadCollectionTree(
		const TArray<TSharedPtr<FKuraEntry>>& Entries);
}
