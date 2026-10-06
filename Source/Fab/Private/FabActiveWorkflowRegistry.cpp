// Copyright Epic Games, Inc. All Rights Reserved.

#include "FabActiveWorkflowRegistry.h"

#include "FabLog.h"

#include "Containers/Array.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

#include "Workflows/FabWorkflow.h"

namespace
{
	// Locked rather than asserted onto the game thread: the JS bridge drives registration, and the
	// pack path marshals its Execute() through an AsyncTask.
	FCriticalSection RegistryLock;
	TArray<TSharedRef<IFabWorkflow>> Workflows;

	// Weak so the workflow's own delegate does not hold the last reference to it.
	void HandleWorkflowFinished(TWeakPtr<IFabWorkflow> InWorkflow, bool bCancelled)
	{
		const TSharedPtr<IFabWorkflow> Workflow = InWorkflow.Pin();
		if (!Workflow.IsValid())
		{
			return;
		}

		FAB_LOG("Fab workflow %s for asset %s", bCancelled ? TEXT("cancelled") : TEXT("completed"), *Workflow->AssetId);

		// Held above the lock so removal does not destroy the workflow under it.
		FScopeLock Lock(&RegistryLock);
		Workflows.Remove(Workflow.ToSharedRef());
	}
}

bool FFabActiveWorkflowRegistry::TryAddWorkflow(const TSharedRef<IFabWorkflow>& InWorkflow)
{
	FScopeLock Lock(&RegistryLock);

	const bool bAlreadyInFlight = Workflows.ContainsByPredicate(
		[&InWorkflow](const TSharedRef<IFabWorkflow>& Workflow)
		{
			return Workflow->AssetId == InWorkflow->AssetId;
		}
	);

	if (bAlreadyInFlight)
	{
		FAB_LOG("The listing with Id %s is already being processed.", *InWorkflow->AssetId);
		return false;
	}

	Workflows.Add(InWorkflow);
	InWorkflow->OnFabWorkflowComplete().BindStatic(&HandleWorkflowFinished, InWorkflow.ToWeakPtr(), false);
	InWorkflow->OnFabWorkflowCancel().BindStatic(&HandleWorkflowFinished, InWorkflow.ToWeakPtr(), true);

	FAB_LOG("Fab workflow registered for asset %s (%d in flight)", *InWorkflow->AssetId, Workflows.Num());
	return true;
}

void FFabActiveWorkflowRegistry::Shutdown()
{
	TArray<TSharedRef<IFabWorkflow>> Released;
	{
		FScopeLock Lock(&RegistryLock);
		Released = MoveTemp(Workflows);
	}

	// The delegates target statics in this module, which an in-flight workflow can outlive.
	for (const TSharedRef<IFabWorkflow>& Workflow : Released)
	{
		Workflow->OnFabWorkflowComplete().Unbind();
		Workflow->OnFabWorkflowCancel().Unbind();
	}
}
