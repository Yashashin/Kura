#include "KuraActions.h"

#include "DesktopPlatformModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Framework/Application/SlateApplication.h"
#include "IDesktopPlatform.h"
#include "Misc/Paths.h"
#include "Framework/Notifications/NotificationManager.h"
#include "KuraLibraryModel.h"
#include "KuraPython.h"
#include "KuraStyle.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/MessageDialog.h"
#if UE_VERSION_OLDER_THAN(5, 8, 0)
#include "SSearchableComboBox.h" // ToolWidgets
#else
// 5.8 moved the widget into Slate; the ToolWidgets header still forwards there
// but is deprecated (C4996) and goes away in the next release.
#include "Widgets/Input/SSearchableComboBox.h"
#endif
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Kura"

namespace
{
	/** Config key the ingest collection picker remembers its last choice
	 *  under - one per class tab, so the collection a batch of materials just
	 *  went into is not what the PCG graph sent next gets filed under
	 *  (2026-10-08). Keyed by the roots the user picked; a batch whose picks
	 *  span tabs keeps the shared key, as every batch did before. A tab that
	 *  has never been used starts at None - falling back to the shared key
	 *  would bring the old cross-class choice straight back. */
	FString IngestCollectionKey(const TArray<TSharedPtr<FKuraPreviewRow>>& Rows)
	{
		const FString Shared = TEXT("ingest_collection");
		int32 Tab = INDEX_NONE;
		for (const TSharedPtr<FKuraPreviewRow>& Row : Rows)
		{
			if (!Row->bSelected)
			{
				continue;
			}
			const int32 RowTab = KuraLibrary::TabIndexForClass(Row->Class);
			if (Tab != INDEX_NONE && RowTab != Tab)
			{
				return Shared;
			}
			Tab = RowTab;
		}
		if (Tab == INDEX_NONE || KuraLibrary::IsAllTab(Tab))
		{
			return Shared;
		}
		// "Static Meshes" -> ingest_collection.static_meshes
		return Shared + TEXT(".")
			+ FString(KuraLibrary::ClassTabs[Tab].Label).ToLower().Replace(TEXT(" "), TEXT("_"));
	}

	/** A conflict resolution choice: the wire key sent to Python (stable) and
	 *  the label shown to the user (Title Case). Kept separate so we can
	 *  reword the UI without touching the Python protocol. */
	struct FKuraDecisionOption
	{
		FString Key;   // "update" | "keep" | "overwrite"
		FText Label;   // "Update" | "Ignore" | "Overwrite"
	};

	FString FormatBytes(double Bytes)
	{
		return Bytes >= 1024.0 * 1024.0
			? FString::Printf(TEXT("%.1f MB"), Bytes / (1024.0 * 1024.0))
			: FString::Printf(TEXT("%.0f KB"), FMath::Max(Bytes, 1.0) / 1024.0);
	}

	/** "20260715-171200[_n]" -> "2026-07-15 17:12:00"; other names pass through. */
	FString FormatBatchName(const FString& Batch)
	{
		if (Batch.Len() >= 15 && Batch[8] == TEXT('-'))
		{
			bool bDigits = true;
			for (int32 Idx = 0; bDigits && Idx < 15; ++Idx)
			{
				bDigits = Idx == 8 || FChar::IsDigit(Batch[Idx]);
			}
			if (bDigits)
			{
				return FString::Printf(TEXT("%s-%s-%s %s:%s:%s%s"),
					*Batch.Mid(0, 4), *Batch.Mid(4, 2), *Batch.Mid(6, 2),
					*Batch.Mid(9, 2), *Batch.Mid(11, 2), *Batch.Mid(13, 2),
					*Batch.Mid(15));
			}
		}
		return Batch;
	}

	TArray<FString> ErrorsFromResult(const TSharedPtr<FJsonObject>& Result)
	{
		TArray<FString> Out;
		if (Result.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (Result->TryGetArrayField(TEXT("errors"), Arr))
			{
				for (const TSharedPtr<FJsonValue>& V : *Arr)
				{
					Out.Add(V->AsString());
				}
			}
		}
		if (Out.Num() == 0)
		{
			Out.Add(TEXT("Unknown error - see Output Log for details."));
		}
		return Out;
	}

