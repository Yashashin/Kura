#pragma once

#include "CoreMinimal.h"

struct FSlateBrush;

/** Slate brushes for Meta-mirror thumbnail PNGs.
 *
 *  Loading is ASYNCHRONOUS and the cache is BOUNDED (2026-08-20, SPEC 7.2).
 *  Both matter at library scale, and neither did at Phase 2 scale:
 *
 *  - A 10k-entry library is ~940 MB of PNGs. Reading one off a spinning disk
 *    measures ~30 ms, so a screenful of fresh tiles was over a second of
 *    frozen editor, repeated for every page scrolled into. Get() never touches
 *    the disk now - it returns what is resident and hands everything else to a
 *    background thread, so tiles fill in progressively like the Content
 *    Browser's do.
 *  - Every thumbnail ever shown used to stay resident as a 256x256 BGRA Slate
 *    texture (256 KB), which is 2.7 GB for a full browse of that library - and
 *    it is never reclaimed, so it evicts the OS file cache that the thumbnails
 *    themselves depend on. The resident set is capped and evicted LRU instead. */
namespace KuraThumbs
{
	/** Brush for a PNG, or nullptr while it is still loading, missing or
	 *  unreadable. Queues a load on a miss. Game thread only.
	 *
	 *  Cheap enough to call from a paint-time attribute, which is how a tile
	 *  picks up its thumbnail without being regenerated. */
	const FSlateBrush* Get(const FString& PngPath);

	/** Start queued loads and publish finished ones. Call once per tick from
	 *  the panel - nothing else drives the queue. */
	void Tick();

	/** Re-check every cached path on its next Get: a re-ingest or a Refresh
	 *  Thumbnail overwrites PNGs in place, and Get no longer stats per paint.
	 *  A resident brush keeps showing until its replacement has been read, so
	 *  a refresh never flashes empty tiles. */
	void Revalidate();
}
