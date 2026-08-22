#include "CoreMinimal.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "KuraActions.h"
#include "KuraStyle.h"
#include "Modules/ModuleManager.h"
#include "SKuraBrowser.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "Kura"

static const FName KuraTabName("KuraLibrary");

class FKuraModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Headless runs (commandlets) have no Slate application; nothing to register.
		if (IsRunningCommandlet() || !FSlateApplication::IsInitialized())
		{
			return;
		}

		KuraStyle::Register();

		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
			KuraTabName, FOnSpawnTab::CreateStatic(&FKuraModule::SpawnKuraTab))
			.SetDisplayName(LOCTEXT("TabTitle", "Kura Library"))
			.SetTooltipText(LOCTEXT("TabTooltip", "Browse and import assets from the Kura library"))
			.SetIcon(FSlateIcon(KuraStyle::GetStyleSetName(), "Kura.Logo"))
			.SetMenuType(ETabSpawnerMenuType::Hidden);

		UToolMenus::RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FKuraModule::RegisterMenus));
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(KuraTabName);
			KuraStyle::Unregister();
		}
	}

private:
	static TSharedRef<SDockTab> SpawnKuraTab(const FSpawnTabArgs& Args)
	{
		const TSharedRef<SDockTab> Tab = SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				SNew(SKuraBrowser)
			];
		Tab->SetTabIcon(KuraStyle::Get().GetBrush("Kura.Logo"));
		return Tab;
	}

	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
		FToolMenuSection& Section = Menu->FindOrAddSection("Kura");
		Section.Label = LOCTEXT("SectionLabel", "Kura");
		Section.AddMenuEntry(
			"OpenKuraLibrary",
			LOCTEXT("OpenKura", "Kura Library"),
			LOCTEXT("OpenKuraTip", "Open the Kura asset library browser"),
			FSlateIcon(KuraStyle::GetStyleSetName(), "Kura.Logo"),
			FUIAction(FExecuteAction::CreateLambda([]()
			{
				FGlobalTabmanager::Get()->TryInvokeTab(KuraTabName);
			})));

		// Ingest entry point (SPEC F1, 2026-07-12): always shown - misplaced
		// or non-whitelisted selections are blocked by the validation dialog,
		// which doubles as the placement-rules teaching moment.
		UToolMenu* AssetMenu = UToolMenus::Get()->ExtendMenu("ContentBrowser.AssetContextMenu");
		// Sit AFTER the engine's "GetAssetActions" section, which holds the
		// per-type entries (Open StaticMesh.h, View Documentation) and carries
		// NO label - placed before it, those entries render under the "Kura"
		// heading and read as ours. Everything after us is labelled, so the
		// section ends where it should (2026-07-25).
		FToolMenuSection& AssetSection = AssetMenu->FindOrAddSection("Kura",
			LOCTEXT("CbSectionLabel", "Kura"),
			FToolMenuInsert("GetAssetActions", EToolMenuInsertType::After));
		AssetSection.AddMenuEntry(
			"SendToKura",
			LOCTEXT("SendToKura", "Send to Kura"),
			LOCTEXT("SendToKuraTip",
				"Validate the selection (plus all dependencies) and preview the upload. Assets must already sit somewhere under /Game/_AssetLib - Kura verifies, it never moves."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Export"),
			FUIAction(FExecuteAction::CreateLambda([]()
			{
				KuraActions::RunIngestFlow(nullptr);
			})));
	}
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FKuraModule, Kura)
