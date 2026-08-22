#pragma once

#include "CoreMinimal.h"

class SWidget;

/** PBR Material Importer (SPEC F6): one disk subfolder = one texture set ->
 *  import textures with correct settings + build an MI under the user's
 *  master. UI shell only - matching/import logic lives in the standalone
 *  Python module pbr_importer.py (reached via kura.editor_api.pbr_*). */
namespace KuraPbrImporter
{
	void RunFlow(TSharedPtr<const SWidget> ModalParent);
}
