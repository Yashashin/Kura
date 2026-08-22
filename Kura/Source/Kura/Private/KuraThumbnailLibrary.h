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
};
