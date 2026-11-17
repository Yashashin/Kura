#include "SKuraBrowser.h"

#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "IAssetTools.h"
#include "KuraActions.h"
#include "KuraPbrImporter.h"
#include "KuraPython.h"
#include "KuraStyle.h"
#include "KuraThumbCache.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/ISlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Textures/SlateIcon.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SHyperlink.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Views/STileView.h"
#include "Widgets/Views/STreeView.h"

#define LOCTEXT_NAMESPACE "Kura"

namespace
{
	/** A class tab's button icon. A plugin-owned class takes its icon from the
	 *  plugin's style set, which is only registered while that plugin is
	 *  enabled in this project - the fallback keeps the tab from showing the
	 *  missing-brush placeholder there (SPEC F9). */
	const FSlateBrush* ClassTabIcon(const KuraLibrary::FKuraClassTab& Tab)
	{
		if (!Tab.StyleSet)
		{
			return FAppStyle::GetBrush(Tab.Icon);
		}
		if (const ISlateStyle* Style = FSlateStyleRegistry::FindSlateStyle(Tab.StyleSet))
		{
			if (const FSlateBrush* Brush = Style->GetOptionalBrush(Tab.Icon, nullptr, nullptr))
			{
				return Brush;
			}
		}
		return FAppStyle::GetBrush(Tab.FallbackIcon);
	}

	/** Icon button that opens the native folder picker and writes the chosen
	 *  path back into the given text box (Library Root fields, F5/setup).
	 *  OnPicked (optional) receives the chosen path: SetText raises no
	 *  OnTextChanged, so a caller tracking the field's value has no other way
	 *  to hear about this. */
	TSharedRef<SWidget> MakeBrowseButton(TSharedRef<SEditableTextBox> RootBox,
		TFunction<void(const FString&)> OnPicked = nullptr)
	{
		return SNew(SButton)
			.ToolTipText(LOCTEXT("BrowseTip", "Choose the library folder"))
			.VAlign(VAlign_Center)
			.OnClicked_Lambda([RootBox, OnPicked]()
			{
				FString Picked;
				if (KuraLibrary::PickDirectory(LOCTEXT("PickRootTitle", "Choose Library Folder").ToString(),
					RootBox->GetText().ToString(), Picked))
				{
					RootBox->SetText(FText::FromString(Picked));
					if (OnPicked)
					{
						OnPicked(Picked);
					}
				}
				return FReply::Handled();
			})
			[
				// 16x16 box with a smaller glyph, same as the toolbar's Refresh
				SNew(SBox)
				.WidthOverride(16.f).HeightOverride(16.f)
				.HAlign(HAlign_Center).VAlign(VAlign_Center)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.FolderOpen"))
					.DesiredSizeOverride(FVector2D(12.f, 12.f))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			];
	}
}

void SKuraBrowser::Construct(const FArguments& InArgs)
{
	// Collections is the primary view (Q23); the physical tree stays one click
	// away. Remembered so the panel opens the way it was last used.
	bCollectionsView = KuraLibrary::GetConfigNumber(TEXT("collections_view"), 1.0) != 0.0;
	bShowDeps = KuraLibrary::GetConfigNumber(TEXT("show_dependencies"), 0.0) != 0.0;
	TileSize = FMath::Clamp((float)KuraLibrary::GetConfigNumber(TEXT("tile_size"), 150.0), 90.f, 260.f);
	SortMode = FMath::Clamp((int32)KuraLibrary::GetConfigNumber(TEXT("sort_mode"), 0.0), 0, 2);
	bSortAscending = KuraLibrary::GetConfigNumber(TEXT("sort_ascending"), 1.0) != 0.0;

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(4.f)
		[
			MakeToolbar()
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(4.f, 0.f, 4.f, 4.f).HAlign(HAlign_Left)
		[
			MakeTypeTabs()
		]

		// Body: folder tree + tags | tiles | details
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SNew(SSplitter)

			+ SSplitter::Slot().Value(0.18f)
			[
				MakeLeftColumn()
			]

			+ SSplitter::Slot().Value(0.57f)
			[
				MakeTilesColumn()
			]

			+ SSplitter::Slot().Value(0.25f)
			[
				MakeDetailsPanel()
			]
		]
	];

	// Ingest/delete/rescan can now be triggered from the Content Browser
	// context menu too - refresh whenever anything changed the library. The
	// payload makes it the incremental path: whatever just changed is a
	// handful of meta files, not the library.
	LibraryChangedHandle = KuraActions::OnLibraryChanged().AddSP(
		this, &SKuraBrowser::RefreshFromDisk, false);

	// Thumbnail loading and the search debounce both need a per-frame pump.
	SetCanTick(true);

	RefreshFromDisk(/*bForceFullReload*/ true);

	// First run (no config.json yet): offer to create the library. Deferred one
	// tick so the modal doesn't open during widget construction. Declining it
	// is not a dead end any more - the empty state then offers the same dialog
	// (2026-08-23).
	if (!KuraLibrary::IsConfigured())
	{
		RegisterActiveTimer(0.f, FWidgetActiveTimerDelegate::CreateLambda(
			[this](double, float) { ShowLibrarySetupDialog(); return EActiveTimerReturnType::Stop; }));
	}
}

SKuraBrowser::~SKuraBrowser()
{
	KuraActions::OnLibraryChanged().Remove(LibraryChangedHandle);
}

void SKuraBrowser::Tick(const FGeometry& AllottedGeometry, const double CurrentTime,
	const float DeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, CurrentTime, DeltaTime);

	// Reads finished on worker threads become Slate textures here; queued ones
	// get their thread. Nothing happens while the panel is hidden, which is
	// exactly right - there is nothing to show a thumbnail on.
	KuraThumbs::Tick();

	// 0.15s: long enough that a burst of keystrokes filters once, short enough
	// that a pause between words reads as instant.
	if (bSearchPending && CurrentTime - SearchChangedTime >= 0.15)
	{
		bSearchPending = false;
		ApplyFilter();
	}
}

