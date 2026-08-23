#include "KuraLibraryModel.h"

#include "DesktopPlatformModule.h"
#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "IDesktopPlatform.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogKura, Log, All);

namespace
{
	const TCHAR* GDefaultLibPrefix = TEXT("/Game/_AssetLib");

	TSharedPtr<FJsonObject> LoadConfig()
	{
		const FString AppData = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));
		if (AppData.IsEmpty())
		{
			return nullptr;
		}
		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *(AppData / TEXT("Kura") / TEXT("config.json"))))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Obj);
		return Obj;
	}

	void ReadStringArray(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, TArray<FString>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Obj->TryGetArrayField(Field, Arr))
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				Out.Add(V->AsString());
			}
		}
	}

	/** A meta json with the stat the walk already had to read anyway. */
	struct FMetaFile
	{
		FString Path;
		FDateTime Stamp;
		int64 Size = 0;
	};

	class FMetaWalker : public IPlatformFile::FDirectoryStatVisitor
	{
	public:
		TArray<FMetaFile> Files;

		virtual bool Visit(const TCHAR* Path, const FFileStatData& Stat) override
		{
			if (!Stat.bIsDirectory)
			{
				FString File(Path);
				if (File.EndsWith(TEXT(".json"), ESearchCase::IgnoreCase))
				{
					Files.Add({ MoveTemp(File), Stat.ModificationTime, Stat.FileSize });
				}
			}
			return true;
		}
	};

	/** ClassTabs' "|"-separated class lists, split once. Doing it per call made
	 *  the tab test five heap allocations per entry per filter pass, which at
	 *  library scale is 50k allocations for one keystroke (2026-08-20). */
	const TArray<TArray<FString>>& TabClassNames()
	{
		static const TArray<TArray<FString>> Cached = []()
		{
			TArray<TArray<FString>> Result;
			for (int32 Idx = 0; Idx < KuraLibrary::NumClassTabs; ++Idx)
			{
				TArray<FString> Names;
				FString(KuraLibrary::ClassTabs[Idx].Classes).ParseIntoArray(Names, TEXT("|"));
				Result.Add(MoveTemp(Names));
			}
			return Result;
		}();
		return Cached;
	}

	/** Name order at every level, matching what the directory walk this
	 *  replaced produced. */
	void SortFolderLevel(TArray<TSharedPtr<FKuraFolderNode>>& Nodes)
	{
		Nodes.Sort([](const TSharedPtr<FKuraFolderNode>& A, const TSharedPtr<FKuraFolderNode>& B)
		{
			return A->Name < B->Name;
		});
		for (const TSharedPtr<FKuraFolderNode>& Node : Nodes)
		{
			SortFolderLevel(Node->Children);
		}
	}
}

FString KuraLibrary::GetDefaultLibraryRoot()
{
	// Fresh-install default: <user>\Documents\Kura Library. Must match
	// kura/config.py default_library_root().
	FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
	if (Home.IsEmpty())
	{
		Home = FPlatformProcess::UserHomeDir();
	}
	return FPaths::Combine(Home, TEXT("Documents"), TEXT("Kura Library"));
}

bool KuraLibrary::IsConfigured()
{
	// Absence of config.json = the user has never saved settings = first run
	// (SPEC F5, 2026-07-14). Keep in sync with kura/config.config_file_exists.
	const FString Path = GetConfigFilePath();
	return !Path.IsEmpty() && FPaths::FileExists(Path);
}

FString KuraLibrary::GetLibraryRoot()
{
	FString Root = GetDefaultLibraryRoot();
	if (const TSharedPtr<FJsonObject> Cfg = LoadConfig())
	{
		Cfg->TryGetStringField(TEXT("library_root"), Root);
	}
	const FString EnvRoot = FPlatformMisc::GetEnvironmentVariable(TEXT("KURA_LIBRARY_ROOT"));
	if (!EnvRoot.IsEmpty())
	{
		Root = EnvRoot;
	}
	return Root;
}

FString KuraLibrary::GetLibPrefix()
{
	FString Prefix = GDefaultLibPrefix;
	if (const TSharedPtr<FJsonObject> Cfg = LoadConfig())
	{
		Cfg->TryGetStringField(TEXT("lib_prefix"), Prefix);
	}
	return Prefix;
}

FString KuraLibrary::GetMaterialPreviewMesh()
{
	// Empty = UE's default material thumbnail (the sphere); matches config.py.
	FString Mesh;
	if (const TSharedPtr<FJsonObject> Cfg = LoadConfig())
	{
		Cfg->TryGetStringField(TEXT("material_preview_mesh"), Mesh);
	}
	return Mesh;
}

