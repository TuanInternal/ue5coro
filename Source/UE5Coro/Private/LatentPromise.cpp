// Copyright © Laura Andelare
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted (subject to the limitations in the disclaimer
// below) provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.
//
// NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE GRANTED BY
// THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
// CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT
// NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
// PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
// OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
// ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <exception>
#include "LatentActions.h"
#include "LatentExitReason.h"
#include "UE5Coro/AsyncAwaiters.h"
#include "UE5Coro/AsyncCoroutine.h"
#include "UE5Coro/LatentAwaiters.h"

using namespace UE5Coro::Private;

namespace
{
class [[nodiscard]] FPendingLatentCoroutine final : public FPendingLatentAction
{
	// The coroutine may move to other threads, but this object only interacts
	// with its promise on the game thread.
	// Since latent promises are destroyed on the game thread, there's nothing
	// to synchronize and the lock is not used to access Extras->Promise.
	std::shared_ptr<FPromiseExtras> Extras;
	FLatentActionInfo LatentInfo;
	FLatentAwaiter* CurrentAwaiter = nullptr;

public:
	explicit FPendingLatentCoroutine(std::shared_ptr<FPromiseExtras> Extras,
	                                 FLatentActionInfo LatentInfo)
		: Extras(std::move(Extras)), LatentInfo(LatentInfo) { }

	UE_NONCOPYABLE(FPendingLatentCoroutine);

	virtual ~FPendingLatentCoroutine() override
	{
		checkf(IsInGameThread(),
		       TEXT("Unexpected latent action off the game thread"));
		if (auto* LatentPromise = static_cast<FLatentPromise*>(Extras->Promise);
		    LIKELY(LatentPromise))
		{
			LatentPromise->Cancel();
			// Process the cancellation right away, there might be no resumption
			LatentPromise->Resume(true);
		}
	}

	virtual void UpdateOperation(FLatentResponse& Response) override
	{
		checkf(IsInGameThread(),
		       TEXT("Internal error: expected game thread update"));
		auto* LatentPromise = static_cast<FLatentPromise*>(Extras->Promise);

		if (UNLIKELY(!LatentPromise))
		{
			Response.DoneIf(true);
			return;
		}

		if (CurrentAwaiter && CurrentAwaiter->ShouldResume())
		{
			CurrentAwaiter = nullptr;
			// This might set the awaiter for next time
			LatentPromise->Resume();
		}

		// Resume() might have deleted LatentPromise, check it again
		if (LIKELY(Extras->Promise))
			LatentPromise->Respond(Response, LatentInfo);
		else
			Response.DoneIf(true);
	}

	virtual void NotifyActionAborted() override
	{
		checkf(IsInGameThread(),
		       TEXT("Internal error: expected callback from the game thread"));
		if (auto* LatentPromise = static_cast<FLatentPromise*>(Extras->Promise);
		    LIKELY(LatentPromise))
			LatentPromise->SetExitReason(ELatentExitReason::ActionAborted);
	}

	virtual void NotifyObjectDestroyed() override
	{
		checkf(IsInGameThread(),
		       TEXT("Internal error: expected callback from the game thread"));
		if (auto* LatentPromise = static_cast<FLatentPromise*>(Extras->Promise);
		    LIKELY(LatentPromise))
			LatentPromise->SetExitReason(ELatentExitReason::ObjectDestroyed);
	}

	const FLatentActionInfo& GetLatentInfo() const { return LatentInfo; }

	void SetCurrentAwaiter(FLatentAwaiter* Awaiter)
	{
		checkf(IsInGameThread(),
		       TEXT("Latent awaiters may only be used on the game thread"));
		if (Awaiter)
			ensureMsgf(!CurrentAwaiter, TEXT("Unexpected double await"));

		CurrentAwaiter = Awaiter;
	}
};
}

void FLatentPromise::CreateLatentAction()
{
	// We're still scanning for the world, so use what we have right now
	auto* WorldNow = World ? World : GWorld;
	auto* Sys = WorldNow->GetSubsystem<UUE5CoroSubsystem>();
	CreateLatentAction(Sys->MakeLatentInfo());
}

// This is a separate function so that template Init() doesn't need the type
void FLatentPromise::CreateLatentAction(FLatentActionInfo&& LatentInfo)
{
	// The static_assert on coroutine_traits prevents this
	checkf(!PendingLatentCoroutine,
	       TEXT("Internal error: multiple latent infos were not prevented"));

	PendingLatentCoroutine = new FPendingLatentCoroutine(Extras, LatentInfo);
}

void FLatentPromise::Init()
{
	// This should have been an async promise without a LatentActionInfo
	checkf(PendingLatentCoroutine,
	       TEXT("Internal error: wrong coroutine promise type used"));

	// Last resort if we got this far without a world
	if (!World)
	{
		World = GWorld;
		checkf(World, TEXT("Could not determine world for latent coroutine"));
	}
}

FLatentPromise::~FLatentPromise()
{
	checkf(IsInGameThread(),
	       TEXT("Unexpected latent coroutine destruction off the game thread"));
	GLatentExitReason = ELatentExitReason::Normal;
}

void FLatentPromise::Resume(bool bBypassCancellationHolds)
{
	if (UNLIKELY(bBypassCancellationHolds))
	{
		// This can only happen from a game thread latent update
		checkf(IsInGameThread() && ShouldCancel(true),
		       TEXT("Internal error: wrong state for bypass request"));

		// If ownership is borrowed, let the guaranteed future Resume call
		// handle this
		if (LatentFlags & LF_Detached)
			return;

		// Otherwise, proceed with re-attaching and destruction
	}

	// Return ownership to the game thread and the latent action manager
	// once the multi-threaded adventure is over
	if (LatentFlags & LF_Detached && IsInGameThread())
		AttachToGameThread();

	FPromise::Resume(bBypassCancellationHolds);
}

void FLatentPromise::ThreadSafeDestroy()
{
	// There's no parent implementation to call

	// Latent coroutines always end on the game thread
	if (!IsInGameThread())
	{
		AsyncTask(ENamedThreads::GameThread, [this] { ThreadSafeDestroy(); });
		return;
	}

	// Since we're on the game thread now, there's no possibility of a race with
	// ~FPendingLatentCoroutine requesting another deletion
	GLatentExitReason = ExitReason;
	auto Handle = stdcoro::coroutine_handle<FLatentPromise>::from_promise(*this);
	Handle.destroy(); // Counts as delete this;
	checkf(GLatentExitReason == ELatentExitReason::Normal,
	       TEXT("Internal error: latent exit reason not restored"));
}

void FLatentPromise::AttachToGameThread()
{
	checkf(IsInGameThread(),
	       TEXT("Internal error: attaching to the GT while not on the GT"));
	LatentFlags &= ~LF_Detached;
}

void FLatentPromise::DetachFromGameThread()
{
	// Calling this method "pins" the promise and coroutine state, deferring any
	// destruction requests from the latent action manager.
	// This is useful for threading or callback-based awaiters to ensure that
	// there will be a valid promise and coroutine state to return to.
	// FLatentAwaiters use a dedicated code path and do not call this, as they
	// support destruction while being co_awaited.
	LatentFlags |= LF_Detached;
}

void FLatentPromise::Respond(FLatentResponse& Response,
                             const FLatentActionInfo& LatentInfo) const
{
	checkf(IsInGameThread(),
	       TEXT("Internal error: latent action tick off the game thread"));
	checkf(!Extras->IsComplete(),
	       TEXT("Internal error: completed promise is still polled"));

	auto Flags = LatentFlags.load();
	bool bDetached = Flags & LF_Detached;
	bool bFinalSuspend = Flags & LF_InFinalSuspend;

	// Cancellations are implicitly held until the coroutine re-attaches.
	// If there's an attached cancellation or final_suspend, the coroutine will
	// not do anything meaningful and the latent action is over.
	if (ShouldCancel(false) && !bDetached || bFinalSuspend)
		Response.DoneIf(true);

	// The coroutine ran to completion and BP should continue
	if (bFinalSuspend)
		Response.TriggerLink(LatentInfo.ExecutionFunction, LatentInfo.Linkage,
		                     LatentInfo.CallbackTarget);
}

void FLatentPromise::SetExitReason(ELatentExitReason Reason)
{
	checkf(ExitReason == ELatentExitReason::Normal,
	       TEXT("Internal error: setting conflicting exit reasons"));
	ExitReason = Reason;
}

void FLatentPromise::SetCurrentAwaiter(FLatentAwaiter* Awaiter)
{
	checkf(IsInGameThread(),
	       TEXT("Latent awaiters may only be used on the game thread"));
	// How is a new latent awaiter getting added in these states?
	checkf(LatentFlags == 0, TEXT("Unexpected state in latent coroutine"));
	auto* Pending = static_cast<FPendingLatentCoroutine*>(PendingLatentCoroutine);
	Pending->SetCurrentAwaiter(Awaiter);
}

FInitialSuspend FLatentPromise::initial_suspend()
{
	checkf(IsInGameThread(),
	       TEXT("Latent coroutines may only be started on the game thread"));

	auto& LAM = World->GetLatentActionManager();
	auto* Pending = static_cast<FPendingLatentCoroutine*>(PendingLatentCoroutine);
	auto& LatentInfo = Pending->GetLatentInfo();

	// Don't let the coroutine run and clean up if this is a duplicate
	if (LAM.FindExistingAction<FPendingLatentCoroutine>(
			LatentInfo.CallbackTarget, LatentInfo.UUID))
		return {FInitialSuspend::Destroy};

	// Also refuse to run if there's no callback target
	if (!ensureMsgf(IsValid(LatentInfo.CallbackTarget),
	                TEXT("Not starting latent coroutine with invalid target")))
		return {FInitialSuspend::Destroy};

	LAM.AddNewAction(LatentInfo.CallbackTarget, LatentInfo.UUID, Pending);

	// Let the coroutine start immediately on its calling thread
	return {FInitialSuspend::Resume};
}

stdcoro::suspend_always FLatentPromise::final_suspend() noexcept
{
	// Too late for cancellations now.
	// Flags are overwritten, i.e., the coroutine is unconditionally reattached.
	LatentFlags = LF_InFinalSuspend;

	// Due to the free-threaded attachment, there's a potential data race now,
	// including another thread deleting `this`, so it may not be used anymore
	return {};
}
