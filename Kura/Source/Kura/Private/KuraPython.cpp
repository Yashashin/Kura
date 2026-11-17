#include "KuraPython.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "IPythonScriptPlugin.h"
#include "Misc/Base64.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogKuraPy, Log, All);

FString KuraPython::EscapePyStr(const FString& In)
{
	FString S = In;
	S.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	S.ReplaceInline(TEXT("'"), TEXT("\\'"));
	S.ReplaceInline(TEXT("\r"), TEXT("\\r"));
	S.ReplaceInline(TEXT("\n"), TEXT("\\n"));
	return S;
}

FString KuraPython::ToJsonArray(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FString& V : Values)
	{
		Items.Add(MakeShared<FJsonValueString>(V));
	}
	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Items, Writer);
	return Out;
}

FString KuraPython::ToJsonObject(const TMap<FString, FString>& Values)
{
	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : Values)
	{
		Obj->SetStringField(Pair.Key, Pair.Value);
	}
	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Obj, Writer);
	return Out;
}

TSharedPtr<FJsonObject> KuraPython::Call(const FString& Statement)
{
	IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
	if (!Python || !Python->IsPythonAvailable())
	{
		UE_LOG(LogKuraPy, Error, TEXT("Python is not available"));
		return nullptr;
	}

	static bool bImported = false;
	if (!bImported)
	{
		bImported = Python->ExecPythonCommand(TEXT("import kura.editor_api"));
		if (!bImported)
		{
			UE_LOG(LogKuraPy, Error, TEXT("Failed to import kura.editor_api"));
			return nullptr;
		}
	}

	FPythonCommandEx Cmd;
	Cmd.Command = FString::Printf(TEXT("kura.editor_api.%s"), *Statement);
	Cmd.ExecutionMode = EPythonCommandExecutionMode::EvaluateStatement;
	if (!Python->ExecPythonCommandEx(Cmd))
	{
		UE_LOG(LogKuraPy, Error, TEXT("Python call failed: %s"), *Cmd.Command);
		return nullptr;
	}

	// editor_api returns base64-encoded JSON: EvaluateStatement hands back the
	// value's repr(), which would mangle a raw JSON string with \' escapes,
	// but leaves a base64 payload untouched apart from the outer quotes.
	FString Payload = Cmd.CommandResult.TrimStartAndEnd();
	// Only a MATCHED pair is the repr's quoting. Anything else is not the
	// string editor_api returns, and trimming it blind turned that into a
	// base64 failure further down instead of a message naming the cause.
	if (Payload.Len() >= 2 && (Payload[0] == TEXT('\'') || Payload[0] == TEXT('"')))
	{
		if (Payload[Payload.Len() - 1] != Payload[0])
		{
			UE_LOG(LogKuraPy, Error, TEXT("Unexpected result from %s (unbalanced quotes): %s"),
				*Cmd.Command, *Cmd.CommandResult);
			return nullptr;
		}
		Payload = Payload.Mid(1, Payload.Len() - 2);
	}
	FString Json;
	TArray<uint8> Bytes;
	if (FBase64::Decode(Payload, Bytes))
	{
		const FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		Json = FString::ConstructFromPtrSize(Converter.Get(), Converter.Length());
	}
	else
	{
		Json = Payload; // lenient fallback: maybe already raw JSON
	}

	TSharedPtr<FJsonObject> Obj;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
	{
		UE_LOG(LogKuraPy, Error, TEXT("Unparseable result from %s: %s"),
			*Cmd.Command, *Cmd.CommandResult);
		return nullptr;
	}
	return Obj;
}
