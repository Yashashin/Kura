#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/** Bridge to kura.editor_api (SPEC 7.3): evaluate a Python statement that
 *  returns a JSON string and parse it. Business logic stays in Python. */
namespace KuraPython
{
	/** Escape a string for embedding inside a single-quoted Python literal. */
	FString EscapePyStr(const FString& In);

	/** Serialize a string array to a JSON array literal (for py args). */
	FString ToJsonArray(const TArray<FString>& Values);

	/** Serialize a string->string map to a JSON object literal (for py args). */
	FString ToJsonObject(const TMap<FString, FString>& Values);

	/** Evaluate `kura.editor_api.<Statement>` and parse the returned JSON.
	 *  Returns nullptr (with a logged error) on failure. */
	TSharedPtr<FJsonObject> Call(const FString& Statement);
}
