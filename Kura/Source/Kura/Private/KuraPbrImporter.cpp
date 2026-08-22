#include "KuraPbrImporter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Framework/Application/SlateApplication.h"
#include "KuraActions.h"
#include "KuraLibraryModel.h"
#include "KuraPython.h"
#include "KuraStyle.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Kura"

namespace
{
	/** Shared dialog state: the Import button acts on the last scan. */
	struct FPbrState
	{
		bool bFlipGreen = false;
		int32 NumReady = 0;   // sets without errors in the last scan
		int32 NumBlocked = 0;
		bool bScanned = false;
	};

	TArray<FString> StringArray(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Obj->TryGetArrayField(Field, Arr))
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				Out.Add(V->AsString());
			}
		}
		return Out;
	}

	/** Fill the preview box from a pbr_scan() result and update the state. */
	void FillPreview(const TSharedRef<SVerticalBox>& PreviewBox,
		const TSharedRef<FPbrState>& State, const TSharedPtr<FJsonObject>& Result)
	{
		PreviewBox->ClearChildren();
		State->NumReady = 0;
		State->NumBlocked = 0;
		State->bScanned = true;

		const TArray<TSharedPtr<FJsonValue>>* Sets = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("sets"), Sets))
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& V : *Sets)
		{
			const TSharedPtr<FJsonObject> Set = V->AsObject();
			if (!Set.IsValid())
			{
				continue;
			}
			FString Name, MiPath;
			Set->TryGetStringField(TEXT("name"), Name);
			Set->TryGetStringField(TEXT("mi"), MiPath);
			const TArray<FString> Errors = StringArray(Set, TEXT("errors"));
			const TArray<FString> Unmatched = StringArray(Set, TEXT("unmatched"));
			const bool bReady = Errors.Num() == 0;
			(bReady ? State->NumReady : State->NumBlocked)++;

			const TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
			Rows->AddSlot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)
				[
					SNew(STextBlock)
					.Text(bReady ? LOCTEXT("PbrReady", "READY") : LOCTEXT("PbrBlocked", "BLOCKED"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
					// Blocked sets are skipped entirely, so that badge is an
					// error, not a warning (guideline: color carries meaning).
					.ColorAndOpacity(bReady
						? KuraStyle::Success().Text
						: KuraStyle::Error().Text)
				]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Name))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				]
			];
			Rows->AddSlot().AutoHeight().Padding(52.f, 0.f, 0.f, 2.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(MiPath))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			];

			const TArray<TSharedPtr<FJsonValue>>* Textures = nullptr;
			if (Set->TryGetArrayField(TEXT("textures"), Textures))
			{
				for (const TSharedPtr<FJsonValue>& T : *Textures)
				{
					const TSharedPtr<FJsonObject> Tex = T->AsObject();
					if (!Tex.IsValid())
					{
						continue;
					}
					FString File, Param, Dest;
					Tex->TryGetStringField(TEXT("file"), File);
					Tex->TryGetStringField(TEXT("param"), Param);
					Tex->TryGetStringField(TEXT("dest"), Dest);
					Rows->AddSlot().AutoHeight().Padding(52.f, 0.f, 0.f, 0.f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(FString::Printf(TEXT("%s   ->   %s   (%s)"),
							*File, *Param, *Dest)))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					];
				}
			}
			if (Unmatched.Num() > 0)
			{
				Rows->AddSlot().AutoHeight().Padding(52.f, 2.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("not imported: ") + FString::Join(Unmatched, TEXT(", "))))
					.Font(FCoreStyle::GetDefaultFontStyle("Italic", 8))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				];
			}
			for (const FString& Err : Errors)
			{
				Rows->AddSlot().AutoHeight().Padding(52.f, 2.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Err))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					.ColorAndOpacity(KuraStyle::Error().Text)
					.AutoWrapText(true)
				];
			}

			PreviewBox->AddSlot().AutoHeight().Padding(0.f, 2.f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
				.Padding(8.f, 6.f)
				[
					Rows
				]
			];
		}
	}
}