double KuraLibrary::GetConfigNumber(const TCHAR* Key, double Default)
{
	double Value = Default;
	if (const TSharedPtr<FJsonObject> Cfg = LoadConfig())
	{
		Cfg->TryGetNumberField(Key, Value);
	}
	return Value;
}

FString KuraLibrary::GetConfigString(const TCHAR* Key)
{
	FString Value;
	if (const TSharedPtr<FJsonObject> Cfg = LoadConfig())
	{
		Cfg->TryGetStringField(Key, Value);
	}
	return Value;
}

bool KuraLibrary::SaveConfig(const TMap<FString, FString>& StringFields,
	const TMap<FString, double>& NumberFields)
{
	const FString AppData = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));
	if (AppData.IsEmpty())
	{
		return false;
	}
	TSharedPtr<FJsonObject> Cfg = LoadConfig(); // keep unknown keys intact
	if (!Cfg.IsValid())
	{
		Cfg = MakeShared<FJsonObject>();
	}
	for (const TPair<FString, FString>& Pair : StringFields)
	{
		Cfg->SetStringField(Pair.Key, Pair.Value);
	}
	for (const TPair<FString, double>& Pair : NumberFields)
	{
		Cfg->SetNumberField(Pair.Key, Pair.Value);
	}
	FString Out;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	if (!FJsonSerializer::Serialize(Cfg.ToSharedRef(), Writer))
	{
		return false;
	}
	const FString Path = AppData / TEXT("Kura") / TEXT("config.json");
	return FFileHelper::SaveStringToFile(Out, *Path,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

FString KuraLibrary::GetConfigFilePath()
{
	const FString AppData = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));
	return AppData.IsEmpty() ? FString() : AppData / TEXT("Kura") / TEXT("config.json");
}

bool KuraLibrary::PickDirectory(const FString& Title, const FString& InitialDir, FString& OutDir)
{
	IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
	if (!Desktop)
	{
		return false;
	}
	const void* ParentHandle =
		FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
	FString Seed = InitialDir;
	if (Seed.IsEmpty() || !FPaths::DirectoryExists(Seed))
	{
		// Fall back to the parent of the intended root so the dialog opens
		// somewhere sensible even before the folder exists.
		Seed = FPaths::GetPath(InitialDir);
	}
	return Desktop->OpenDirectoryDialog(ParentHandle, Title, Seed, OutDir);
}

FString KuraLibrary::GetTreeDir()
{
	// "/Game/_AssetLib" -> "<root>/Content/_AssetLib" (SPEC 5.1)
	FString Rel = GetLibPrefix();
	Rel.RemoveFromStart(TEXT("/Game/"));
	return GetLibraryRoot() / TEXT("Content") / Rel;
}

