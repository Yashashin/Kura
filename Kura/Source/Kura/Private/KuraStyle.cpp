#include "KuraStyle.h"

#include "Brushes/SlateImageBrush.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"

namespace
{
	/** Guideline hexes are sRGB; Slate wants linear. */
	FLinearColor FromHex(const TCHAR* Hex)
	{
		return FLinearColor::FromSRGBColor(FColor::FromHex(Hex));
	}

	TSharedPtr<FSlateStyleSet> StyleSet;
}

namespace KuraStyle
{
	const FStatusColors& Error()
	{
		static const FStatusColors Colors = {
			FromHex(TEXT("#D64545")), FromHex(TEXT("#E25C5C")), FromHex(TEXT("#F07878")),
			FromHex(TEXT("#3A1B1B")), FromHex(TEXT("#6E2C2C")) };
		return Colors;
	}

	const FStatusColors& Warning()
	{
		static const FStatusColors Colors = {
			FromHex(TEXT("#D69A3A")), FromHex(TEXT("#E4AC52")), FromHex(TEXT("#EDBE6E")),
			FromHex(TEXT("#382A12")), FromHex(TEXT("#6E5320")) };
		return Colors;
	}

	const FStatusColors& Success()
	{
		static const FStatusColors Colors = {
			FromHex(TEXT("#4CAF6E")), FromHex(TEXT("#63BF82")), FromHex(TEXT("#7ECD97")),
			FromHex(TEXT("#16301F")), FromHex(TEXT("#2C5C3C")) };
		return Colors;
	}

	const FStatusColors& Info()
	{
		static const FStatusColors Colors = {
			FromHex(TEXT("#3E8FD6")), FromHex(TEXT("#56A0E2")), FromHex(TEXT("#74B3EC")),
			FromHex(TEXT("#13273A")), FromHex(TEXT("#255074")) };
		return Colors;
	}

	FLinearColor Accent()
	{
		return FromHex(TEXT("#0070E0"));
	}

	FLinearColor AccentText()
	{
		return FromHex(TEXT("#3B9BE0"));
	}

	FLinearColor TextSecondary()
	{
		return FromHex(TEXT("#8F8F8F"));
	}

	void Register()
	{
		if (StyleSet.IsValid())
		{
			return;
		}
		StyleSet = MakeShared<FSlateStyleSet>(GetStyleSetName());
		if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Kura")))
		{
			StyleSet->SetContentRoot(Plugin->GetBaseDir() / TEXT("Resources"));
			// Tab/menu glyph: the 16px-optimized cut of the logo, n-7 fill.
			StyleSet->Set("Kura.Logo", new FSlateVectorImageBrush(
				StyleSet->RootToContentDir(TEXT("KuraLogo"), TEXT(".svg")),
				FVector2f(16.f, 16.f)));
			// Empty states: n-5 tiles, accent center tile (guideline section 2).
			StyleSet->Set("Kura.LogoWatermark", new FSlateVectorImageBrush(
				StyleSet->RootToContentDir(TEXT("KuraLogoWatermark"), TEXT(".svg")),
				FVector2f(56.f, 56.f)));
		}
		FSlateStyleRegistry::RegisterSlateStyle(*StyleSet);
	}

	void Unregister()
	{
		if (StyleSet.IsValid())
		{
			FSlateStyleRegistry::UnRegisterSlateStyle(*StyleSet);
			StyleSet.Reset();
		}
	}

	const ISlateStyle& Get()
	{
		check(StyleSet.IsValid());
		return *StyleSet;
	}

	FName GetStyleSetName()
	{
		static const FName Name(TEXT("KuraStyle"));
		return Name;
	}
}