TSharedRef<SWidget> SKuraBrowser::MakeToolbar()
{
	return SNew(SHorizontalBox)
	+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
	[
		SNew(SSearchBox)
		.HintText(LOCTEXT("SearchHint", "Search name, path, tags, notes..."))
		.ToolTipText(LOCTEXT("SearchTip", "Searches within the current tab and left-column selection"))
		.OnTextChanged_Lambda([this](const FText& NewText)
		{
			SearchText = NewText.ToString().ToLower();
			bSearchPending = true;
			SearchChangedTime = FSlateApplication::Get().GetCurrentTime();
		})
	]
	+ SHorizontalBox::Slot().AutoWidth().Padding(8.f, 0.f, 0.f, 0.f).VAlign(VAlign_Center)
	[
		SNew(SCheckBox)
		.IsChecked_Lambda([this]() { return bShowDeps ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
		{
			bShowDeps = (State == ECheckBoxState::Checked);
			KuraLibrary::SaveConfig({}, { { TEXT("show_dependencies"), bShowDeps ? 1.0 : 0.0 } });
			ApplyFilter();
		})
		.ToolTipText(LOCTEXT("ShowDepsTip",
			"Also show what the listed assets are made of. In Collections that means the materials and textures they exclusively own - shared masters and functions stay out, they belong to everything. In Folders it means the dependency files stored in the selected folder."))
		[
			SNew(STextBlock).Text(LOCTEXT("ShowDeps", "Show Dependencies"))
		]
	]
	+ SHorizontalBox::Slot().AutoWidth().Padding(8.f, 0.f, 0.f, 0.f).VAlign(VAlign_Center)
	[
		SNew(SCheckBox)
		.IsChecked_Lambda([this]() { return bIncludeSubfolders ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		// Folder-only control: in Collections there is no folder to recurse into.
		.IsEnabled_Lambda([this]() { return !bCollectionsView; })
		.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
		{
			bIncludeSubfolders = (State == ECheckBoxState::Checked);
			ApplyFilter();
		})
		.ToolTipText(LOCTEXT("SubfoldersTip", "Include entries from subfolders of the selected folder (Folders view only)"))
		[
			SNew(STextBlock).Text(LOCTEXT("Subfolders", "Subfolders"))
		]
	]
	+ SHorizontalBox::Slot().AutoWidth().Padding(8.f, 0.f, 0.f, 0.f).VAlign(VAlign_Center)
	[
		SNew(SButton)
		.ToolTipText(LOCTEXT("RefreshTip", "Reload the panel from the library on disk"))
		.OnClicked(this, &SKuraBrowser::OnRefreshClicked)
		.VAlign(VAlign_Center)
		[
			// 16x16 box like the Settings button next door, smaller glyph inside
			SNew(SBox)
			.WidthOverride(16.f).HeightOverride(16.f)
			.HAlign(HAlign_Center).VAlign(VAlign_Center)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.Refresh"))
				.DesiredSizeOverride(FVector2D(12.f, 12.f))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		]
	]
	+ SHorizontalBox::Slot().AutoWidth().Padding(4.f, 0.f, 0.f, 0.f).VAlign(VAlign_Center)
	[
		SNew(SComboButton)
		.ToolTipText(LOCTEXT("MenuTip", "Kura menu: tools, library maintenance and settings"))
		.HasDownArrow(false)
		.OnGetMenuContent(this, &SKuraBrowser::MakeGearMenu)
		.ButtonContent()
		[
			// Kebab, not a gear: the gear belongs to the Settings entry inside.
			SNew(SImage)
			.Image(FAppStyle::GetBrush("Icons.EllipsisVerticalNarrow"))
			.ColorAndOpacity(FSlateColor::UseForeground())
		]
	];
}

TSharedRef<SWidget> SKuraBrowser::MakeTypeTabs()
{
	// A class filter over ROOT entries since 2026-08-10 (Q24), not a folder
	// switch: the tabs answer "what kind of asset am I looking for", the left
	// column answers "which of them", Show Dependencies answers "and what are
	// they made of".
	// Built from KuraLibrary::ClassTabs rather than listed here: two copies of
	// the tab order is exactly how a reorder goes wrong (2026-08-13).
	const TSharedRef<SSegmentedControl<int32>> Tabs = SNew(SSegmentedControl<int32>)
	.Value_Lambda([this]() { return ActiveTab; })
	.OnValueChanged(this, &SKuraBrowser::SetActiveTab)
	.ToolTipText(LOCTEXT("TypeTabsTip",
		"Narrow the tiles to one kind of asset. All Assets shows every kind at once - use it to see a whole collection, or to find material functions and other assets that only ever arrive as dependencies. The tabs never change the left column: the number on each collection is what THIS tab would list."));

	for (int32 Idx = 0; Idx < KuraLibrary::NumClassTabs; ++Idx)
	{
		Tabs->AddSlot(Idx, /*bRebuildChildren*/ false)
			.Text(FText::FromString(KuraLibrary::ClassTabs[Idx].Label))
			.Icon(ClassTabIcon(KuraLibrary::ClassTabs[Idx]));
	}
	Tabs->RebuildChildren();
	return Tabs;
}

void SKuraBrowser::SetActiveTab(int32 NewTab)
{
	// Folder and collection selections are shared across tabs now: they are
	// orthogonal to class, so switching tab keeps you in the same place.
	// The tab NEVER touches the left column - not its contents, not its
	// selection (2026-08-14). The row counts are live attributes, so they
	// follow the tab without a rebuild.
	ActiveTab = FMath::Clamp(NewTab, 0, KuraLibrary::NumClassTabs - 1);
	ApplyFilter();
}

TSharedRef<SWidget> SKuraBrowser::MakeGearMenu()
{
	// Three labeled sections (2026-07-17): what it does to your PROJECT
	// (Tools), what it does to the LIBRARY, and configuration.
	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr);

	// "This Project", not "Tools": both entries below act on the CURRENT
	// PROJECT, which is what the Library section contrasts with. Naming the
	// direction stops the PBR tool reading as "import into Kura" (2026-07-25).
	Menu.BeginSection("KuraTools", LOCTEXT("MenuSectionTools", "This Project"));
	Menu.AddMenuEntry(
		LOCTEXT("PbrImporter", "Build Material Instances from Textures"),
		LOCTEXT("PbrImporterTip",
			"Import downloaded PBR texture sets (one subfolder per set) into this project and build a Material Instance for each, wired to your master material. Nothing is sent to the library."),
		// Convert, not Import: this transforms texture files into project
		// assets - the Import glyph belongs to Import Package (into the library).
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Convert"),
		FUIAction(FExecuteAction::CreateLambda([this]()
		{
			KuraPbrImporter::RunFlow(AsShared());
		})));
	Menu.AddMenuEntry(
		LOCTEXT("CreateFolders", "Create _AssetLib Folder"),
		LOCTEXT("CreateFoldersTip",
			"Create the /Game/_AssetLib folder in the current project. Arrange your assets inside it however you like, then use Send to Kura."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Plus"),
		FUIAction(FExecuteAction::CreateLambda([this]()
		{
			KuraActions::RunCreateFoldersFlow(AsShared());
		})));
	Menu.EndSection();

	Menu.BeginSection("KuraLibrary", LOCTEXT("MenuSectionLibrary", "Library"));
	Menu.AddMenuEntry(
		LOCTEXT("OpenFolder", "Open Library Folder"),
		LOCTEXT("OpenFolderTip", "Show the library root in Explorer"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.FolderOpen"),
		FUIAction(FExecuteAction::CreateLambda([]()
		{
			FPlatformProcess::ExploreFolder(*KuraLibrary::GetLibraryRoot());
		})));
	Menu.AddMenuEntry(
		LOCTEXT("ImportPackage", "Import Package"),
		LOCTEXT("ImportPackageTip",
			"Import a .kurapack file into the library; identical entries are skipped and changed ones prompt for a decision"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Import"),
		FUIAction(FExecuteAction::CreateLambda([this]()
		{
			KuraActions::RunImportPackageFlow(AsShared());
		})));
	Menu.AddMenuEntry(
		LOCTEXT("RecentlyDeleted", "Recently Deleted"),
		LOCTEXT("RecentlyDeletedTip",
			"Browse archived library versions; restore them or delete them permanently"),
		// A history glyph, not the trash can: this entry is about getting
		// things BACK. The trash stays with the actions that archive (Delete,
		// Clean Unused Dependencies).
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.RecentAssets"),
		FUIAction(FExecuteAction::CreateLambda([this]()
		{
			KuraActions::RunArchiveFlow(AsShared());
		})));
	Menu.AddMenuEntry(
		LOCTEXT("CleanUnused", "Clean Unused Dependencies"),
		LOCTEXT("CleanUnusedTip",
			"Find dependencies nothing in the library references any more (left behind by deletes) and archive the ones you pick. Assets you sent to Kura yourself are never listed."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Delete"),
		FUIAction(FExecuteAction::CreateLambda([this]()
		{
			KuraActions::RunCleanUnusedFlow(AsShared());
		})));
	Menu.AddMenuEntry(
		LOCTEXT("Verify", "Verify Library"),
		LOCTEXT("VerifyTip",
			"Check the whole library file by file (hashes, missing or unregistered files, broken references) and rebuild the index. Slow on big libraries; not needed day to day."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search"),
		FUIAction(FExecuteAction::CreateLambda([this]()
		{
			KuraActions::RunRescanFlow(AsShared());
		})));
	Menu.EndSection();

	Menu.BeginSection("KuraSettings", LOCTEXT("MenuSectionSettings", "Settings"));
	Menu.AddMenuEntry(
		LOCTEXT("Settings", "Settings"),
		LOCTEXT("SettingsTip", "Library root / stable path prefix / thumbnails / browser defaults"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Settings"),
		FUIAction(FExecuteAction::CreateSP(this, &SKuraBrowser::ShowSettingsDialog)));
	Menu.AddMenuEntry(
		LOCTEXT("OpenConfig", "Open config.json"),
		LOCTEXT("OpenConfigTip", "Open %APPDATA%\\Kura\\config.json in the default editor"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.OpenInExternalEditor"),
		FUIAction(FExecuteAction::CreateLambda([]()
		{
			const FString Path = KuraLibrary::GetConfigFilePath();
			if (!Path.IsEmpty())
			{
				FPlatformProcess::LaunchFileInDefaultExternalApplication(*Path);
			}
		})));
	Menu.EndSection();

	return Menu.MakeWidget();
}

TSharedRef<SWidget> SKuraBrowser::MakeViewOptionsMenu()
{
	// Content Browser-style View Options (2026-07-17): presentation controls
	// live here, filters (Show Dependencies / Subfolders) stay in the toolbar.
	// Keep the menu open so slider drags and sort clicks don't dismiss it.
	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ false, nullptr);

	const auto SortKeyEntry = [this, &Menu](int32 Mode, const FText& Label, const FText& Tip)
	{
		Menu.AddMenuEntry(Label, Tip, FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([this, Mode]()
				{
					SortMode = Mode;
					KuraLibrary::SaveConfig({}, { { TEXT("sort_mode"), (double)Mode } });
					ApplyFilter();
				}),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([this, Mode]() { return SortMode == Mode; })),
			NAME_None, EUserInterfaceActionType::RadioButton);
	};

	Menu.BeginSection("KuraSort", LOCTEXT("SortSection", "Sort By"));
	SortKeyEntry(0, LOCTEXT("SortByName", "Name"),
		LOCTEXT("SortByNameTip", "Order tiles by asset name"));
	SortKeyEntry(1, LOCTEXT("SortByDate", "Date Added"),
		LOCTEXT("SortByDateTip", "Order tiles by when the entry entered the library"));
	SortKeyEntry(2, LOCTEXT("SortBySize", "Size"),
		LOCTEXT("SortBySizeTip", "Order tiles by file size on disk"));
	Menu.EndSection();

	const auto SortDirEntry = [this, &Menu](bool bAscending, const FText& Label, const FText& Tip)
	{
		Menu.AddMenuEntry(Label, Tip, FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([this, bAscending]()
				{
					bSortAscending = bAscending;
					KuraLibrary::SaveConfig({}, { { TEXT("sort_ascending"), bAscending ? 1.0 : 0.0 } });
					ApplyFilter();
				}),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([this, bAscending]() { return bSortAscending == bAscending; })),
			NAME_None, EUserInterfaceActionType::RadioButton);
	};

	Menu.BeginSection("KuraSortType", LOCTEXT("SortTypeSection", "Sort Type"));
	SortDirEntry(true, LOCTEXT("SortAscending", "Ascending"),
		LOCTEXT("SortAscendingTip", "A-Z, oldest first, smallest first"));
	SortDirEntry(false, LOCTEXT("SortDescending", "Descending"),
		LOCTEXT("SortDescendingTip", "Z-A, newest first, largest first"));
	Menu.EndSection();

	Menu.BeginSection("KuraTileSize", LOCTEXT("TileSizeSection", "Tile Size"));
	Menu.AddWidget(
		SNew(SBox).WidthOverride(180.f).Padding(12.f, 4.f)
		[
			SNew(SSlider)
			.MinValue(90.f)
			.MaxValue(260.f)
			.Value_Lambda([this]() { return TileSize; })
			.OnValueChanged_Lambda([this](float NewValue)
			{
				TileSize = NewValue;
				if (TileView.IsValid())
				{
					TileView->RequestListRefresh();
				}
			})
			.OnMouseCaptureEnd(this, &SKuraBrowser::SaveTileSize)
		],
		FText::GetEmpty());
	Menu.EndSection();

	return Menu.MakeWidget();
}

TSharedRef<SWidget> SKuraBrowser::MakeLeftColumn()
{
	return SNew(SSplitter)
	.Orientation(Orient_Vertical)

	+ SSplitter::Slot().Value(0.65f)
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(4.f)
		[
			SNew(SVerticalBox)

			// Collections (how you organise) / Folders (where the files are).
			// Two views of the same library, one at a time (Q23).
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(SSegmentedControl<bool>)
					.Value_Lambda([this]() { return bCollectionsView; })
					.OnValueChanged_Lambda([this](bool bNew)
					{
						bCollectionsView = bNew;
						KuraLibrary::SaveConfig({}, { { TEXT("collections_view"), bNew ? 1.0 : 0.0 } });
						ApplyFilter();
					})
					+ SSegmentedControl<bool>::Slot(true)
					.Text(LOCTEXT("ViewCollections", "Collections"))
					.ToolTip(LOCTEXT("ViewCollectionsTip",
						"Your own classification. Filing an asset here never moves or renames anything - an asset can sit in several collections at once."))
					+ SSegmentedControl<bool>::Slot(false)
					.Text(LOCTEXT("ViewFolders", "Folders"))
					.ToolTip(LOCTEXT("ViewFoldersTip",
						"The library's folders on disk, which are also the asset paths. Fixed at upload time - use collections to reorganise."))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(4.f, 0.f, 0.f, 0.f).VAlign(VAlign_Center)
				[
					SNew(SButton)
					.ToolTipText(LOCTEXT("NewCollectionTip", "Create a top-level collection"))
					.Visibility_Lambda([this]()
					{
						return bCollectionsView ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.OnClicked_Lambda([this]()
					{
						CreateCollectionUnder(FString());
						return FReply::Handled();
					})
					.VAlign(VAlign_Center)
					[
						SNew(SBox)
						.WidthOverride(16.f).HeightOverride(16.f)
						.HAlign(HAlign_Center).VAlign(VAlign_Center)
						[
							SNew(SImage)
							.Image(FAppStyle::GetBrush("Icons.Plus"))
							.DesiredSizeOverride(FVector2D(12.f, 12.f))
							.ColorAndOpacity(FSlateColor::UseForeground())
						]
					]
				]
			]

			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SBox)
					.Visibility_Lambda([this]()
					{
						return bCollectionsView ? EVisibility::Visible : EVisibility::Collapsed;
					})
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().FillHeight(1.f)
						[
							SAssignNew(CollectionTreeView, STreeView<TSharedPtr<FKuraCollectionNode>>)
							.TreeItemsSource(&CollectionRoots)
							.OnGenerateRow(this, &SKuraBrowser::MakeCollectionRow)
							.OnGetChildren(this, &SKuraBrowser::GetCollectionChildren)
							.OnSelectionChanged_Lambda([this](TSharedPtr<FKuraCollectionNode> Node, ESelectInfo::Type)
							{
								// No selection = the whole library.
								SelectedCollection = Node.IsValid() ? Node->Id : FString();
								ApplyFilter();
							})
							.OnExpansionChanged_Lambda([this](TSharedPtr<FKuraCollectionNode> Node, bool bExpanded)
							{
								if (bExpanded) { ExpandedCollections.Add(Node->Id); }
								else { ExpandedCollections.Remove(Node->Id); }
							})
							.OnContextMenuOpening(this, &SKuraBrowser::MakeCollectionContextMenu)
							.SelectionMode(ESelectionMode::Single)
						]
						// The saved queries are pinned below the scroll area
						// (2026-08-14): they are the two rows you always want
						// reachable, and a seeded tree buries them a hundred
						// collections deep. Empty ones are not built at all,
						// so a fully filed library gets its space back.
						+ SVerticalBox::Slot().AutoHeight()
						[
							SAssignNew(QueryRowsBox, SVerticalBox)
						]
					]
				]
				+ SOverlay::Slot()
				[
					SNew(SBox)
					.Visibility_Lambda([this]()
					{
						return bCollectionsView ? EVisibility::Collapsed : EVisibility::Visible;
					})
					[
						SAssignNew(FolderTreeView, STreeView<TSharedPtr<FKuraFolderNode>>)
						.TreeItemsSource(&FolderRoots)
						.OnGenerateRow(this, &SKuraBrowser::MakeFolderRow)
						.OnGetChildren(this, &SKuraBrowser::GetFolderChildren)
						.OnSelectionChanged_Lambda([this](TSharedPtr<FKuraFolderNode> Node, ESelectInfo::Type)
						{
							// No selection = the whole library.
							SelectedFolder = Node.IsValid() ? Node->RelPath : FString();
							ApplyFilter();
						})
						.OnExpansionChanged_Lambda([this](TSharedPtr<FKuraFolderNode> Node, bool bExpanded)
						{
							if (bExpanded)
							{
								ExpandedFolders.Add(Node->RelPath);
							}
							else
							{
								ExpandedFolders.Remove(Node->RelPath);
							}
						})
						.OnContextMenuOpening(this, &SKuraBrowser::MakeFolderContextMenu)
						.SelectionMode(ESelectionMode::Single)
					]
				]
				+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Top).Padding(8.f, 24.f)
				[
					// A library with entries but no collections yet: say what
					// the empty tree is for instead of showing nothing.
					SNew(STextBlock)
					.Visibility_Lambda([this]()
					{
						return bCollectionsView && CollectionRoots.Num() == 0
							&& AllEntries.Num() > 0
							? EVisibility::HitTestInvisible : EVisibility::Collapsed;
					})
					.Text(LOCTEXT("NoCollections",
						"No collections yet.\nUse + to create one, then drop assets into it from the tile menu."))
					.Justification(ETextJustify::Center)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
		]
	]

	+ SSplitter::Slot().Value(0.35f)
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(4.f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f).HAlign(HAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("TagsHeader", "Tags"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				SAssignNew(TagListView, SListView<TSharedPtr<FString>>)
				.ListItemsSource(&AllTags)
				.OnGenerateRow(this, &SKuraBrowser::MakeTagRow)
				.SelectionMode(ESelectionMode::None)
			]
		]
	];
}

TSharedRef<SWidget> SKuraBrowser::MakeTilesColumn()
{
	return SNew(SBorder)
	.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
	.Padding(4.f)
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SAssignNew(TileView, STileView<TSharedPtr<FKuraEntry>>)
				.ListItemsSource(&FilteredEntries)
				.ItemWidth_Lambda([this]() { return TileSize; })
				.ItemHeight_Lambda([this]() { return TileSize + 34.f; })
				.OnGenerateTile(this, &SKuraBrowser::MakeEntryTile)
				.OnSelectionChanged_Lambda([this](TSharedPtr<FKuraEntry> Item, ESelectInfo::Type)
				{
					const TArray<TSharedPtr<FKuraEntry>> Sel =
						TileView.IsValid() ? TileView->GetSelectedItems() : TArray<TSharedPtr<FKuraEntry>>();
					if (Item.IsValid())
					{
						Selected = Item;
					}
					if (!Selected.IsValid() || !Sel.Contains(Selected))
					{
						Selected = Sel.Num() > 0 ? Sel.Last() : nullptr;
					}
					RebuildLinkLists();
				})
				.OnContextMenuOpening(this, &SKuraBrowser::MakeTileContextMenu)
				.SelectionMode(ESelectionMode::Multi)
			]
			+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(16.f)
			[
				// Filter matched nothing (library not empty). When the class tab
				// is what emptied it, say where the assets went: collections
				// hold roots only, so a collection of meshes IS empty under
				// Textures, and a blank grid reads as a bug (2026-08-14).
				SNew(STextBlock)
				.Visibility_Lambda([this]()
				{
					return FilteredEntries.Num() == 0 && AllEntries.Num() > 0
						? EVisibility::HitTestInvisible : EVisibility::Collapsed;
				})
				.Text_Lambda([this]()
				{
					const KuraLibrary::FKuraClassTab& Tab =
						KuraLibrary::ClassTabs[FMath::Clamp(ActiveTab, 0, KuraLibrary::NumClassTabs - 1)];
					if (ScopeCountOtherTabs > 0 && !Tab.bPointToOtherTabs)
					{
						return FText::Format(LOCTEXT("EmptyThisTabOnly", "No {0} here."),
							FText::FromString(Tab.Label));
					}
					if (ScopeCountOtherTabs > 0)
					{
						return FText::Format(LOCTEXT("EmptyThisTab",
							"No {0} here.\n{1} {1}|plural(one=asset,other=assets) in this selection {1}|plural(one=is,other=are) under the other tabs - switch to {2} to see {1}|plural(one=it,other=them)."),
							FText::FromString(KuraLibrary::ClassTabs[
								FMath::Clamp(ActiveTab, 0, KuraLibrary::NumClassTabs - 1)].Label),
							FText::AsNumber(ScopeCountOtherTabs),
							FText::FromString(KuraLibrary::ClassTabs[KuraLibrary::AllTabIndex()].Label));
					}
					return LOCTEXT("EmptyFilter", "No entries match the current filter.");
				})
				.Justification(ETextJustify::Center)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(16.f)
			[
				// No library at all (no config.json). Must come before the
				// empty-library tutorial: following that one without a library
				// ends at Send to Kura failing over a folder that was never
				// created (2026-08-23).
				SNew(SVerticalBox)
				.Visibility_Lambda([this]()
				{
					return AllEntries.Num() == 0 && !bLibraryConfigured
						? EVisibility::Visible : EVisibility::Collapsed;
				})
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.f, 0.f, 0.f, 12.f)
				[
					SNew(SImage)
					.Image(KuraStyle::Get().GetBrush("Kura.LogoWatermark"))
					.DesiredSizeOverride(FVector2D(56.f, 56.f))
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.f, 0.f, 0.f, 4.f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("NoLibraryTitle", "Kura has no library yet"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.f, 0.f, 0.f, 10.f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("NoLibraryBody",
						"Your library is one folder on disk that every project can reach.\nChoose where it goes and Kura creates it - that is the whole setup."))
					.Justification(ETextJustify::Center)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
				[
					SNew(SButton)
					.Text(LOCTEXT("NoLibrarySetUp", "Set Up Library..."))
					.OnClicked_Lambda([this]()
					{
						ShowLibrarySetupDialog();
						return FReply::Handled();
					})
				]
			]
			+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(16.f)
			[
				// Library exists but holds nothing: walk the user through their
				// first ingest, or let them start from a package.
				SNew(SVerticalBox)
				.Visibility_Lambda([this]()
				{
					return AllEntries.Num() == 0 && bLibraryConfigured && !bLibraryRootMissing
						? EVisibility::Visible : EVisibility::Collapsed;
				})
				// Logo watermark above the guidance, per the design guideline's
				// empty-state rule (48-64px, subdued).
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.f, 0.f, 0.f, 12.f)
				[
					SNew(SImage)
					.Image(KuraStyle::Get().GetBrush("Kura.LogoWatermark"))
					.DesiredSizeOverride(FVector2D(56.f, 56.f))
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.f, 0.f, 0.f, 16.f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("EmptyLibraryTitle", "Your library is empty"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
				]
				// Two ways in, in the order a new user should try them
				// (2026-08-23). The sample pack leads and says so: it needs no
				// assets of their own, it is one download and one click, and it
				// shows what a filled library looks like before they commit to
				// arranging one. Left-aligned inside a width cap: centring was
				// tried and reverted (2026-08-23) - a centred numbered list
				// puts 1. 2. 3. on three different x positions, and the cap is
				// needed either way or a wide panel stretches each paragraph
				// into one unreadable line.
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
				[
					SNew(SBox).MaxDesiredWidth(460.f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
						[
							// One heading line, with the recommendation on it
							// rather than stacked above: a badge on its own row
							// reads as a section label, which this is not.
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("EmptyStarterTitle", "Start with the sample pack"))
								.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom)
							.Padding(5.f, 0.f, 0.f, 1.f)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("EmptyRecommended", "(RECOMMENDED)"))
								.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
								.ColorAndOpacity(KuraStyle::Success().Text)
							]
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("EmptyStarterBody",
								"Every Kura release comes with Kura_Starter.kurapack. Download it from the project page, then import it here."))
							.AutoWrapText(true)
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						]
						+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left)
						[
							SNew(SButton)
							.Text(LOCTEXT("EmptyImportPackage", "Import Package..."))
							.ToolTipText(LOCTEXT("EmptyImportPackageTip",
								"Pick a .kurapack file and import it into the library. Also in the panel menu, under Library."))
							.OnClicked_Lambda([this]()
							{
								KuraActions::RunImportPackageFlow(AsShared());
								return FReply::Handled();
							})
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 18.f, 0.f, 14.f)
						[
							SNew(SSeparator).Thickness(1.f)
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("EmptyOwnTitle", "Or send in assets of your own"))
							.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("EmptyLibrarySteps",
								// The gap after the number lines a wrapped step
								// up under its own first word.
								"1.   Create the /Game/_AssetLib folder in this project.\n"
								"2.   Move the assets you want to send into /Game/_AssetLib.\n"
								"3.   Select them, right-click, and choose \"Send to Kura\"."))
							.AutoWrapText(true)
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						]
						// The one rule a new user cannot afford to miss, in the
						// warning colour because missing it is the only thing
						// that makes Send to Kura refuse a selection outright
						// (2026-08-23). Two sentences, no icon and nothing after
						// it: an aside about folder layout underneath only
						// competed with it, and layout is advice, not a rule.
						// Not a filled banner either - SBorder tinting needs a
						// solid brush, and that combination already failed to
						// render once in this file (the tile placeholder uses
						// SColorBlock for the same reason); the token's text
						// layer is meant for exactly this.
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 10.f)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("EmptyAssetLibRule",
								"Everything Kura works with lives under /Game/_AssetLib. Kura refuses anything kept somewhere else."))
							.AutoWrapText(true)
							.ColorAndOpacity(KuraStyle::Warning().Text)
						]
						+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left)
						[
							SNew(SButton)
							// Longer than the same action's label in the panel
							// menu on purpose: there it sits under a "This
							// Project" heading, here it is the first thing a
							// new user will click and has to say what appears
							// and where.
							.Text(LOCTEXT("EmptyCreateFolders", "Create _AssetLib/ in Content Browser"))
							.ToolTipText(LOCTEXT("CreateFoldersTip",
								"Create the /Game/_AssetLib folder in the current project. Arrange your assets inside it however you like, then use Send to Kura."))
							.OnClicked_Lambda([this]()
							{
								KuraActions::RunCreateFoldersFlow(AsShared());
								return FReply::Handled();
							})
						]
					]
				]
			]
			+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(16.f)
			[
				// Configured root gone from disk (moved / renamed / disconnected
				// drive): steer to Settings instead of the tutorial, which would
				// rebuild a fresh library at the stale path (2e step 2).
				SNew(SVerticalBox)
				.Visibility_Lambda([this]()
				{
					return AllEntries.Num() == 0 && bLibraryRootMissing
						? EVisibility::Visible : EVisibility::Collapsed;
				})
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.f, 0.f, 0.f, 4.f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("RootMissingTitle", "Library folder not found"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					.ColorAndOpacity(KuraStyle::Warning().Text)
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.f, 0.f, 0.f, 10.f)
				[
					SNew(STextBlock)
					.Text_Lambda([]()
					{
						return FText::Format(LOCTEXT("RootMissingBody",
							"Kura is configured to use {0}, but that folder does not exist - it may have been moved, renamed, or be on a disconnected drive. If you moved the library, point Settings at its new location; if it is gone for good, set up a new one. Nothing is created or changed until then."),
							FText::FromString(KuraLibrary::GetLibraryRoot()));
					})
					.Justification(ETextJustify::Center)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
				[
					SNew(SHorizontalBox)
					// Settings only re-points the config; it never creates
					// anything, so a library that is gone for good left no way
					// forward from this card at all (2026-08-23).
					+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)
					[
						SNew(SButton)
						.Text(LOCTEXT("RootMissingSetUp", "Set Up Library..."))
						.ToolTipText(LOCTEXT("RootMissingSetUpTip",
							"Choose a folder and create the library there - or point Kura at an existing library to pick it up as it is."))
						.OnClicked_Lambda([this]()
						{
							ShowLibrarySetupDialog();
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("RootMissingSettings", "Open Settings"))
						.OnClicked_Lambda([this]()
						{
							ShowSettingsDialog();
							return FReply::Handled();
						})
					]
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(4.f, 6.f, 4.f, 2.f)
		[
			SNew(SHorizontalBox)
			// Bottom left, Content Browser wording: the selection count matters
			// here because Import/Export/Delete all act on a multi-selection.
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text_Lambda([this]()
				{
					const int32 NumSelected =
						TileView.IsValid() ? TileView->GetNumItemsSelected() : 0;
					if (NumSelected > 0)
					{
						return FText::Format(
							LOCTEXT("ItemCountSelected",
								"{0} {0}|plural(one=item,other=items) ({1} selected)"),
							FText::AsNumber(FilteredEntries.Num()),
							FText::AsNumber(NumSelected));
					}
					return FText::Format(
						LOCTEXT("ItemCount", "{0} {0}|plural(one=item,other=items)"),
						FText::AsNumber(FilteredEntries.Num()));
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SComboButton)
				.ToolTipText(LOCTEXT("ViewOptionsTip", "View options: sort and tile size"))
				.HasDownArrow(false)
				.OnGetMenuContent(this, &SKuraBrowser::MakeViewOptionsMenu)
				.ButtonContent()
				[
					SNew(SBox)
					.WidthOverride(16.f).HeightOverride(16.f)
					.HAlign(HAlign_Center).VAlign(VAlign_Center)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.Visible"))
						.DesiredSizeOverride(FVector2D(12.f, 12.f))
						.ColorAndOpacity(FSlateColor::UseForeground())
					]
				]
			]
		]
	];
}