TArray<TSharedPtr<FKuraEntry>> KuraLibrary::LoadEntries(
	const TArray<TSharedPtr<FKuraEntry>>* Previous)
{
	TArray<TSharedPtr<FKuraEntry>> Out;
	const FString MetaDir = GetLibraryRoot() / TEXT("Meta");
	const FString Prefix = GetLibPrefix() + TEXT("/");

	// One stat-carrying walk instead of a name walk plus an open per file: the
	// timestamps are what decide whether a file has to be parsed at all.
	FMetaWalker Walker;
	IFileManager::Get().IterateDirectoryStatRecursively(*MetaDir, Walker);
	TArray<FMetaFile>& Files = Walker.Files;
	Files.Sort([](const FMetaFile& A, const FMetaFile& B) { return A.Path < B.Path; });

	TMap<FString, TSharedPtr<FKuraEntry>> Reusable;
	if (Previous)
	{
		Reusable.Reserve(Previous->Num());
		for (const TSharedPtr<FKuraEntry>& E : *Previous)
		{
			if (!E->MetaFile.IsEmpty())
			{
				Reusable.Add(E->MetaFile, E);
			}
		}
	}

	TMap<FString, TSharedPtr<FKuraEntry>> ByPackage;
	for (const FMetaFile& Meta : Files)
	{
		const FString& File = Meta.Path;
		// Meta/ also hosts collections.json at its top, which describes the
		// classification tree rather than an asset. Matched at the top level
		// only: an asset named "collections" further down owns a meta file
		// with exactly that name, and skipping it by name anywhere would drop
		// that entry from the panel without a word (2026-08-23; keep in sync
		// with config.RESERVED_META_FILES and store.iter_metas).
		if (FPaths::GetCleanFilename(File) == TEXT("collections.json")
			&& FPaths::IsSamePath(FPaths::GetPath(File), MetaDir))
		{
			continue;
		}
		if (const TSharedPtr<FKuraEntry>* Kept = Reusable.Find(File))
		{
			if ((*Kept)->MetaStamp == Meta.Stamp && (*Kept)->MetaSize == Meta.Size)
			{
				// Reverse edges are rebuilt over the whole set below, so the
				// carried-over entry has to arrive without last pass's.
				(*Kept)->Referrers.Reset();
				ByPackage.Add((*Kept)->Package, *Kept);
				Out.Add(*Kept);
				continue;
			}
		}
		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *File))
		{
			continue;
		}
		TSharedPtr<FJsonObject> M;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, M) || !M.IsValid())
		{
			UE_LOG(LogKura, Warning, TEXT("Failed to parse meta %s"), *File);
			continue;
		}

		const TSharedPtr<FKuraEntry> E = MakeShared<FKuraEntry>();
		M->TryGetStringField(TEXT("package"), E->Package);
		if (!E->Package.StartsWith(Prefix))
		{
			UE_LOG(LogKura, Warning, TEXT("Meta with foreign package skipped: %s"), *File);
			continue;
		}
		E->RelPath = E->Package.Mid(Prefix.Len());
		E->ThumbPath = MetaDir / E->RelPath + TEXT(".png");
		int32 SlashIdx = INDEX_NONE;
		if (E->RelPath.FindLastChar(TEXT('/'), SlashIdx))
		{
			E->Folder = E->RelPath.Left(SlashIdx);
			E->Name = E->RelPath.Mid(SlashIdx + 1);
		}
		else
		{
			E->Name = E->RelPath;
		}

		M->TryGetStringField(TEXT("class"), E->Class);
		M->TryGetStringField(TEXT("sha256"), E->Sha256);
		double Size = 0.0;
		M->TryGetNumberField(TEXT("size"), Size);
		E->Size = static_cast<int64>(Size);
		M->TryGetStringField(TEXT("engine_version"), E->EngineVersion);
		M->TryGetStringField(TEXT("ingested_at"), E->IngestedAt);
		M->TryGetStringField(TEXT("updated_at"), E->UpdatedAt);
		M->TryGetStringField(TEXT("source_project"), E->SourceProject);
		M->TryGetStringField(TEXT("notes"), E->Notes);
		M->TryGetBoolField(TEXT("is_root"), E->bIsRoot);
		ReadStringArray(M, TEXT("tags"), E->Tags);
		ReadStringArray(M, TEXT("collections"), E->Collections);
		ReadStringArray(M, TEXT("hard_deps"), E->HardDeps);
		ReadStringArray(M, TEXT("engine_deps"), E->EngineDeps);

		E->MetaFile = File;
		E->MetaStamp = Meta.Stamp;
		E->MetaSize = Meta.Size;
		E->PackageLower = E->Package.ToLower();
		E->NotesLower = E->Notes.ToLower();
		E->TagsLower.Reserve(E->Tags.Num());
		for (const FString& T : E->Tags)
		{
			E->TagsLower.Add(T.ToLower());
		}

		ByPackage.Add(E->Package, E);
		Out.Add(E);
	}

	// Reverse dependency edges (SPEC 5.3): detail panel + delete guard hints.
	for (const TSharedPtr<FKuraEntry>& E : Out)
	{
		for (const FString& Dep : E->HardDeps)
		{
			if (const TSharedPtr<FKuraEntry>* Target = ByPackage.Find(Dep))
			{
				(*Target)->Referrers.Add(E->Package);
			}
		}
	}
	for (const TSharedPtr<FKuraEntry>& E : Out)
	{
		E->Referrers.Sort();
	}
	return Out;
}

bool KuraLibrary::ClassMatchesTab(const FString& ClassName, int32 Tab)
{
	if (Tab < 0 || Tab >= NumClassTabs)
	{
		return true;
	}
	const TArray<FString>& Names = TabClassNames()[Tab];
	if (Names.Num() == 0)
	{
		return true; // the All tab
	}
	return Names.Contains(ClassName);
}

int32 KuraLibrary::AllTabIndex()
{
	for (int32 Idx = 0; Idx < NumClassTabs; ++Idx)
	{
		if (TabClassNames()[Idx].Num() == 0)
		{
			return Idx;
		}
	}
	return 0;
}

bool KuraLibrary::IsAllTab(int32 Tab)
{
	return Tab >= 0 && Tab < NumClassTabs && TabClassNames()[Tab].Num() == 0;
}

