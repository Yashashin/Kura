#pragma once

#include "CoreMinimal.h"
#include "Input/Reply.h"
#include "KuraLibraryModel.h"
#include "Types/SlateEnums.h"
#include "Widgets/SCompoundWidget.h"

class ITableRow;
class STableViewBase;
template <typename ItemType> class SListView;
template <typename ItemType> class STileView;
template <typename ItemType> class STreeView;

/** Kura Library browser panel (SPEC F2): class tabs / collections or folder
 *  tree / tile grid / details.
 *
 *  Since 2026-08-10 (Q23/Q24) the left column has two views: Collections, the
 *  virtual classification layer that is the primary way to organise a library
 *  (SPEC F8), and Folders, the physical asset tree kept as the identity view.
 *  The four tabs filter by CLASS, not by folder - the library's layout no
 *  longer encodes type.
 *
 *  Workflows (ingest/import/delete/rescan) live in KuraActions and are shared
 *  with the Content Browser context menu (2026-07-12). */
class SKuraBrowser : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SKuraBrowser) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SKuraBrowser() override;

	/** Pumps the thumbnail queue and the search box's debounce - the panel is
	 *  the only thing that drives either (2026-08-20). */
	virtual void Tick(const FGeometry& AllottedGeometry, const double CurrentTime,
		const float DeltaTime) override;