TSharedPtr<SWidget> SKuraBrowser::MakeTileContextMenu()
{
	const TArray<FString> Packages = SelectedPackages();
	if (Packages.Num() == 0)
	{
		return nullptr;
	}
	const bool bSingle = Packages.Num() == 1;

	// Root state of the selection, to enable/disable Set/Unset Root.
	int32 NumRoots = 0, NumNonRoots = 0;
	if (TileView.IsValid())
	{
		for (const TSharedPtr<FKuraEntry>& E : TileView->GetSelectedItems())
		{
			if (E.IsValid()) { (E->bIsRoot ? NumRoots : NumNonRoots)++; }
		}
	}
	if (NumRoots == 0 && NumNonRoots == 0 && Selected.IsValid())
	{
		(Selected->bIsRoot ? NumRoots : NumNonRoots)++;
	}

	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr);
	Menu.AddMenuEntry(
		bSingle
			? LOCTEXT("CtxImportOne", "Import")
			: FText::Format(LOCTEXT("CtxImportMany", "Import {0} Entries"),
				FText::AsNumber(Packages.Num())),
		LOCTEXT("ImportTip",
			"Copy the selection and its current dependency closure into the project at the stable /Game/_AssetLib paths"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Import"),
		FUIAction(FExecuteAction::CreateLambda([this, Packages]()
		{
			KuraActions::RunImportFlow(Packages, AsShared());
		})));
	// Sections (2026-07-17): the one project-changing action | navigation
	// helpers | library edits | the destructive one.
	Menu.AddMenuSeparator();
	Menu.AddMenuEntry(
		LOCTEXT("CtxLocate", "Locate in Folder Tree"),
		LOCTEXT("CtxLocateTip", "Select this entry's folder in the tree"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Find"),
		FUIAction(
			FExecuteAction::CreateLambda([this]()
			{
				if (Selected.IsValid())
				{
					SelectEntryByPackage(Selected->Package, /*bShowFolders*/ true);
				}
			}),
			FCanExecuteAction::CreateLambda([bSingle]() { return bSingle; })));
	Menu.AddMenuEntry(
		bSingle
			? LOCTEXT("CtxCopyOne", "Copy Package Path")
			: LOCTEXT("CtxCopyMany", "Copy Package Paths"),
		LOCTEXT("CtxCopyTip", "Copy the package path(s) to the clipboard"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Clipboard"),
		FUIAction(FExecuteAction::CreateLambda([Packages]()
		{
			FPlatformApplicationMisc::ClipboardCopy(*FString::Join(Packages, LINE_TERMINATOR));
		})));
	Menu.AddMenuEntry(
		bSingle
			? LOCTEXT("CtxExportOne", "Export as Package")
			: FText::Format(LOCTEXT("CtxExportMany", "Export {0} Entries as Package"),
				FText::AsNumber(Packages.Num())),
		LOCTEXT("CtxExportTip",
			"Save the selection and its current dependency closure as a .kurapack file that another Kura library can import"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Save"),
		FUIAction(FExecuteAction::CreateLambda([this, Packages]()
		{
			KuraActions::RunExportPackageFlow(Packages, AsShared());
		})));
	// Filing (F8) sits with the other library edits, above the destructive one.
	Menu.AddMenuSeparator();
	Menu.AddSubMenu(
		LOCTEXT("CtxAddToCollection", "Add to Collection"),
		LOCTEXT("CtxAddToCollectionTip",
			"File the selection under a collection. Nothing is moved or copied - an asset can be in several collections at once."),
		FNewMenuDelegate::CreateLambda([this, Packages](FMenuBuilder& SubMenu)
		{
			FillAddToCollectionMenu(SubMenu, Packages);
		}),
		false,
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Plus"));
	Menu.AddSubMenu(
		LOCTEXT("CtxRemoveFromCollection", "Remove from Collection"),
		LOCTEXT("CtxRemoveFromCollectionTip",
			"Unfile the selection from a collection it is currently in. The assets stay in the library."),
		FNewMenuDelegate::CreateLambda([this, Packages](FMenuBuilder& SubMenu)
		{
			FillRemoveFromCollectionMenu(SubMenu, Packages);
		}),
		false,
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Minus"));
	Menu.AddMenuSeparator();
	Menu.AddMenuEntry(
		LOCTEXT("CtxSetRoot", "Set as Root"),
		LOCTEXT("CtxSetRootTip",
			"Treat these entries as assets in their own right: they show without Show Dependencies, and Clean Unused Dependencies never archives them."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Star"),
		FUIAction(
			FExecuteAction::CreateLambda([this, Packages]()
			{
				KuraActions::RunSetRootFlow(Packages, /*bRoot*/ true, AsShared());
			}),
			FCanExecuteAction::CreateLambda([NumNonRoots]() { return NumNonRoots > 0; })));
	Menu.AddMenuEntry(
		LOCTEXT("CtxUnsetRoot", "Unset Root"),
		LOCTEXT("CtxUnsetRootTip",
			"Treat these entries as dependencies again: they only appear with Show Dependencies, alongside whatever uses them."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Star"),
		FUIAction(
			FExecuteAction::CreateLambda([this, Packages]()
			{
				KuraActions::RunSetRootFlow(Packages, /*bRoot*/ false, AsShared());
			}),
			FCanExecuteAction::CreateLambda([NumRoots]() { return NumRoots > 0; })));
	Menu.AddMenuEntry(
		LOCTEXT("CtxRethumb", "Refresh Thumbnail"),
		LOCTEXT("CtxRethumbTip",
			"Re-render the library thumbnail with the current camera and preview-mesh settings. The asset must exist in this project and match the library version."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Refresh"),
		FUIAction(FExecuteAction::CreateLambda([this, Packages]()
		{
			KuraActions::RunRethumbFlow(Packages, AsShared());
		})));
	Menu.AddMenuSeparator();
	Menu.AddMenuEntry(
		LOCTEXT("CtxDelete", "Delete (Archive)"),
		LOCTEXT("CtxDeleteTip",
			"Move the selection into _archive. Blocked while other entries still reference it."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Delete"),
		FUIAction(FExecuteAction::CreateLambda([this, Packages]()
		{
			KuraActions::RunDeleteFlow(Packages, AsShared());
		})));
	return Menu.MakeWidget();
}

TSharedPtr<SWidget> SKuraBrowser::MakeFolderContextMenu()
{
	if (!FolderTreeView.IsValid())
	{
		return nullptr;
	}
	const TArray<TSharedPtr<FKuraFolderNode>> Sel = FolderTreeView->GetSelectedItems();
	if (Sel.Num() == 0 || !Sel[0].IsValid() || Sel[0]->RelPath.IsEmpty())
	{
		return nullptr;
	}
	const FString RelPath = Sel[0]->RelPath;
	const FString Prefix = RelPath + TEXT("/");
	TArray<FString> Packages;
	for (const TSharedPtr<FKuraEntry>& E : AllEntries)
	{
		if (E.IsValid() && (E->Folder == RelPath || E->Folder.StartsWith(Prefix)))
		{
			Packages.Add(E->Package);
		}
	}

	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr);
	Menu.AddMenuEntry(
		FText::Format(LOCTEXT("CtxExportFolder",
			"Export Folder as Package ({0} {0}|plural(one=entry,other=entries))"),
			FText::AsNumber(Packages.Num())),
		LOCTEXT("CtxExportFolderTip",
			"Save every entry under this folder (plus dependencies outside it) as a .kurapack file that another Kura library can import"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Save"),
		FUIAction(
			FExecuteAction::CreateLambda([this, Packages, RelPath]()
			{
				KuraActions::RunExportPackageFlow(Packages, AsShared(),
					FPaths::GetCleanFilename(RelPath));
			}),
			FCanExecuteAction::CreateLambda([Num = Packages.Num()]() { return Num > 0; })));
	// Physical-layout repairs (F4). Rare by design: what a folder is FOR is a
	// collection's job, and the referrer guard below refuses most folders
	// anyway - a root asset references the textures co-located with it.
	Menu.AddMenuSeparator();
	Menu.AddMenuEntry(
		LOCTEXT("CtxRenameFolder", "Rename Folder"),
		LOCTEXT("CtxRenameFolderTip",
			"Rename this library folder. Only possible while nothing inside it is referenced by another entry - Kura never rewrites .uasset files, so a co-located root and its textures cannot be renamed this way. Browsing categories belong in collections, not in folder names."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Edit"),
		FUIAction(FExecuteAction::CreateLambda([this, RelPath]()
		{
			KuraActions::RunFolderRenameFlow(RelPath, AsShared(),
				[this](const FString& NewRel) { SelectFolderAfterMove(NewRel); });
		})));
	Menu.AddSubMenu(
		LOCTEXT("CtxMoveFolder", "Move to"),
		LOCTEXT("CtxMoveFolderTip",
			"Move this folder elsewhere in the library. Same rule as Rename: blocked while anything inside is referenced. The entries keep their file names, and their stable /Game/_AssetLib path moves with the folder."),
		FNewMenuDelegate::CreateLambda([this, RelPath](FMenuBuilder& SubMenu)
		{
			FillMoveFolderMenu(SubMenu, RelPath);
		}),
		false,
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.ArrowRight"));
	return Menu.MakeWidget();
}

void SKuraBrowser::FillMoveFolderMenu(FMenuBuilder& Menu, const FString& RelPath)
{
	Menu.AddMenuEntry(
		LOCTEXT("CtxMoveFolderTop", "Top Level"),
		LOCTEXT("CtxMoveFolderTopTip", "Move it to the top of the asset tree"),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this, RelPath]()
		{
			MoveFolderTo(RelPath, FString());
		})));
	Menu.AddMenuSeparator();
	AddMoveFolderTargets(Menu, FolderRoots, RelPath);
}

void SKuraBrowser::AddMoveFolderTargets(FMenuBuilder& Menu,
	const TArray<TSharedPtr<FKuraFolderNode>>& Nodes, const FString& RelPath)
{
	// One submenu per level, mirroring the tree the user already browses. A
	// flat indented list (what the collection version does) would be hundreds
	// of rows in a real library; Slate builds a submenu only when hovered, so
	// depth costs nothing here.
	for (const TSharedPtr<FKuraFolderNode>& Node : Nodes)
	{
		if (!Node.IsValid() || Node->RelPath.IsEmpty() || Node->RelPath == RelPath)
		{
			continue; // skipping the folder itself also skips its subtree
		}
		const FString Target = Node->RelPath;
		if (Node->Children.Num() == 0)
		{
			Menu.AddMenuEntry(
				FText::FromString(Node->Name), FText::FromString(Target), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([this, RelPath, Target]()
				{
					MoveFolderTo(RelPath, Target);
				})));
			continue;
		}
		Menu.AddSubMenu(
			FText::FromString(Node->Name),
			FText::FromString(Target),
			FNewMenuDelegate::CreateLambda(
				[this, RelPath, Target, Children = Node->Children](FMenuBuilder& SubMenu)
			{
				SubMenu.AddMenuEntry(
					LOCTEXT("CtxMoveFolderHere", "Move Here"),
					FText::Format(LOCTEXT("CtxMoveFolderHereTip", "Move it into {0}"),
						FText::FromString(Target)),
					FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([this, RelPath, Target]()
					{
						MoveFolderTo(RelPath, Target);
					})));
				SubMenu.AddMenuSeparator();
				AddMoveFolderTargets(SubMenu, Children, RelPath);
			}),
			false,
			FSlateIcon());
	}
}

void SKuraBrowser::MoveFolderTo(const FString& RelPath, const FString& NewParentRel)
{
	KuraActions::RunFolderMoveFlow(RelPath, NewParentRel, AsShared(),
		[this](const FString& NewRel) { SelectFolderAfterMove(NewRel); });
}

void SKuraBrowser::SelectFolderAfterMove(const FString& NewRel)
{
	// The refresh runs off OnLibraryChanged, which fires after this - so the
	// selection and the expansion of every new ancestor are recorded here and
	// restored by RebuildFolderTree (same ordering as CreateCollectionUnder).
	TArray<FString> Segments;
	NewRel.ParseIntoArray(Segments, TEXT("/"));
	FString Path;
	for (const FString& Segment : Segments)
	{
		Path = Path.IsEmpty() ? Segment : Path + TEXT("/") + Segment;
		ExpandedFolders.Add(Path);
	}
	SelectedFolder = NewRel;
}

TArray<FString> SKuraBrowser::SelectedPackages() const
{
	TArray<FString> Out;
	if (TileView.IsValid())
	{
		for (const TSharedPtr<FKuraEntry>& E : TileView->GetSelectedItems())
		{
			if (E.IsValid())
			{
				Out.Add(E->Package);
			}
		}
	}
	if (Out.Num() == 0 && Selected.IsValid())
	{
		Out.Add(Selected->Package);
	}
	return Out;
}

void SKuraBrowser::RefreshFromDisk(bool bForceFullReload)
{
	// A configured root that is gone from disk (moved, renamed, disconnected
	// drive) must show the missing-root notice, not the empty-library
	// tutorial - following the tutorial would rebuild a fresh library at the
	// stale path. Cached here so the empty-state lambdas don't stat per frame.
	bLibraryConfigured = KuraLibrary::IsConfigured();
	bLibraryRootMissing = bLibraryConfigured
		&& !IFileManager::Get().DirectoryExists(*KuraLibrary::GetLibraryRoot());

	// Carrying the previous entries in lets unchanged meta files be reused
	// rather than re-read; the ones that did change are re-parsed, and new or
	// deleted files are picked up by the walk either way.
	TArray<TSharedPtr<FKuraEntry>> Previous = MoveTemp(AllEntries);
	int32 Unreadable = 0;
	AllEntries = KuraLibrary::LoadEntries(bForceFullReload ? nullptr : &Previous, &Unreadable);
	Previous.Reset();
	// Entries whose meta is damaged are in the library but cannot be listed.
	// Say so once per change in their number - every refresh would nag, and
	// never saying it is how an asset "disappears" (D1, 2026-11-17).
	if (Unreadable > 0 && Unreadable != UnreadableMetaNotified)
	{
		KuraActions::Notify(false, FText::Format(
			LOCTEXT("UnreadableMeta",
				"Kura: {0} library {0}|plural(one=entry has,other=entries have) damaged metadata and {0}|plural(one=is,other=are) hidden - run Verify Library to list {0}|plural(one=it,other=them)"),
			Unreadable));
	}
	UnreadableMetaNotified = Unreadable;
	EntryByPackage.Reset();
	EntryByPackage.Reserve(AllEntries.Num());
	for (const TSharedPtr<FKuraEntry>& E : AllEntries)
	{
		EntryByPackage.Add(E->Package, E);
	}
	// Thumbnails are read again only where the PNG actually changed, and the
	// check itself happens on a worker thread.
	KuraThumbs::Revalidate();

	FolderRoots = KuraLibrary::LoadFolderTree(AllEntries);
	// The loader returns the saved queries as trailing roots; they live in
	// their own pinned strip below the tree from here on.
	CollectionRoots = KuraLibrary::LoadCollectionTree(AllEntries);
	QueryNodes.Reset();
	for (int32 Idx = CollectionRoots.Num() - 1; Idx >= 0; --Idx)
	{
		if (CollectionRoots[Idx]->IsQuery())
		{
			QueryNodes.Insert(CollectionRoots[Idx], 0);
			CollectionRoots.RemoveAt(Idx);
		}
	}

	RebuildTagList();
	RebuildFolderTree();
	RebuildCollectionTree();
	ApplyFilter();
}

void SKuraBrowser::RebuildTagList()
{
	TSet<FString> TagSet;
	for (const TSharedPtr<FKuraEntry>& E : AllEntries)
	{
		for (const FString& T : E->Tags)
		{
			TagSet.Add(T);
		}
	}
	TArray<FString> SortedTags = TagSet.Array();
	SortedTags.Sort();
	AllTags.Reset();
	for (const FString& T : SortedTags)
	{
		AllTags.Add(MakeShared<FString>(T));
	}
	// A tag that no entry carries any more cannot stay an active filter.
	ActiveTags = ActiveTags.Intersect(TagSet);

	if (TagListView.IsValid())
	{
		TagListView->RequestListRefresh();
	}
}

void SKuraBrowser::RebuildFolderTree()
{
	if (!FolderTreeView.IsValid())
	{
		return;
	}
	FolderTreeView->RequestTreeRefresh();
	const FString Want = SelectedFolder;
	bool bFound = Want.IsEmpty();
	TFunction<void(const TArray<TSharedPtr<FKuraFolderNode>>&)> Restore =
		[this, &Restore, &Want, &bFound](const TArray<TSharedPtr<FKuraFolderNode>>& Nodes)
	{
		for (const TSharedPtr<FKuraFolderNode>& Node : Nodes)
		{
			FolderTreeView->SetItemExpansion(Node, ExpandedFolders.Contains(Node->RelPath));
			if (Node->RelPath == Want)
			{
				FolderTreeView->SetSelection(Node);
				bFound = true;
			}
			Restore(Node->Children);
		}
	};
	Restore(FolderRoots);
	if (!bFound)
	{
		// The remembered folder is gone from disk - fall back to the whole
		// library rather than showing an empty grid with no way back.
		SelectedFolder.Empty();
		FolderTreeView->ClearSelection();
	}
}

void SKuraBrowser::CreateCollectionUnder(const FString& ParentId)
{
	// The refresh that follows the create runs off OnLibraryChanged, so the
	// selection has to be recorded BEFORE it - the callback fires first.
	KuraActions::RunCollectionCreateFlow(ParentId, AsShared(), TArray<FString>(),
		[this, ParentId](const FString& NewId)
		{
			if (NewId.IsEmpty())
			{
				return;
			}
			if (!ParentId.IsEmpty())
			{
				ExpandedCollections.Add(ParentId);
			}
			SelectedCollection = NewId;
		});
}

void SKuraBrowser::RebuildCollectionTree()
{
	if (!CollectionTreeView.IsValid())
	{
		return;
	}
	RebuildQueryRows();
	CollectionTreeView->RequestTreeRefresh();
	const FString Want = SelectedCollection;
	// A selected query lives outside the tree, so it counts as found here and
	// is validated against QueryNodes instead.
	bool bFound = Want.IsEmpty()
		|| QueryNodes.ContainsByPredicate([&Want](const TSharedPtr<FKuraCollectionNode>& N)
			{ return N->Id == Want; });
	TFunction<void(const TArray<TSharedPtr<FKuraCollectionNode>>&)> Restore =
		[this, &Restore, &Want, &bFound](const TArray<TSharedPtr<FKuraCollectionNode>>& Nodes)
	{
		for (const TSharedPtr<FKuraCollectionNode>& Node : Nodes)
		{
			CollectionTreeView->SetItemExpansion(Node, ExpandedCollections.Contains(Node->Id));
			if (Node->Id == Want)
			{
				CollectionTreeView->SetSelection(Node);
				bFound = true;
			}
			Restore(Node->Children);
		}
	};
	Restore(CollectionRoots);
	if (!bFound)
	{
		// Selected collection was deleted, or Unsorted just emptied out.
		SelectedCollection.Empty();
		CollectionTreeView->ClearSelection();
	}
}

void SKuraBrowser::SelectQueryNode(const FString& Id)
{
	// One selection across two widgets: picking a pinned row has to release
	// the tree's, or both would look selected and only one would be in force.
	SelectedCollection = Id;
	if (CollectionTreeView.IsValid())
	{
		CollectionTreeView->ClearSelection();
	}
	ApplyFilter();
}

void SKuraBrowser::RebuildQueryRows()
{
	if (!QueryRowsBox.IsValid())
	{
		return;
	}
	QueryRowsBox->ClearChildren();
	if (QueryNodes.Num() == 0)
	{
		return;
	}
	QueryRowsBox->AddSlot().AutoHeight().Padding(0.f, 2.f, 0.f, 2.f)
	[
		SNew(SSeparator).Thickness(1.f)
	];
	for (const TSharedPtr<FKuraCollectionNode>& Node : QueryNodes)
	{
		QueryRowsBox->AddSlot().AutoHeight()
		[
			SNew(SButton)
			// Borderless so it reads as a tree row, not a button; the style's
			// hover/press states still give it the same affordance.
			.ButtonStyle(FAppStyle::Get(), "NoBorder")
			.ContentPadding(FMargin(2.f, 1.f))
			.HAlign(HAlign_Fill)
			.OnClicked_Lambda([this, Id = Node->Id]()
			{
				SelectQueryNode(SelectedCollection == Id ? FString() : Id);
				return FReply::Handled();
			})
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Menu.Background"))
				.BorderBackgroundColor_Lambda([this, Id = Node->Id]()
				{
					return SelectedCollection == Id
						? FAppStyle::Get().GetSlateColor("SelectionColor").GetSpecifiedColor()
						: FLinearColor::Transparent;
				})
				.Padding(FMargin(2.f, 2.f))
				[
					MakeCollectionRowContent(Node)
				]
			]
		];
	}
}

TSet<FString> SKuraBrowser::SelectedCollectionIds() const
{
	TSet<FString> Ids;
	if (SelectedCollection.IsEmpty())
	{
		return Ids;
	}
	TFunction<bool(const TArray<TSharedPtr<FKuraCollectionNode>>&)> Collect =
		[this, &Collect, &Ids](const TArray<TSharedPtr<FKuraCollectionNode>>& Nodes) -> bool
	{
		for (const TSharedPtr<FKuraCollectionNode>& Node : Nodes)
		{
			if (Node->Id == SelectedCollection)
			{
				// Selecting a collection shows what it holds INCLUDING its
				// children, matching the count on the row and what "Import
				// this collection" would take (kura.catalog.members).
				TFunction<void(const TSharedPtr<FKuraCollectionNode>&)> AddAll =
					[&AddAll, &Ids](const TSharedPtr<FKuraCollectionNode>& N)
				{
					Ids.Add(N->Id);
					for (const TSharedPtr<FKuraCollectionNode>& C : N->Children)
					{
						AddAll(C);
					}
				};
				AddAll(Node);
				return true;
			}
			if (Collect(Node->Children))
			{
				return true;
			}
		}
		return false;
	};
	Collect(CollectionRoots);
	return Ids;
}

TArray<FString> SKuraBrowser::CollectionNamesOf(const FKuraEntry& Entry) const
{
	TMap<FString, FString> NameById;
	TFunction<void(const TArray<TSharedPtr<FKuraCollectionNode>>&)> Walk =
		[&Walk, &NameById](const TArray<TSharedPtr<FKuraCollectionNode>>& Nodes)
	{
		for (const TSharedPtr<FKuraCollectionNode>& Node : Nodes)
		{
			NameById.Add(Node->Id, Node->Name);
			Walk(Node->Children);
		}
	};
	Walk(CollectionRoots);

	TArray<FString> Names;
	for (const FString& Id : Entry.Collections)
	{
		if (const FString* Name = NameById.Find(Id))
		{
			Names.Add(*Name);
		}
	}
	Names.Sort();
	return Names;
}

void SKuraBrowser::CollectExclusiveDeps(const TArray<TSharedPtr<FKuraEntry>>& Roots,
	TArray<TSharedPtr<FKuraEntry>>& OutDeps) const
{
	if (Roots.Num() == 0)
	{
		return;
	}
	const TMap<FString, TSharedPtr<FKuraEntry>>& ByPackage = EntryByPackage;
	TSet<FString> InView;
	for (const TSharedPtr<FKuraEntry>& R : Roots)
	{
		InView.Add(R->Package);
	}

	// Everything reachable from the roots that is not itself a root.
	TSet<FString> Candidates;
	TArray<FString> Queue;
	TSet<FString> Seen;
	for (const TSharedPtr<FKuraEntry>& R : Roots)
	{
		Queue.Append(R->HardDeps);
	}
	while (Queue.Num() > 0)
	{
		const FString Pkg = Queue.Pop();
		if (Seen.Contains(Pkg))
		{
			continue;
		}
		Seen.Add(Pkg);
		const TSharedPtr<FKuraEntry>* Found = ByPackage.Find(Pkg);
		if (!Found || (*Found)->bIsRoot || InView.Contains(Pkg))
		{
			continue; // roots stand on their own; broken edges are Verify's job
		}
		Candidates.Add(Pkg);
		Queue.Append((*Found)->HardDeps);
	}

	// Shrink to the ones nothing outside the view uses. Same fixpoint as
	// library.deletable_closure: a candidate held by an outside referrer
	// survives, and a survivor keeps its own dependencies visible in turn.
	// That is exactly the shared/exclusive split the folder convention
	// describes (SPEC 5.4) - computed from the graph, so a texture that
	// becomes shared later drops out on its own, with no file ever moving.
	bool bChanged = true;
	while (bChanged)
	{
		bChanged = false;
		for (auto It = Candidates.CreateIterator(); It; ++It)
		{
			const TSharedPtr<FKuraEntry>* Found = ByPackage.Find(*It);
			if (!Found)
			{
				continue;
			}
			bool bExclusive = true;
			for (const FString& Ref : (*Found)->Referrers)
			{
				if (!InView.Contains(Ref) && !Candidates.Contains(Ref))
				{
					bExclusive = false;
					break;
				}
			}
			if (!bExclusive)
			{
				It.RemoveCurrent();
				bChanged = true;
			}
		}
	}

	for (const FString& Pkg : Candidates)
	{
		if (const TSharedPtr<FKuraEntry>* Found = ByPackage.Find(Pkg))
		{
			OutDeps.Add(*Found);
		}
	}
}

void SKuraBrowser::ApplyFilter()
{
	FilteredEntries.Reset();
	const FString& Needle = SearchText; // the box lowercases on the way in
	// Two independent narrowings: the tab picks a CLASS (Q24), the left column
	// picks a collection (F8) or a folder. An empty left-column selection means
	// the whole library.
	const TSet<FString> ScopeIds = bCollectionsView ? SelectedCollectionIds() : TSet<FString>();
	const bool bUnsortedScope = bCollectionsView && SelectedCollection == KuraLibrary::UnsortedId;
	const bool bSharedScope = bCollectionsView && SelectedCollection == KuraLibrary::SharedId;
	const FString& Scope = SelectedFolder;
	// Collections scopes a SET OF ROOTS, so dependencies are pulled in
	// afterwards by relation. Folders scopes a LOCATION, so a dependency
	// stored there is simply another entry in scope and goes through the
	// normal filters (that is how shared masters stay browsable at all).
	// Shared Dependencies is the exception on both counts: what it lists is
	// non-root by nature, and it is scoped by the reference graph rather than
	// by filing (SPEC F8, 2026-08-14).
	const bool bRootsOnlyPass = (bCollectionsView || !bShowDeps) && !bSharedScope;
	// Counts what the scope holds in the OTHER tabs, so an empty grid can say
	// where the assets went instead of just being blank.
	ScopeCountOtherTabs = 0;
	for (const TSharedPtr<FKuraEntry>& E : AllEntries)
	{
		if (bRootsOnlyPass && !E->bIsRoot)
		{
			continue;
		}
		const bool bClassOk = KuraLibrary::ClassMatchesTab(E->Class, ActiveTab);
		if (bCollectionsView)
		{
			if (bSharedScope)
			{
				if (E->Referrers.Num() < KuraLibrary::SharedReferrerThreshold)
				{
					continue;
				}
			}
			else if (bUnsortedScope)
			{
				// Unsorted is the to-file queue, so only roots: dependencies
				// were never meant to be filed in the first place (SPEC F8).
				if (!E->bIsRoot || E->Collections.Num() > 0)
				{
					continue;
				}
			}
			else if (ScopeIds.Num() > 0)
			{
				bool bFiled = false;
				for (const FString& Cid : E->Collections)
				{
					if (ScopeIds.Contains(Cid)) { bFiled = true; break; }
				}
				if (!bFiled)
				{
					continue;
				}
			}
		}
		else if (!Scope.IsEmpty())
		{
			const bool bInFolder = bIncludeSubfolders
				? (E->Folder == Scope || E->Folder.StartsWith(Scope + TEXT("/")))
				: (E->Folder == Scope);
			if (!bInFolder)
			{
				continue;
			}
		}
		if (!Needle.IsEmpty())
		{
			// Both sides are already lowercase, so this can be the cheap
			// case-sensitive search - the default Contains is not.
			bool bMatch = E->PackageLower.Contains(Needle, ESearchCase::CaseSensitive)
				|| E->NotesLower.Contains(Needle, ESearchCase::CaseSensitive);
			for (int32 Idx = 0; !bMatch && Idx < E->TagsLower.Num(); ++Idx)
			{
				bMatch = E->TagsLower[Idx].Contains(Needle, ESearchCase::CaseSensitive);
			}
			if (!bMatch)
			{
				continue;
			}
		}
		bool bTagsOk = true;
		for (const FString& T : ActiveTags)
		{
			if (!E->Tags.Contains(T))
			{
				bTagsOk = false;
				break;
			}
		}
		if (!bTagsOk)
		{
			continue;
		}
		if (bClassOk)
		{
			FilteredEntries.Add(E);
		}
		else
		{
			++ScopeCountOtherTabs;
		}
	}

	if (bCollectionsView && bShowDeps && !bSharedScope)
	{
		// "What are these made of": walk out from the roots that survived the
		// filters. The dependencies themselves bypass tab / search / tag - they
		// are here because of what they belong to, not on their own merits.
		TArray<TSharedPtr<FKuraEntry>> Deps;
		CollectExclusiveDeps(FilteredEntries, Deps);
		FilteredEntries.Append(Deps);
	}

	// One key + one direction, Content Browser style (2026-07-25). Ascending
	// always means A-Z / oldest / smallest, so the direction reads the same
	// whichever key is active.
	FilteredEntries.Sort([this](const TSharedPtr<FKuraEntry>& A, const TSharedPtr<FKuraEntry>& B)
	{
		int32 Cmp = 0;
		switch (SortMode)
		{
		case 1:
			// IngestedAt is ISO-8601, so string order is chronological;
			// identity survives updates (SPEC F1), so re-uploads keep their
			// original spot.
			Cmp = A->IngestedAt.Compare(B->IngestedAt);
			break;
		case 2:
			Cmp = A->Size < B->Size ? -1 : (A->Size > B->Size ? 1 : 0);
			break;
		default:
			Cmp = A->Name.Compare(B->Name, ESearchCase::IgnoreCase);
			break;
		}
		// Tie-break stays ascending either way, so equal keys hold one order.
		return Cmp != 0 ? (bSortAscending ? Cmp < 0 : Cmp > 0) : A->RelPath < B->RelPath;
	});

	// Membership set, not a linear search per item: "select all, then narrow
	// the filter" is otherwise quadratic in the size of the library.
	TSet<const FKuraEntry*> Visible;
	Visible.Reserve(FilteredEntries.Num());
	for (const TSharedPtr<FKuraEntry>& E : FilteredEntries)
	{
		Visible.Add(E.Get());
	}

	if (TileView.IsValid())
	{
		// Drop selections the filter just hid (or stale entries from a disk
		// refresh) - Import/Delete must never act on items the user can no
		// longer see.
		for (const TSharedPtr<FKuraEntry>& Item : TileView->GetSelectedItems())
		{
			if (!Visible.Contains(Item.Get()))
			{
				TileView->SetItemSelection(Item, false);
			}
		}
		TileView->RequestListRefresh();
	}
	if (Selected.IsValid() && !Visible.Contains(Selected.Get()))
	{
		Selected.Reset();
		RebuildLinkLists();
	}
}

void SKuraBrowser::SelectEntryByPackage(const FString& Package, bool bShowFolders)
{
	const TSharedPtr<FKuraEntry>* Found = EntryByPackage.Find(Package);
	if (!Found)
	{
		return;
	}
	if (!bShowDeps && !(*Found)->bIsRoot)
	{
		bShowDeps = true; // the target must be visible after the jump
	}
	// Cross-class jump (deps / referrers / Locate) switches to the target's tab.
	ActiveTab = KuraLibrary::TabIndexForClass((*Found)->Class);
	if (bShowFolders)
	{
		bCollectionsView = false; // "Locate in Folder Tree" is a folder action
	}
	// Whichever view is showing has to end up with the entry visible: the
	// folder always exists, but the target may be filed in no collection at
	// all (a dependency usually is), so widen the collection scope to all.
	SelectedCollection.Empty();
	if (CollectionTreeView.IsValid())
	{
		CollectionTreeView->ClearSelection();
	}
	SelectedFolder = (*Found)->Folder;
	SelectFolderInTree(SelectedFolder);
	ApplyFilter();
	Selected = *Found;
	if (TileView.IsValid())
	{
		TileView->SetSelection(*Found);
		TileView->RequestScrollIntoView(*Found);
	}
	RebuildLinkLists();
}

void SKuraBrowser::SelectFolderInTree(const FString& RelPath)
{
	if (!FolderTreeView.IsValid() || RelPath.IsEmpty())
	{
		return;
	}
	TArray<TSharedPtr<FKuraFolderNode>> Chain;
	TFunction<bool(const TArray<TSharedPtr<FKuraFolderNode>>&)> Find =
		[this, &Chain, &RelPath, &Find](const TArray<TSharedPtr<FKuraFolderNode>>& Nodes) -> bool
	{
		for (const TSharedPtr<FKuraFolderNode>& Node : Nodes)
		{
			const bool bOnPath = !Node->RelPath.IsEmpty()
				&& (Node->RelPath == RelPath || RelPath.StartsWith(Node->RelPath + TEXT("/")));
			if (!bOnPath)
			{
				continue;
			}
			Chain.Add(Node);
			if (Node->RelPath == RelPath || Find(Node->Children))
			{
				return true;
			}
			Chain.Pop();
		}
		return false;
	};
	if (!Find(FolderRoots))
	{
		// Entry sits directly at the top of the asset tree (no folder node for
		// it) - no tree selection = the whole library.
		FolderTreeView->ClearSelection();
		return;
	}
	for (int32 Idx = 0; Idx < Chain.Num() - 1; ++Idx)
	{
		FolderTreeView->SetItemExpansion(Chain[Idx], true);
		ExpandedFolders.Add(Chain[Idx]->RelPath);
	}
	FolderTreeView->SetSelection(Chain.Last());
}

TSharedRef<ITableRow> SKuraBrowser::MakeFolderRow(TSharedPtr<FKuraFolderNode> Node, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(STableRow<TSharedPtr<FKuraFolderNode>>, Owner)
	[
		SNew(STextBlock).Text(FText::FromString(Node->Name))
	];
}

void SKuraBrowser::GetFolderChildren(TSharedPtr<FKuraFolderNode> Node, TArray<TSharedPtr<FKuraFolderNode>>& OutChildren)
{
	OutChildren = Node->Children;
}

TSharedRef<ITableRow> SKuraBrowser::MakeCollectionRow(TSharedPtr<FKuraCollectionNode> Node,
	const TSharedRef<STableViewBase>& Owner)
{
	return SNew(STableRow<TSharedPtr<FKuraCollectionNode>>, Owner)
	[
		MakeCollectionRowContent(Node)
	];
}

/** Icon + name + per-tab count. Shared by the tree rows and the pinned query
 *  rows below it, which are the same thing in a different container. */
TSharedRef<SWidget> SKuraBrowser::MakeCollectionRowContent(TSharedPtr<FKuraCollectionNode> Node)
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 4.f, 0.f)
		[
			SNew(SImage)
			// The saved queries are not containers - different glyphs so they
			// do not read as "just another collection".
			.Image(FAppStyle::GetBrush(
				Node->bUnsorted ? "Icons.Filter"
				: Node->bShared ? "Icons.GroupActors"
				: "Icons.FolderClosed"))
			.DesiredSizeOverride(FVector2D(14.f, 14.f))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Node->Name))
			.ToolTipText(
				Node->bUnsorted
				? LOCTEXT("UnsortedTip",
					"Assets you sent to the library that are not in any collection yet. This is a view, not a collection - it empties itself as you file things.")
				: Node->bShared
				? LOCTEXT("SharedTip",
					"Assets more than one other asset uses: master materials, material functions, parameter collections, shared textures. This is a view, not a collection - membership is read from the reference graph, so it is always current and nothing has to be filed here.")
				: FText::GetEmpty())
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.f, 0.f, 2.f, 0.f)
		[
			SNew(STextBlock)
			// What THIS tab would list, not the collection's total: a row
			// reading 12 under Textures when the tab shows 3 is a lie.
			.Text_Lambda([this, Node]() { return FText::AsNumber(Node->CountForTab(ActiveTab)); })
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
}

void SKuraBrowser::GetCollectionChildren(TSharedPtr<FKuraCollectionNode> Node,
	TArray<TSharedPtr<FKuraCollectionNode>>& OutChildren)
{
	OutChildren = Node->Children;
}

TSharedPtr<SWidget> SKuraBrowser::MakeCollectionContextMenu()
{
	if (!CollectionTreeView.IsValid())
	{
		return nullptr;
	}
	const TArray<TSharedPtr<FKuraCollectionNode>> Sel = CollectionTreeView->GetSelectedItems();
	const TSharedPtr<FKuraCollectionNode> Node = Sel.Num() > 0 ? Sel[0] : nullptr;
	// Unsorted and Shared Dependencies are views, not collections: they cannot
	// be renamed, nested into or deleted. Right-clicking one still offers the
	// one thing that makes sense there (create a collection to file into).
	const bool bReal = Node.IsValid() && !Node->IsQuery();

	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr);
	if (bReal)
	{
		Menu.AddMenuEntry(
			FText::Format(LOCTEXT("CtxCollectionImport", "Import {0} {0}|plural(one=Entry,other=Entries)"),
				FText::AsNumber(Node->Count)),
			LOCTEXT("CtxCollectionImportTip",
				"Copy everything filed in this collection (nested collections included), plus the dependencies it needs, into the project at the stable /Game/_AssetLib paths"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Import"),
			FUIAction(
				FExecuteAction::CreateLambda([this, Id = Node->Id]()
				{
					KuraActions::RunCollectionImportFlow(Id, AsShared());
				}),
				FCanExecuteAction::CreateLambda([Count = Node->Count]() { return Count > 0; })));
		Menu.AddMenuSeparator();
	}
	// One gesture, one meaning (2026-08-13): right-clicking a collection
	// creates INSIDE it, which is what every other tree in the editor does.
	// The top-level route is right-clicking empty space (Slate clears the
	// selection for us) or the + button in the header.
	if (bReal)
	{
		Menu.AddMenuEntry(
			FText::Format(LOCTEXT("CtxCollectionNewChild", "New Collection Inside \"{0}\""),
				FText::FromString(Node->Name)),
			LOCTEXT("CtxCollectionNewChildTip",
				"Create a collection nested inside this one. To create a top-level collection, right-click empty space in this panel or use the + button."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Plus"),
			FUIAction(FExecuteAction::CreateLambda([this, Id = Node->Id]()
			{
				CreateCollectionUnder(Id);
			})));
	}
	else
	{
		Menu.AddMenuEntry(
			LOCTEXT("CtxCollectionNew", "New Collection"),
			LOCTEXT("CtxCollectionNewTip", "Create a top-level collection"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Plus"),
			FUIAction(FExecuteAction::CreateLambda([this]()
			{
				CreateCollectionUnder(FString());
			})));
	}
	if (bReal)
	{
		Menu.AddMenuEntry(
			LOCTEXT("CtxCollectionRename", "Rename"),
			LOCTEXT("CtxCollectionRenameTip", "Rename this collection; nothing on disk changes"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Edit"),
			FUIAction(FExecuteAction::CreateLambda([this, Id = Node->Id, Name = Node->Name]()
			{
				KuraActions::RunCollectionRenameFlow(Id, Name, AsShared());
			})));
		Menu.AddSubMenu(
			LOCTEXT("CtxCollectionMove", "Move to"),
			LOCTEXT("CtxCollectionMoveTip",
				"Nest this collection under another one, or lift it back to the top level. Its contents come with it."),
			FNewMenuDelegate::CreateLambda([this, Id = Node->Id](FMenuBuilder& SubMenu)
			{
				FillMoveCollectionMenu(SubMenu, Id);
			}),
			false,
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.ArrowRight"));
		Menu.AddMenuSeparator();
		Menu.AddMenuEntry(
			LOCTEXT("CtxCollectionDelete", "Delete Collection"),
			LOCTEXT("CtxCollectionDeleteTip",
				"Remove this collection and any collections inside it. The assets stay in the library - they just stop being filed here."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Delete"),
			FUIAction(FExecuteAction::CreateLambda([this, Id = Node->Id, Name = Node->Name]()
			{
				KuraActions::RunCollectionDeleteFlow(Id, Name, AsShared());
			})));
	}
	return Menu.MakeWidget();
}

void SKuraBrowser::FillAddToCollectionMenu(FMenuBuilder& Menu, const TArray<FString>& Packages)
{
	// Flat list with indented names: a submenu-per-level would bury a leaf
	// collection three hovers deep for no gain.
	int32 Count = 0;
	TFunction<void(const TArray<TSharedPtr<FKuraCollectionNode>>&, int32)> Add =
		[this, &Add, &Menu, &Packages, &Count](
			const TArray<TSharedPtr<FKuraCollectionNode>>& Nodes, int32 Depth)
	{
		for (const TSharedPtr<FKuraCollectionNode>& Node : Nodes)
		{
			if (Node->IsQuery())
			{
				continue; // not a real collection - nothing to file into
			}
			++Count;
			const FString Indent = FString::ChrN(Depth * 4, TEXT(' '));
			Menu.AddMenuEntry(
				FText::FromString(Indent + Node->Name),
				FText::GetEmpty(), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([this, Id = Node->Id, Packages]()
				{
					KuraActions::RunCollectionAssignFlow(Packages, Id, /*bAdd*/ true, AsShared());
				})));
			Add(Node->Children, Depth + 1);
		}
	};
	Add(CollectionRoots, 0);

	if (Count > 0)
	{
		Menu.AddMenuSeparator();
	}
	Menu.AddMenuEntry(
		LOCTEXT("CtxAddToNewCollection", "New Collection"),
		LOCTEXT("CtxAddToNewCollectionTip",
			"Create a collection and file the selection into it"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Plus"),
		FUIAction(FExecuteAction::CreateLambda([this, Packages]()
		{
			KuraActions::RunCollectionCreateFlow(FString(), AsShared(), Packages);
		})));
}

void SKuraBrowser::FillRemoveFromCollectionMenu(FMenuBuilder& Menu, const TArray<FString>& Packages)
{
	// Only offer collections the selection is actually in - listing every
	// collection would mostly be no-ops.
	TSet<FString> Filed;
	for (const TSharedPtr<FKuraEntry>& E : AllEntries)
	{
		if (Packages.Contains(E->Package))
		{
			Filed.Append(TSet<FString>(E->Collections));
		}
	}

	int32 Count = 0;
	TFunction<void(const TArray<TSharedPtr<FKuraCollectionNode>>&, int32)> Add =
		[this, &Add, &Menu, &Packages, &Filed, &Count](
			const TArray<TSharedPtr<FKuraCollectionNode>>& Nodes, int32 Depth)
	{
		for (const TSharedPtr<FKuraCollectionNode>& Node : Nodes)
		{
			if (Filed.Contains(Node->Id))
			{
				++Count;
				Menu.AddMenuEntry(
					FText::FromString(FString::ChrN(Depth * 4, TEXT(' ')) + Node->Name),
					FText::GetEmpty(), FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([this, Id = Node->Id, Packages]()
					{
						KuraActions::RunCollectionAssignFlow(Packages, Id, /*bAdd*/ false, AsShared());
					})));
			}
			Add(Node->Children, Depth + 1);
		}
	};
	Add(CollectionRoots, 0);

	if (Count == 0)
	{
		Menu.AddWidget(
			SNew(SBox).Padding(12.f, 4.f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("CtxRemoveNone", "Not in any collection"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			],
			FText::GetEmpty());
	}
}

void SKuraBrowser::FillMoveCollectionMenu(FMenuBuilder& Menu, const FString& Id)
{
	Menu.AddMenuEntry(
		LOCTEXT("CtxMoveTop", "Top Level"),
		FText::GetEmpty(),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this, Id]()
		{
			KuraActions::RunCollectionMoveFlow(Id, FString(), AsShared());
		})));
	Menu.AddMenuSeparator();

	// Skip the moving node's own subtree: nesting a collection inside itself
	// would detach it from the tree (kura.catalog refuses it anyway).
	TFunction<void(const TArray<TSharedPtr<FKuraCollectionNode>>&, int32)> Add =
		[this, &Add, &Menu, &Id](const TArray<TSharedPtr<FKuraCollectionNode>>& Nodes, int32 Depth)
	{
		for (const TSharedPtr<FKuraCollectionNode>& Node : Nodes)
		{
			if (Node->IsQuery() || Node->Id == Id)
			{
				continue;
			}
			Menu.AddMenuEntry(
				FText::FromString(FString::ChrN(Depth * 4, TEXT(' ')) + Node->Name),
				FText::GetEmpty(), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([this, Id, Parent = Node->Id]()
				{
					KuraActions::RunCollectionMoveFlow(Id, Parent, AsShared());
				})));
			Add(Node->Children, Depth + 1);
		}
	};
	Add(CollectionRoots, 0);
}

TSharedRef<ITableRow> SKuraBrowser::MakeEntryTile(TSharedPtr<FKuraEntry> Item, const TSharedRef<STableViewBase>& Owner)
{
	// Meta-mirror PNG over a class-colored block (SColorBlock, because
	// SBorder+WhiteBrush tinting did not render). Both are always built and
	// the thumbnail is fetched at PAINT time, not here: loading is async since
	// 2026-08-20, so a tile generated before its PNG has been read has to be
	// able to pick it up without being regenerated. The block doubles as the
	// placeholder - a class-colored tile is what an entry with no thumbnail at
	// all looks like too, so nothing flickers between the two states.
	const FString ThumbPath = Item->ThumbPath;
	TSharedRef<SWidget> Preview =
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			// Whole placeholder goes away once the PNG is up, so a thumbnail
			// that is not exactly tile-shaped letterboxes against the panel
			// background rather than against a colored slab.
			SNew(SOverlay)
			.Visibility_Lambda([ThumbPath]()
			{
				return KuraThumbs::Get(ThumbPath) ? EVisibility::Collapsed : EVisibility::HitTestInvisible;
			})
			+ SOverlay::Slot()
			[
				SNew(SColorBlock)
				.Color(ClassColor(Item->Class))
			]
			+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(ClassAbbrev(Item->Class)))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 22))
				.ColorAndOpacity(FLinearColor::White)
			]
		]
		+ SOverlay::Slot()
		[
			// keep the PNG square inside the non-square tile slot
			SNew(SScaleBox)
			.Stretch(EStretch::ScaleToFit)
			[
				SNew(SImage)
				.Image_Lambda([ThumbPath]() { return KuraThumbs::Get(ThumbPath); })
			]
		];

	// A dependency is a part of something, not a thing in its own right. Half
	// opacity keeps that legible when Show Dependencies mixes both into one
	// grid, without inventing a badge nobody would recognise.
	const float Emphasis = Item->bIsRoot ? 1.f : 0.55f;

	return SNew(STableRow<TSharedPtr<FKuraEntry>>, Owner)
	.Padding(4.f)
	.ToolTipText(Item->bIsRoot
		? FText::FromString(Item->Package)
		: FText::Format(LOCTEXT("TileDepTip", "{0}\n(dependency - it came in with something that uses it)"),
			FText::FromString(Item->Package)))
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SBox)
				.RenderOpacity(Emphasis)
				[
					Preview
				]
			]
			// Class color strip (Content Browser style) so the type reads at
			// a glance even when a PNG thumbnail covers the tile.
			+ SOverlay::Slot().VAlign(VAlign_Bottom)
			[
				SNew(SBox).HeightOverride(3.f)
				[
					SNew(SColorBlock).Color(ClassColor(Item->Class))
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Item->Name))
			.Font(FCoreStyle::GetDefaultFontStyle(
				Item->bIsRoot ? "Bold" : "Regular", 9))
			.RenderOpacity(Emphasis)
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::FromString(Item->Folder))
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.RenderOpacity(Emphasis)
		]
	];
}