	TArray<TSharedPtr<FKuraPreviewRow>> RowsFromPreview(const TSharedPtr<FJsonObject>& Preview,
		const FString& DefaultDecision)
	{
		TArray<TSharedPtr<FKuraPreviewRow>> Rows;
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Preview->TryGetArrayField(TEXT("items"), Items))
		{
			return Rows;
		}
		for (const TSharedPtr<FJsonValue>& V : *Items)
		{
			const TSharedPtr<FJsonObject> Item = V->AsObject();
			if (!Item.IsValid())
			{
				continue;
			}
			const TSharedPtr<FKuraPreviewRow> RowData = MakeShared<FKuraPreviewRow>();
			Item->TryGetStringField(TEXT("package"), RowData->Package);
			Item->TryGetStringField(TEXT("class"), RowData->Class);
			Item->TryGetStringField(TEXT("status"), RowData->Status);
			Item->TryGetBoolField(TEXT("selected"), RowData->bSelected);
			const TArray<TSharedPtr<FJsonValue>>* Refs = nullptr;
			if (Item->TryGetArrayField(TEXT("referrers"), Refs))
			{
				for (const TSharedPtr<FJsonValue>& R : *Refs)
				{
					RowData->Referrers.Add(R->AsString());
				}
			}
			if (RowData->Status == TEXT("conflict"))
			{
				RowData->Decision = MakeShared<FString>(DefaultDecision);
			}
			Rows.Add(RowData);
		}
		return Rows;
	}

	FText StatusBadgeText(const FString& Status)
	{
		if (Status == TEXT("new"))
		{
			return LOCTEXT("BadgeNew", "NEW");
		}
		return Status == TEXT("same") ? LOCTEXT("BadgeSkip", "SKIP") : LOCTEXT("BadgeConflict", "CONFLICT");
	}

	FLinearColor StatusBadgeColor(const FString& Status)
	{
		// Guideline tokens: success = goes in, neutral = nothing to do,
		// warning = needs a decision.
		if (Status == TEXT("new"))
		{
			return KuraStyle::Success().Text;
		}
		return Status == TEXT("same")
			? KuraStyle::TextSecondary()
			: KuraStyle::Warning().Text;
	}

	/** Colored, Title-Case count segment for the summary line ("21 New"). Zero
	 *  counts are subdued so the eye lands on what actually needs a decision. */
	TSharedRef<SWidget> CountSegment(int32 Count, const FText& Noun, const FLinearColor& Color)
	{
		return SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("CountSeg", "{0} {1}"), FText::AsNumber(Count), Noun))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			.ColorAndOpacity(Count > 0 ? FSlateColor(Color) : FSlateColor::UseSubduedForeground());
	}

	/** The asset cell of a preview row: name first, then class and folder
	 *  trailing it, dimmed. Name-first because a package path is long and the
	 *  dialog is not wide - trailing the path lets it ellipsize where that
	 *  costs nothing, and keeps the names in one readable column instead of
	 *  wrapping every row onto two lines.
	 *  bBold marks an asset the user picked: in a batch whose closure runs to
	 *  hundreds of entries, the handful actually selected have to stand out. */
	TSharedRef<SWidget> MakePreviewLabel(const FString& Package, const FString& Class,
		bool bBold)
	{
		FString Folder, Leaf;
		if (!Package.Split(TEXT("/"), &Folder, &Leaf, ESearchCase::CaseSensitive,
			ESearchDir::FromEnd))
		{
			Leaf = Package;
		}
		FString Trailing = Class;
		if (!Folder.IsEmpty())
		{
			Trailing += TEXT("   ") + Folder;
		}
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Leaf))
				.Font(FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", 9))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			.Padding(8.f, 0.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Trailing))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			];
	}

	/** One entry of the collection list, flattened for menus and combo boxes
	 *  (SPEC F8). An empty Id is the "none" choice. */
	struct FKuraCollectionOption
	{
		FString Id;
		FString Label; // leaf name, indented by nesting depth
		/** "Surfaces / Wood / Flooring". Unique (sibling names are), so it can
		 *  identify the option, and the right thing to show once the list is
		 *  closed - an indented bare leaf reads as a stray space, and two
		 *  collections may well both be called "Textures". */
		FString Path;
	};

	/** collection_list() as a flat, display-ordered array. */
	TArray<TSharedPtr<FKuraCollectionOption>> FetchCollections()
	{
		TArray<TSharedPtr<FKuraCollectionOption>> Out;
		TArray<FString> Ancestry; // names of the open path, indexed by depth
		const TSharedPtr<FJsonObject> Result = KuraPython::Call(TEXT("collection_list()"));
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("collections"), Items))
		{
			return Out;
		}
		for (const TSharedPtr<FJsonValue>& V : *Items)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!V->TryGetObject(Obj))
			{
				continue;
			}
			FString Id, Name;
			double Depth = 0.0;
			(*Obj)->TryGetStringField(TEXT("id"), Id);
			(*Obj)->TryGetStringField(TEXT("name"), Name);
			(*Obj)->TryGetNumberField(TEXT("depth"), Depth);
			if (Id.IsEmpty())
			{
				continue;
			}
			// collection_list() is depth-first in display order, so the open
			// path is just "truncate to this depth, then append".
			const int32 Level = FMath::Max(0, (int32)Depth);
			Ancestry.SetNum(Level);
			Ancestry.Add(Name);

			const TSharedPtr<FKuraCollectionOption> Opt = MakeShared<FKuraCollectionOption>();
			Opt->Id = Id;
			Opt->Label = FString::ChrN(Level * 4, TEXT(' ')) + Name;
			Opt->Path = FString::Join(Ancestry, TEXT(" / "));
			Out.Add(Opt);
		}
		return Out;
	}

	/** One row of a collection picker: the ancestry dimmed, the collection
	 *  itself in normal text. Hierarchy without relying on indent surviving
	 *  a proportional font - and the row still reads correctly when the
	 *  search box has filtered its parents out of the list. */
	TSharedRef<SWidget> MakeCollectionPathRow(const FString& Path)
	{
		FString Prefix, Leaf;
		if (!Path.Split(TEXT(" / "), &Prefix, &Leaf, ESearchCase::CaseSensitive,
			ESearchDir::FromEnd))
		{
			Leaf = Path;
		}
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Prefix.IsEmpty() ? FString() : Prefix + TEXT(" / ")))
				.Visibility(Prefix.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(FText::FromString(Leaf))
			];
	}

	/** Wire value meaning "file these the way the package itself had them"
	 *  (SPEC F7 import step 5). Must match kura.pack.PACK_COLLECTIONS. Not a
	 *  collection id (those are "c_" + 8 hex), so one field carries both. */
	const TCHAR* const KuraPackCollections = TEXT("__package__");

	/** Optional "Collection" picker for ShowPreviewDialog. */
	struct FKuraCollectionPicker
	{
		/** Receives the chosen wire value: "" (Unsorted), a collection id, or
		 *  KuraPackCollections. Invalid = no picker at all. */
		TSharedPtr<FString> Selected;
		/** When set, a leading option mapping to KuraPackCollections, which
		 *  also becomes the default. Left empty for a package that carries no
		 *  filing, so the list is then exactly what ingest shows. */
		FText PackOptionLabel;
		/** Overrides the picker tooltip; empty keeps the ingest wording. */
		FText ToolTip;
	};

	/** Which status groups the preview list shows. Deliberately not
	 *  persisted: this is "what do I want to look at in this batch", not a
	 *  preference, and a remembered filter would open the next dialog on a
	 *  list the user cannot predict. */
	struct FKuraStatusFilter
	{
		bool bNew = true;
		bool bSame = true;
		bool bConflict = true;
	};

	/** The flag for one status, by reference so a chip can read and write it. */
	bool& FilterFlagFor(FKuraStatusFilter& Filter, const FString& Status)
	{
		if (Status == TEXT("new")) { return Filter.bNew; }
		return Status == TEXT("same") ? Filter.bSame : Filter.bConflict;
	}

	/** Preview list + per-conflict decisions; OnConfirm gets {package: decision}.
	 *  DecisionOptions carry a stable wire key + a Title-Case label. When
	 *  DepsAsRootsFlag / BatchTags / Picker.Selected are valid, the
	 *  "Mark dependencies as roots" checkbox, "Add Tags" field and "Collection"
	 *  picker are shown; the caller reads them in OnConfirm. */
	void ShowPreviewDialog(const FText& Title, const FText& Warning,
		const TArray<TSharedPtr<FKuraPreviewRow>>& Rows,
		const TArray<FKuraDecisionOption>& DecisionOptions,
		TSharedPtr<bool> DepsAsRootsFlag,
		TSharedPtr<FString> BatchTags,
		const FKuraCollectionPicker& Picker,
		TSharedPtr<const SWidget> ModalParent,
		TFunction<void(const TMap<FString, FString>&)> OnConfirm)
	{
		TSharedPtr<FString> CollectionId = Picker.Selected;
		TSharedRef<TArray<TSharedPtr<FString>>> Options = MakeShared<TArray<TSharedPtr<FString>>>();
		for (const FKuraDecisionOption& Opt : DecisionOptions)
		{
			Options->Add(MakeShared<FString>(Opt.Key));
		}
		TSharedRef<TArray<FKuraDecisionOption>> Opts =
			MakeShared<TArray<FKuraDecisionOption>>(DecisionOptions);
		const auto LabelForKey = [Opts](const FString& Key) -> FText
		{
			for (const FKuraDecisionOption& O : *Opts)
			{
				if (O.Key == Key) { return O.Label; }
			}
			return FText::FromString(Key);
		};
		TSharedRef<TArray<TSharedPtr<FKuraPreviewRow>>> Held = MakeShared<TArray<TSharedPtr<FKuraPreviewRow>>>(Rows);

		int32 NumNew = 0, NumSame = 0, NumConflict = 0, NumSelected = 0;
		for (const TSharedPtr<FKuraPreviewRow>& R : Rows)
		{
			if (R->Status == TEXT("new")) { ++NumNew; }
			else if (R->Status == TEXT("same")) { ++NumSame; }
			else { ++NumConflict; }
			if (R->bSelected) { ++NumSelected; }
		}
		// A .kurapack preview marks no row selected - the user picked the whole
		// file - so the role summary and the bold names drop out there.
		const bool bHasRoleAxis = NumSelected > 0;

		// Identical rows start hidden whenever there is anything else to look
		// at (2026-08-23): they need no decision, and a closure that is mostly
		// already in the library buries the rows that do. Their count stays in
		// the header, and a line under the list says how many are hidden.
		// Not when there are none to hide, though - an unticked box next to a
		// zero reads as "something is missing" when nothing is.
		const TSharedRef<FKuraStatusFilter> Filter = MakeShared<FKuraStatusFilter>();
		Filter->bSame = (NumSame == 0 || NumNew + NumConflict == 0);

		const TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(Title)
			.ClientSize(FVector2D(720.f, 420.f))
			.SupportsMaximize(false)
			.SupportsMinimize(false);
		const TWeakPtr<SWindow> WeakWindow = Window;

		// Decisions first, then what changes, then what does not: in a large
		// batch the rows that need a decision must not be buried under
		// thousands of new/same rows. Stable, so each group keeps the package
		// order Python sorted it into.
		const auto StatusRank = [](const FString& Status) -> int32
		{
			if (Status == TEXT("conflict")) { return 0; }
			return Status == TEXT("new") ? 1 : 2;
		};
		Held->StableSort([StatusRank](const TSharedPtr<FKuraPreviewRow>& A, const TSharedPtr<FKuraPreviewRow>& B)
		{
			return StatusRank(A->Status) < StatusRank(B->Status);
		});
		// Held stays whole. Confirm reads every conflict's decision off it and
		// "Set all" writes to every conflict, so a row the filter hides must
		// still carry its decision into the commit - only the list's item
		// source is filtered.
		const TSharedRef<TArray<TSharedPtr<FKuraPreviewRow>>> Visible =
			MakeShared<TArray<TSharedPtr<FKuraPreviewRow>>>();
		// Virtualized list: a full-site .kurapack (F7) previews thousands of
		// entries; a widget-per-row SVerticalBox would hang the dialog.
		const TSharedRef<SListView<TSharedPtr<FKuraPreviewRow>>> List =
			SNew(SListView<TSharedPtr<FKuraPreviewRow>>)
			.ListItemsSource(&Visible.Get())
			.SelectionMode(ESelectionMode::None)
			.OnGenerateRow_Lambda([Options, LabelForKey, bHasRoleAxis](TSharedPtr<FKuraPreviewRow> R,
				const TSharedRef<STableViewBase>& OwnerTable) -> TSharedRef<ITableRow>
		{
			TSharedRef<SHorizontalBox> RowBox = SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(SBox).WidthOverride(64.f)
				[
					SNew(STextBlock)
					.Text(StatusBadgeText(R->Status))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
					.ColorAndOpacity(StatusBadgeColor(R->Status))
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				MakePreviewLabel(R->Package, R->Class, bHasRoleAxis && R->bSelected)
			];
			if (R->Status == TEXT("conflict"))
			{
				int32 InitialIdx = Options->IndexOfByPredicate(
					[&R](const TSharedPtr<FString>& O) { return *O == *R->Decision; });
				InitialIdx = FMath::Max(InitialIdx, 0);
				RowBox->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(6.f, 0.f, 0.f, 0.f)
				[
					SNew(SComboBox<TSharedPtr<FString>>)
					.OptionsSource(&Options.Get())
					.InitiallySelectedItem((*Options)[InitialIdx])
					// Both the dropdown rows and the closed display show the
					// Title-Case label; the row's Decision keeps the wire key.
					.OnGenerateWidget_Lambda([LabelForKey](TSharedPtr<FString> O)
					{
						return SNew(STextBlock).Text(LabelForKey(*O));
					})
					// Options captured by value: the shared ref keeps the array
					// alive for as long as the combo (OptionsSource is a raw ptr).
					.OnSelectionChanged_Lambda([R, Options](TSharedPtr<FString> O, ESelectInfo::Type)
					{
						if (O.IsValid())
						{
							*R->Decision = *O;
						}
					})
					[
						// Text_Lambda so "Update All"/"Ignore All" reflect live.
						SNew(STextBlock).Text_Lambda([R, LabelForKey]() { return LabelForKey(*R->Decision); })
					]
				];
			}
			TSharedRef<SVerticalBox> Cell = SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f) [ RowBox ];
			if (R->Status == TEXT("conflict") && R->Referrers.Num() > 0)
			{
				Cell->AddSlot().AutoHeight().Padding(72.f, 0.f, 0.f, 2.f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("referenced by: ") + FString::Join(R->Referrers, TEXT(", "))))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				];
			}
			return SNew(STableRow<TSharedPtr<FKuraPreviewRow>>, OwnerTable) [ Cell ];
		});

		// Rebuilds the list's item source from Held. Weak on the list itself:
		// the chips hold this function, and the list holds the chips' window.
		const TWeakPtr<SListView<TSharedPtr<FKuraPreviewRow>>> WeakList = List;
		const TSharedRef<TFunction<void()>> ApplyFilter = MakeShared<TFunction<void()>>(
			[Held, Visible, Filter, WeakList]()
			{
				Visible->Reset();
				for (const TSharedPtr<FKuraPreviewRow>& R : *Held)
				{
					if (FilterFlagFor(*Filter, R->Status))
					{
						Visible->Add(R);
					}
				}
				if (const TSharedPtr<SListView<TSharedPtr<FKuraPreviewRow>>> L = WeakList.Pin())
				{
					L->RequestListRefresh();
				}
			});
		(*ApplyFilter)();

		// Header chip: the count, plus a checkbox filtering that status in and
		// out of the list. Disabled at zero - nothing to show or hide.
		const auto MakeStatusChip = [Filter, ApplyFilter](int32 Count, const FText& Noun,
			const FString& Status) -> TSharedRef<SWidget>
		{
			return SNew(SCheckBox)
				.IsEnabled(Count > 0)
				.IsChecked_Lambda([Filter, Status]()
				{
					return FilterFlagFor(*Filter, Status)
						? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([Filter, Status, ApplyFilter](ECheckBoxState State)
				{
					FilterFlagFor(*Filter, Status) = (State == ECheckBoxState::Checked);
					(*ApplyFilter)();
				})
				.ToolTipText(LOCTEXT("StatusChipTip",
					"Show or hide these rows. This only changes what the list displays - every asset counted here is part of the operation either way."))
				[
					CountSegment(Count, Noun, StatusBadgeColor(Status))
				];
		};

		// "Update All" / "Ignore All" - one per option, shown only when there
		// is more than one conflict to resolve. Sets every conflict's decision;
		// the combos are Text_Lambda-bound so they update on the next tick.
		TSharedRef<SHorizontalBox> BulkRow = SNew(SHorizontalBox);
		BulkRow->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("SetAllConflicts", "Set all conflicts:"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
		for (const FKuraDecisionOption& Opt : DecisionOptions)
		{
			const FString Key = Opt.Key;
			BulkRow->AddSlot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
			[
				SNew(SButton)
				.Text(FText::Format(LOCTEXT("DecideAll", "{0} All"), Opt.Label))
				.OnClicked_Lambda([Held, Key]()
				{
					for (const TSharedPtr<FKuraPreviewRow>& R : *Held)
					{
						if (R->Status == TEXT("conflict") && R->Decision.IsValid())
						{
							*R->Decision = Key;
						}
					}
					return FReply::Handled();
				})
			];
		}

		// Optional ingest-only "Add Tags" field (batch tags for new/updated).
		TSharedRef<SWidget> TagsRow = SNew(SBox);
		if (BatchTags.IsValid())
		{
			TagsRow = SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 6.f, 0.f)
			[
				SNew(STextBlock).Text(LOCTEXT("BatchTags", "Add Tags"))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(SEditableTextBox)
				.HintText(LOCTEXT("BatchTagsHint", "optional, comma separated"))
				.ToolTipText(LOCTEXT("BatchTagsTip",
					"Tags added to every NEW and UPDATED entry in this upload (e.g. a source tag like \"polyhaven\"). Entries the library already has keep their tags."))
				.OnTextChanged_Lambda([BatchTags](const FText& NewText)
				{
					*BatchTags = NewText.ToString();
				})
			];
		}

		// Optional Collection picker (SPEC F8; also import, F7 step 5).
		// Deliberately optional: ingest is capture, filing is curation, and
		// forcing a choice here would stall every bulk upload. Anything left
		// unfiled shows up under Unsorted in the panel.
		TSharedRef<SWidget> CollectionRow = SNew(SBox);
		if (CollectionId.IsValid())
		{
			// Searchable, and keyed by full path: a seeded tree runs to a
			// hundred-odd collections, which is well past the point where
			// scrolling a plain combo box is usable. SSearchableComboBox
			// matches whitespace-separated tokens against the option string,
			// so "wood floor" finds "Surfaces / Wood / Flooring".
			TSharedRef<TArray<TSharedPtr<FString>>> CollectionOptions =
				MakeShared<TArray<TSharedPtr<FString>>>();
			TSharedRef<TMap<FString, FString>> PathToId =
				MakeShared<TMap<FString, FString>>();
			// "Use the package's collections" leads, and so becomes the
			// default, whenever the package brought filing with it.
			if (!Picker.PackOptionLabel.IsEmpty())
			{
				const FString PackLabel = Picker.PackOptionLabel.ToString();
				CollectionOptions->Add(MakeShared<FString>(PackLabel));
				PathToId->Add(PackLabel, KuraPackCollections);
			}
			const FString NoneLabel = LOCTEXT("CollectionNone", "None (Unsorted)").ToString();
			CollectionOptions->Add(MakeShared<FString>(NoneLabel));
			PathToId->Add(NoneLabel, FString());
			TSharedPtr<FString> Initial = (*CollectionOptions)[0];
			for (const TSharedPtr<FKuraCollectionOption>& Opt : FetchCollections())
			{
				const TSharedPtr<FString> Item = MakeShared<FString>(Opt->Path);
				CollectionOptions->Add(Item);
				PathToId->Add(Opt->Path, Opt->Id);
				if (Opt->Id == *CollectionId)
				{
					Initial = Item; // remembered from the last upload
				}
			}
			*CollectionId = PathToId->FindRef(*Initial);
			TSharedRef<TSharedPtr<FString>> Current =
				MakeShared<TSharedPtr<FString>>(Initial);

			CollectionRow = SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 6.f, 0.f)
			[
				SNew(STextBlock).Text(LOCTEXT("BatchCollection", "Collection"))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(SSearchableComboBox)
				.OptionsSource(&CollectionOptions.Get())
				.InitiallySelectedItem(Initial)
				.ToolTipText(Picker.ToolTip.IsEmpty()
					? LOCTEXT("BatchCollectionTip",
						"File the assets you selected under this collection. Auto-collected dependencies are never filed - a shared master belongs to everything that uses it.")
					: Picker.ToolTip)
				// CollectionOptions is captured to keep the array the combo box points
				// at alive for as long as the widget is.
				.OnGenerateWidget_Lambda([CollectionOptions](TSharedPtr<FString> Item)
				{
					return MakeCollectionPathRow(*Item);
				})
				.OnSelectionChanged_Lambda([CollectionId, Current, PathToId](
					TSharedPtr<FString> Item, ESelectInfo::Type)
				{
					if (Item.IsValid())
					{
						*Current = Item;
						*CollectionId = PathToId->FindRef(*Item);
					}
				})
				[
					SNew(STextBlock)
					.Text_Lambda([Current]()
					{
						return FText::FromString(**Current);
					})
				]
			];
		}

		// Optional ingest-only "mark dependencies as roots" checkbox.
		TSharedRef<SWidget> DepsRootRow = SNew(SBox);
		if (DepsAsRootsFlag.IsValid())
		{
			DepsRootRow = SNew(SCheckBox)
				.IsChecked_Lambda([DepsAsRootsFlag]()
				{
					return *DepsAsRootsFlag ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([DepsAsRootsFlag](ECheckBoxState State)
				{
					*DepsAsRootsFlag = (State == ECheckBoxState::Checked);
				})
				.ToolTipText(LOCTEXT("DepsAsRootsTip",
					"Also list auto-collected dependencies as assets in their own right, so you don't have to send them again yourself. Off = only the assets you selected are roots; the rest show up under Show Dependencies, alongside whatever uses them."))
				[
					SNew(STextBlock).Text(LOCTEXT("DepsAsRoots", "Mark Dependencies as Roots"))
				];
		}

		Window->SetContent(
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(10.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 2.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 18.f, 0.f)
					[
						MakeStatusChip(NumNew, LOCTEXT("NounNew", "New"), TEXT("new"))
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 18.f, 0.f)
					[
						MakeStatusChip(NumSame, LOCTEXT("NounSame", "Identical (Skipped)"), TEXT("same"))
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						MakeStatusChip(NumConflict,
							FText::Format(LOCTEXT("NounConflict", "{0}|plural(one=Conflict,other=Conflicts)"), NumConflict),
							TEXT("conflict"))
					]
				]
				// The one line that answers "why is this list so long".
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
				[
					SNew(STextBlock)
					.Visibility(bHasRoleAxis ? EVisibility::Visible : EVisibility::Collapsed)
					.Text(FText::Format(LOCTEXT("RoleSummary",
						"{0} selected, {1} collected as dependencies"),
						NumSelected, Held->Num() - NumSelected))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot().FillHeight(1.f)
				[
					List
				]
				// Receipt for the filters above, so a shortened list never
				// reads as "some of my assets went missing".
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Visibility_Lambda([Held, Visible]()
					{
						return Visible->Num() < Held->Num()
							? EVisibility::Visible : EVisibility::Collapsed;
					})
					.Text_Lambda([Held, Visible]()
					{
						return FText::Format(LOCTEXT("HiddenRows",
							"The filters hide {0} of {1} rows; the hidden rows are still part of this operation."),
							Held->Num() - Visible->Num(), Held->Num());
					})
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
				.HAlign(HAlign_Left)
				[
					SNew(SBox).Visibility(NumConflict >= 2 ? EVisibility::Visible : EVisibility::Collapsed)
					[
						BulkRow
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Text(Warning)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
				[
					TagsRow
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
				[
					CollectionRow
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
					[
						DepsRootRow
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
					[
						SNew(SButton)
						.Text(LOCTEXT("Confirm", "Confirm"))
						.OnClicked_Lambda([WeakWindow, Held, OnConfirm]()
						{
							TMap<FString, FString> Decisions;
							for (const TSharedPtr<FKuraPreviewRow>& R : *Held)
							{
								if (R->Decision.IsValid())
								{
									Decisions.Add(R->Package, *R->Decision);
								}
							}
							if (const TSharedPtr<SWindow> W = WeakWindow.Pin())
							{
								W->RequestDestroyWindow();
							}
							OnConfirm(Decisions);
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("CancelBatch", "Cancel"))
						.OnClicked_Lambda([WeakWindow]()
						{
							if (const TSharedPtr<SWindow> W = WeakWindow.Pin())
							{
								W->RequestDestroyWindow();
							}
							return FReply::Handled();
						})
					]
				]
			]);
		FSlateApplication::Get().AddModalWindow(Window, ModalParent);
	}

	// Declared ahead of the definition because a rejected value reopens it.
	void ShowTextPrompt(const FText& Title, const FText& Label, const FString& InitialValue,
		TSharedPtr<const SWidget> ModalParent,
		TFunction<void(const FString&, TFunction<void(const FString&)>)> OnAccept,
		const FText& Context = FText::GetEmpty());

	/** Modal with a single text field. OnAccept runs only on OK/Enter with a
	 *  non-empty value; validation lives in Python, so this stays dumb.
	 *
	 *  OnAccept receives a "retry" callback: Python owns the rules (duplicate
	 *  sibling names, slashes), and a rejected name should come back with the
	 *  text still in the box rather than making the user retype it.
	 *
	 *  Context: optional subdued line above the field, for prompts whose
	 *  effect depends on where they were invoked from ("Create in: ..."). */
	void ShowTextPrompt(const FText& Title, const FText& Label, const FString& InitialValue,
		TSharedPtr<const SWidget> ModalParent,
		TFunction<void(const FString&, TFunction<void(const FString&)>)> OnAccept,
		const FText& Context)
	{
		TSharedRef<FString> Value = MakeShared<FString>(InitialValue);
		const TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(Title)
			.ClientSize(FVector2D(380.f, Context.IsEmpty() ? 120.f : 150.f))
			.SupportsMaximize(false)
			.SupportsMinimize(false);
		const TWeakPtr<SWindow> WeakWindow = Window;

		// Shared by the OK button and Enter in the field. The window closes
		// first so any error dialog OnAccept raises is not stacked on top of
		// it; reopening is how a rejected value gets a second try.
		TSharedRef<TFunction<void()>> Accept = MakeShared<TFunction<void()>>(
			[WeakWindow, Value, OnAccept, Title, Label, ModalParent, Context]()
			{
				const FString Trimmed = Value->TrimStartAndEnd();
				if (const TSharedPtr<SWindow> W = WeakWindow.Pin())
				{
					W->RequestDestroyWindow();
				}
				if (Trimmed.IsEmpty())
				{
					return;
				}
				OnAccept(Trimmed, [Title, Label, ModalParent, OnAccept, Context](const FString& Retry)
				{
					ShowTextPrompt(Title, Label, Retry, ModalParent, OnAccept, Context);
				});
			});

		const TSharedRef<SEditableTextBox> Field = SNew(SEditableTextBox)
			.Text(FText::FromString(InitialValue))
			.SelectAllTextWhenFocused(true)
			.OnTextChanged_Lambda([Value](const FText& NewText) { *Value = NewText.ToString(); })
			.OnTextCommitted_Lambda([Accept](const FText&, ETextCommit::Type CommitType)
			{
				if (CommitType == ETextCommit::OnEnter)
				{
					(*Accept)();
				}
			});

		Window->SetContent(
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(10.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
				[
					SNew(STextBlock)
					.Text(Context)
					.Visibility(Context.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
				[
					SNew(STextBlock).Text(Label)
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					Field
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f).HAlign(HAlign_Right)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
					[
						SNew(SButton)
						.Text(LOCTEXT("PromptOk", "OK"))
						.OnClicked_Lambda([Accept]()
						{
							(*Accept)();
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("PromptCancel", "Cancel"))
						.OnClicked_Lambda([WeakWindow]()
						{
							if (const TSharedPtr<SWindow> W = WeakWindow.Pin())
							{
								W->RequestDestroyWindow();
							}
							return FReply::Handled();
						})
					]
				]
			]);
		Window->SetWidgetToFocusOnActivate(Field);
		FSlateApplication::Get().AddModalWindow(Window, ModalParent);
	}

	/** One row of the delete confirmation (F4 unused dependencies, 2026-08-02).
	 *  Entries the user picked are fixed; dependencies the delete would leave
	 *  unused carry a checkbox, on by default. */
	struct FKuraDeleteRow
	{
		FString Package;
		bool bDependency = false;
		TArray<FString> Blocked;              // referrers that will refuse it
		TSharedRef<bool> Include = MakeShared<bool>(true);
	};

	/** Confirmation for archiving entries; OnConfirm gets the final package
	 *  list (picked entries + the dependency rows still checked). */
	void ShowDeleteDialog(const FText& Title, const FText& Intro,
		const TArray<TSharedPtr<FKuraDeleteRow>>& Rows,
		TSharedPtr<const SWidget> ModalParent,
		TFunction<void(const TArray<FString>&)> OnConfirm)
	{
		TSharedRef<TArray<TSharedPtr<FKuraDeleteRow>>> Held =
			MakeShared<TArray<TSharedPtr<FKuraDeleteRow>>>(Rows);
		int32 NumPicked = 0, NumDeps = 0;
		for (const TSharedPtr<FKuraDeleteRow>& R : *Held)
		{
			R->bDependency ? ++NumDeps : ++NumPicked;
		}
		// Picked entries first: the dependency rows are the addition, and the
		// user should read what they asked for before what comes along.
		Held->StableSort([](const TSharedPtr<FKuraDeleteRow>& A, const TSharedPtr<FKuraDeleteRow>& B)
		{
			return A->bDependency < B->bDependency;
		});

		const TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(Title)
			.ClientSize(FVector2D(720.f, 420.f))
			.SupportsMaximize(false)
			.SupportsMinimize(false);
		const TWeakPtr<SWindow> WeakWindow = Window;

		const TSharedRef<SListView<TSharedPtr<FKuraDeleteRow>>> List =
			SNew(SListView<TSharedPtr<FKuraDeleteRow>>)
			.ListItemsSource(&Held.Get())
			.SelectionMode(ESelectionMode::None)
			.OnGenerateRow_Lambda([](TSharedPtr<FKuraDeleteRow> R,
				const TSharedRef<STableViewBase>& OwnerTable) -> TSharedRef<ITableRow>
		{
			TSharedRef<SHorizontalBox> RowBox = SNew(SHorizontalBox);
			if (R->bDependency)
			{
				RowBox->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
				[
					SNew(SBox).WidthOverride(72.f)
					[
						SNew(SCheckBox)
						.IsChecked_Lambda([R]()
						{
							return *R->Include ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
						})
						.OnCheckStateChanged_Lambda([R](ECheckBoxState State)
						{
							*R->Include = (State == ECheckBoxState::Checked);
						})
						[
							SNew(STextBlock)
							.Text(LOCTEXT("BadgeUnused", "unused"))
							.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
							.ColorAndOpacity(FSlateColor(KuraStyle::Warning().Text))
						]
					]
				];
			}
			else
			{
				RowBox->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
				[
					SNew(SBox).WidthOverride(72.f)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("BadgePicked", "SELECTED"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
				];
			}
			RowBox->AddSlot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(FText::FromString(R->Package)).AutoWrapText(true)
			];
			TSharedRef<SVerticalBox> Cell = SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f) [ RowBox ];
			if (R->Blocked.Num() > 0)
			{
				Cell->AddSlot().AutoHeight().Padding(80.f, 0.f, 0.f, 2.f)
				[
					SNew(STextBlock)
					.Text(FText::Format(LOCTEXT("DeleteRowBlocked", "blocked - still referenced by: {0}"),
						FText::FromString(FString::Join(R->Blocked, TEXT(", ")))))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					.ColorAndOpacity(FSlateColor(KuraStyle::Warning().Text))
					.AutoWrapText(true)
				];
			}
			return SNew(STableRow<TSharedPtr<FKuraDeleteRow>>, OwnerTable) [ Cell ];
		});

		// Bulk toggles for the dependency rows, mirroring the preview dialog's
		// "Update All" / "Ignore All".
		TSharedRef<SHorizontalBox> BulkRow = SNew(SHorizontalBox);
		BulkRow->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("SetAllDeps", "Unused dependencies:"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
		for (int32 Idx = 0; Idx < 2; ++Idx)
		{
			const bool bOn = (Idx == 0);
			BulkRow->AddSlot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
			[
				SNew(SButton)
				.Text(bOn ? LOCTEXT("DepsAll", "All") : LOCTEXT("DepsNone", "None"))
				.OnClicked_Lambda([Held, bOn]()
				{
					for (const TSharedPtr<FKuraDeleteRow>& R : *Held)
					{
						if (R->bDependency)
						{
							*R->Include = bOn;
						}
					}
					return FReply::Handled();
				})
			];
		}

		Window->SetContent(
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(10.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 18.f, 0.f)
					[
						CountSegment(NumPicked, LOCTEXT("NounPicked", "Selected"),
							KuraStyle::TextSecondary())
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						CountSegment(NumDeps, LOCTEXT("NounUnusedDeps", "Unused Dependencies"),
							KuraStyle::Warning().Text)
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
				[
					SNew(STextBlock).Text(Intro).AutoWrapText(true)
				]
				+ SVerticalBox::Slot().FillHeight(1.f)
				[
					List
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
				.HAlign(HAlign_Left)
				[
					SNew(SBox).Visibility(NumDeps >= 2 ? EVisibility::Visible : EVisibility::Collapsed)
					[
						BulkRow
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("DeleteRestoreHint",
						"Nothing is deleted for good: everything below moves into _archive as one batch and comes back together from Recently Deleted."))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
				.HAlign(HAlign_Right)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
					[
						SNew(SButton)
						.Text(LOCTEXT("ConfirmArchive", "Archive"))
						.OnClicked_Lambda([WeakWindow, Held, OnConfirm]()
						{
							TArray<FString> Chosen;
							for (const TSharedPtr<FKuraDeleteRow>& R : *Held)
							{
								if (!R->bDependency || *R->Include)
								{
									Chosen.Add(R->Package);
								}
							}
							if (const TSharedPtr<SWindow> W = WeakWindow.Pin())
							{
								W->RequestDestroyWindow();
							}
							if (Chosen.Num() > 0)
							{
								OnConfirm(Chosen);
							}
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("CancelDelete", "Cancel"))
						.OnClicked_Lambda([WeakWindow]()
						{
							if (const TSharedPtr<SWindow> W = WeakWindow.Pin())
							{
								W->RequestDestroyWindow();
							}
							return FReply::Handled();
						})
					]
				]
			]);
		FSlateApplication::Get().AddModalWindow(Window, ModalParent);
	}

	/** delete_preview -> rows for ShowDeleteDialog. Returns false (and reports)
	 *  when the Python call failed. */
	bool BuildDeleteRows(const TArray<FString>& Packages,
		TArray<TSharedPtr<FKuraDeleteRow>>& OutRows, int32& OutNumDeps,
		TSharedPtr<const SWidget> ModalParent)
	{
		const TSharedPtr<FJsonObject> Preview = KuraPython::Call(FString::Printf(
			TEXT("delete_preview('%s')"),
			*KuraPython::EscapePyStr(KuraPython::ToJsonArray(Packages))));
		bool bOk = false;
		if (Preview.IsValid())
		{
			Preview->TryGetBoolField(TEXT("ok"), bOk);
		}
		if (!bOk)
		{
			KuraActions::ShowErrorsDialog(LOCTEXT("DeletePreviewFailedTitle", "Kura - Delete"),
				ErrorsFromResult(Preview), ModalParent);
			return false;
		}
		const TSharedPtr<FJsonObject>* Blocked = nullptr;
		Preview->TryGetObjectField(TEXT("blocked"), Blocked);
		for (const FString& Pkg : Packages)
		{
			const TSharedPtr<FKuraDeleteRow> Row = MakeShared<FKuraDeleteRow>();
			Row->Package = Pkg;
			const TArray<TSharedPtr<FJsonValue>>* Refs = nullptr;
			if (Blocked && (*Blocked)->TryGetArrayField(Pkg, Refs))
			{
				for (const TSharedPtr<FJsonValue>& R : *Refs)
				{
					Row->Blocked.Add(R->AsString());
				}
			}
			OutRows.Add(Row);
		}
		OutNumDeps = 0;
		const TArray<TSharedPtr<FJsonValue>>* Unused = nullptr;
		if (Preview->TryGetArrayField(TEXT("unused_deps"), Unused))
		{
			for (const TSharedPtr<FJsonValue>& V : *Unused)
			{
				const TSharedPtr<FKuraDeleteRow> Row = MakeShared<FKuraDeleteRow>();
				Row->Package = V->AsString();
				Row->bDependency = true;
				OutRows.Add(Row);
				++OutNumDeps;
			}
		}
		return true;
	}

	/** Archive the given entries and report the outcome (partial completion is
	 *  explicit: what went in, what stayed blocked). */
	void ArchiveEntries(const TArray<FString>& Packages, TSharedPtr<const SWidget> ModalParent)
	{
		const TSharedPtr<FJsonObject> Result = KuraPython::Call(FString::Printf(
			TEXT("delete_entries('%s')"),
			*KuraPython::EscapePyStr(KuraPython::ToJsonArray(Packages))));
		bool bOk = false;
		TArray<FString> Archived;
		if (Result.IsValid())
		{
			Result->TryGetBoolField(TEXT("ok"), bOk);
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (Result->TryGetArrayField(TEXT("archived"), Arr))
			{
				for (const TSharedPtr<FJsonValue>& V : *Arr)
				{
					Archived.Add(V->AsString());
				}
			}
		}
		if (!bOk)
		{
			// Partial completion must be explicit: list what WAS archived before
			// the blocked entries, not just the failures.
			TArray<FString> Lines;
			if (Archived.Num() > 0)
			{
				Lines.Add(FString::Printf(TEXT("Archived (%d):"), Archived.Num()));
				for (const FString& Pkg : Archived)
				{
					Lines.Add(TEXT("    ") + Pkg);
				}
				Lines.Add(FString::Printf(TEXT("Blocked (%d):"), Packages.Num() - Archived.Num()));
			}
			Lines.Append(ErrorsFromResult(Result));
			KuraActions::ShowErrorsDialog(Archived.Num() > 0
				? LOCTEXT("DeletePartialTitle", "Kura - Delete Partially Completed")
				: LOCTEXT("DeleteBlockedTitle", "Kura - Delete Blocked"),
				Lines, ModalParent);
		}
		else
		{
			KuraActions::Notify(true, Packages.Num() == 1
				? LOCTEXT("DeleteDoneOne", "Kura: entry archived")
				: FText::Format(LOCTEXT("DeleteDoneMany", "Kura: {0} entries archived"),
					FText::AsNumber(Packages.Num())));
		}
		KuraActions::OnLibraryChanged().Broadcast();
	}
}

FSimpleMulticastDelegate& KuraActions::OnLibraryChanged()
{
	static FSimpleMulticastDelegate Delegate;
	return Delegate;
}

void KuraActions::Notify(bool bSuccess, const FText& Message)
{
	FNotificationInfo Info(Message);
	Info.ExpireDuration = 4.f;
	Info.bUseSuccessFailIcons = true;
	const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
	if (Item.IsValid())
	{
		Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
	}
}

void KuraActions::ShowErrorsDialog(const FText& Title, const TArray<FString>& Lines,
	TSharedPtr<const SWidget> ModalParent)
{
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(Title)
		.ClientSize(FVector2D(620.f, 320.f))
		.SupportsMaximize(false)
		.SupportsMinimize(false);
	const TWeakPtr<SWindow> WeakWindow = Window;

	TSharedRef<SVerticalBox> List = SNew(SVerticalBox);
	for (const FString& Line : Lines)
	{
		List->AddSlot().AutoHeight().Padding(0.f, 2.f)
		[
			SNew(STextBlock).Text(FText::FromString(Line)).AutoWrapText(true)
		];
	}

	Window->SetContent(
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(10.f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				SNew(SScrollBox) + SScrollBox::Slot() [ List ]
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0.f, 8.f, 0.f, 0.f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Close", "Close"))
				.OnClicked_Lambda([WeakWindow]()
				{
					if (const TSharedPtr<SWindow> W = WeakWindow.Pin())
					{
						W->RequestDestroyWindow();
					}
					return FReply::Handled();
				})
			]
		]);
	FSlateApplication::Get().AddModalWindow(Window, ModalParent);
}

void KuraActions::RunIngestFlow(TSharedPtr<const SWidget> ModalParent)
{
	const TSharedPtr<FJsonObject> Preview = KuraPython::Call(TEXT("ingest_preview()"));
	if (!Preview.IsValid())
	{
		Notify(false, LOCTEXT("PyFailed", "Kura: Python call failed - see Output Log"));
		return;
	}
	bool bOk = false;
	Preview->TryGetBoolField(TEXT("ok"), bOk);
	if (!bOk)
	{
		ShowErrorsDialog(LOCTEXT("IngestBlockedTitle", "Kura - Ingest Blocked"),
			ErrorsFromResult(Preview), ModalParent);
		return;
	}

	TArray<FString> Roots;
	const TArray<TSharedPtr<FJsonValue>>* RootsArr = nullptr;
	if (Preview->TryGetArrayField(TEXT("roots"), RootsArr))
	{
		for (const TSharedPtr<FJsonValue>& V : *RootsArr)
		{
			Roots.Add(V->AsString());
		}
	}

	// Remembered across sessions so a user who always wants deps as roots isn't
	// re-ticking it every upload (SPEC F1, 2026-07-14).
	TSharedPtr<bool> DepsAsRoots = MakeShared<bool>(
		KuraLibrary::GetConfigNumber(TEXT("ingest_deps_as_roots"), 0.0) != 0.0);
	TSharedPtr<FString> BatchTags = MakeShared<FString>();

	const TArray<TSharedPtr<FKuraPreviewRow>> Rows = RowsFromPreview(Preview, TEXT("keep"));
	// Remembered like the deps checkbox: filling a collection usually takes
	// several uploads in a row (SPEC F8) - per class, see IngestCollectionKey.
	const FString CollectionKey = IngestCollectionKey(Rows);
	TSharedPtr<FString> CollectionId = MakeShared<FString>(
		KuraLibrary::GetConfigString(*CollectionKey));
	ShowPreviewDialog(
		LOCTEXT("IngestPreviewTitle", "Send to Kura - Upload Preview"),
		LOCTEXT("IngestWarning", "Update: the current library version is moved to _archive, then replaced by the version you are sending. Ignore: this asset is not uploaded and the library keeps its current version."),
		Rows,
		{ { TEXT("update"), LOCTEXT("DecUpdate", "Update") },
		  { TEXT("keep"), LOCTEXT("DecIgnore", "Ignore") } },
		DepsAsRoots,
		BatchTags,
		FKuraCollectionPicker{ CollectionId },
		ModalParent,
		[ModalParent, Roots, DepsAsRoots, BatchTags, CollectionId, CollectionKey](const TMap<FString, FString>& Decisions)
		{
			KuraLibrary::SaveConfig({ { CollectionKey, *CollectionId } },
				{ { TEXT("ingest_deps_as_roots"), *DepsAsRoots ? 1.0 : 0.0 } });
			const FString Statement = FString::Printf(TEXT("ingest_commit('%s', '%s', %s, '%s', '%s')"),
				*KuraPython::EscapePyStr(KuraPython::ToJsonArray(Roots)),
				*KuraPython::EscapePyStr(KuraPython::ToJsonObject(Decisions)),
				*(*DepsAsRoots ? FString(TEXT("True")) : FString(TEXT("False"))),
				*KuraPython::EscapePyStr(*BatchTags),
				*KuraPython::EscapePyStr(*CollectionId));
			const TSharedPtr<FJsonObject> Result = KuraPython::Call(Statement);
			bool bCommitOk = false;
			if (Result.IsValid())
			{
				Result->TryGetBoolField(TEXT("ok"), bCommitOk);
			}
			if (!bCommitOk)
			{
				ShowErrorsDialog(LOCTEXT("IngestFailedTitle", "Kura - Ingest Failed"),
					ErrorsFromResult(Result), ModalParent);
			}
			else
			{
				Notify(true, LOCTEXT("IngestDone", "Kura: ingest finished"));
				FString FilingError;
				if (Result->TryGetStringField(TEXT("filing_error"), FilingError)
					&& !FilingError.IsEmpty())
				{
					// The assets are in; only the filing tripped, so this is
					// a warning next to a successful upload - same shape as
					// the .kurapack import's collections.error.
					Notify(false, FText::Format(
						LOCTEXT("IngestFilingFailed",
							"Kura: assets uploaded, but adding them to the collection failed - {0}"),
						FText::FromString(FilingError)));
				}
			}
			OnLibraryChanged().Broadcast();
		});
}

void KuraActions::RunImportFlow(const TArray<FString>& Packages,
	TSharedPtr<const SWidget> ModalParent, bool bForceVersion)
{
	if (Packages.Num() == 0)
	{
		return;
	}
	const FString PkgArg = KuraPython::EscapePyStr(KuraPython::ToJsonArray(Packages));
	const FString Force = bForceVersion ? TEXT("True") : TEXT("False");

	const TSharedPtr<FJsonObject> Preview = KuraPython::Call(
		FString::Printf(TEXT("import_preview('%s', %s)"), *PkgArg, *Force));
	if (!Preview.IsValid())
	{
		Notify(false, LOCTEXT("PyFailed", "Kura: Python call failed - see Output Log"));
		return;
	}
	bool bOk = false;
	Preview->TryGetBoolField(TEXT("ok"), bOk);
	if (!bOk)
	{
		TArray<FString> Errors = ErrorsFromResult(Preview);
		const bool bVersionGuard = Errors.ContainsByPredicate(
			[](const FString& E) { return E.Contains(TEXT("newer-engine")); });
		if (bVersionGuard && !bForceVersion)
		{
			const EAppReturnType::Type Answer = FMessageDialog::Open(EAppMsgType::YesNo,
				FText::Format(LOCTEXT("ForceVersionAsk",
					"{0}\n\nImport anyway (force past the engine version guard)?"),
					FText::FromString(FString::Join(Errors, TEXT("\n")))));
			if (Answer == EAppReturnType::Yes)
			{
				RunImportFlow(Packages, ModalParent, /*bForceVersion*/ true);
			}
			return;
		}
		ShowErrorsDialog(LOCTEXT("ImportBlockedTitle", "Kura - Import Blocked"), Errors, ModalParent);
		return;
	}

	const TArray<TSharedPtr<FKuraPreviewRow>> Rows = RowsFromPreview(Preview, TEXT("keep"));
	ShowPreviewDialog(
		LOCTEXT("ImportPreviewTitle", "Kura - Import Preview"),
		LOCTEXT("ImportWarning", "Overwrite: the project file is replaced by the library version (the replaced project file is not archived). Ignore: the project file is left unchanged."),
		Rows,
		{ { TEXT("overwrite"), LOCTEXT("DecOverwrite", "Overwrite") },
		  { TEXT("keep"), LOCTEXT("DecIgnore", "Ignore") } },
		/*DepsAsRootsFlag*/ nullptr,
		/*BatchTags*/ nullptr,
		/*Picker*/ {},
		ModalParent,
		[ModalParent, PkgArg, Force](const TMap<FString, FString>& Decisions)
		{
			const FString Statement = FString::Printf(TEXT("import_commit('%s', '%s', %s)"),
				*PkgArg,
				*KuraPython::EscapePyStr(KuraPython::ToJsonObject(Decisions)),
				*Force);
			const TSharedPtr<FJsonObject> Result = KuraPython::Call(Statement);
			bool bCommitOk = false;
			if (Result.IsValid())
			{
				Result->TryGetBoolField(TEXT("ok"), bCommitOk);
			}
			if (!bCommitOk)
			{
				ShowErrorsDialog(LOCTEXT("ImportFailedTitle", "Kura - Import Failed"),
					ErrorsFromResult(Result), ModalParent);
			}
			else
			{
				Notify(true, LOCTEXT("ImportDone", "Kura: import finished"));
			}
		});
}

void KuraActions::RunDeleteFlow(const TArray<FString>& Packages, TSharedPtr<const SWidget> ModalParent)
{
	if (Packages.Num() == 0)
	{
		return;
	}
	TArray<TSharedPtr<FKuraDeleteRow>> Rows;
	int32 NumDeps = 0;
	if (!BuildDeleteRows(Packages, Rows, NumDeps, ModalParent))
	{
		return;
	}
	if (NumDeps == 0)
	{
		// Nothing would be stranded - keep the cheap confirm.
		const FText Ask = Packages.Num() == 1
			? FText::Format(LOCTEXT("DeleteAskOne", "Move {0} into _archive?"),
				FText::FromString(Packages[0]))
			: FText::Format(LOCTEXT("DeleteAskMany", "Move {0} entries into _archive?\n\n{1}"),
				FText::AsNumber(Packages.Num()),
				FText::FromString(FString::Join(Packages, TEXT("\n"))));
		if (FMessageDialog::Open(EAppMsgType::YesNo, Ask) == EAppReturnType::Yes)
		{
			ArchiveEntries(Packages, ModalParent);
		}
		return;
	}
	ShowDeleteDialog(LOCTEXT("DeleteTitle", "Kura - Delete"),
		FText::Format(LOCTEXT("DeleteIntro",
			"{0}|plural(one=This entry pulls,other=These entries pull) in {1} {1}|plural(one=dependency,other=dependencies) nothing else in the library uses. Archive them too, or uncheck the ones you want to keep."),
			Packages.Num(), NumDeps),
		Rows, ModalParent,
		[ModalParent](const TArray<FString>& Chosen)
		{
			ArchiveEntries(Chosen, ModalParent);
		});
}

void KuraActions::RunCleanUnusedFlow(TSharedPtr<const SWidget> ModalParent)
{
	TArray<TSharedPtr<FKuraDeleteRow>> Rows;
	int32 NumDeps = 0;
	if (!BuildDeleteRows(TArray<FString>(), Rows, NumDeps, ModalParent))
	{
		return;
	}
	if (NumDeps == 0)
	{
		Notify(true, LOCTEXT("CleanNone", "Kura: no unused dependencies"));
		return;
	}
	ShowDeleteDialog(LOCTEXT("CleanTitle", "Kura - Clean Unused Dependencies"),
		FText::Format(LOCTEXT("CleanIntro",
			"{0} auto-collected {0}|plural(one=dependency,other=dependencies) in the library {0}|plural(one=is,other=are) referenced by nothing. Uncheck anything you want to keep - or mark it as a root instead, and it will stop showing up here."),
			NumDeps),
		Rows, ModalParent,
		[ModalParent](const TArray<FString>& Chosen)
		{
			ArchiveEntries(Chosen, ModalParent);
		});
}

/** Shared tail of the two folder operations (F4). Both send a different
 *  statement but come back the same way, and the guard refusal (entries still
 *  referenced) arrives as one dialog line per fact. NewRel is where the folder
 *  ends up; it reaches OnMoved only on success. Returns false when Python
 *  refused, so a prompt can reopen with the value still in the box. */
static bool RunFolderMoveCall(const FString& Statement, const FString& NewRel,
	TSharedPtr<const SWidget> ModalParent, TFunction<void(const FString&)> OnMoved)
{
	const TSharedPtr<FJsonObject> Result = KuraPython::Call(Statement);
	bool bOk = false;
	if (Result.IsValid())
	{
		Result->TryGetBoolField(TEXT("ok"), bOk);
	}
	if (!bOk)
	{
		KuraActions::ShowErrorsDialog(
			LOCTEXT("FolderMoveFailedTitle", "Kura - Move Folder"),
			ErrorsFromResult(Result), ModalParent);
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Moved = nullptr;
	const int32 Num = Result->TryGetArrayField(TEXT("moved"), Moved) ? Moved->Num() : 0;
	if (OnMoved)
	{
		OnMoved(NewRel); // before any broadcast: the refresh reads it
	}
	KuraActions::Notify(true, FText::Format(
		LOCTEXT("FolderMoved", "Kura: moved {0} {0}|plural(one=entry,other=entries) to {1}"),
		FText::AsNumber(Num), FText::FromString(NewRel)));
	KuraActions::OnLibraryChanged().Broadcast();
	return true;
}

/** The folder's last path segment - the part a rename replaces. */
static FString FolderLeafName(const FString& RelPath)
{
	FString Parent, Leaf;
	return RelPath.Split(TEXT("/"), &Parent, &Leaf, ESearchCase::CaseSensitive,
		ESearchDir::FromEnd) ? Leaf : RelPath;
}

void KuraActions::RunFolderMoveFlow(const FString& RelPath, const FString& NewParentRel,
	TSharedPtr<const SWidget> ModalParent, TFunction<void(const FString&)> OnMoved)
{
	// Python takes the folder's new full path, not its new parent: moving and
	// renaming are one operation there (kura.manage.move_folder).
	const FString Leaf = FolderLeafName(RelPath);
	const FString NewRel = NewParentRel.IsEmpty()
		? Leaf : NewParentRel + TEXT("/") + Leaf;
	RunFolderMoveCall(FString::Printf(TEXT("move_folder('%s', '%s')"),
		*KuraPython::EscapePyStr(RelPath), *KuraPython::EscapePyStr(NewRel)),
		NewRel, ModalParent, OnMoved);
}

void KuraActions::RunFolderRenameFlow(const FString& RelPath,
	TSharedPtr<const SWidget> ModalParent, TFunction<void(const FString&)> OnMoved)
{
	FString Parent;
	FString Leaf;
	if (!RelPath.Split(TEXT("/"), &Parent, &Leaf, ESearchCase::CaseSensitive,
		ESearchDir::FromEnd))
	{
		Parent.Empty();
		Leaf = RelPath;
	}
	ShowTextPrompt(
		LOCTEXT("RenameFolderTitle", "Kura - Rename Folder"),
		LOCTEXT("FolderNameLabel", "Name"),
		Leaf, ModalParent,
		[RelPath, Parent, ModalParent, OnMoved](const FString& Name,
			TFunction<void(const FString&)> Retry)
		{
			// What a folder name may contain is Python's rule (a name is one
			// path segment), so this just sends it and reopens on refusal.
			const FString NewRel = Parent.IsEmpty() ? Name : Parent + TEXT("/") + Name;
			if (!RunFolderMoveCall(FString::Printf(TEXT("rename_folder('%s', '%s')"),
				*KuraPython::EscapePyStr(RelPath), *KuraPython::EscapePyStr(Name)),
				NewRel, ModalParent, OnMoved))
			{
				Retry(Name);
			}
		},
		FText::Format(LOCTEXT("RenameFolderContext", "Folder: {0}"),
			FText::FromString(RelPath)));
}

void KuraActions::RunRethumbFlow(const TArray<FString>& Packages, TSharedPtr<const SWidget> ModalParent)
{
	if (Packages.Num() == 0)
	{
		return;
	}
	const TSharedPtr<FJsonObject> Result = KuraPython::Call(FString::Printf(
		TEXT("rerender_thumbnails('%s')"),
		*KuraPython::EscapePyStr(KuraPython::ToJsonArray(Packages))));
	bool bOk = false;
	if (Result.IsValid())
	{
		Result->TryGetBoolField(TEXT("ok"), bOk);
	}
	if (!bOk)
	{
		ShowErrorsDialog(LOCTEXT("RethumbFailedTitle", "Kura - Re-render Thumbnail"),
			ErrorsFromResult(Result), ModalParent);
	}
	else
	{
		Notify(true, LOCTEXT("RethumbDone", "Kura: thumbnails re-rendered"));
	}
	OnLibraryChanged().Broadcast();
}

void KuraActions::RunSetRootFlow(const TArray<FString>& Packages, bool bRoot,
	TSharedPtr<const SWidget> ModalParent)
{
	if (Packages.Num() == 0)
	{
		return;
	}
	const TSharedPtr<FJsonObject> Result = KuraPython::Call(FString::Printf(
		TEXT("set_roots('%s', %s)"),
		*KuraPython::EscapePyStr(KuraPython::ToJsonArray(Packages)),
		bRoot ? TEXT("True") : TEXT("False")));
	bool bOk = false;
	if (Result.IsValid())
	{
		Result->TryGetBoolField(TEXT("ok"), bOk);
	}
	if (!bOk)
	{
		ShowErrorsDialog(LOCTEXT("SetRootFailedTitle", "Kura - Set Root"),
			ErrorsFromResult(Result), ModalParent);
	}
	else
	{
		Notify(true, bRoot
			? LOCTEXT("SetRootDone", "Kura: marked as root")
			: LOCTEXT("UnsetRootDone", "Kura: unmarked as root"));
	}
	OnLibraryChanged().Broadcast();
}

void KuraActions::RunCreateFoldersFlow(TSharedPtr<const SWidget> ModalParent)
{
	const TSharedPtr<FJsonObject> Result = KuraPython::Call(TEXT("create_project_folders()"));
	bool bOk = false;
	if (Result.IsValid())
	{
		Result->TryGetBoolField(TEXT("ok"), bOk);
	}
	if (!bOk)
	{
		Notify(false, LOCTEXT("PyFailed", "Kura: Python call failed - see Output Log"));
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* Created = nullptr;
	const int32 NumCreated = Result->TryGetArrayField(TEXT("created"), Created) ? Created->Num() : 0;
	Notify(true, NumCreated > 0
		? LOCTEXT("FoldersCreated", "Kura: created /Game/_AssetLib in this project")
		: LOCTEXT("FoldersExist", "Kura: /Game/_AssetLib already exists in this project"));
}

bool KuraActions::RunCreateLibraryFlow(const FString& InRoot,
	TSharedPtr<const SWidget> ModalParent)
{
	const FText Title = LOCTEXT("CreateLibTitle", "Kura - Library Not Created");
	const FString Root = InRoot.TrimStartAndEnd();
	if (Root.IsEmpty())
	{
		ShowErrorsDialog(Title, { LOCTEXT("CreateLibNoRoot",
			"Choose a folder for the library first.").ToString() }, ModalParent);
		return false;
	}
	// A relative path resolves against the editor's working directory, which
	// is the engine's Binaries folder - a library scattered under Program
	// Files is not what anyone typing "Kura Library" meant.
	if (FPaths::IsRelative(Root))
	{
		ShowErrorsDialog(Title, { FString::Printf(TEXT(
			"\"%s\" is not a full path. Give a drive and folder - for example "
			"C:\\Users\\you\\Documents\\Kura Library - or pick one with the "
			"folder button."), *Root) }, ModalParent);
		return false;
	}
	if (!KuraLibrary::SaveConfig({ { TEXT("library_root"), Root } }, {}))
	{
		ShowErrorsDialog(Title, { FString::Printf(TEXT(
			"Could not write the settings file %s."),
			*KuraLibrary::GetConfigFilePath()) }, ModalParent);
		return false;
	}
	const TSharedPtr<FJsonObject> Result = KuraPython::Call(TEXT("init_library()"));
	bool bOk = false;
	if (Result.IsValid())
	{
		Result->TryGetBoolField(TEXT("ok"), bOk);
	}
	if (!bOk)
	{
		// The config now points at a root that could not be created. That is
		// on purpose: the panel's missing-root state shows the path and offers
		// this flow again, so the failed attempt is visible and fixable rather
		// than forgotten.
		ShowErrorsDialog(Title, ErrorsFromResult(Result), ModalParent);
		return false;
	}
	Notify(true, LOCTEXT("CreateLibDone", "Kura: library ready"));
	OnLibraryChanged().Broadcast();
	return true;
}

// --- Collections (SPEC F8) -------------------------------------------------
// Every one of these is a metadata edit: no asset is moved, copied, renamed or
// archived, so they apply straight away with no preview step.

void KuraActions::RunCollectionCreateFlow(const FString& ParentId,
	TSharedPtr<const SWidget> ModalParent, const TArray<FString>& AssignPackages,
	TFunction<void(const FString&)> OnCreated)
{
	// Say where this will land. The same dialog is reachable from the header
	// +, a right-clicked collection and the tile menu, and the only thing
	// that told them apart used to be which entry you happened to click.
	FString ParentPath;
	if (!ParentId.IsEmpty())
	{
		for (const TSharedPtr<FKuraCollectionOption>& Opt : FetchCollections())
		{
			if (Opt->Id == ParentId)
			{
				ParentPath = Opt->Path;
				break;
			}
		}
	}
	const FText Context = ParentPath.IsEmpty()
		? LOCTEXT("CreateInTopLevel", "Create in: Top Level")
		: FText::Format(LOCTEXT("CreateInCollection", "Create in: {0}"),
			FText::FromString(ParentPath));

	ShowTextPrompt(
		LOCTEXT("NewCollectionTitle", "Kura - New Collection"),
		LOCTEXT("CollectionNameLabel", "Name"),
		FString(), ModalParent,
		[ParentId, ModalParent, AssignPackages, OnCreated](const FString& Name,
			TFunction<void(const FString&)> Retry)
		{
			const TSharedPtr<FJsonObject> Result = KuraPython::Call(
				FString::Printf(TEXT("collection_create('%s', '%s')"),
					*KuraPython::EscapePyStr(Name),
					*KuraPython::EscapePyStr(ParentId)));
			bool bOk = false;
			if (Result.IsValid())
			{
				Result->TryGetBoolField(TEXT("ok"), bOk);
			}
			if (!bOk)
			{
				ShowErrorsDialog(LOCTEXT("CollectionFailedTitle", "Kura - Collection"),
					ErrorsFromResult(Result), ModalParent);
				Retry(Name);
				return;
			}
			FString NewId;
			Result->TryGetStringField(TEXT("id"), NewId);
			if (OnCreated)
			{
				OnCreated(NewId); // before any broadcast: the refresh reads it
			}
			if (AssignPackages.Num() > 0 && !NewId.IsEmpty())
			{
				RunCollectionAssignFlow(AssignPackages, NewId, /*bAdd*/ true, ModalParent);
				return; // that call notifies and broadcasts
			}
			Notify(true, FText::Format(LOCTEXT("CollectionCreated",
				"Kura: created collection \"{0}\""), FText::FromString(Name)));
			OnLibraryChanged().Broadcast();
		},
		Context);
}

void KuraActions::RunCollectionRenameFlow(const FString& Id, const FString& CurrentName,
	TSharedPtr<const SWidget> ModalParent)
{
	ShowTextPrompt(
		LOCTEXT("RenameCollectionTitle", "Kura - Rename Collection"),
		LOCTEXT("CollectionNameLabel", "Name"),
		CurrentName, ModalParent,
		[Id, ModalParent](const FString& Name, TFunction<void(const FString&)> Retry)
		{
			const TSharedPtr<FJsonObject> Result = KuraPython::Call(
				FString::Printf(TEXT("collection_rename('%s', '%s')"),
					*KuraPython::EscapePyStr(Id), *KuraPython::EscapePyStr(Name)));
			bool bOk = false;
			if (Result.IsValid())
			{
				Result->TryGetBoolField(TEXT("ok"), bOk);
			}
			if (!bOk)
			{
				ShowErrorsDialog(LOCTEXT("CollectionFailedTitle", "Kura - Collection"),
					ErrorsFromResult(Result), ModalParent);
				Retry(Name);
				return;
			}
			OnLibraryChanged().Broadcast();
		});
}

void KuraActions::RunCollectionDeleteFlow(const FString& Id, const FString& Name,
	TSharedPtr<const SWidget> ModalParent)
{
	// Worth confirming even though nothing is destroyed: nested collections go
	// with it, and rebuilding a hierarchy by hand is tedious.
	const EAppReturnType::Type Answer = FMessageDialog::Open(EAppMsgType::OkCancel,
		FText::Format(LOCTEXT("CollectionDeleteConfirm",
			"Delete the collection \"{0}\" and any collections inside it?\n\n"
			"No assets are deleted - they stay in the library and simply stop being filed here."),
			FText::FromString(Name)));
	if (Answer != EAppReturnType::Ok)
	{
		return;
	}
	const TSharedPtr<FJsonObject> Result = KuraPython::Call(
		FString::Printf(TEXT("collection_delete('%s')"), *KuraPython::EscapePyStr(Id)));
	bool bOk = false;
	if (Result.IsValid())
	{
		Result->TryGetBoolField(TEXT("ok"), bOk);
	}
	if (!bOk)
	{
		ShowErrorsDialog(LOCTEXT("CollectionFailedTitle", "Kura - Collection"),
			ErrorsFromResult(Result), ModalParent);
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* Unfiled = nullptr;
	const int32 NumUnfiled = Result->TryGetArrayField(TEXT("unfiled"), Unfiled) ? Unfiled->Num() : 0;
	Notify(true, FText::Format(LOCTEXT("CollectionDeleted",
		"Kura: deleted \"{0}\"; {1} {1}|plural(one=entry,other=entries) unfiled"),
		FText::FromString(Name), FText::AsNumber(NumUnfiled)));
	OnLibraryChanged().Broadcast();
}

void KuraActions::RunCollectionAssignFlow(const TArray<FString>& Packages, const FString& Id,
	bool bAdd, TSharedPtr<const SWidget> ModalParent)
{
	if (Packages.Num() == 0 || Id.IsEmpty())
	{
		return;
	}
	const TSharedPtr<FJsonObject> Result = KuraPython::Call(
		FString::Printf(TEXT("collection_assign('%s', '%s', %s)"),
			*KuraPython::EscapePyStr(KuraPython::ToJsonArray(Packages)),
			*KuraPython::EscapePyStr(Id),
			*(bAdd ? FString(TEXT("True")) : FString(TEXT("False")))));
	bool bOk = false;
	if (Result.IsValid())
	{
		Result->TryGetBoolField(TEXT("ok"), bOk);
	}
	if (!bOk)
	{
		ShowErrorsDialog(LOCTEXT("CollectionFailedTitle", "Kura - Collection"),
			ErrorsFromResult(Result), ModalParent);
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	const int32 NumChanged = Result->TryGetArrayField(TEXT("changed"), Changed) ? Changed->Num() : 0;
	Notify(true, bAdd
		? FText::Format(LOCTEXT("CollectionAdded",
			"Kura: filed {0} {0}|plural(one=entry,other=entries)"), FText::AsNumber(NumChanged))
		: FText::Format(LOCTEXT("CollectionRemoved",
			"Kura: unfiled {0} {0}|plural(one=entry,other=entries)"), FText::AsNumber(NumChanged)));
	OnLibraryChanged().Broadcast();
}

void KuraActions::RunCollectionMoveFlow(const FString& Id, const FString& ParentId,
	TSharedPtr<const SWidget> ModalParent)
{
	// order < 0 = append to the new parent's row (kura.editor_api contract).
	const TSharedPtr<FJsonObject> Result = KuraPython::Call(
		FString::Printf(TEXT("collection_move('%s', '%s', -1)"),
			*KuraPython::EscapePyStr(Id), *KuraPython::EscapePyStr(ParentId)));
	bool bOk = false;
	if (Result.IsValid())
	{
		Result->TryGetBoolField(TEXT("ok"), bOk);
	}
	if (!bOk)
	{
		ShowErrorsDialog(LOCTEXT("CollectionFailedTitle", "Kura - Collection"),
			ErrorsFromResult(Result), ModalParent);
		return;
	}
	OnLibraryChanged().Broadcast();
}

void KuraActions::RunCollectionImportFlow(const FString& Id, TSharedPtr<const SWidget> ModalParent)
{
	const TSharedPtr<FJsonObject> Result = KuraPython::Call(
		FString::Printf(TEXT("collection_members('%s')"), *KuraPython::EscapePyStr(Id)));
	bool bOk = false;
	if (Result.IsValid())
	{
		Result->TryGetBoolField(TEXT("ok"), bOk);
	}
	if (!bOk)
	{
		ShowErrorsDialog(LOCTEXT("CollectionFailedTitle", "Kura - Collection"),
			ErrorsFromResult(Result), ModalParent);
		return;
	}
	TArray<FString> Packages;
	const TArray<TSharedPtr<FJsonValue>>* Members = nullptr;
	if (Result->TryGetArrayField(TEXT("packages"), Members))
	{
		for (const TSharedPtr<FJsonValue>& V : *Members)
		{
			Packages.Add(V->AsString());
		}
	}
	if (Packages.Num() == 0)
	{
		Notify(false, LOCTEXT("CollectionEmpty", "Kura: this collection is empty"));
		return;
	}
	// Regular F3 import from here: the dependency closure and the conflict
	// prompts are exactly the same as importing the entries by hand.
	RunImportFlow(Packages, ModalParent);
}

void KuraActions::RunArchiveFlow(TSharedPtr<const SWidget> ModalParent)
{
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("ArchiveTitle", "Kura - Recently Deleted"))
		.ClientSize(FVector2D(640.f, 420.f))
		.SupportsMaximize(false)
		.SupportsMinimize(false);
	const TWeakPtr<SWindow> WeakWindow = Window;

	const TSharedRef<SVerticalBox> ListBox = SNew(SVerticalBox);
	// Rebuild re-queries list_archive() and refills the rows; shared so the
	// row buttons can trigger it after restore/delete.
	const TSharedRef<TFunction<void()>> Rebuild = MakeShared<TFunction<void()>>();
	*Rebuild = [ListBox, Rebuild, ModalParent]()
	{
		ListBox->ClearChildren();
		const TSharedPtr<FJsonObject> Result = KuraPython::Call(TEXT("list_archive()"));
		const TArray<TSharedPtr<FJsonValue>>* Batches = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("batches"), Batches))
		{
			return;
		}
		if (Batches->Num() == 0)
		{
			ListBox->AddSlot().AutoHeight().Padding(0.f, 12.f).HAlign(HAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ArchiveEmpty",
					"Nothing here. Deleted entries and overwritten library versions land in _archive and can be restored from this list."))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
			];
			return;
		}
		for (const TSharedPtr<FJsonValue>& V : *Batches)
		{
			const TSharedPtr<FJsonObject> Batch = V->AsObject();
			if (!Batch.IsValid())
			{
				continue;
			}
			FString Name;
			Batch->TryGetStringField(TEXT("batch"), Name);
			double Bytes = 0.0;
			Batch->TryGetNumberField(TEXT("bytes"), Bytes);
			TArray<FString> Entries;
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (Batch->TryGetArrayField(TEXT("entries"), Arr))
			{
				for (const TSharedPtr<FJsonValue>& E : *Arr)
				{
					Entries.Add(E->AsString());
				}
			}
			// Meta / thumbnails archived without their asset file. A batch
			// holding only those used to read "0 entries" next to a size.
			double Orphans = 0.0;
			Batch->TryGetNumberField(TEXT("orphans"), Orphans);
			const FText BatchInfo = Orphans > 0.0
				? FText::Format(LOCTEXT("BatchInfoOrphans",
					"{0} {0}|plural(one=entry,other=entries), {1} metadata-only (no asset file to restore)  -  {2}"),
					Entries.Num(), static_cast<int32>(Orphans),
					FText::FromString(FormatBytes(Bytes)))
				: FText::Format(LOCTEXT("BatchInfo",
					"{0} {0}|plural(one=entry,other=entries)  -  {1}"),
					FText::AsNumber(Entries.Num()),
					FText::FromString(FormatBytes(Bytes)));

			ListBox->AddSlot().AutoHeight().Padding(0.f, 2.f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
				.Padding(8.f, 6.f)
				.ToolTipText(FText::FromString(FString::Join(Entries, TEXT("\n"))))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(STextBlock)
							.Text(FText::FromString(FormatBatchName(Name)))
							.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(STextBlock)
							.Text(BatchInfo)
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.f, 0.f, 0.f, 0.f)
					[
						SNew(SButton)
						.Text(LOCTEXT("BatchRestore", "Restore"))
						.ToolTipText(LOCTEXT("BatchRestoreTip",
							"Move this batch back into the library. Entries whose path is already taken stay archived and are reported."))
						.OnClicked_Lambda([Name, Rebuild, ModalParent]()
						{
							const TSharedPtr<FJsonObject> R = KuraPython::Call(FString::Printf(
								TEXT("restore_archive('%s')"), *KuraPython::EscapePyStr(Name)));
							bool bOk = false;
							if (R.IsValid())
							{
								R->TryGetBoolField(TEXT("ok"), bOk);
							}
							if (!bOk)
							{
								KuraActions::ShowErrorsDialog(LOCTEXT("RestoreFailedTitle", "Kura - Restore Failed"),
									ErrorsFromResult(R), ModalParent);
							}
							else
							{
								const TArray<TSharedPtr<FJsonValue>>* Restored = nullptr;
								const TArray<TSharedPtr<FJsonValue>>* Blocked = nullptr;
								const int32 NumRestored = R->TryGetArrayField(TEXT("restored"), Restored) ? Restored->Num() : 0;
								if (R->TryGetArrayField(TEXT("blocked"), Blocked) && Blocked->Num() > 0)
								{
									TArray<FString> Lines;
									Lines.Add(FString::Printf(TEXT("Restored %d; blocked %d:"), NumRestored, Blocked->Num()));
									for (const TSharedPtr<FJsonValue>& B : *Blocked)
									{
										Lines.Add(TEXT("    ") + B->AsString());
									}
									KuraActions::ShowErrorsDialog(
										LOCTEXT("RestorePartialTitle", "Kura - Restore Partially Blocked"), Lines, ModalParent);
								}
								else
								{
									KuraActions::Notify(true, FText::Format(
										LOCTEXT("RestoreDone", "Kura: restored {0} {0}|plural(one=entry,other=entries)"),
										FText::AsNumber(NumRestored)));
								}
								KuraActions::OnLibraryChanged().Broadcast();
							}
							(*Rebuild)();
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.f, 0.f, 0.f, 0.f)
					[
						SNew(SButton)
						.Text(LOCTEXT("BatchDelete", "Delete"))
						.ToolTipText(LOCTEXT("BatchDeleteTip", "Permanently delete this batch. Cannot be undone."))
						.OnClicked_Lambda([Name, Entries, Bytes, Rebuild]()
						{
							const EAppReturnType::Type Answer = FMessageDialog::Open(EAppMsgType::YesNo,
								FText::Format(LOCTEXT("BatchDeleteAsk",
									"Permanently delete archive batch {0} ({1} {1}|plural(one=entry,other=entries), {2})?\n\nThis cannot be undone."),
									FText::FromString(FormatBatchName(Name)),
									FText::AsNumber(Entries.Num()),
									FText::FromString(FormatBytes(Bytes))));
							if (Answer == EAppReturnType::Yes)
							{
								const TSharedPtr<FJsonObject> R = KuraPython::Call(FString::Printf(
									TEXT("delete_archive_batch('%s')"), *KuraPython::EscapePyStr(Name)));
								bool bOk = false;
								if (R.IsValid())
								{
									R->TryGetBoolField(TEXT("ok"), bOk);
								}
								KuraActions::Notify(bOk, bOk
									? LOCTEXT("BatchDeleted", "Kura: archive batch deleted")
									: LOCTEXT("BatchDeleteFailed", "Kura: could not delete the batch - see Output Log"));
								(*Rebuild)();
							}
							return FReply::Handled();
						})
					]
				]
			];
		}
	};
	(*Rebuild)();

	Window->SetContent(
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(10.f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				SNew(SScrollBox) + SScrollBox::Slot() [ ListBox ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SNew(SBox) // spacer
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
				[
					SNew(SButton)
					.Text(LOCTEXT("ArchiveClearAll", "Clear All"))
					.ToolTipText(LOCTEXT("ArchiveClearAllTip", "Permanently delete every archived batch. Cannot be undone."))
					.OnClicked_Lambda([Rebuild]()
					{
						const EAppReturnType::Type Answer = FMessageDialog::Open(EAppMsgType::YesNo,
							LOCTEXT("ArchiveClearAsk",
								"Permanently delete ALL archived batches?\n\nThis cannot be undone."));
						if (Answer == EAppReturnType::Yes)
						{
							const TSharedPtr<FJsonObject> R = KuraPython::Call(TEXT("clear_archive()"));
							bool bOk = false;
							if (R.IsValid())
							{
								R->TryGetBoolField(TEXT("ok"), bOk);
							}
							double Freed = 0.0;
							if (bOk)
							{
								R->TryGetNumberField(TEXT("freed_bytes"), Freed);
							}
							KuraActions::Notify(bOk, bOk
								? FText::Format(LOCTEXT("ArchiveCleared", "Kura: archive cleared ({0} freed)"),
									FText::FromString(FormatBytes(Freed)))
								: LOCTEXT("ArchiveClearFailed", "Kura: could not clear the archive - see Output Log"));
							(*Rebuild)();
						}
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("Close", "Close"))
					.OnClicked_Lambda([WeakWindow]()
					{
						if (const TSharedPtr<SWindow> W = WeakWindow.Pin())
						{
							W->RequestDestroyWindow();
						}
						return FReply::Handled();
					})
				]
			]
		]);
	FSlateApplication::Get().AddModalWindow(Window, ModalParent);
}

namespace
{
	/** Native save/open dialog filtered to .kurapack (F7). */
	bool PickPackageFile(bool bSave, const FString& DefaultName, FString& OutPath)
	{
		IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
		if (!Desktop)
		{
			return false;
		}
		const void* ParentHandle =
			FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
		const FString Filter = TEXT("Kura Package (*.kurapack)|*.kurapack");
		TArray<FString> Files;
		const bool bPicked = bSave
			? Desktop->SaveFileDialog(ParentHandle,
				LOCTEXT("ExportPackTitle", "Export Kura Package").ToString(),
				FString(), DefaultName, Filter, EFileDialogFlags::None, Files)
			: Desktop->OpenFileDialog(ParentHandle,
				LOCTEXT("ImportPackTitle", "Import Kura Package").ToString(),
				FString(), FString(), Filter, EFileDialogFlags::None, Files);
		if (!bPicked || Files.Num() == 0)
		{
			return false;
		}
		OutPath = FPaths::ConvertRelativePathToFull(Files[0]);
		return true;
	}

	/** Preview + decisions + commit for one package file; recurses once with
	 *  the guard forced when the user confirms (same contract as F3 import). */
	void ImportPackageAtPath(const FString& PackPath,
		TSharedPtr<const SWidget> ModalParent, bool bForceVersion)
	{
		const FString PathArg = KuraPython::EscapePyStr(PackPath);
		const FString Force = bForceVersion ? TEXT("True") : TEXT("False");
		const TSharedPtr<FJsonObject> Preview = KuraPython::Call(
			FString::Printf(TEXT("package_preview('%s', %s)"), *PathArg, *Force));
		if (!Preview.IsValid())
		{
			KuraActions::Notify(false, LOCTEXT("PyFailed", "Kura: Python call failed - see Output Log"));
			return;
		}
		bool bOk = false;
		Preview->TryGetBoolField(TEXT("ok"), bOk);
		if (!bOk)
		{
			TArray<FString> Errors = ErrorsFromResult(Preview);
			const bool bVersionGuard = Errors.ContainsByPredicate(
				[](const FString& E) { return E.Contains(TEXT("newer-engine")); });
			if (bVersionGuard && !bForceVersion)
			{
				const EAppReturnType::Type Answer = FMessageDialog::Open(EAppMsgType::YesNo,
					FText::Format(LOCTEXT("ForceVersionAsk",
						"{0}\n\nImport anyway (force past the engine version guard)?"),
						FText::FromString(FString::Join(Errors, TEXT("\n")))));
				if (Answer == EAppReturnType::Yes)
				{
					ImportPackageAtPath(PackPath, ModalParent, /*bForceVersion*/ true);
				}
				return;
			}
			KuraActions::ShowErrorsDialog(
				LOCTEXT("PackBlockedTitle", "Kura - Import Package Blocked"), Errors, ModalParent);
			return;
		}

		FString PackName = FPaths::GetBaseFilename(PackPath);
		// How many collections the package brought, and how many of them this
		// library does not have yet (SPEC F7 import step 5). Zero total = the
		// package predates the feature or was never filed: the picker then
		// shows only Unsorted plus this library's own collections, so the
		// behaviour is exactly what it was before packages carried filing.
		int32 PackCollections = 0, NewCollections = 0;
		const TSharedPtr<FJsonObject>* Manifest = nullptr;
		if (Preview->TryGetObjectField(TEXT("manifest"), Manifest))
		{
			FString ManifestName;
			if ((*Manifest)->TryGetStringField(TEXT("name"), ManifestName) && !ManifestName.IsEmpty())
			{
				PackName = ManifestName;
			}
			const TSharedPtr<FJsonObject>* Cols = nullptr;
			if ((*Manifest)->TryGetObjectField(TEXT("collections"), Cols))
			{
				(*Cols)->TryGetNumberField(TEXT("total"), PackCollections);
				(*Cols)->TryGetNumberField(TEXT("new"), NewCollections);
			}
		}

		FKuraCollectionPicker Picker;
		Picker.Selected = MakeShared<FString>();
		if (PackCollections > 0)
		{
			Picker.PackOptionLabel = LOCTEXT("CollectionFromPack",
				"Use the package's collections");
			Picker.ToolTip = FText::Format(LOCTEXT("PackCollectionTip",
				"Where the imported assets get filed. \"Use the package's collections\" rebuilds the {0} {0}|plural(one=collection,other=collections) this package was filed under, reusing yours where the names match and adding the {1} you do not have yet. Pick one of your own collections instead to drop everything into that one, flat. Either way only roots are filed - a shared master belongs to everything that uses it."),
				PackCollections, NewCollections);
		}

		const TArray<TSharedPtr<FKuraPreviewRow>> Rows = RowsFromPreview(Preview, TEXT("keep"));
		ShowPreviewDialog(
			FText::Format(LOCTEXT("PackPreviewTitle", "Kura - Import Package: {0}"),
				FText::FromString(PackName)),
			LOCTEXT("PackWarning",
				"Update: the current library version is moved to _archive, then replaced by the package version. Ignore: this entry is not imported and the library keeps its current version."),
			Rows,
			{ { TEXT("update"), LOCTEXT("DecUpdate", "Update") },
			  { TEXT("keep"), LOCTEXT("DecIgnore", "Ignore") } },
			/*DepsAsRootsFlag*/ nullptr,
			/*BatchTags*/ nullptr,
			Picker,
			ModalParent,
			[ModalParent, PathArg, Force, Chosen = Picker.Selected](const TMap<FString, FString>& Decisions)
			{
				const TSharedPtr<FJsonObject> Result = KuraPython::Call(FString::Printf(
					TEXT("package_commit('%s', '%s', %s, '%s')"), *PathArg,
					*KuraPython::EscapePyStr(KuraPython::ToJsonObject(Decisions)), *Force,
					*KuraPython::EscapePyStr(*Chosen)));
				bool bCommitOk = false;
				if (Result.IsValid())
				{
					Result->TryGetBoolField(TEXT("ok"), bCommitOk);
				}
				if (!bCommitOk)
				{
					KuraActions::ShowErrorsDialog(
						LOCTEXT("PackFailedTitle", "Kura - Import Package Failed"),
						ErrorsFromResult(Result), ModalParent);
				}
				else
				{
					const auto Count = [&Result](const TCHAR* Field) -> int32
					{
						const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
						return Result->TryGetArrayField(Field, Arr) ? Arr->Num() : 0;
					};
					int32 Filed = 0, Created = 0;
					FString FilingError;
					const TSharedPtr<FJsonObject>* Cols = nullptr;
					if (Result->TryGetObjectField(TEXT("collections"), Cols))
					{
						const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
						if ((*Cols)->TryGetArrayField(TEXT("filed"), Arr)) { Filed = Arr->Num(); }
						if ((*Cols)->TryGetArrayField(TEXT("created"), Arr)) { Created = Arr->Num(); }
						(*Cols)->TryGetStringField(TEXT("error"), FilingError);
					}
					KuraActions::Notify(true, FText::Format(
						LOCTEXT("PackDone", "Kura: package imported - {0} added, {1} updated, {2} skipped"),
						FText::AsNumber(Count(TEXT("added"))),
						FText::AsNumber(Count(TEXT("updated"))),
						FText::AsNumber(Count(TEXT("skipped")))));
					if (Filed > 0)
					{
						KuraActions::Notify(true, Created > 0
							? FText::Format(LOCTEXT("PackFiledNew",
								"Kura: filed {0} {0}|plural(one=asset,other=assets), creating {1} {1}|plural(one=collection,other=collections)"),
								Filed, Created)
							: FText::Format(LOCTEXT("PackFiledExisting",
								"Kura: filed {0} {0}|plural(one=asset,other=assets) into existing collections"),
								Filed));
					}
					if (!FilingError.IsEmpty())
					{
						// The assets are in; only the filing tripped, so this
						// is a warning next to a successful import.
						KuraActions::Notify(false, FText::Format(
							LOCTEXT("PackFilingFailed",
								"Kura: assets imported, but filing them into collections failed - {0}"),
							FText::FromString(FilingError)));
					}
				}
				KuraActions::OnLibraryChanged().Broadcast();
			});
	}
}

void KuraActions::RunExportPackageFlow(const TArray<FString>& Packages,
	TSharedPtr<const SWidget> ModalParent, const FString& DefaultNameHint)
{
	if (Packages.Num() == 0)
	{
		return;
	}
	FString BaseName = DefaultNameHint;
	if (BaseName.IsEmpty())
	{
		BaseName = Packages.Num() == 1
			? FPaths::GetBaseFilename(Packages[0]) : FString(TEXT("KuraPackage"));
	}
	FString OutPath;
	if (!PickPackageFile(/*bSave*/ true, BaseName + TEXT(".kurapack"), OutPath))
	{
		return;
	}
	const TSharedPtr<FJsonObject> Result = KuraPython::Call(FString::Printf(
		TEXT("package_export('%s', '%s', '%s')"),
		*KuraPython::EscapePyStr(KuraPython::ToJsonArray(Packages)),
		*KuraPython::EscapePyStr(OutPath),
		*KuraPython::EscapePyStr(FPaths::GetBaseFilename(OutPath))));
	bool bOk = false;
	if (Result.IsValid())
	{
		Result->TryGetBoolField(TEXT("ok"), bOk);
	}
	if (!bOk)
	{
		ShowErrorsDialog(LOCTEXT("ExportPackFailedTitle", "Kura - Export Package Failed"),
			ErrorsFromResult(Result), ModalParent);
		return;
	}
	double Entries = 0.0, Bytes = 0.0;
	Result->TryGetNumberField(TEXT("entries"), Entries);
	Result->TryGetNumberField(TEXT("bytes"), Bytes);
	FString FinalPath = OutPath;
	Result->TryGetStringField(TEXT("path"), FinalPath); // Python appends .kurapack if missing
	Notify(true, FText::Format(
		LOCTEXT("ExportPackDone", "Kura: exported {0} {0}|plural(one=entry,other=entries) ({1}) to {2}"),
		FText::AsNumber(static_cast<int32>(Entries)),
		FText::FromString(FormatBytes(Bytes)),
		FText::FromString(FPaths::GetCleanFilename(FinalPath))));
}

void KuraActions::RunImportPackageFlow(TSharedPtr<const SWidget> ModalParent)
{
	FString PackPath;
	if (!PickPackageFile(/*bSave*/ false, FString(), PackPath))
	{
		return;
	}
	ImportPackageAtPath(PackPath, ModalParent, /*bForceVersion*/ false);
}

void KuraActions::RunRescanFlow(TSharedPtr<const SWidget> ModalParent)
{
	// Verify hashes every file in the library; on a big library that is a long
	// blocking pass with no progress feedback, so say so before starting.
	if (FMessageDialog::Open(EAppMsgType::OkCancel,
		LOCTEXT("RescanAsk",
			"Verify Library reads every file in the library, checks it against its metadata and rebuilds the index.\n\nWith a lot of assets this can take a while, and the editor will not respond until it finishes.\n\nContinue?"),
		LOCTEXT("RescanAskTitle", "Kura - Verify Library")) != EAppReturnType::Ok)
	{
		return;
	}
	const TSharedPtr<FJsonObject> Result = KuraPython::Call(TEXT("rescan()"));
	if (!Result.IsValid())
	{
		Notify(false, LOCTEXT("PyFailed", "Kura: Python call failed - see Output Log"));
		return;
	}
	TArray<FString> Lines;
	double Entries = 0.0;
	Result->TryGetNumberField(TEXT("entries"), Entries);
	Lines.Add(FString::Printf(TEXT("%d entr%s indexed"), (int32)Entries, (int32)Entries == 1 ? TEXT("y") : TEXT("ies")));
	const auto AddList = [&Result, &Lines](const TCHAR* Field, const TCHAR* Label)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Result->TryGetArrayField(Field, Arr) && Arr->Num() > 0)
		{
			Lines.Add(FString::Printf(TEXT("%s (%d):"), Label, Arr->Num()));
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				Lines.Add(TEXT("    ") + V->AsString());
			}
		}
	};
	AddList(TEXT("unreadable"),
		TEXT("Damaged metadata - hidden from the panel; send the asset to Kura again or restore the .json from a backup"));
	AddList(TEXT("unregistered"), TEXT("Files without meta"));
	AddList(TEXT("missing"), TEXT("Meta without file"));
	AddList(TEXT("modified"), TEXT("Externally modified"));
	AddList(TEXT("orphans"), TEXT("Unused dependencies (Clean Unused Dependencies archives these)"));
	const TSharedPtr<FJsonObject>* Broken = nullptr;
	if (Result->TryGetObjectField(TEXT("broken_deps"), Broken) && (*Broken)->Values.Num() > 0)
	{
		Lines.Add(FString::Printf(TEXT("Broken references (%d):"), (*Broken)->Values.Num()));
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Broken)->Values)
		{
			Lines.Add(TEXT("    ") + Pair.Key);
		}
	}
	AddList(TEXT("collections_error"),
		TEXT("Collections file could not be read - no memberships were cleared"));
	AddList(TEXT("pruned_collections"),
		TEXT("Cleared memberships pointing at a deleted collection"));
	AddList(TEXT("newer_schema"),
		TEXT("Written by a newer Kura - readable here, but not editable until you update"));
	if (Lines.Num() == 1)
	{
		Lines.Add(TEXT("No drift detected."));
	}
	ShowErrorsDialog(LOCTEXT("RescanTitle", "Kura - Rescan Report"), Lines, ModalParent);
	OnLibraryChanged().Broadcast();
}

#undef LOCTEXT_NAMESPACE