private:
	// Data
	TArray<TSharedPtr<FKuraEntry>> AllEntries;
	/** AllEntries by package path. Built once per refresh: the dependency
	 *  walks and the jump-to-entry links used to build their own map of the
	 *  whole library on every call. */
	TMap<FString, TSharedPtr<FKuraEntry>> EntryByPackage;
	TArray<TSharedPtr<FKuraEntry>> FilteredEntries;
	TArray<TSharedPtr<FKuraFolderNode>> FolderRoots;         // whole asset tree
	/** SPEC F8. The WHOLE tree, in every tab: the class tab filters the tiles,
	 *  never the left column (2026-08-14). A tab-scoped tree made the tab mean
	 *  two different things in the two views - a filter in Folders, a whole
	 *  change of classification in Collections. Rows carry a per-tab count
	 *  instead, so an empty tab is visible before it is clicked. */
	TArray<TSharedPtr<FKuraCollectionNode>> CollectionRoots;
	/** Unsorted / Shared Dependencies. Kept out of the tree so they can be
	 *  pinned under it instead of scrolling away below the collections. */
	TArray<TSharedPtr<FKuraCollectionNode>> QueryNodes;
	TArray<TSharedPtr<FString>> AllTags;
	TSet<FString> ActiveTags;
	TSet<FString> ExpandedFolders;
	TSet<FString> ExpandedCollections;
	int32 ActiveTab = 0;    // index into KuraLibrary::ClassTabs; 0 = All (not persisted)
	bool bCollectionsView = true; // left column: Collections (default) / Folders
	FString SelectedFolder;       // library-relative; empty = whole library
	FString SelectedCollection;   // collection id, a query sentinel, or empty = all
	/** Entries the left column holds that only the OTHER class tabs would show
	 *  (ApplyFilter writes it). Turns a blank grid into an explanation. */
	int32 ScopeCountOtherTabs = 0;
	FString SearchText;
	/** Filtering runs once the typing pauses, not per keystroke: at library
	 *  scale a pass costs more than the gap between two keys, so without this
	 *  the box lags a word behind the user (2026-08-20). */
	bool bSearchPending = false;
	double SearchChangedTime = 0.0;
	/** "Show Dependencies" (SPEC Q10, inverted 2026-08-10): off shows only the
	 *  assets the user sent to Kura. What "on" adds depends on the view -
	 *  Collections adds the dependencies those roots exclusively own,
	 *  Folders adds the non-root entries stored in the selected folder. */
	bool bShowDeps = false;
	/** Three library states, not two (2026-08-23): never set up (no
	 *  config.json), set up but the folder is gone, set up and fine. Each gets
	 *  its own empty-state card - collapsing the first two sent a new user who
	 *  declined the setup dialog into the "your library is empty" tutorial,
	 *  which then failed with a message about a folder that never existed. */
	bool bLibraryConfigured = true;
	bool bLibraryRootMissing = false; // configured root gone from disk (2e step 2)
	int32 UnreadableMetaNotified = 0; // damaged-meta count last announced
	bool bIncludeSubfolders = true;
	float TileSize = 150.f;        // config tile_size; slider writes it back
	int32 SortMode = 0;            // sort key: 0 = Name, 1 = Date Added, 2 = Size (config sort_mode)
	bool bSortAscending = true;    // config sort_ascending; A-Z / oldest / smallest first
	TSharedPtr<FKuraEntry> Selected; // last-clicked item (details panel; multi-select aware)
	FDelegateHandle LibraryChangedHandle;

	// Widgets
	TSharedPtr<STreeView<TSharedPtr<FKuraFolderNode>>> FolderTreeView;
	TSharedPtr<STreeView<TSharedPtr<FKuraCollectionNode>>> CollectionTreeView;
	TSharedPtr<SVerticalBox> QueryRowsBox; // pinned Unsorted / Shared rows
	TSharedPtr<STileView<TSharedPtr<FKuraEntry>>> TileView;
	TSharedPtr<SListView<TSharedPtr<FString>>> TagListView;
	/** Dependency / reverse-dependency / engine-dependency lists. Virtualised
	 *  (2026-08-20): a shared master is referenced by every instance built on
	 *  it - 2,707 of them in the reference library - and a widget per row meant
	 *  selecting one froze the panel for seconds and then kept every one of
	 *  those rows in the layout. */
	TArray<TSharedPtr<FString>> DepsItems;
	TArray<TSharedPtr<FString>> ReferrersItems;
	TArray<TSharedPtr<FString>> EngineDepsItems;
	TSharedPtr<SListView<TSharedPtr<FString>>> DepsListView;
	TSharedPtr<SListView<TSharedPtr<FString>>> ReferrersListView;
	TSharedPtr<SListView<TSharedPtr<FString>>> EngineDepsListView;

	// Tag autocomplete on the details tags editor (F2, 2026-07-15): suggests
	// existing library tags for the token after the last comma.
	TSharedPtr<class SMenuAnchor> TagSuggestAnchor;
	TSharedPtr<class SEditableTextBox> TagsEditBox;
	TArray<TSharedPtr<FString>> TagSuggestions;
	TSharedPtr<SListView<TSharedPtr<FString>>> TagSuggestListView;
	void UpdateTagSuggestions(const FText& Text);
	void AcceptTagSuggestion(const FString& Suggestion);

	/** Re-read the library. Incremental by default - only meta files whose
	 *  stat changed are parsed again (2026-08-20); the toolbar's Refresh passes
	 *  true to re-parse everything, which is the escape hatch for a filesystem
	 *  whose timestamps are too coarse to trust. */
	void RefreshFromDisk(bool bForceFullReload = false);
	void ApplyFilter();
	void SetActiveTab(int32 NewTab);
	void RebuildFolderTree();     // tree source / expansion / selection
	void RebuildCollectionTree(); // same, for the F8 tree
	/** Left-column tag filter list, rebuilt from AllEntries. Split out of the
	 *  disk refresh so a tag edit can update it without one (2026-08-23). */
	void RebuildTagList();
	void RebuildQueryRows();      // the pinned Unsorted / Shared rows
	/** Select a saved query (or clear it), keeping the tree selection - the
	 *  other half of the same one-selection model - in step. */
	void SelectQueryNode(const FString& Id);
	/** New collection under ParentId ("" = top level), then select it. */
	void CreateCollectionUnder(const FString& ParentId);
	void SelectEntryByPackage(const FString& Package, bool bShowFolders = false);
	void SelectFolderInTree(const FString& RelPath);
	void RebuildLinkLists();
	TArray<FString> SelectedPackages() const;
	/** Ids of the selected collection and everything nested under it - what a
	 *  collection selection actually filters by (SPEC F8 members). */
	TSet<FString> SelectedCollectionIds() const;
	/** Dependencies exclusively owned by `Roots`: reachable through their
	 *  dependency edges and referenced from nowhere outside the set. Shared
	 *  infrastructure (masters, functions, placeholder textures) therefore
	 *  never shows up as part of "what this asset is made of". */
	void CollectExclusiveDeps(const TArray<TSharedPtr<FKuraEntry>>& Roots,
		TArray<TSharedPtr<FKuraEntry>>& OutDeps) const;
	/** Collection display names of an entry, for the details panel. */
	TArray<FString> CollectionNamesOf(const FKuraEntry& Entry) const;

	// Panel construction
	TSharedRef<SWidget> MakeToolbar();
	TSharedRef<SWidget> MakeTypeTabs();
	TSharedRef<SWidget> MakeLeftColumn();
	TSharedRef<SWidget> MakeTilesColumn();
	TSharedRef<SWidget> MakeDetailsPanel();
	TSharedRef<SWidget> MakeGearMenu();
	TSharedRef<SWidget> MakeViewOptionsMenu(); // sort + tile size (status bar eye)
	TSharedPtr<SWidget> MakeTileContextMenu();
	TSharedPtr<SWidget> MakeFolderContextMenu();     // export (F7), move/rename (F4)
	TSharedPtr<SWidget> MakeCollectionContextMenu(); // new/rename/delete/import (F8)
	/** "Add to Collection" submenu on the tile context menu (F8). */
	void FillAddToCollectionMenu(class FMenuBuilder& Menu, const TArray<FString>& Packages);
	/** "Remove from Collection" submenu: the collections the selection is
	 *  actually filed in (all of them, for a multi-selection). */
	void FillRemoveFromCollectionMenu(class FMenuBuilder& Menu, const TArray<FString>& Packages);
	/** "Move to" submenu on a collection: every collection that could become
	 *  its parent - itself and its own descendants excluded. */
	void FillMoveCollectionMenu(class FMenuBuilder& Menu, const FString& Id);
	/** "Move to" submenu on a folder (F4): the asset tree itself, drilled into
	 *  one level per submenu, with the moving folder (and so its subtree) out. */
	void FillMoveFolderMenu(class FMenuBuilder& Menu, const FString& RelPath);
	void AddMoveFolderTargets(class FMenuBuilder& Menu,
		const TArray<TSharedPtr<FKuraFolderNode>>& Nodes, const FString& RelPath);
	/** Move RelPath under NewParentRel ("" = top of the asset tree), then keep
	 *  it selected where it lands. */
	void MoveFolderTo(const FString& RelPath, const FString& NewParentRel);
	/** Record the post-move selection - runs before the refresh, like
	 *  CreateCollectionUnder. */
	void SelectFolderAfterMove(const FString& NewRel);

	// Row/tile generators
	TSharedRef<ITableRow> MakeFolderRow(TSharedPtr<FKuraFolderNode> Node, const TSharedRef<STableViewBase>& Owner);
	void GetFolderChildren(TSharedPtr<FKuraFolderNode> Node, TArray<TSharedPtr<FKuraFolderNode>>& OutChildren);
	TSharedRef<ITableRow> MakeCollectionRow(TSharedPtr<FKuraCollectionNode> Node, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<SWidget> MakeCollectionRowContent(TSharedPtr<FKuraCollectionNode> Node);
	void GetCollectionChildren(TSharedPtr<FKuraCollectionNode> Node, TArray<TSharedPtr<FKuraCollectionNode>>& OutChildren);
	TSharedRef<ITableRow> MakeEntryTile(TSharedPtr<FKuraEntry> Item, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<ITableRow> MakeTagRow(TSharedPtr<FString> InTag, const TSharedRef<STableViewBase>& Owner);
	/** One package path in a details-panel list; clickable jumps to it. */
	TSharedRef<ITableRow> MakeLinkRow(TSharedPtr<FString> Package,
		const TSharedRef<STableViewBase>& Owner, bool bClickable);
	/** The list widget those rows live in, height-capped so a long list scrolls
	 *  inside the details panel instead of stretching it. */
	TSharedRef<SWidget> MakeLinkList(TSharedPtr<SListView<TSharedPtr<FString>>>& OutView,
		TArray<TSharedPtr<FString>>& Items, bool bClickable);

	// Actions (thin wrappers over KuraActions)
	FReply OnImportClicked();
	FReply OnRefreshClicked();
	void ShowSettingsDialog();
	/** Pick a folder and create (or adopt) the library there. Opened
	 *  automatically on first run, and by hand from the two empty-state cards
	 *  that have no library to show. InitialRoot empty = whatever the config
	 *  points at; a value is passed when reopening after a rejected path, so
	 *  the user does not have to retype it (SPEC F5). */
	void ShowLibrarySetupDialog(const FString& InitialRoot = FString());
	void SaveTileSize();
	void CommitTags(const FString& Package, const FString& TagsCsv);
	void CommitNotes(const FString& Package, const FString& Notes);

	static FString FormatSize(int64 Bytes);
	static FLinearColor ClassColor(const FString& ClassName);
	static FString ClassAbbrev(const FString& ClassName);
};