TSharedRef<ITableRow> SKuraBrowser::MakeTagRow(TSharedPtr<FString> InTag, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(STableRow<TSharedPtr<FString>>, Owner)
	[
		SNew(SCheckBox)
		.IsChecked_Lambda([this, InTag]()
		{
			return ActiveTags.Contains(*InTag) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this, InTag](ECheckBoxState State)
		{
			if (State == ECheckBoxState::Checked)
			{
				ActiveTags.Add(*InTag);
			}
			else
			{
				ActiveTags.Remove(*InTag);
			}
			ApplyFilter();
		})
		[
			SNew(STextBlock).Text(FText::FromString(*InTag))
		]
	];
}

void SKuraBrowser::RebuildLinkLists()
{
	const auto Fill = [](TArray<TSharedPtr<FString>>& Items, const TArray<FString>& Packages,
		const TSharedPtr<SListView<TSharedPtr<FString>>>& View)
	{
		Items.Reset();
		Items.Reserve(Packages.Num());
		for (const FString& Pkg : Packages)
		{
			Items.Add(MakeShared<FString>(Pkg));
		}
		if (View.IsValid())
		{
			View->RequestListRefresh();
		}
	};
	static const TArray<FString> Empty;
	Fill(DepsItems, Selected.IsValid() ? Selected->HardDeps : Empty, DepsListView);
	Fill(ReferrersItems, Selected.IsValid() ? Selected->Referrers : Empty, ReferrersListView);
	Fill(EngineDepsItems, Selected.IsValid() ? Selected->EngineDeps : Empty, EngineDepsListView);
}