int32 KuraLibrary::TabIndexForClass(const FString& ClassName)
{
	for (int32 Idx = 0; Idx < NumClassTabs; ++Idx)
	{
		if (IsAllTab(Idx))
		{
			continue; // the All tab is the fallback, not a match
		}
		if (ClassMatchesTab(ClassName, Idx))
		{
			return Idx;
		}
	}
	return AllTabIndex();
}

int32 FKuraCollectionNode::CountForTab(int32 Tab) const
{
	if (KuraLibrary::IsAllTab(Tab))
	{
		return Count;
	}
	int32 Total = 0;
	for (const TPair<FString, int32>& Pair : ClassCounts)
	{
		if (KuraLibrary::ClassMatchesTab(Pair.Key, Tab))
		{
			Total += Pair.Value;
		}
	}
	return Total;
}

TArray<TSharedPtr<FKuraFolderNode>> KuraLibrary::LoadFolderTree(
	const TArray<TSharedPtr<FKuraEntry>>& Entries)
{
	// One tree for the whole asset tree since 2026-08-10 (Q24): the folder
	// layout no longer encodes type, so there is nothing to split it by.
	//
	// Built from the entries rather than from a directory walk (2026-08-23).
	// The tree is exactly "folders that hold entries, plus their ancestors",
	// and every entry already carries its own folder - so the walk was a
	// second full pass over the library on top of the meta walk, re-deriving
	// what was in memory already (2,717 directory enumerations per refresh at
	// reference scale; 0.05 s warm, 2.4 s cold). It also drops phantom nodes:
	// a directory left behind on disk with nothing registered in it used to
	// show up in the tree as an empty folder.
	TArray<TSharedPtr<FKuraFolderNode>> Roots;
	TMap<FString, TSharedPtr<FKuraFolderNode>> ByPath;
	TArray<FString> Segments;
	for (const TSharedPtr<FKuraEntry>& E : Entries)
	{
		if (E->Folder.IsEmpty())
		{
			continue; // an entry at the top of the tree adds no folder
		}
		Segments.Reset();
		E->Folder.ParseIntoArray(Segments, TEXT("/"));
		FString Path;
		TArray<TSharedPtr<FKuraFolderNode>>* Siblings = &Roots;
		for (const FString& Segment : Segments)
		{
			Path = Path.IsEmpty() ? Segment : Path + TEXT("/") + Segment;
			// The node lives on the heap, so the pointer into its Children
			// survives whatever the map does to itself on the next insert.
			TSharedPtr<FKuraFolderNode>& Node = ByPath.FindOrAdd(Path);
			if (!Node.IsValid())
			{
				Node = MakeShared<FKuraFolderNode>();
				Node->Name = Segment;
				Node->RelPath = Path;
				Siblings->Add(Node);
			}
			Siblings = &Node->Children;
		}
	}
	SortFolderLevel(Roots);
	return Roots;
}

