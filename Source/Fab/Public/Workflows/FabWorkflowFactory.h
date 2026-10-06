// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "FabWorkflow.h"

class IFabWorkflowFactory
{
public:
	IFabWorkflowFactory() = default;
	virtual ~IFabWorkflowFactory() = default;

	IFabWorkflowFactory(const IFabWorkflowFactory&) = delete;
	IFabWorkflowFactory(IFabWorkflowFactory&&) = delete;

	IFabWorkflowFactory& operator=(const IFabWorkflowFactory&) = delete;
	IFabWorkflowFactory& operator=(IFabWorkflowFactory&&) = delete;

	virtual bool CanImportAssetType(const FString& InAssetType)
	{
		return GetImportAssetTypes().Contains(InAssetType);
	}

	virtual const TArray<FString>& GetImportAssetTypes() = 0;

	virtual TSharedPtr<IFabWorkflow> Create(const FFabAssetMetadata& InImportAssetMetadata, const FString& InDownloadUrl = "") = 0;
};