TSharedRef<ITableRow> SKuraBrowser::MakeLinkRow(TSharedPtr<FString> Package,
	const TSharedRef<STableViewBase>& Owner, bool bClickable)
{
	const FString Pkg = Package.IsValid() ? *Package : FString();
	// Engine deps are recorded but never packaged (SPEC 7.1) - they exist in
	// every installation, so plain text instead of a jump link.
	TSharedRef<SWidget> Content = bClickable
		? StaticCastSharedRef<SWidget>(
			SNew(SHyperlink)
			.Text(FText::FromString(Pkg))
			.ToolTipText(LOCTEXT("JumpTip", "Jump to this entry"))
			.OnNavigate_Lambda([this, Pkg]() { SelectEntryByPackage(Pkg); }))
		: StaticCastSharedRef<SWidget>(
			SNew(STextBlock)
			.Text(FText::FromString(Pkg))
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.AutoWrapText(true));

	return SNew(STableRow<TSharedPtr<FString>>, Owner)
	.Style(&FAppStyle::Get().GetWidgetStyle<FTableRowStyle>("TableView.NoHoverTableRow"))
	.Padding(FMargin(0.f, 1.f))
	[
		Content
	];
}

TSharedRef<SWidget> SKuraBrowser::MakeLinkList(TSharedPtr<SListView<TSharedPtr<FString>>>& OutView,
	TArray<TSharedPtr<FString>>& Items, bool bClickable)
{
	// A list wants to be as tall as everything it holds; the cap is what turns
	// "referenced by 2,707 instances" into a scrollable strip instead of a
	// details panel a thousand screens long. Short lists still size to fit.
	return SNew(SBox)
		.MaxDesiredHeight(160.f)
		[
			SAssignNew(OutView, SListView<TSharedPtr<FString>>)
			.ListItemsSource(&Items)
			.SelectionMode(ESelectionMode::None)
			.OnGenerateRow_Lambda([this, bClickable](TSharedPtr<FString> Item,
				const TSharedRef<STableViewBase>& Owner)
			{
				return MakeLinkRow(Item, Owner, bClickable);
			})
		];
}

