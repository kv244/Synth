// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include "SampleData.h"

/**
    @file SampleSlot.h

    Hands the current SampleData from the message/loader side to the audio
    thread without locks, and frees replaced samples only once the audio
    thread can no longer be using them (an RCU-style scheme).

    Audio thread, at the start of every block:
        const SampleData* sample = slot.beginBlock();
    It may use `sample` for the rest of the block. A voice that keeps a
    sample beyond the block (a note that is still sounding) increments the
    sample's `users` count during the block and decrements it when done.

    Other threads: publish() swaps in a new sample and moves the old one to a
    retire list. A retired sample is freed when both
      - the audio thread has started a block after the swap (so the block
        that might have read the old pointer has finished, and any voice
        that took the sample has counted itself in `users`), and
      - its `users` count is back to zero.
    collectGarbage() frees what qualifies; publish() and the destructor call
    it too. While the audio thread isn't running (setAudioRunning (false)),
    nothing can be reading the pointer, so replaced samples are freed at once.

    The audio thread never allocates, frees or locks here.
*/
class SampleSlot
{
public:
    SampleSlot() = default;
    ~SampleSlot() { current.store (nullptr); }   // owned and retired samples are freed by their unique_ptrs

    //==========================================================================
    // Audio thread

    /** Call once at the start of each block; the result stays valid for the block. */
    const SampleData* beginBlock() noexcept
    {
        blocksStarted.fetch_add (1);
        return current.load();
    }

    //==========================================================================
    // Message or loader thread (calls are serialised by an internal lock)

    /** Makes `newSample` the current sample; the previous one is retired. */
    void publish (std::unique_ptr<SampleData> newSample)
    {
        const juce::ScopedLock sl (lock);

        current.store (newSample.get());
        const auto retiredAt = blocksStarted.load();

        if (owned != nullptr)
            retired.push_back ({ std::move (owned), retiredAt });

        owned = std::move (newSample);
        collectGarbageLocked();
    }

    /** Frees retired samples that the audio thread can no longer reach. */
    void collectGarbage()
    {
        const juce::ScopedLock sl (lock);
        collectGarbageLocked();
    }

    /**
        Calls `fn (const SampleData*)` with the current sample (nullptr before
        the first publish) while holding the lock, so a concurrent publish
        can't free it mid-read. For reading its origin, hash or embedded FLAC;
        keep `fn` short. Not for the audio thread.
    */
    template <typename Fn>
    auto withCurrent (Fn&& fn) const
    {
        const juce::ScopedLock sl (lock);
        return fn (static_cast<const SampleData*> (owned.get()));
    }

    /** prepareToPlay() / releaseResources(): whether processBlock can be running. */
    void setAudioRunning (bool shouldBeRunning)
    {
        const juce::ScopedLock sl (lock);
        audioRunning = shouldBeRunning;
        collectGarbageLocked();
    }

    /** Retired samples still waiting to be freed (for tests). */
    int getNumRetired() const
    {
        const juce::ScopedLock sl (lock);
        return (int) retired.size();
    }

private:
    struct Retired
    {
        std::unique_ptr<SampleData> data;
        juce::uint64 retiredAt;   // blocksStarted when it was replaced
    };

    void collectGarbageLocked()
    {
        const auto started = blocksStarted.load();

        retired.erase (std::remove_if (retired.begin(), retired.end(), [&] (const Retired& r)
        {
            const bool blockFinished = ! audioRunning || started > r.retiredAt;
            return blockFinished && r.data->users.load() == 0;
        }), retired.end());
    }

    std::atomic<const SampleData*> current { nullptr };
    std::atomic<juce::uint64> blocksStarted { 0 };

    juce::CriticalSection lock;                // never taken by the audio thread
    std::unique_ptr<SampleData> owned;
    std::vector<Retired> retired;
    bool audioRunning = false;
};
