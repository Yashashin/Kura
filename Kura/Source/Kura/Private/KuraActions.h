#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class SWidget;

/** One row of an ingest/import preview dialog (SPEC F1 step 5 / F3 step 3). */
struct FKuraPreviewRow
{
	FString Package;
	FString Class;
	FString Status; // new | same | conflict
	/** The user picked this asset; everything else was collected around it.
	 *  A .kurapack preview sets it on no row at all - the user picked the
	 *  whole file - and the dialog then drops the distinction. */
	bool bSelected = false;
	TArray<FString> Referrers; // ingest conflicts: affected library referrers
	TSharedPtr<FString> Decision; // conflicts only; one of the dialog's options
};

/** Library workflows shared by the browser panel and the Content Browser
 *  context menu (2026-07-12). Dialogs are modal; ModalParent may be null
 *  (Content Browser entry point without an open panel). */
namespace KuraActions
{
	/** Fires after an operation changed the library on disk (ingest, delete,
	 *  rescan, thumbnail re-render); the browser panel refreshes on it. */
	FSimpleMulticastDelegate& OnLibraryChanged();

	/** F1: validate the Content Browser selection, preview, commit. */
	void RunIngestFlow(TSharedPtr<const SWidget> ModalParent);

	/** F3: preview + conflict decisions + commit for the given entries.
	 *  Handles the engine-version guard (prompted force retry). */
	void RunImportFlow(const TArray<FString>& Packages,
		TSharedPtr<const SWidget> ModalParent, bool bForceVersion = false);

	/** F4: confirm, then archive the given entries. In-batch referrers are
	 *  archived before their dependencies (kura.manage.delete_entries).
	 *  Dependencies the delete would leave unused are offered along with it
	 *  (checked by default) so nothing is stranded in the library. */
	void RunDeleteFlow(const TArray<FString>& Packages, TSharedPtr<const SWidget> ModalParent);

	/** F4 Clean Unused Dependencies: library-wide sweep for dependencies
	 *  nobody references any more; same opt-in dialog as the delete cascade. */
	void RunCleanUnusedFlow(TSharedPtr<const SWidget> ModalParent);

	/** F4 level 2: re-parent a library folder (NewParentRel empty = the top
	 *  of the asset tree) / rename it in place. Both are the same operation -
	 *  a folder's path IS its identity - and both are refused while anything
	 *  inside is referenced, because Kura never rewrites .uasset files.
	 *  OnMoved receives the folder's new library-relative path before
	 *  OnLibraryChanged fires, so the panel's refresh can land on it. */
	void RunFolderMoveFlow(const FString& RelPath, const FString& NewParentRel,
		TSharedPtr<const SWidget> ModalParent,
		TFunction<void(const FString&)> OnMoved = nullptr);
	void RunFolderRenameFlow(const FString& RelPath,
		TSharedPtr<const SWidget> ModalParent,
		TFunction<void(const FString&)> OnMoved = nullptr);

	/** SPEC 7.2: re-render library PNGs for entries present in this project. */
	void RunRethumbFlow(const TArray<FString>& Packages, TSharedPtr<const SWidget> ModalParent);

	/** Mark/unmark entries as roots (SPEC 3); pure meta edit, no confirmation. */
	void RunSetRootFlow(const TArray<FString>& Packages, bool bRoot, TSharedPtr<const SWidget> ModalParent);

	/** F1 onboarding: create /Game/_AssetLib in the CURRENT project (folder
	 *  only - Kura never moves assets, and the layout below it is yours). */
	void RunCreateFoldersFlow(TSharedPtr<const SWidget> ModalParent);

	/** F5 setup: point the library at Root and create the skeleton plus an
	 *  empty index there. Idempotent, so a Root that already holds a library
	 *  is adopted rather than reset. Returns false and SAYS WHY on failure -
	 *  this is the one step a brand-new user cannot work around, so the reason
	 *  must not be swallowed (2026-08-23). Broadcasts on success. */
	bool RunCreateLibraryFlow(const FString& Root, TSharedPtr<const SWidget> ModalParent);

	/** F8 collections. All of these are pure meta edits - no asset moves, no
	 *  archiving - so they apply immediately and broadcast OnLibraryChanged.
	 *  ParentId empty = top level. */
	/** AssignPackages: file these into the collection right after creating it
	 *  (the tile menu's "Add to Collection > New Collection").
	 *  OnCreated: the new id, before OnLibraryChanged fires, so a caller can
	 *  arrange for the refresh to land on it (the panel selects it). */
	void RunCollectionCreateFlow(const FString& ParentId,
		TSharedPtr<const SWidget> ModalParent,
		const TArray<FString>& AssignPackages = TArray<FString>(),
		TFunction<void(const FString&)> OnCreated = nullptr);
	void RunCollectionRenameFlow(const FString& Id, const FString& CurrentName,
		TSharedPtr<const SWidget> ModalParent);
	/** Confirmed; removes nested collections too. Assets only lose the filing. */
	void RunCollectionDeleteFlow(const FString& Id, const FString& Name,
		TSharedPtr<const SWidget> ModalParent);
	void RunCollectionAssignFlow(const TArray<FString>& Packages, const FString& Id,
		bool bAdd, TSharedPtr<const SWidget> ModalParent);
	/** Re-parent a collection; empty ParentId moves it to the top level. */
	void RunCollectionMoveFlow(const FString& Id, const FString& ParentId,
		TSharedPtr<const SWidget> ModalParent);
	/** F3 import of everything filed under a collection (nested included). */
	void RunCollectionImportFlow(const FString& Id, TSharedPtr<const SWidget> ModalParent);

	/** F4 Recently Deleted: browse _archive batches; restore, delete one
	 *  batch (confirmed), or clear all. */
	void RunArchiveFlow(TSharedPtr<const SWidget> ModalParent);

	/** F4: full disk-vs-meta reconciliation, report dialog. */
	void RunRescanFlow(TSharedPtr<const SWidget> ModalParent);

	/** F7: export the given entries (plus their live dependency closure) into
	 *  a .kurapack picked in a save dialog. DefaultNameHint seeds the file
	 *  name (folder exports pass the folder name). */
	void RunExportPackageFlow(const TArray<FString>& Packages,
		TSharedPtr<const SWidget> ModalParent,
		const FString& DefaultNameHint = FString());

	/** F7: pick a .kurapack, preview it against the library (ingest-style
	 *  conflict decisions, engine-version guard), then import it. */
	void RunImportPackageFlow(TSharedPtr<const SWidget> ModalParent);

	void Notify(bool bSuccess, const FText& Message);
	void ShowErrorsDialog(const FText& Title, const TArray<FString>& Lines,
		TSharedPtr<const SWidget> ModalParent);
}