TArray<TSharedPtr<FKuraCollectionNode>> KuraLibrary::LoadCollectionTree(
	const TArray<TSharedPtr<FKuraEntry>>& Entries)
{
	TArray<TSharedPtr<FKuraCollectionNode>> Roots;

	FString Json;
	const FString Path = GetLibraryRoot() / TEXT("Meta") / TEXT("collections.json");
	TSharedPtr<FJsonObject> Doc;
	if (FFileHelper::LoadFileToString(Json, *Path))
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Doc))
		{
			UE_LOG(LogKura, Warning, TEXT("Failed to parse %s"), *Path);
			Doc.Reset();
		}
	}

	// Direct membership counts; rolled up into ancestors after the tree is
	// built. Kept split by class as well so each tab can show its own number
	// and hide the collections it has nothing to say about (F2/F8).
	TMap<FString, int32> DirectCounts;
	TMap<FString, TMap<FString, int32>> DirectClassCounts;
	TMap<FString, int32> UnsortedClassCounts;
	TMap<FString, int32> SharedClassCounts;
	int32 UnsortedCount = 0;
	int32 SharedCount = 0;
	for (const TSharedPtr<FKuraEntry>& E : Entries)
	{
		for (const FString& Cid : E->Collections)
		{
			DirectCounts.FindOrAdd(Cid)++;
			DirectClassCounts.FindOrAdd(Cid).FindOrAdd(E->Class)++;
		}
		if (E->bIsRoot && E->Collections.Num() == 0)
		{
			++UnsortedCount;
			UnsortedClassCounts.FindOrAdd(E->Class)++;
		}
		if (E->Referrers.Num() >= SharedReferrerThreshold)
		{
			++SharedCount;
			SharedClassCounts.FindOrAdd(E->Class)++;
		}
	}

	// Flat id -> node, then parent wiring. A node whose parent is missing
	// (hand-edited file, interrupted delete) surfaces at the top rather than
	// disappearing - same rule as kura.catalog.
	TMap<FString, TSharedPtr<FKuraCollectionNode>> ById;
	TArray<TPair<FString, FString>> Parents; // id -> parent id
	TArray<TPair<FString, int32>> Order;     // id -> sort order
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (Doc.IsValid() && Doc->TryGetArrayField(TEXT("collections"), Items))
	{
		for (const TSharedPtr<FJsonValue>& V : *Items)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!V->TryGetObject(Obj))
			{
				continue;
			}
			FString Id, Name, Parent;
			(*Obj)->TryGetStringField(TEXT("id"), Id);
			(*Obj)->TryGetStringField(TEXT("name"), Name);
			(*Obj)->TryGetStringField(TEXT("parent"), Parent);
			if (Id.IsEmpty() || Name.IsEmpty() || ById.Contains(Id))
			{
				continue;
			}
			double SortOrder = 0.0;
			(*Obj)->TryGetNumberField(TEXT("order"), SortOrder);
			const TSharedPtr<FKuraCollectionNode> Node = MakeShared<FKuraCollectionNode>();
			Node->Id = Id;
			Node->Name = Name;
			Node->Count = DirectCounts.FindRef(Id);
			Node->ClassCounts = DirectClassCounts.FindRef(Id);
			ById.Add(Id, Node);
			Parents.Emplace(Id, Parent);
			Order.Emplace(Id, (int32)SortOrder);
		}
	}
	for (const TPair<FString, FString>& Pair : Parents)
	{
		const TSharedPtr<FKuraCollectionNode> Node = ById[Pair.Key];
		if (const TSharedPtr<FKuraCollectionNode>* Parent = ById.Find(Pair.Value))
		{
			(*Parent)->Children.Add(Node);
		}
		else
		{
			Roots.Add(Node);
		}
	}

	TMap<FString, int32> OrderById;
	for (const TPair<FString, int32>& Pair : Order)
	{
		OrderById.Add(Pair.Key, Pair.Value);
	}
	TFunction<void(TArray<TSharedPtr<FKuraCollectionNode>>&)> SortAndRollUp =
		[&SortAndRollUp, &OrderById](TArray<TSharedPtr<FKuraCollectionNode>>& Nodes)
	{
		Nodes.Sort([&OrderById](const TSharedPtr<FKuraCollectionNode>& A,
			const TSharedPtr<FKuraCollectionNode>& B)
		{
			const int32 OA = OrderById.FindRef(A->Id);
			const int32 OB = OrderById.FindRef(B->Id);
			return OA != OB ? OA < OB : A->Name.Compare(B->Name, ESearchCase::IgnoreCase) < 0;
		});
		for (const TSharedPtr<FKuraCollectionNode>& Node : Nodes)
		{
			SortAndRollUp(Node->Children);
			// Show what clicking the node will actually list, which includes
			// what is filed in its children (kura.catalog.members).
			for (const TSharedPtr<FKuraCollectionNode>& Child : Node->Children)
			{
				Node->Count += Child->Count;
				for (const TPair<FString, int32>& Pair : Child->ClassCounts)
				{
					Node->ClassCounts.FindOrAdd(Pair.Key) += Pair.Value;
				}
			}
		}
	};
	SortAndRollUp(Roots);

	if (UnsortedCount > 0)
	{
		// Last, and only while it has something in it: an always-present empty
		// bucket would just be noise in a fully filed library.
		const TSharedPtr<FKuraCollectionNode> Unsorted = MakeShared<FKuraCollectionNode>();
		Unsorted->Id = UnsortedId;
		Unsorted->Name = TEXT("Unsorted");
		Unsorted->Count = UnsortedCount;
		Unsorted->ClassCounts = UnsortedClassCounts;
		Unsorted->bUnsorted = true;
		Roots.Add(Unsorted);
	}

	if (SharedCount > 0)
	{
		// The one browsing entrance to shared infrastructure (masters, material
		// functions, parameter collections, placeholder textures). They are
		// non-root and, being shared, are excluded from every collection's
		// "owned dependencies" by definition - so without this node they exist
		// in the library and appear nowhere in this view (SPEC 5.4).
		const TSharedPtr<FKuraCollectionNode> Shared = MakeShared<FKuraCollectionNode>();
		Shared->Id = SharedId;
		Shared->Name = TEXT("Shared Dependencies");
		Shared->Count = SharedCount;
		Shared->ClassCounts = SharedClassCounts;
		Shared->bShared = true;
		Roots.Add(Shared);
	}
	return Roots;
}
