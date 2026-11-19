#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "KuraThumbnailLibrary.generated.h"

/** Thumbnail pipeline entry points (SPEC 7.2), called from kura/ingest.py
 *  via the Python-exposed UFUNCTION bridge.
 *
 *  Project assets are never modified: the material preview mesh is applied
 *  transiently for the library PNG render and restored afterwards, so the
 *  project's own Content Browser thumbnails keep the default sphere
 *  (decision 2026-07-12). */
UCLASS()
class UKuraThumbnailLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Render the asset's thumbnail and write it as a PNG (Meta mirror).
	 *  Waits for pending asset/shader compilation first - rendering earlier
	 *  produces default-grid/blank thumbnails. Creates the target folder.
	 *  Resolution comes from config thumb_resolution (SPEC F5). */
	UFUNCTION(BlueprintCallable, Category = "Kura")
	static bool ExportThumbnailPng(const FString& PackageName, const FString& OutPngPath,
		int32 Resolution = 256);

	/** Material-family variant: temporarily swap in a preview-mesh thumbnail
	 *  info (USceneThumbnailInfoWithPrimitive with the given mesh and orbit
	 *  angles), render the PNG, then restore the original info and dirty
	 *  state. Falls back to a plain render (default sphere) when the mesh
	 *  fails to load (SPEC 7.2). */
	UFUNCTION(BlueprintCallable, Category = "Kura")
	static bool ExportMaterialThumbnailPng(const FString& PackageName, const FString& OutPngPath,
		const FString& PreviewMeshPath, float OrbitPitch, float OrbitYaw, float OrbitZoom,
		int32 Resolution = 256);

	/** Fix Up Redirectors on the given redirectors (SPEC F1 step 2): re-save
	 *  their referencers to point at the real asset, then delete the
	 *  redirectors. IAssetTools::FixupReferencers is not a UFUNCTION, so
	 *  Python cannot reach it on its own - ingest called a fixup_referencers
	 *  that does not exist on 5.7 / 5.8 and every upload with a redirector
	 *  under /Game/_AssetLib was blocked (2026-11-19). Lives here because
	 *  this is the plugin's one Python-exposed library; it has nothing to do
	 *  with thumbnails. Waits for the asset registry first: while it is still
	 *  discovering, the engine defers the fixup to a dialog instead.
	 *  Takes package names, not objects: a Python-held reference would keep
	 *  the redirector alive in memory and stop the engine deleting it
	 *  (UObjectRedirector is also an intrinsic class UHT cannot see).
	 *  Packages that hold no redirector are ignored.
	 *
	 *  The engine always ends with its modal Redirector Update Report (the
	 *  same window the Content Browser command shows), so this needs an
	 *  interactive editor: headless, or under -ExecutePythonScript, the
	 *  engine asserts. Returns false without doing anything there; the caller
	 *  then finds the redirectors still in place and says so. */
	UFUNCTION(BlueprintCallable, Category = "Kura")
	static bool FixupRedirectors(const TArray<FString>& PackageNames);
};