void KuraPbrImporter::RunFlow(TSharedPtr<const SWidget> ModalParent)
{
	// Seed the fields from the tool's remembered defaults (pbr_importer.json).
	FString DefaultMaster, DefaultCategory;
	bool bDefaultFlip = false;
	if (const TSharedPtr<FJsonObject> Defaults = KuraPython::Call(TEXT("pbr_defaults()")))
	{
		Defaults->TryGetStringField(TEXT("master_material"), DefaultMaster);
		Defaults->TryGetStringField(TEXT("category"), DefaultCategory);
		Defaults->TryGetBoolField(TEXT("flip_green"), bDefaultFlip);
	}

	const TSharedRef<FPbrState> State = MakeShared<FPbrState>();
	State->bFlipGreen = bDefaultFlip;

	const TSharedRef<SEditableTextBox> FolderBox = SNew(SEditableTextBox)
		.HintText(LOCTEXT("PbrFolderHint", "folder whose subfolders are one texture set each"));
	const TSharedRef<SEditableTextBox> CategoryBox = SNew(SEditableTextBox)
		.Text(FText::FromString(DefaultCategory))
		.HintText(LOCTEXT("PbrCategoryHint", "e.g. Materials - each set gets its own folder under it"));
	const TSharedRef<SEditableTextBox> MasterBox = SNew(SEditableTextBox)
		.Text(FText::FromString(DefaultMaster))
		.HintText(LOCTEXT("PbrMasterHint", "/Game/_AssetLib/Shared/Master/M_YourMaster"));

	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("PbrTitle", "Kura - PBR Material Importer"))
		.ClientSize(FVector2D(760.f, 560.f))
		.SupportsMaximize(false)
		.SupportsMinimize(false);
	const TWeakPtr<SWindow> WeakWindow = Window;

	const TSharedRef<SVerticalBox> PreviewBox = SNew(SVerticalBox);

	const auto LabeledRow = [](const FText& Label, TSharedRef<SWidget> Field) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(120.f)
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
				LabeledRow(LOCTEXT("PbrFolder", "Source Folder"),
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center) [ FolderBox ]
					+ SHorizontalBox::Slot().AutoWidth().Padding(4.f, 0.f, 0.f, 0.f)
					[
						SNew(SButton)
						.ToolTipText(LOCTEXT("PbrBrowseTip", "Choose the downloaded-textures folder"))
						.VAlign(VAlign_Center)
						.OnClicked_Lambda([FolderBox]()
						{
							FString Picked;
							if (KuraLibrary::PickDirectory(
								LOCTEXT("PbrPickTitle", "Choose PBR Texture Folder").ToString(),
								FolderBox->GetText().ToString(), Picked))
							{
								FolderBox->SetText(FText::FromString(Picked));
							}
							return FReply::Handled();
						})
						[
							SNew(SBox)
							.WidthOverride(16.f).HeightOverride(16.f)
							.HAlign(HAlign_Center).VAlign(VAlign_Center)
							[
								SNew(SImage)
								.Image(FAppStyle::GetBrush("Icons.FolderOpen"))
								.DesiredSizeOverride(FVector2D(12.f, 12.f))
								.ColorAndOpacity(FSlateColor::UseForeground())
							]
						]
					])
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
			[
				LabeledRow(LOCTEXT("PbrCategory", "Destination Folder"), CategoryBox)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
			[
				LabeledRow(LOCTEXT("PbrMaster", "Master Material"), MasterBox)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([State]()
					{
						return State->bFlipGreen ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([State](ECheckBoxState NewState)
					{
						State->bFlipGreen = (NewState == ECheckBoxState::Checked);
					})
					.ToolTipText(LOCTEXT("PbrFlipTip",
						"Flip the normal map's green channel on import - use for OpenGL-style normals (Poly Haven \"GL\", ambientCG \"NormalGL\")"))
					[
						SNew(STextBlock).Text(LOCTEXT("PbrFlip", "Flip Normal Green Channel"))
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SNew(SBox) // spacer
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("PbrScan", "Scan"))
					.ToolTipText(LOCTEXT("PbrScanTip",
						"Dry run: show what would be imported and wired up, without touching any assets"))
					.OnClicked_Lambda([FolderBox, CategoryBox, PreviewBox, State, ModalParent]()
					{
						const FString Statement = FString::Printf(TEXT("pbr_scan('%s', '%s')"),
							*KuraPython::EscapePyStr(FolderBox->GetText().ToString().TrimStartAndEnd()),
							*KuraPython::EscapePyStr(CategoryBox->GetText().ToString().TrimStartAndEnd()));
						const TSharedPtr<FJsonObject> Result = KuraPython::Call(Statement);
						bool bOk = false;
						if (Result.IsValid())
						{
							Result->TryGetBoolField(TEXT("ok"), bOk);
						}
						if (!bOk)
						{
							TArray<FString> Errors;
							const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
							if (Result.IsValid() && Result->TryGetArrayField(TEXT("errors"), Arr))
							{
								for (const TSharedPtr<FJsonValue>& V : *Arr)
								{
									Errors.Add(V->AsString());
								}
							}
							if (Errors.Num() == 0)
							{
								Errors.Add(TEXT("Scan failed - see Output Log for details."));
							}
							KuraActions::ShowErrorsDialog(
								LOCTEXT("PbrScanFailed", "PBR Importer - Scan Failed"), Errors, ModalParent);
							return FReply::Handled();
						}
						FillPreview(PreviewBox, State, Result);
						return FReply::Handled();
					})
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(4.f)
				[
					SNew(SScrollBox) + SScrollBox::Slot() [ PreviewBox ]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text_Lambda([State]()
					{
						if (!State->bScanned)
						{
							return LOCTEXT("PbrHintScan",
								"Scan first - nothing is imported until you confirm.");
						}
						return FText::Format(LOCTEXT("PbrSummary",
							"{0} {0}|plural(one=set,other=sets) ready, {1} blocked. Review, then import and Send to Kura yourself."),
							FText::AsNumber(State->NumReady), FText::AsNumber(State->NumBlocked));
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
				[
					SNew(SButton)
					.Text(LOCTEXT("PbrImport", "Import"))
					.IsEnabled_Lambda([State]() { return State->NumReady > 0; })
					.ToolTipText(LOCTEXT("PbrImportTip",
						"Import the READY sets: textures with correct settings, one MI per set under the master. BLOCKED sets are skipped."))
					.OnClicked_Lambda([FolderBox, CategoryBox, MasterBox, PreviewBox, State, WeakWindow, ModalParent]()
					{
						const FString Statement = FString::Printf(TEXT("pbr_commit('%s', '%s', '%s', %s)"),
							*KuraPython::EscapePyStr(FolderBox->GetText().ToString().TrimStartAndEnd()),
							*KuraPython::EscapePyStr(CategoryBox->GetText().ToString().TrimStartAndEnd()),
							*KuraPython::EscapePyStr(MasterBox->GetText().ToString().TrimStartAndEnd()),
							State->bFlipGreen ? TEXT("True") : TEXT("False"));
						const TSharedPtr<FJsonObject> Result = KuraPython::Call(Statement);
						if (!Result.IsValid())
						{
							KuraActions::Notify(false,
								LOCTEXT("PbrPyFailed", "PBR Importer: Python call failed - see Output Log"));
							return FReply::Handled();
						}
						bool bOk = false;
						Result->TryGetBoolField(TEXT("ok"), bOk);
						const TArray<TSharedPtr<FJsonValue>>* Created = nullptr;
						const int32 NumCreated =
							Result->TryGetArrayField(TEXT("created"), Created) ? Created->Num() : 0;
						if (!bOk)
						{
							TArray<FString> Lines;
							if (NumCreated > 0)
							{
								Lines.Add(FString::Printf(TEXT("Created %d material instance(s) before the errors below:"), NumCreated));
							}
							const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
							if (Result->TryGetArrayField(TEXT("errors"), Arr))
							{
								for (const TSharedPtr<FJsonValue>& V : *Arr)
								{
									Lines.Add(V->AsString());
								}
							}
							KuraActions::ShowErrorsDialog(
								LOCTEXT("PbrCommitFailed", "PBR Importer - Finished With Errors"), Lines, ModalParent);
							return FReply::Handled();
						}
						KuraActions::Notify(true, FText::Format(LOCTEXT("PbrDone",
							"PBR Importer: created {0} material {0}|plural(one=instance,other=instances)"),
							FText::AsNumber(NumCreated)));
						if (const TSharedPtr<SWindow> W = WeakWindow.Pin())
						{
							W->RequestDestroyWindow();
						}
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("PbrClose", "Close"))
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

#undef LOCTEXT_NAMESPACE