TSharedRef<SWidget> SKuraBrowser::MakeDetailsPanel()
{
	const auto Row = [this](const FText& Label, TFunction<FString(const FKuraEntry&)> Getter) -> TSharedRef<SWidget>
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text_Lambda([this, Getter]()
				{
					return Selected.IsValid() ? FText::FromString(Getter(*Selected)) : FText::GetEmpty();
				})
			];
	};

	return SNew(SBorder)
	.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
	.Padding(8.f)
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					// Guideline type ramp: detail-panel asset name is the
					// "heading" tier (13px semibold), not a display size.
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					.AutoWrapText(true)
					.Text_Lambda([this]()
					{
						if (!Selected.IsValid())
						{
							return LOCTEXT("NoSelection", "Select an entry");
						}
						const int32 Num = TileView.IsValid() ? TileView->GetNumItemsSelected() : 0;
						return Num > 1
							? FText::Format(LOCTEXT("MultiSelHeader", "{0}  (+{1} more selected)"),
								FText::FromString(Selected->Name), FText::AsNumber(Num - 1))
							: FText::FromString(Selected->Name);
					})
				]
				// Large thumbnail when the Meta PNG exists (Phase 2)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f).HAlign(HAlign_Left)
				[
					SNew(SBox)
					.WidthOverride(192.f).HeightOverride(192.f)
					.Visibility_Lambda([this]()
					{
						return (Selected.IsValid() && KuraThumbs::Get(Selected->ThumbPath) != nullptr)
							? EVisibility::Visible : EVisibility::Collapsed;
					})
					[
						SNew(SImage)
						.Image_Lambda([this]() -> const FSlateBrush*
						{
							return Selected.IsValid() ? KuraThumbs::Get(Selected->ThumbPath) : nullptr;
						})
					]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					Row(LOCTEXT("DetailPackage", "Package"), [](const FKuraEntry& E) { return E.Package; })
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					Row(LOCTEXT("DetailClass", "Class"), [](const FKuraEntry& E)
					{
						return E.bIsRoot ? E.Class : E.Class + TEXT("  (dependency)");
					})
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					Row(LOCTEXT("DetailSize", "Size"), [](const FKuraEntry& E) { return FormatSize(E.Size); })
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					Row(LOCTEXT("DetailEngine", "Engine Version"), [](const FKuraEntry& E) { return E.EngineVersion; })
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					Row(LOCTEXT("DetailSource", "Source Project"), [](const FKuraEntry& E) { return E.SourceProject; })
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					Row(LOCTEXT("DetailUpdated", "Updated"), [](const FKuraEntry& E) { return E.UpdatedAt; })
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					// Where this entry is filed (F8). Read-only here; the tile
					// context menu is where filing happens.
					Row(LOCTEXT("DetailCollections", "Collections"),
						[this](const FKuraEntry& E)
						{
							const TArray<FString> Names = CollectionNamesOf(E);
							return Names.Num() > 0 ? FString::Join(Names, TEXT(", "))
								: FString(TEXT("-"));
						})
				]

				// Tags: editable, committed on Enter (writes meta + index)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("DetailTags", "Tags"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					// SMenuAnchor hosts the autocomplete dropdown under the box;
					// suggestions come from every tag already in the library.
					SAssignNew(TagSuggestAnchor, SMenuAnchor)
					.Placement(MenuPlacement_ComboBox)
					.MenuContent(
						SNew(SBox).MaxDesiredHeight(160.f).MinDesiredWidth(180.f)
						[
							SAssignNew(TagSuggestListView, SListView<TSharedPtr<FString>>)
							.ListItemsSource(&TagSuggestions)
							.OnGenerateRow_Lambda([](TSharedPtr<FString> Item, const TSharedRef<STableViewBase>& Owner)
							{
								return SNew(STableRow<TSharedPtr<FString>>, Owner)
								[
									SNew(STextBlock).Text(FText::FromString(*Item)).Margin(FMargin(4.f, 2.f))
								];
							})
							.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item, ESelectInfo::Type SelectInfo)
							{
								if (Item.IsValid() && SelectInfo != ESelectInfo::Direct)
								{
									AcceptTagSuggestion(*Item);
								}
							})
						])
					[
						SAssignNew(TagsEditBox, SEditableTextBox)
						.IsEnabled_Lambda([this]() { return Selected.IsValid(); })
						.HintText(LOCTEXT("TagsHint", "comma separated - press Enter to save"))
						.Text_Lambda([this]()
						{
							return Selected.IsValid()
								? FText::FromString(FString::Join(Selected->Tags, TEXT(", ")))
								: FText::GetEmpty();
						})
						.OnTextChanged(this, &SKuraBrowser::UpdateTagSuggestions)
						.OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type CommitType)
						{
							if (TagSuggestAnchor.IsValid())
							{
								TagSuggestAnchor->SetIsOpen(false, false);
							}
							if (CommitType == ETextCommit::OnEnter && Selected.IsValid())
							{
								CommitTags(Selected->Package, NewText.ToString());
							}
						})
					]
				]

				// Notes: editable, committed on focus loss or Enter
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("DetailNotes", "Notes"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBox).MinDesiredHeight(48.f)
					[
						SNew(SMultiLineEditableTextBox)
						.IsEnabled_Lambda([this]() { return Selected.IsValid(); })
						.Text_Lambda([this]()
						{
							return Selected.IsValid() ? FText::FromString(Selected->Notes) : FText::GetEmpty();
						})
						.OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type)
						{
							if (Selected.IsValid() && NewText.ToString() != Selected->Notes)
							{
								CommitNotes(Selected->Package, NewText.ToString());
							}
						})
					]
				]

				// Dependencies (clickable jump)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("DetailDeps", "Depends On"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Visibility_Lambda([this]()
					{
						return (Selected.IsValid() && Selected->HardDeps.Num() == 0)
							? EVisibility::Visible : EVisibility::Collapsed;
					})
					.Text(LOCTEXT("NoneDeps", "(none)"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeLinkList(DepsListView, DepsItems, /*bClickable*/ true)
				]

				// Referenced by (clickable jump; delete guard preview)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("DetailReferrers", "Referenced By"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Visibility_Lambda([this]()
					{
						return (Selected.IsValid() && Selected->Referrers.Num() == 0)
							? EVisibility::Visible : EVisibility::Collapsed;
					})
					.Text(LOCTEXT("NoneRefs", "(none)"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeLinkList(ReferrersListView, ReferrersItems, /*bClickable*/ true)
				]

				// Engine dependencies (read-only; recorded in meta, not packaged)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("DetailEngineDeps", "Engine Dependencies"))
					.ToolTipText(LOCTEXT("EngineDepsTip",
						"/Engine/ assets referenced by this entry. They ship with every UE installation, so the library records them without packaging them."))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Visibility_Lambda([this]()
					{
						return (Selected.IsValid() && Selected->EngineDeps.Num() == 0)
							? EVisibility::Visible : EVisibility::Collapsed;
					})
					.Text(LOCTEXT("NoneEngineDeps", "(none)"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeLinkList(EngineDepsListView, EngineDepsItems, /*bClickable*/ false)
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
		[
			SNew(SButton)
			.HAlign(HAlign_Center)
			.Text_Lambda([this]()
			{
				const int32 Num = TileView.IsValid() ? TileView->GetNumItemsSelected() : 0;
				return Num > 1
					? FText::Format(LOCTEXT("ImportMany", "Import {0} Entries into Current Project"),
						FText::AsNumber(Num))
					: LOCTEXT("Import", "Import into Current Project");
			})
			.ToolTipText(LOCTEXT("ImportBtnTip",
				"Copy the selection and its current dependency closure into the project at the stable /Game/_AssetLib paths. Delete is on the tile right-click menu."))
			.IsEnabled_Lambda([this]() { return Selected.IsValid(); })
			.OnClicked(this, &SKuraBrowser::OnImportClicked)
		]
	];
}

// --- actions -------------------------------------------------------------

FReply SKuraBrowser::OnImportClicked()
{
	KuraActions::RunImportFlow(SelectedPackages(), AsShared());
	return FReply::Handled();
}

FReply SKuraBrowser::OnRefreshClicked()
{
	// The explicit Refresh is the full re-read: it is the answer to "the panel
	// disagrees with the disk", so it must not trust the timestamps that the
	// incremental path is built on.
	RefreshFromDisk(/*bForceFullReload*/ true);
	return FReply::Handled();
}

void SKuraBrowser::SaveTileSize()
{
	TMap<FString, double> Numbers;
	Numbers.Add(TEXT("tile_size"), (double)FMath::RoundToInt(TileSize));
	if (!KuraLibrary::SaveConfig({}, Numbers))
	{
		KuraActions::Notify(false, LOCTEXT("TileSizeSaveFailed",
			"Kura: could not save the tile size to config.json"));
	}
}

void SKuraBrowser::ShowLibrarySetupDialog(const FString& InitialRoot)
{
	// Opens on whatever the config already points at, so the missing-root card
	// lands on the path that went missing rather than on the default. An
	// explicit InitialRoot wins: that is how a rejected path comes back with
	// the text still in the box instead of making the user retype it.
	const bool bConfigured = KuraLibrary::IsConfigured();
	const FString StartRoot = !InitialRoot.IsEmpty()
		? InitialRoot
		: (bConfigured ? KuraLibrary::GetLibraryRoot()
			: KuraLibrary::GetDefaultLibraryRoot());

	// Create or adopt is the difference between an empty folder and someone
	// else's library (a reinstall, a second machine, a share), and the button
	// has to say which one is about to happen - "Create Library" over an
	// existing library reads as "and empty it". Cached rather than stat-ed per
	// frame, and refreshed from both ways the field can change, because
	// SetText raises no OnTextChanged.
	const TSharedRef<bool> bRootExists = MakeShared<bool>(
		IFileManager::Get().DirectoryExists(*StartRoot));
	const TSharedRef<SEditableTextBox> RootBox = SNew(SEditableTextBox)
		.Text(FText::FromString(StartRoot))
		.OnTextChanged_Lambda([bRootExists](const FText& NewText)
		{
			*bRootExists = IFileManager::Get().DirectoryExists(
				*NewText.ToString().TrimStartAndEnd());
		});

	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(bConfigured
			? LOCTEXT("LibSetupTitle", "Kura - Set Up Library")
			: LOCTEXT("FirstRunTitle", "Welcome to Kura"))
		.ClientSize(FVector2D(560.f, 240.f))
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		.SizingRule(ESizingRule::FixedSize);
	const TWeakPtr<SWindow> WeakWindow = Window;

	Window->SetContent(
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12.f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 10.f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(LOCTEXT("FirstRunBody",
					"Choose where your asset library lives on disk. Kura creates the folder and an empty index there, or picks up a library already sitting in it - that is how you reconnect to one after a reinstall. You can move it later in Settings."))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 6.f, 0.f)
				[
					SNew(STextBlock).Text(LOCTEXT("FirstRunRoot", "Library Root"))
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center) [ RootBox ]
				+ SHorizontalBox::Slot().AutoWidth().Padding(4.f, 0.f, 0.f, 0.f)
				[
					MakeBrowseButton(RootBox, [bRootExists](const FString& Picked)
					{
						*bRootExists = IFileManager::Get().DirectoryExists(*Picked);
					})
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.f) [ SNew(SBox) ]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
				[
					SNew(SButton)
					.Text_Lambda([bRootExists]()
					{
						return *bRootExists
							? LOCTEXT("FirstRunUse", "Use This Folder")
							: LOCTEXT("FirstRunCreate", "Create Library");
					})
					.OnClicked_Lambda([this, WeakWindow, RootBox]()
					{
						const FString Root = RootBox->GetText().ToString().TrimStartAndEnd();
						// Asked to close before the flow runs, so this window
						// is on its way out while the flow reports; a failure
						// then reopens it with the same text rather than
						// making the user retype the path. (Destruction is
						// deferred, so an error dialog does briefly sit on top
						// of this one - the same shape ShowTextPrompt uses for
						// its retry.)
						if (const TSharedPtr<SWindow> W = WeakWindow.Pin())
						{
							W->RequestDestroyWindow();
						}
						if (!KuraActions::RunCreateLibraryFlow(Root, AsShared()))
						{
							ShowLibrarySetupDialog(Root);
						}
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("FirstRunLater", "Not Now"))
					.ToolTipText(LOCTEXT("FirstRunLaterTip",
						"Leave the library unset. The panel keeps offering this from its empty state - nothing else works until a library exists."))
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
	FSlateApplication::Get().AddModalWindow(Window, AsShared());
}

// UE's own USceneThumbnailInfo defaults. Kura ships the same numbers, so the
// orbit fields left untouched render exactly like the Content Browser - and
// "Reset" below means "go back to looking like the Content Browser".
static constexpr double KuraDefaultOrbitPitch = -11.25;
static constexpr double KuraDefaultOrbitYaw = -157.5;
static constexpr double KuraDefaultOrbitZoom = 0.0;

// Label column of the settings rows. MinDesiredWidth rather than WidthOverride
// so a label that outgrows the column widens it instead of being clipped -
// "Show Dependencies by Default" lost its last letter at the old fixed 170
// (2026-08-15), and a fixed width would clip again at another font or DPI.
static constexpr float KuraSettingsLabelWidth = 210.f;

void SKuraBrowser::ShowSettingsDialog()
{
	const TSharedRef<SEditableTextBox> RootBox = SNew(SEditableTextBox)
		.Text(FText::FromString(KuraLibrary::GetLibraryRoot()));
	const TSharedRef<SEditableTextBox> PrefixBox = SNew(SEditableTextBox)
		.Text(FText::FromString(KuraLibrary::GetLibPrefix()));
	const TSharedRef<SEditableTextBox> MeshBox = SNew(SEditableTextBox)
		.Text(FText::FromString(KuraLibrary::GetMaterialPreviewMesh()))
		.HintText(LOCTEXT("CfgMeshHint", "empty = UE default sphere"))
		.ToolTipText(LOCTEXT("CfgMeshTip",
			"Static mesh to preview material thumbnails on. Leave empty to use UE's own default (the sphere), matching the Content Browser."));
	// The orbit camera only reaches a render through the material path, which
	// needs a preview mesh (see ingest._render_thumbnail). With the field empty
	// every class renders on its own thumbnail info and these three are inert -
	// so disable them rather than let someone type numbers that do nothing.
	const TAttribute<bool> OrbitEnabled = TAttribute<bool>::Create(
		TAttribute<bool>::FGetter::CreateLambda([MeshBox]()
		{
			return !MeshBox->GetText().IsEmptyOrWhitespace();
		}));

	const TSharedRef<SEditableTextBox> PitchBox = SNew(SEditableTextBox)
		.IsEnabled(OrbitEnabled)
		.Text(FText::AsNumber(KuraLibrary::GetConfigNumber(TEXT("thumb_orbit_pitch"), KuraDefaultOrbitPitch)));
	const TSharedRef<SEditableTextBox> YawBox = SNew(SEditableTextBox)
		.IsEnabled(OrbitEnabled)
		.Text(FText::AsNumber(KuraLibrary::GetConfigNumber(TEXT("thumb_orbit_yaw"), KuraDefaultOrbitYaw)));
	const TSharedRef<SEditableTextBox> ZoomBox = SNew(SEditableTextBox)
		.IsEnabled(OrbitEnabled)
		.Text(FText::AsNumber(KuraLibrary::GetConfigNumber(TEXT("thumb_orbit_zoom"), KuraDefaultOrbitZoom)));
	const TSharedRef<SEditableTextBox> ResBox = SNew(SEditableTextBox)
		.Text(FText::FromString(FString::FromInt(
			(int32)KuraLibrary::GetConfigNumber(TEXT("thumb_resolution"), 256.0))));
	const TSharedRef<SEditableTextBox> TileBox = SNew(SEditableTextBox)
		.Text(FText::FromString(FString::FromInt(
			(int32)KuraLibrary::GetConfigNumber(TEXT("tile_size"), 150.0))));
	const TSharedRef<SCheckBox> ShowDepsBox = SNew(SCheckBox)
		.IsChecked(KuraLibrary::GetConfigNumber(TEXT("show_dependencies"), 0.0) != 0.0
			? ECheckBoxState::Checked : ECheckBoxState::Unchecked);

	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("SettingsTitle", "Kura Settings"))
		// Wider than the other dialogs: the label column plus three orbit
		// fields and their Reset button all have to fit on one row.
		.ClientSize(FVector2D(620.f, 400.f))
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		.SizingRule(ESizingRule::FixedSize);
	const TWeakPtr<SWindow> WeakWindow = Window;

	const auto LabeledRow = [](const FText& Label, TSharedRef<SWidget> Field) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).MinDesiredWidth(KuraSettingsLabelWidth)
				[
					SNew(STextBlock).Text(Label)
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				Field
			];
	};

	Window->SetContent(
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(10.f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
			[
				LabeledRow(LOCTEXT("CfgRoot", "Library Root"),
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center) [ RootBox ]
					+ SHorizontalBox::Slot().AutoWidth().Padding(4.f, 0.f, 0.f, 0.f)
					[
						MakeBrowseButton(RootBox)
					])
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
			[
				LabeledRow(LOCTEXT("CfgPrefix", "Stable Path Prefix"), PrefixBox)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
			[
				LabeledRow(LOCTEXT("CfgMesh", "Material Preview Mesh"), MeshBox)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					// Label stays enabled on purpose: it carries the tooltip that
					// explains why the fields next to it may be greyed out, and a
					// disabled widget would not show one.
					SNew(SBox).MinDesiredWidth(KuraSettingsLabelWidth)
					[
						SNew(STextBlock).Text(LOCTEXT("CfgOrbit", "Thumb Camera P / Y / Z"))
						.ToolTipText(LOCTEXT("CfgOrbitTip",
							"Orbit pitch / yaw / zoom for library MATERIAL thumbnails, and only when a Material Preview Mesh is set above - without one, every class renders on its own thumbnail info and these are ignored. Static meshes always keep the angle you set on them in the Content Browser. Use Refresh Thumbnail (or re-send the asset) to apply."))
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0.f, 0.f, 4.f, 0.f) [ PitchBox ]
				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0.f, 0.f, 4.f, 0.f) [ YawBox ]
				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0.f, 0.f, 4.f, 0.f) [ ZoomBox ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SButton)
					.IsEnabled(OrbitEnabled)
					.Text(LOCTEXT("CfgOrbitReset", "Reset"))
					.ToolTipText(LOCTEXT("CfgOrbitResetTip",
						"Restore UE's default thumbnail camera (-11.25 / -157.5 / 0)"))
					.OnClicked_Lambda([PitchBox, YawBox, ZoomBox]()
					{
						PitchBox->SetText(FText::AsNumber(KuraDefaultOrbitPitch));
						YawBox->SetText(FText::AsNumber(KuraDefaultOrbitYaw));
						ZoomBox->SetText(FText::AsNumber(KuraDefaultOrbitZoom));
						return FReply::Handled();
					})
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
			[
				LabeledRow(LOCTEXT("CfgRes", "Thumbnail Resolution"), ResBox)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
			[
				LabeledRow(LOCTEXT("CfgTile", "Default Tile Size"), TileBox)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
			[
				LabeledRow(LOCTEXT("CfgShowDeps", "Show Dependencies by Default"), ShowDepsBox)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(LOCTEXT("CfgNote",
					"Changing the stable path prefix after assets were ingested breaks every stored reference - existing libraries should keep it. Saved to %APPDATA%\\Kura\\config.json."))
			]
			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				SNew(SBox) // spacer
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
				[
					SNew(SButton)
					.Text(LOCTEXT("CfgSave", "Save"))
					.OnClicked_Lambda([this, WeakWindow, RootBox, PrefixBox, MeshBox, PitchBox, YawBox, ZoomBox,
						ResBox, TileBox, ShowDepsBox]()
					{
						TMap<FString, FString> Fields;
						Fields.Add(TEXT("library_root"), RootBox->GetText().ToString().TrimStartAndEnd());
						Fields.Add(TEXT("lib_prefix"), PrefixBox->GetText().ToString().TrimStartAndEnd());
						Fields.Add(TEXT("material_preview_mesh"), MeshBox->GetText().ToString().TrimStartAndEnd());
						TMap<FString, double> Numbers;
						Numbers.Add(TEXT("thumb_orbit_pitch"), FCString::Atod(*PitchBox->GetText().ToString()));
						Numbers.Add(TEXT("thumb_orbit_yaw"), FCString::Atod(*YawBox->GetText().ToString()));
						Numbers.Add(TEXT("thumb_orbit_zoom"), FCString::Atod(*ZoomBox->GetText().ToString()));
						Numbers.Add(TEXT("thumb_resolution"),
							FMath::Clamp(FCString::Atod(*ResBox->GetText().ToString()), 32.0, 2048.0));
						const double NewTileSize =
							FMath::Clamp(FCString::Atod(*TileBox->GetText().ToString()), 90.0, 260.0);
						Numbers.Add(TEXT("tile_size"), NewTileSize);
						Numbers.Add(TEXT("show_dependencies"),
							ShowDepsBox->IsChecked() ? 1.0 : 0.0);
						bool bOk = KuraLibrary::SaveConfig(Fields, Numbers);
						if (bOk)
						{
							const TSharedPtr<FJsonObject> Result = KuraPython::Call(TEXT("reload_config()"));
							bOk = Result.IsValid();
						}
						KuraActions::Notify(bOk, bOk
							? LOCTEXT("CfgSaved", "Kura: settings saved")
							: LOCTEXT("CfgFailed", "Kura: saving settings failed"));
						if (bOk)
						{
							// keep the dialog (and its values) open on failure
							TileSize = (float)NewTileSize;
							if (const TSharedPtr<SWindow> W = WeakWindow.Pin())
							{
								W->RequestDestroyWindow();
							}
							// Settings can move the library root itself, so
							// nothing about the previous read carries over.
							RefreshFromDisk(/*bForceFullReload*/ true);
						}
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("CfgCancel", "Cancel"))
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
	FSlateApplication::Get().AddModalWindow(Window, AsShared());
}

void SKuraBrowser::UpdateTagSuggestions(const FText& Text)
{
	TagSuggestions.Reset();
	const FString Full = Text.ToString();

	// Autocomplete the token after the last comma; tags already present in
	// the field are excluded so a suggestion always adds something new.
	int32 CommaIdx = INDEX_NONE;
	FString Token = Full.FindLastChar(TEXT(','), CommaIdx) ? Full.Mid(CommaIdx + 1) : Full;
	Token.TrimStartAndEndInline();

	if (!Token.IsEmpty())
	{
		TSet<FString> Present;
		TArray<FString> Parts;
		Full.ParseIntoArray(Parts, TEXT(","));
		for (FString& Part : Parts)
		{
			Part.TrimStartAndEndInline();
			Present.Add(Part.ToLower());
		}
		for (const TSharedPtr<FString>& Candidate : AllTags)
		{
			if (Candidate->Contains(Token, ESearchCase::IgnoreCase)
				&& !Present.Contains(Candidate->ToLower()))
			{
				TagSuggestions.Add(Candidate);
				if (TagSuggestions.Num() >= 8)
				{
					break;
				}
			}
		}
	}

	if (TagSuggestListView.IsValid())
	{
		TagSuggestListView->ClearSelection();
		TagSuggestListView->RequestListRefresh();
	}
	if (TagSuggestAnchor.IsValid())
	{
		// bFocusMenu=false: keep the caret in the text box while the list shows.
		TagSuggestAnchor->SetIsOpen(TagSuggestions.Num() > 0, /*bFocusMenu*/ false);
	}
}

void SKuraBrowser::AcceptTagSuggestion(const FString& Suggestion)
{
	if (!TagsEditBox.IsValid())
	{
		return;
	}
	const FString Full = TagsEditBox->GetText().ToString();
	int32 CommaIdx = INDEX_NONE;
	const FString Prefix = Full.FindLastChar(TEXT(','), CommaIdx)
		? Full.Left(CommaIdx + 1) + TEXT(" ") : FString();
	TagsEditBox->SetText(FText::FromString(Prefix + Suggestion));
	if (TagSuggestAnchor.IsValid())
	{
		TagSuggestAnchor->SetIsOpen(false, false);
	}
	// Back to typing: the user still confirms the whole set with Enter.
	FSlateApplication::Get().SetKeyboardFocus(TagsEditBox);
}

void SKuraBrowser::CommitTags(const FString& Package, const FString& TagsCsv)
{
	const TSharedPtr<FJsonObject> Result = KuraPython::Call(FString::Printf(
		TEXT("set_tags('%s', '%s')"),
		*KuraPython::EscapePyStr(Package), *KuraPython::EscapePyStr(TagsCsv)));
	bool bOk = false;
	if (Result.IsValid())
	{
		Result->TryGetBoolField(TEXT("ok"), bOk);
	}
	KuraActions::Notify(bOk, bOk
		? LOCTEXT("TagsSaved", "Kura: tags saved")
		: LOCTEXT("TagsFailed", "Kura: saving tags failed"));
	if (!bOk)
	{
		return;
	}
	// One field on one entry is not worth re-reading the library. The disk
	// refresh this used to call stats every meta file and every folder under
	// the library root - a fifth of a second on a warm cache, seconds on a
	// cold one - and tagging is something the user does in a burst, one Enter
	// after another (2026-08-23). Python hands back the canonical tag list, so
	// the entry in memory is brought up to date from that instead.
	//
	// MetaStamp is deliberately left as it was: the next real refresh sees the
	// file's timestamp move, re-reads that one meta and confirms the edit, so
	// a divergence here cannot outlive the next refresh.
	if (const TSharedPtr<FKuraEntry>* Found = EntryByPackage.Find(Package))
	{
		const TSharedPtr<FKuraEntry>& E = *Found;
		E->Tags.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Result->TryGetArrayField(TEXT("tags"), Arr))
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				E->Tags.Add(V->AsString());
			}
		}
		E->TagsLower.Reset();
		E->TagsLower.Reserve(E->Tags.Num());
		for (const FString& T : E->Tags)
		{
			E->TagsLower.Add(T.ToLower());
		}
		RebuildTagList();
	}
	// Not SelectEntryByPackage: that NAVIGATES to the entry - clears the
	// collection scope, switches the class tab, turns Show Dependencies on for
	// a non-root - which made sense only while this refreshed from disk and
	// had to re-resolve a dangling Selected. Editing a tag must not move the
	// user somewhere else. A filter pass is still needed: an active tag filter
	// or a search over tags decides whether this entry still belongs on screen.
	ApplyFilter();
}

void SKuraBrowser::CommitNotes(const FString& Package, const FString& Notes)
{
	const TSharedPtr<FJsonObject> Result = KuraPython::Call(FString::Printf(
		TEXT("set_notes('%s', '%s')"),
		*KuraPython::EscapePyStr(Package), *KuraPython::EscapePyStr(Notes)));
	bool bOk = false;
	if (Result.IsValid())
	{
		Result->TryGetBoolField(TEXT("ok"), bOk);
	}
	if (!bOk)
	{
		KuraActions::Notify(false, LOCTEXT("NotesFailed", "Kura: saving notes failed"));
		return;
	}
	// Same reasoning as CommitTags: notes are typed, and re-walking the
	// library for one field is what made that feel heavy. Search covers notes,
	// so the lowercase copy has to move with it.
	if (const TSharedPtr<FKuraEntry>* Found = EntryByPackage.Find(Package))
	{
		(*Found)->Notes = Notes;
		(*Found)->NotesLower = Notes.ToLower();
	}
	ApplyFilter(); // search covers notes; see CommitTags for why not Select...
}

FString SKuraBrowser::FormatSize(int64 Bytes)
{
	if (Bytes >= 1024 * 1024)
	{
		return FString::Printf(TEXT("%.2f MB"), Bytes / (1024.0 * 1024.0));
	}
	return FString::Printf(TEXT("%.1f KB"), Bytes / 1024.0);
}

FLinearColor SKuraBrowser::ClassColor(const FString& ClassName)
{
	// Ask the engine for the very color the Content Browser paints on its own
	// tiles (UAssetDefinition::GetAssetColor, reached through IAssetTools)
	// instead of keeping a parallel palette that drifts from it (2026-07-25).
	// Types we never enumerated - anything in the Others tab - get their real
	// color too, and an engine re-skin carries over for free.
	static TMap<FString, FLinearColor> Cache;
	if (const FLinearColor* Hit = Cache.Find(ClassName))
	{
		return *Hit;
	}
	if (const UClass* Class = UClass::TryFindTypeSlow<UClass>(ClassName))
	{
		if (const TOptional<FLinearColor> TypeColor = IAssetTools::Get().GetTypeColor(Class))
		{
			Cache.Add(ClassName, *TypeColor);
			return *TypeColor;
		}
	}
	// Unresolved: the class may belong to a plugin that loads later, so this
	// one stays out of the cache and gets another chance next time.
	return FLinearColor(0.25f, 0.25f, 0.28f);
}

FString SKuraBrowser::ClassAbbrev(const FString& ClassName)
{
	if (ClassName == TEXT("StaticMesh"))
	{
		return TEXT("SM");
	}
	if (ClassName == TEXT("Material"))
	{
		return TEXT("M");
	}
	if (ClassName == TEXT("MaterialInstanceConstant"))
	{
		return TEXT("MI");
	}
	if (ClassName == TEXT("MaterialFunction"))
	{
		return TEXT("MF");
	}
	if (ClassName.StartsWith(TEXT("Texture")))
	{
		return TEXT("T");
	}
	// No thumbnail renderer exists for PCG assets, so this tile is all they
	// ever show (SPEC F9).
	if (ClassName == TEXT("PCGGraph"))
	{
		return TEXT("PCG");
	}
	if (ClassName == TEXT("PCGGraphInstance"))
	{
		return TEXT("PCGI");
	}
	return TEXT("?");
}

#undef LOCTEXT_NAMESPACE
