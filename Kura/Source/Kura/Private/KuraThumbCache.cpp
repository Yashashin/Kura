#include "KuraThumbCache.h"

#include "Async/Async.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogKuraThumbCache, Log, All);

namespace
{
	/** Resident brushes. 1024 x 256 KB (a 256-square BGRA texture) = 256 MB
	 *  worst case, the same order as the Content Browser's own thumbnail pool. */
	constexpr int32 MaxResident = 1024;
	/** Dropped in one sweep once the cap is hit, so the LRU pass is amortised
	 *  over many inserts instead of running on every single one. */
	constexpr int32 EvictBatch = 128;
	/** Concurrent background loads. The bottleneck is one disk head, so more
	 *  threads only make the queue longer; this also caps how many results can
	 *  be waiting for the game thread at once, and therefore how much pixel
	 *  copying a single tick can be asked to do. */
	constexpr int32 MaxInFlight = 8;
	/** Requests waiting for a thread. Newest first, so the tiles on screen win
	 *  over ones the user has already scrolled past; the stale end is dropped
	 *  when a fast scroll overflows it, and a dropped path is simply requested
	 *  again the next time it is painted. */
	constexpr int32 MaxQueued = 256;

	/** What Get can show right now. Whether a load is in flight is tracked
	 *  separately (bPending), because a resident-but-stale entry keeps drawing
	 *  its old brush while the new one is read. */
	enum class EThumbState : uint8
	{
		Unloaded,
		Ready,
		Missing, // no PNG on disk, or undecodable - do not retry every paint
	};

	struct FCachedThumb
	{
		TSharedPtr<FSlateDynamicImageBrush> Brush;
		FDateTime Stamp = FDateTime::MinValue(); // mtime the brush was built from
		EThumbState State = EThumbState::Unloaded;
		bool bPending = false;
		uint64 LastUsed = 0;
		uint32 CheckedGen = 0; // Revalidate generation this entry was last read in
	};

	struct FThumbRequest
	{
		FString Path;
		FDateTime KnownStamp = FDateTime::MinValue();
		uint32 Gen = 0;
	};

	struct FThumbResult
	{
		FString Path;
		FDateTime Stamp = FDateTime::MinValue();
		TArray<uint8> Bgra;
		int32 Width = 0;
		int32 Height = 0;
		uint32 Gen = 0;
		bool bMissing = false;   // gone from disk, or unreadable
		bool bUnchanged = false; // same timestamp as the resident brush
	};

	TMap<FString, FCachedThumb> GCache;
	TArray<FThumbRequest> GQueue; // request stack, popped from the back
	FCriticalSection GResultsLock;
	TArray<FThumbResult> GResults;
	int32 GInFlight = 0;   // game thread only: bumped on launch, cleared on drain
	int32 GResident = 0;   // entries in EThumbState::Ready
	uint64 GClock = 0;     // LRU stamp
	uint32 GGeneration = 1;
	uint64 GResourceCounter = 0;

	void ReleaseBrush(FCachedThumb& Entry)
	{
		if (Entry.State == EThumbState::Ready)
		{
			--GResident;
		}
		// The last reference going away is what releases the Slate texture
		// (~FSlateDynamicImageBrush -> ReleaseDynamicResource), and the RHI
		// release itself is deferred to the render thread, so dropping one
		// that this frame's draw elements still reference is safe.
		Entry.Brush.Reset();
		Entry.Stamp = FDateTime::MinValue();
		Entry.State = EThumbState::Unloaded;
	}

	/** Runs on a worker thread: stat, read, decode. Never touches GCache. */
	void LoadThumb(IImageWrapperModule* Module, FThumbRequest Request)
	{
		FThumbResult Result;
		Result.Path = Request.Path;
		Result.Gen = Request.Gen;
		Result.Stamp = IFileManager::Get().GetTimeStamp(*Request.Path);

		if (Result.Stamp == FDateTime::MinValue())
		{
			Result.bMissing = true;
		}
		else if (Result.Stamp == Request.KnownStamp)
		{
			// Untouched since it was decoded - this is the whole point of doing
			// the stat out here: a refresh costs no game-thread work at all.
			Result.bUnchanged = true;
		}
		else
		{
			TArray<uint8> FileData;
			const TSharedPtr<IImageWrapper> Wrapper = Module
				? Module->CreateImageWrapper(EImageFormat::PNG) : nullptr;
			if (!FFileHelper::LoadFileToArray(FileData, *Request.Path)
				|| !Wrapper.IsValid()
				|| !Wrapper->SetCompressed(FileData.GetData(), FileData.Num())
				|| !Wrapper->GetRaw(ERGBFormat::BGRA, 8, Result.Bgra))
			{
				UE_LOG(LogKuraThumbCache, Warning, TEXT("Unreadable thumbnail PNG: %s"), *Request.Path);
				Result.bMissing = true;
			}
			else
			{
				Result.Width = Wrapper->GetWidth();
				Result.Height = Wrapper->GetHeight();
			}
		}

		FScopeLock Lock(&GResultsLock);
		GResults.Add(MoveTemp(Result));
	}

