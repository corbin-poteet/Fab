// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AssetRegistry/AssetData.h"

#include "Workflows/FabWorkflow.h"

class SNotificationItem;
class SNotificationProgressWidget;

class FPackImportWorkflow : public IFabWorkflow
{
public:
	FPackImportWorkflow(const FString& InAssetId, const FString& InAssetName, const FString& InManifestDownloadUrl, const FString& InBaseUrls);

	virtual void Execute() override;

protected:
	virtual void DownloadContent() override;

	virtual void OnContentDownloadProgress(const FFabDownloadRequest* Request, const FFabDownloadStats& DownloadStats) override;
	virtual void OnContentDownloadComplete(const FFabDownloadRequest* Request, const FFabDownloadStats& DownloadStats) override;

	virtual void CreateDownloadNotification() override;

private:
	void OpenNewPluginWizard(const FString& Seller, const FString& ListingUrl);

	FString BaseUrls;
	FString InstallDir;
	FString PluginName;       // Empty when importing into /Game

	TSharedPtr<FFabDownloadRequest> DownloadRequest;
};
