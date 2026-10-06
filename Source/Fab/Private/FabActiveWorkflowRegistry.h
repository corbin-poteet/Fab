// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Templates/SharedPointer.h"

class IFabWorkflow;

/**
 * Module-scoped owner of every in-flight Fab workflow.
 *
 * Workflows outlive the browser tab that starts them, so owning them from UFabBrowserApi left
 * their completion delegates firing on a collected UObject.
 */
class FFabActiveWorkflowRegistry
{
public:
	/**
	 * Takes ownership of the workflow and binds its completion and cancellation delegates, which
	 * this registry owns exclusively. Call before Execute() so a synchronous finish still
	 * unregisters.
	 *
	 * @return False if a workflow for the same asset id is already in flight, in which case the
	 *         workflow is neither stored nor bound and must not be executed.
	 */
	[[nodiscard]] static bool TryAddWorkflow(const TSharedRef<IFabWorkflow>& InWorkflow);

	/** Releases and unbinds everything still in flight. Called at module shutdown. */
	static void Shutdown();
};