	void Publish(FThumbResult& Result)
	{
		FCachedThumb* Entry = GCache.Find(Result.Path);
		if (!Entry)
		{
			return; // dropped while the load was in flight
		}
		Entry->bPending = false;
		// The generation at REQUEST time: a Revalidate that landed mid-flight
		// has to leave the entry due for another check.
		Entry->CheckedGen = Result.Gen;

		if (Result.bUnchanged)
		{
			return;
		}
		if (Result.bMissing || Result.Width <= 0 || Result.Height <= 0)
		{
			ReleaseBrush(*Entry);
			Entry->State = EThumbState::Missing;
			return;
		}

		// A bare counter, not the path: the name becomes an FName, and FNames
		// are never freed - keying 10k long paths would leak the name table a
		// little more on every reload.
		const FName ResourceName(*FString::Printf(TEXT("KuraThumb_%llu"), GResourceCounter++));
		if (!FSlateApplication::Get().GetRenderer()->GenerateDynamicImageResource(
			ResourceName, Result.Width, Result.Height, Result.Bgra))
		{
			UE_LOG(LogKuraThumbCache, Warning, TEXT("Slate resource creation failed for %s"), *Result.Path);
			ReleaseBrush(*Entry);
			Entry->State = EThumbState::Missing;
			return;
		}
		ReleaseBrush(*Entry);
		Entry->Brush = MakeShared<FSlateDynamicImageBrush>(
			ResourceName, FVector2D(Result.Width, Result.Height));
		Entry->Stamp = Result.Stamp;
		Entry->State = EThumbState::Ready;
		++GResident;
	}

	void EvictOverBudget()
	{
		if (GResident <= MaxResident)
		{
			return;
		}
		TArray<TPair<uint64, FString>> Candidates;
		Candidates.Reserve(GResident);
		for (const TPair<FString, FCachedThumb>& Pair : GCache)
		{
			if (Pair.Value.State == EThumbState::Ready)
			{
				Candidates.Emplace(Pair.Value.LastUsed, Pair.Key);
			}
		}
		Candidates.Sort([](const TPair<uint64, FString>& A, const TPair<uint64, FString>& B)
		{
			return A.Key < B.Key;
		});
		const int32 Target = FMath::Min(GResident - MaxResident + EvictBatch, Candidates.Num());
		for (int32 Idx = 0; Idx < Target; ++Idx)
		{
			if (FCachedThumb* Entry = GCache.Find(Candidates[Idx].Value))
			{
				ReleaseBrush(*Entry);
			}
		}
	}
}

const FSlateBrush* KuraThumbs::Get(const FString& PngPath)
{
	check(IsInGameThread());

	FCachedThumb& Entry = GCache.FindOrAdd(PngPath);
	Entry.LastUsed = ++GClock;

	const bool bNeedsRead = Entry.State == EThumbState::Unloaded || Entry.CheckedGen != GGeneration;
	if (bNeedsRead && !Entry.bPending)
	{
		Entry.bPending = true;
		if (GQueue.Num() >= MaxQueued)
		{
			// Oldest request out: it belongs to a tile that has long since
			// scrolled away, and painting it again re-queues it.
			if (FCachedThumb* Stale = GCache.Find(GQueue[0].Path))
			{
				Stale->bPending = false;
			}
			GQueue.RemoveAt(0);
		}
		GQueue.Add({ PngPath, Entry.Stamp, GGeneration });
	}

	return Entry.State == EThumbState::Ready ? Entry.Brush.Get() : nullptr;
}

void KuraThumbs::Tick()
{
	check(IsInGameThread());

	TArray<FThumbResult> Finished;
	{
		FScopeLock Lock(&GResultsLock);
		Finished = MoveTemp(GResults);
		GResults.Reset();
	}
	GInFlight -= Finished.Num();
	for (FThumbResult& Result : Finished)
	{
		Publish(Result);
	}

	if (GQueue.Num() > 0 && GInFlight < MaxInFlight)
	{
		// Resolved here rather than on the worker: module loading is game
		// thread only. ImageWrapper is already up (the editor uses it), so
		// this is a lookup, not a load.
		IImageWrapperModule* Module =
			FModuleManager::LoadModulePtr<IImageWrapperModule>(TEXT("ImageWrapper"));
		while (GQueue.Num() > 0 && GInFlight < MaxInFlight)
		{
			FThumbRequest Request = GQueue.Pop(EAllowShrinking::No);
			++GInFlight;
			AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
				[Module, Request = MoveTemp(Request)]() mutable
			{
				LoadThumb(Module, MoveTemp(Request));
			});
		}
	}

	EvictOverBudget();
}

void KuraThumbs::Revalidate()
{
	check(IsInGameThread());
	++GGeneration;
}
