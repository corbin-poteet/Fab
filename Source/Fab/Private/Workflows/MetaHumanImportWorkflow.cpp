// Copyright Epic Games, Inc. All Rights Reserved.

#include "MetaHumanImportWorkflow.h"

#include "FabLog.h"
#include "IAssetTools.h"

#include "Engine/StaticMesh.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"

#include "Misc/MessageDialog.h"

#include "Utilities/AssetUtils.h"
#include "Utilities/FabAssetsCache.h"

FMetaHumanImportWorkflow::FMetaHumanImportWorkflow(const FString& InAssetId, const FString& InAssetName, const FString& InDownloadURL)
	: IFabWorkflow(InAssetId, InAssetName, InDownloadURL)
{}

void FMetaHumanImportWorkflow::Execute()
{
	DownloadContent();
}

void FMetaHumanImportWorkflow::DownloadContent()
{
	CreateDownloadNotification();

	const FString DownloadLocation = FFabAssetsCache::GetCacheLocation() / AssetId;

	DownloadRequest = MakeShared<FFabDownloadRequest>(AssetId, DownloadUrl, DownloadLocation, EFabDownloadType::HTTP);
	DownloadRequest->OnDownloadProgress().AddSP(this, &FMetaHumanImportWorkflow::OnContentDownloadProgress);
	DownloadRequest->OnDownloadComplete().AddSP(this, &FMetaHumanImportWorkflow::OnContentDownloadComplete);
	DownloadRequest->ExecuteRequest();
}

void FMetaHumanImportWorkflow::OnContentDownloadProgress(const FFabDownloadRequest* Request, const FFabDownloadStats& DownloadStats)
{
	SetDownloadNotificationProgress(DownloadStats.PercentComplete);
}

void FMetaHumanImportWorkflow::OnContentDownloadComplete(const FFabDownloadRequest* Request, const FFabDownloadStats& DownloadStats)
{
	if (!DownloadStats.bIsSuccess)
	{
		FAB_LOG_ERROR("Failed to download FAB Asset %s", *AssetName);
		ExpireDownloadNotification(false);
		CancelWorkflow();
		return;
	}
	ExpireDownloadNotification(true);

	ImportContent(DownloadStats.DownloadedFiles);
}

void FMetaHumanImportWorkflow::ImportContent(const TArray<FString>& ImportFiles)
{
	const TArray<UObject*> ImportedAssets = IAssetTools::Get().ImportAssets(ImportFiles, "/Game/Fab/MetaHuman");
	if (ImportedAssets.Num())
	{
		CompleteWorkflow();
	}
	else
	{
		CancelWorkflow();
	}
}
