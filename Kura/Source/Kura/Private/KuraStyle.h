#pragma once

#include "CoreMinimal.h"

class ISlateStyle;

/** Color tokens and logo brushes from the Kura design guidelines (v1.0,
 *  2026-07). The neutral ramp tracks UE Slate, so widgets keep using
 *  FAppStyle brushes and default text colors; these tokens cover only what
 *  Kura colors explicitly - status text on dark panels, the accent, and the
 *  logo. Guideline rule: accent means selection / primary action, status
 *  colors mean status, never decoration. */
namespace KuraStyle
{
	/** One status family. The guideline defines five layers per status:
	 *  base (solid controls), hover, text (on dark panels), bg + border
	 *  (banners, badges). */
	struct FStatusColors
	{
		FLinearColor Base;
		FLinearColor Hover;
		FLinearColor Text;
		FLinearColor Bg;
		FLinearColor Border;
	};

	const FStatusColors& Error();
	const FStatusColors& Warning();
	const FStatusColors& Success();
	const FStatusColors& Info();

	FLinearColor Accent();
	FLinearColor AccentText();

	/** n-6: secondary text / neutral badges on dark panels. */
	FLinearColor TextSecondary();

	/** Style set with the logo brushes ("Kura.Logo" 16px for tabs and menus,
	 *  "Kura.LogoWatermark" 56px for empty states). */
	void Register();
	void Unregister();
	const ISlateStyle& Get();
	FName GetStyleSetName();
}
