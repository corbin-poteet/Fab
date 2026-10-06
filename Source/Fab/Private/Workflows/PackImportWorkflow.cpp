// Copyright Epic Games, Inc. All Rights Reserved.

#include "PackImportWorkflow.h"

#include "FabDownloader.h"
#include "FabLog.h"
#include "NotificationProgressWidget.h"
#include "AssetRegistry/AssetRegistryModule.h"

#include "Framework/Docking/TabManager.h" 
#include "Framework/Notifications/NotificationManager.h"

#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"

#include "FabBrowser.h"
#include "FabBrowserApi.h"
#include "Containers/Ticker.h"
#include "FabSettings.h"
#include "IDetailsView.h"
#include "IPluginBrowser.h"
#include "IPluginWizardDefinition.h"
#include "IWebBrowserWindow.h"
#include "PluginUtils.h"
#include "UnrealEdMisc.h"

#include "Features/IPluginsEditorFeature.h"
#include "Misc/FileHelper.h"
#include "UObject/CoreRedirects.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Views/SListView.h"

#include "Utilities/AssetUtils.h"
#include "Utilities/FabLocalAssets.h"

#include "Widgets/Notifications/SNotificationList.h"

FPackImportWorkflow::FPackImportWorkflow(const FString& InAssetId, const FString& InAssetName, const FString& InManifestDownloadUrl, const FString& InBaseUrls)
	: IFabWorkflow(InAssetId, InAssetName, InManifestDownloadUrl)
	, BaseUrls(InBaseUrls)
{}

// Asks the Fab page the user clicked "Add to Project" on for the listing's seller and URL.
// Calls back with empty strings if the listing can't be found or the page doesn't answer within a few seconds.
static void FetchListingInfo(const FString& AssetId, const FString& AssetName, TFunction<void(const FString&, const FString&)> OnDone)
{
	FFabBrowserTabContext* Context = FFabBrowser::GetActiveDockTabContext();
	UFabBrowserApi* Api = Context ? Context->JavascriptApi.Get() : nullptr;
	if (!Api || !Context->WebBrowserWindow.IsValid())
	{
		OnDone(FString(), FString());
		return;
	}

	// Whichever of the page's reply or the timeout comes first wins
	const TSharedRef<bool> bDone = MakeShared<bool>(false);
	const TFunction<void(const FString&, const FString&)> Finish = [bDone, OnDone](const FString& Seller, const FString& Url)
	{
		if (!*bDone)
		{
			*bDone = true;
			OnDone(Seller, Url);
		}
	};
	Api->OnListingInfo = [Finish](const FString& Seller, const FString& Url, const FString& Note)
	{
		UE_LOGF(LogFab, Display, "Listing info: seller='%ls' url='%ls' (%ls)", *Seller, *Url, *Note);
		Finish(Seller, Url);
	};
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Finish](float)
	{
		Finish(FString(), FString());
		return false;
	}), 5.0f);

	auto JsString = [](const FString& Value)
	{
		return TEXT("'") + Value.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("'"), TEXT("\'")).Replace(TEXT("\n"), TEXT(" ")) + TEXT("'");
	};

	// Finds the listing from, in order: the page URL (listing pages), the asset id itself, or the link next to the
	// asset's title on the page (My Library). The page's own fetch carries its cookies, so it gets past the bot
	// protection that blocks requests from C++. Falls back to the seller link on the page for the seller name.
	Context->WebBrowserWindow->ExecuteJavascript(FString::Printf(TEXT(R"JS(
(async (assetId, assetName) => {
	let seller = '', url = '', note = '', container = document;
	const uidOf = href => { const m = /\/listings\/([0-9a-f-]{36})/i.exec(href || ''); return m ? m[1] : ''; };
	const fetchListing = async uid => {
		try {
			const response = await fetch('/i/listings/' + uid, { credentials: 'include' });
			note += 'fetch ' + uid + ' = ' + response.status + '; ';
			return response.ok ? await response.json() : null;
		} catch (e) { note += 'fetch failed: ' + e + '; '; return null; }
	};
	try {
		let uid = uidOf(location.pathname);
		let listing = null;
		if (uid) {
			note += 'from page url; ';
		} else if ((listing = await fetchListing(assetId))) {
			uid = assetId;
			note += 'asset id is the listing id; ';
		} else {
			for (const el of document.querySelectorAll('body *')) {
				if (el.children.length || el.textContent.trim() !== assetName) continue;
				let parent = el;
				for (let depth = 0; parent && depth < 8 && !uid; ++depth, parent = parent.parentElement) {
					const link = parent.querySelector('a[href*="/listings/"]');
					if (link) { uid = uidOf(link.getAttribute('href')); container = parent; }
				}
				if (uid) { note += 'from link near title; '; break; }
			}
		}
		if (uid) {
			url = location.origin + '/listings/' + uid;
			listing = listing || await fetchListing(uid);
			seller = (listing && listing.user && (listing.user.sellerName || listing.user.displayName)) || '';
			if (!seller) {
				const link = container.querySelector('a[href*="/sellers/"]');
				seller = link ? link.textContent.trim() : '';
			}
		} else {
			note += 'not found on ' + location.pathname + ' (' + document.querySelectorAll('a[href*="/listings/"]').length + ' listing links); ';
		}
	} catch (e) {
		note += 'error: ' + e;
	} finally {
		window.ue.fab.receivelistinginfo(seller, url, note);
	}
})(%s, %s);
)JS"), *JsString(AssetId), *JsString(AssetName)));
}

// Wraps one of the editor's own plugin templates so the wizard opens pre-filled for the Fab pack
class FFabPackPluginTemplate : public FPluginTemplateDescription
{
public:
	FFabPackPluginTemplate(const TSharedRef<FPluginTemplateDescription>& InInner, const FString& InDefaultName, const FString& InDefaultPath, const FString& InFriendlyName,
		const TSharedRef<bool>& InDefaultsApplied, const TFunction<void(TSharedPtr<IPlugin>)>& InOnCreated)
		: FPluginTemplateDescription(InInner->Name, InInner->Description, InInner->OnDiskPath, InInner->bCanContainContent, InInner->ModuleDescriptorType,
			InInner->LoadingPhase, InInner->bSupportsContentOnlyProjects, InInner->PostCreatePythonScriptPath, InInner->PostCreatePythonScriptArguments)
		, Inner(InInner)
		, DefaultName(InDefaultName)
		, DefaultPath(InDefaultPath)
		, FriendlyName(InFriendlyName)
		, bDefaultsApplied(InDefaultsApplied)
		, OnCreated(InOnCreated)
	{
		SortPriority         = Inner->SortPriority;
		bCanBePlacedInEngine = false;
	}

	virtual bool ValidatePathForPlugin(const FString& ProposedAbsolutePluginPath, FText& OutErrorMessage) override { return Inner->ValidatePathForPlugin(ProposedAbsolutePluginPath, OutErrorMessage); }
	virtual void UpdatePathWhenTemplateUnselected(FString& InOutPath) override { Inner->UpdatePathWhenTemplateUnselected(InOutPath); }
	virtual void UpdatePluginNameTextWhenTemplateUnselected(FText& OutPluginNameText) override { Inner->UpdatePluginNameTextWhenTemplateUnselected(OutPluginNameText); }

	// Defaults are applied on the first selection only, so switching templates keeps what the user typed
	virtual void UpdatePathWhenTemplateSelected(FString& InOutPath) override
	{
		Inner->UpdatePathWhenTemplateSelected(InOutPath);
		if (!*bDefaultsApplied)
		{
			InOutPath = DefaultPath;
		}
	}
	virtual void UpdatePluginNameTextWhenTemplateSelected(FText& OutPluginNameText) override
	{
		Inner->UpdatePluginNameTextWhenTemplateSelected(OutPluginNameText);
		if (!*bDefaultsApplied)
		{
			OutPluginNameText = FText::FromString(DefaultName);
			*bDefaultsApplied = true; // Called after UpdatePathWhenTemplateSelected
		}
	}

	virtual void CustomizeDescriptorBeforeCreation(FPluginDescriptor& Descriptor) override
	{
		Inner->CustomizeDescriptorBeforeCreation(Descriptor);
		Descriptor.FriendlyName       = FriendlyName;
		Descriptor.bCanContainContent = true; // The pack goes in Content, whichever template was picked
	}
	virtual void OnPluginCreated(TSharedPtr<IPlugin> NewPlugin) override
	{
		Inner->OnPluginCreated(NewPlugin);
		OnCreated(NewPlugin);
	}

	const TSharedRef<FPluginTemplateDescription> Inner;

private:
	FString DefaultName;
	FString DefaultPath;
	FString FriendlyName;
	TSharedRef<bool> bDefaultsApplied;
	TFunction<void(TSharedPtr<IPlugin>)> OnCreated;
};

// Same behaviour as the editor's default wizard definition (which isn't exported), over the wrapped templates
class FFabPackWizardDefinition : public IPluginWizardDefinition
{
public:
	explicit FFabPackWizardDefinition(TArray<TSharedRef<FPluginTemplateDescription>>&& InTemplates) : Templates(MoveTemp(InTemplates)) {}

	virtual const TArray<TSharedRef<FPluginTemplateDescription>>& GetTemplatesSource() const override { return Templates; }
	virtual void OnTemplateSelectionChanged(TSharedPtr<FPluginTemplateDescription> InSelectedItem, ESelectInfo::Type) override { Selected = InSelectedItem; }
	virtual bool HasValidTemplateSelection() const override { return Selected.IsValid(); }
	virtual TSharedPtr<FPluginTemplateDescription> GetSelectedTemplate() const override { return Selected; }
	virtual void ClearTemplateSelection() override { Selected.Reset(); }
	virtual bool CanShowOnStartup() const override { return false; }
	virtual bool HasModules() const override { return Selected.IsValid() && FPaths::DirectoryExists(Selected->OnDiskPath / TEXT("Source")); }
	virtual bool IsMod() const override { return false; }
	virtual void OnShowOnStartupCheckboxChanged(ECheckBoxState) override {}
	virtual ECheckBoxState GetShowOnStartupCheckBoxState() const override { return ECheckBoxState::Unchecked; }
	virtual TSharedPtr<SWidget> GetCustomHeaderWidget() override { return nullptr; }
	virtual FText GetInstructions() const override { return NSLOCTEXT("Fab", "PackWizardInstructions", "Choose a template and then specify a name for the plugin that this Fab pack will be added to."); }
	virtual bool GetPluginIconPath(FString& OutIconPath) const override { return Selected.IsValid() && GetTemplateIconPath(Selected.ToSharedRef(), OutIconPath); }
	virtual EHostType::Type GetPluginModuleDescriptor() const override { return Selected.IsValid() ? Selected->ModuleDescriptorType : EHostType::Runtime; }
	virtual ELoadingPhase::Type GetPluginLoadingPhase() const override { return Selected.IsValid() ? Selected->LoadingPhase : ELoadingPhase::Default; }
	virtual bool GetTemplateIconPath(TSharedRef<FPluginTemplateDescription> Template, FString& OutIconPath) const override
	{
		OutIconPath = Template->OnDiskPath / TEXT("Resources/Icon128.png");
		if (FPaths::FileExists(OutIconPath))
		{
			return false;
		}
		OutIconPath = IPluginManager::Get().FindPlugin(TEXT("PluginBrowser"))->GetBaseDir() / TEXT("Resources/DefaultIcon128.png");
		return true;
	}
	virtual FString GetPluginFolderPath() const override { return Selected.IsValid() ? Selected->OnDiskPath : FString(); }
	virtual TArray<FString> GetFoldersForSelection() const override { return Selected.IsValid() ? TArray<FString>{ Selected->OnDiskPath } : TArray<FString>(); }
	virtual void PluginCreated(const FString&, bool) const override {}

private:
	TArray<TSharedRef<FPluginTemplateDescription>> Templates;
	TSharedPtr<FPluginTemplateDescription> Selected;
};

static void ForEachWidget(const TSharedRef<SWidget>& Widget, TFunctionRef<void(const TSharedRef<SWidget>&)> Visit)
{
	Visit(Widget);
	FChildren* Children = Widget->GetChildren();
	for (int32 Index = 0; Index < Children->Num(); ++Index)
	{
		ForEachWidget(Children->GetChildAt(Index), Visit);
	}
}

// The wizard keeps its template list and descriptor fields private, so reach them through its widgets:
// select the default template (which fills in the name and path) and pre-fill Author / Author URL.
static void PrefillNewPluginWizard(const TSharedRef<SWidget>& Wizard, const TSharedPtr<FPluginTemplateDescription>& DefaultTemplate, const FString& Seller, const FString& ListingUrl)
{
	ForEachWidget(Wizard, [&](const TSharedRef<SWidget>& Widget)
	{
		const FString Type = Widget->GetTypeAsString();
		if (Type.Contains(TEXT("FPluginTemplateDescription")) && DefaultTemplate.IsValid())
		{
			StaticCastSharedRef<SListView<TSharedRef<FPluginTemplateDescription>>>(Widget)->SetSelection(DefaultTemplate.ToSharedRef());
		}
		else if (Type == TEXT("SDetailsView"))
		{
			const TSharedRef<IDetailsView> DetailsView = StaticCastSharedRef<IDetailsView>(Widget);
			for (const TWeakObjectPtr<UObject>& Object : DetailsView->GetSelectedObjects())
			{
				if (UObject* Descriptor = Object.Get(); Descriptor && Descriptor->GetClass()->GetName() == TEXT("NewPluginDescriptorData"))
				{
					if (FStrProperty* CreatedBy = FindFProperty<FStrProperty>(Descriptor->GetClass(), TEXT("CreatedBy")))
					{
						CreatedBy->SetPropertyValue_InContainer(Descriptor, Seller);
					}
					if (FStrProperty* CreatedByURL = FindFProperty<FStrProperty>(Descriptor->GetClass(), TEXT("CreatedByURL")))
					{
						CreatedByURL->SetPropertyValue_InContainer(Descriptor, ListingUrl);
					}
					DetailsView->ForceRefresh();
				}
			}
		}
	});
}

void FPackImportWorkflow::Execute()
{
	if (!GetDefault<UFabSettings>()->bImportPacksAsPlugins)
	{
		DownloadContent();
		return;
	}

	TSharedRef<FPackImportWorkflow> Self = StaticCastSharedRef<FPackImportWorkflow>(AsShared());
	FetchListingInfo(AssetId, AssetName, [Self](const FString& Seller, const FString& ListingUrl)
	{
		Self->OpenNewPluginWizard(Seller, ListingUrl);
	});
}

void FPackImportWorkflow::OpenNewPluginWizard(const FString& Seller, const FString& ListingUrl)
{
	static const FName TabName(TEXT("FabNewPlugin"));

	FString DefaultName;
	for (const TCHAR C : AssetName)
	{
		if (FChar::IsAlnum(C) || C == TEXT('_'))
		{
			DefaultName.AppendChar(C);
		}
	}
	FString DefaultPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectPluginsDir() / TEXT("Marketplace"));
	FPaths::MakePlatformFilename(DefaultPath);

	TSharedRef<FPackImportWorkflow> Self = StaticCastSharedRef<FPackImportWorkflow>(AsShared());
	const TFunction<void(TSharedPtr<IPlugin>)> OnCreated = [Self](TSharedPtr<IPlugin> Plugin)
	{
		Self->PluginName = Plugin->GetName();
		Self->InstallDir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
		Self->DownloadContent();
	};

	// The same templates the editor's own New Plugin wizard lists, in the same order
	IPluginBrowser& PluginBrowser = IPluginBrowser::Get();
	TArray<TSharedRef<FPluginTemplateDescription>> SourceTemplates = PluginBrowser.GetDefaultPluginTemplates();
	SourceTemplates.Append(PluginBrowser.GetAddedPluginTemplates());

	const TSharedRef<bool> bDefaultsApplied = MakeShared<bool>(false);
	TArray<TSharedRef<FPluginTemplateDescription>> Templates;
	TSharedPtr<FPluginTemplateDescription> ContentOnlyTemplate;
	for (const TSharedRef<FPluginTemplateDescription>& Source : SourceTemplates)
	{
		const TSharedRef<FPluginTemplateDescription> Template = MakeShared<FFabPackPluginTemplate>(Source, DefaultName, DefaultPath, AssetName, bDefaultsApplied, OnCreated);
		Templates.Add(Template);
		if (FPaths::GetCleanFilename(Source->OnDiskPath) == TEXT("ContentOnly"))
		{
			ContentOnlyTemplate = Template;
		}
	}
	Templates.Sort([](const TSharedRef<FPluginTemplateDescription>& A, const TSharedRef<FPluginTemplateDescription>& B)
	{
		return A->SortPriority != B->SortPriority ? A->SortPriority > B->SortPriority : A->Name.CompareTo(B->Name) <= 0;
	});
	const TSharedRef<FFabPackWizardDefinition> Definition = MakeShared<FFabPackWizardDefinition>(MoveTemp(Templates));

	// Nomad tabs are single-instance; a wizard left open from an earlier pack gets closed (cancelling that import)
	const TSharedRef<FGlobalTabmanager> TabManager = FGlobalTabmanager::Get();
	if (const TSharedPtr<SDockTab> ExistingTab = TabManager->FindExistingLiveTab(FTabId(TabName)))
	{
		ExistingTab->RequestCloseTab();
	}
	if (TabManager->HasTabSpawner(TabName))
	{
		TabManager->UnregisterNomadTabSpawner(TabName);
	}
	TabManager->RegisterNomadTabSpawner(TabName, FOnSpawnTab::CreateLambda([Definition](const FSpawnTabArgs& Args)
		{
			return IPluginBrowser::Get().SpawnPluginCreatorTab(Args, Definition);
		}))
		.SetDisplayName(NSLOCTEXT("Fab", "NewPluginTab", "New Plugin"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
	FTabManager::RegisterDefaultTabWindowSize(TabName, FVector2D(1000.0f, 750.0f));

	const TSharedPtr<SDockTab> Tab = TabManager->TryInvokeTab(FTabId(TabName));
	if (!Tab.IsValid())
	{
		CancelWorkflow();
		return;
	}

	// Closing the wizard without creating the plugin cancels the import
	Tab->SetOnTabClosed(SDockTab::FOnTabClosedCallback::CreateLambda([Self](TSharedRef<SDockTab>)
	{
		if (Self->PluginName.IsEmpty())
		{
			Self->CancelWorkflow();
		}
	}));

	PrefillNewPluginWizard(Tab->GetContent(), ContentOnlyTemplate, Seller, ListingUrl);
}

// Pack assets reference each other by their original /Game paths, so redirect those to the plugin.
// Registered now for this session and appended to DefaultEngine.ini for later ones.
static void AddPackageRedirect(const FString& OldPath, const FString& NewPath)
{
	const FCoreRedirect Redirect(ECoreRedirectFlags::Type_Package | ECoreRedirectFlags::Option_MatchSubstring, OldPath, NewPath);
	FCoreRedirects::AddRedirectList(MakeArrayView(&Redirect, 1), TEXT("Fab"));

	const FString IniPath = FPaths::ProjectConfigDir() / TEXT("DefaultEngine.ini");
	const FString Line    = FString::Printf(TEXT("+PackageRedirects=(OldName=\"%s\",NewName=\"%s\",MatchSubstring=true)"), *OldPath, *NewPath);

	FString IniContents;
	FFileHelper::LoadFileToString(IniContents, *IniPath);
	if (!IniContents.Contains(Line))
	{
		FFileHelper::SaveStringToFile(TEXT("\n[CoreRedirects]\n") + Line + TEXT("\n"), *IniPath, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
	}
}

void FPackImportWorkflow::DownloadContent()
{
	const FString DownloadURL = DownloadUrl + ',' + BaseUrls;
	if (InstallDir.IsEmpty())
	{
		InstallDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	}

	DownloadRequest = MakeShared<FFabDownloadRequest>(AssetId, DownloadURL, InstallDir, EFabDownloadType::BuildPatchRequest);
	DownloadRequest->OnDownloadComplete().AddSP(this, &FPackImportWorkflow::OnContentDownloadComplete);
	DownloadRequest->OnDownloadProgress().AddSP(this, &FPackImportWorkflow::OnContentDownloadProgress);
	DownloadRequest->ExecuteRequest();

	CreateDownloadNotification();
}

void FPackImportWorkflow::OnContentDownloadProgress(const FFabDownloadRequest* Request, const FFabDownloadStats& DownloadStats)
{
	SetDownloadNotificationProgress(DownloadStats.PercentComplete);
}

TSharedPtr<IPlugin> FindPluginOwningModule(const FString& ModuleName)
{
	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
	{
		const FPluginDescriptor& Desc = Plugin->GetDescriptor();
		for (const FModuleDescriptor& Mod : Desc.Modules)
		{
			if (Mod.Name.ToString().Equals(ModuleName, ESearchCase::IgnoreCase))
			{
				return Plugin;
			}
		}
	}

	return nullptr;
}

void PromptToEnablePlugins(const TArray<FString>& PluginNames, const TMap<FString, TArray<FString>>& PluginToAffectedAssets)
{
	if (PluginNames.IsEmpty())
	{
		return;
	}

	FString PluginList;
	for (const FString& PluginName : PluginNames)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
		PluginList += (Plugin.IsValid() ? Plugin->GetFriendlyName() : PluginName) + TEXT("\n");
	}

	FString AssetList;
	for (const FString& PluginName : PluginNames)
	{
		if (const TArray<FString>* Assets = PluginToAffectedAssets.Find(PluginName))
		{
			for (const FString& AssetPath : *Assets)
			{
				AssetList += AssetPath + TEXT("\n");
			}
		}
	}

	const FText TitleText = NSLOCTEXT("Fab", "MissingPluginsTitle", "Missing Plugins!");
	const FText SubText = FText::Format(
		NSLOCTEXT("Fab", "MissingPluginsSubText", "Needed Plugins:\n{0}\nAffected Assets:\n{1}\nThe imported assets need the plugins listed above. Related assets may not display properly.\nAttempting to save these assets may result in irreversible modification due to missing plugins."),
		FText::AsCultureInvariant(PluginList),
		FText::AsCultureInvariant(AssetList));

	TSharedPtr<TWeakPtr<SNotificationItem>> NotificationWeak = MakeShared<TWeakPtr<SNotificationItem>>();

	FNotificationInfo Info(TitleText);
	Info.bFireAndForget = false;
	Info.FadeOutDuration = 0.0f;
	Info.ExpireDuration = 0.0f;
	Info.WidthOverride = FOptionalSize();
	Info.SubText = SubText;

	Info.HyperlinkText = NSLOCTEXT("Fab", "OpenPluginBrowser", "Open Plugin Browser");
	Info.Hyperlink = FSimpleDelegate::CreateLambda([]()
	{
		FGlobalTabmanager::Get()->TryInvokeTab(FName("PluginsEditor"));
	});

	Info.ButtonDetails.Add(FNotificationButtonInfo(
		NSLOCTEXT("Fab", "EnableMissingAndRestart", "Enable Missing and Restart"),
		NSLOCTEXT("Fab", "EnableMissingAndRestartTT", "Enable missing plugins and restart the editor"),
		FSimpleDelegate::CreateLambda([PluginNames, NotificationWeak]()
		{
			TArray<FString> EnabledPlugins;
			bool bEnableSuccess = true;
			for (const FString& PluginName : PluginNames)
			{
				FText FailReason;
				if (!IProjectManager::Get().SetPluginEnabled(PluginName, true, FailReason))
				{
					UE_LOGF(LogFab, Error, "Failed to enable plugin '%ls': %ls", *PluginName, *FailReason.ToString());
					bEnableSuccess = false;
					break;
				}
				EnabledPlugins.Add(PluginName);
			}

			if (!bEnableSuccess)
			{
				// Rollback successfully enabled plugins to restore consistent state
				for (int32 i = EnabledPlugins.Num() - 1; i >= 0; --i)
				{
					FText RollbackReason;
					if (!IProjectManager::Get().SetPluginEnabled(EnabledPlugins[i], false, RollbackReason))
					{
						UE_LOGF(LogFab, Error, "Failed to rollback plugin '%ls': %ls", *EnabledPlugins[i], *RollbackReason.ToString());
					}
				}

				if (const TSharedPtr<SNotificationItem> Notification = NotificationWeak->Pin())
				{
					Notification->SetCompletionState(SNotificationItem::CS_Fail);
					Notification->ExpireAndFadeout();
				}
				return;
			}

			if (IProjectManager::Get().IsCurrentProjectDirty())
			{
				FText FailReason;
				if (!IProjectManager::Get().SaveCurrentProjectToDisk(FailReason))
				{
					UE_LOGF(LogFab, Error, "Failed to save project: %ls. Restarting to reconcile state.", *FailReason.ToString());
				}
			}

			if (const TSharedPtr<SNotificationItem> Notification = NotificationWeak->Pin())
			{
				Notification->SetCompletionState(SNotificationItem::CS_Success);
				Notification->ExpireAndFadeout();
			}

			FUnrealEdMisc::Get().RestartEditor(false);
		}),
		SNotificationItem::CS_None
	));

	Info.ButtonDetails.Add(FNotificationButtonInfo(
		NSLOCTEXT("Fab", "DismissPluginPrompt", "Dismiss"),
		NSLOCTEXT("Fab", "DismissPluginPromptTT", "Dismiss this notification"),
		FSimpleDelegate::CreateLambda([NotificationWeak]()
		{
			if (const TSharedPtr<SNotificationItem> Notification = NotificationWeak->Pin())
			{
				Notification->SetCompletionState(SNotificationItem::CS_None);
				Notification->ExpireAndFadeout();
			}
		}),
		SNotificationItem::CS_None
	));

	TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
	*NotificationWeak = Notification;

	if (Notification.IsValid())
	{
		Notification->SetCompletionState(SNotificationItem::CS_None);
	}
}

void CheckForDependencies(const TArray<FString>& ImportedFiles)
{
	if (ImportedFiles.Num() == 0)
	{
		UE_LOGF(LogFab, Warning, "No files imported");
		return;
	}

	// Gather all .uasset files
	TArray<FString> FoundFiles;
	for (const FString& FullPath : ImportedFiles)
	{
		if (FullPath.EndsWith(TEXT(".uasset")))
		{
			FoundFiles.AddUnique(FullPath);
			UE_LOGF(LogFab, Log, "Found %ls", *FullPath);
		}
	}

	// Load Asset Registry
	const FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	const IAssetRegistry& Registry = AssetRegistryModule.Get();

	UE_LOGF(LogFab, Display, "Scanning %d assets for plugin dependencies...\n", FoundFiles.Num());

	TArray<FString> PluginsToEnable;
	TMap<FString, TArray<FString>> PluginToAffectedAssets;

	for (const FString& FilePath : FoundFiles)
	{
		FString LongPackageName;
		if (!FPackageName::TryConvertFilenameToLongPackageName(FilePath, LongPackageName))
		{
			UE_LOGF(LogFab, Warning, "Failed to convert to package name: %ls", *FilePath);
			continue;
		}

		const FName PackageName(*LongPackageName);

		TArray<FAssetData> Assets;
		Registry.GetAssetsByPackageName(PackageName, Assets);

		for (const FAssetData& AssetData : Assets)
		{
			const FString ClassPath = AssetData.AssetClassPath.ToString();
			if (!ClassPath.StartsWith(TEXT("/Script/")))
			{
				continue;
			}

			// Extract module name from /Script/Module.Class
			FString Remainder = ClassPath.RightChop(8); // remove "/Script/"

			FString ModuleName;
			Remainder.Split(TEXT("."), &ModuleName, nullptr);

			if (FModuleManager::Get().IsModuleLoaded(*ModuleName))
			{
				continue;
			}

			const TSharedPtr<IPlugin> Plugin = FindPluginOwningModule(ModuleName);

			if (!Plugin.IsValid())
			{
				continue;
			}

			if (Plugin->IsEnabled())
			{
				// Already enabled — nothing to do
				continue;
			}

			const FString PluginName = Plugin->GetName();
			PluginsToEnable.AddUnique(PluginName);
			PluginToAffectedAssets.FindOrAdd(PluginName).AddUnique(LongPackageName);
		}
	}

	if (PluginsToEnable.Num() > 0)
	{
		PromptToEnablePlugins(PluginsToEnable, PluginToAffectedAssets);
	}
}

void FPackImportWorkflow::OnContentDownloadComplete(const FFabDownloadRequest* Request, const FFabDownloadStats& DownloadStats)
{
	if (!DownloadStats.bIsSuccess || DownloadStats.DownloadedFiles.IsEmpty())
	{
		ExpireDownloadNotification(false);
		CancelWorkflow();
		return;
	}

	ExpireDownloadNotification(true);

	TArray<FString> PathParts;
	DownloadStats.DownloadedFiles[0].ParseIntoArray(PathParts, TEXT("/"));

	if (PluginName.IsEmpty())
	{
		if (PathParts.Num() >= 2)
		{
			ImportLocation = "/Game" / PathParts[1];
		}
	}
	else
	{
		// Redirect every top-level Content folder the pack shipped. World Partition's __ExternalActors__ etc. are shared
		// with the project, so redirect their pack subfolder rather than the whole thing.
		TSet<FString> PackFolders;
		for (const FString& File : DownloadStats.DownloadedFiles)
		{
			TArray<FString> Parts;
			File.ParseIntoArray(Parts, TEXT("/"));
			if (Parts.Num() >= 3 && Parts[0] == TEXT("Content"))
			{
				PackFolders.Add(Parts[1].StartsWith(TEXT("__")) && Parts.Num() >= 4 ? Parts[1] / Parts[2] : Parts[1]);
			}
		}
		for (const FString& Folder : PackFolders)
		{
			AddPackageRedirect("/Game/" + Folder + "/", "/" + PluginName + "/" + Folder + "/");
		}
		ImportLocation = PathParts.Num() >= 2 ? "/" + PluginName / PathParts[1] : "/" + PluginName;
	}

	if (!ImportLocation.IsEmpty())
	{
		UFabLocalAssets::AddLocalAsset(ImportLocation, AssetId);
		FAssetUtils::ScanForAssets(ImportLocation);
		FAssetUtils::SyncContentBrowserToFolder(ImportLocation);
	}

	// Check for dependencies
	TArray<FString> InstalledFiles;
	for (const FString& File : DownloadStats.DownloadedFiles)
	{
		InstalledFiles.Add(FPaths::ConvertRelativePathToFull(InstallDir / File));
	}
	CheckForDependencies(InstalledFiles);

	CompleteWorkflow();
}

void FPackImportWorkflow::CreateDownloadNotification()
{
	// Create the notification info
	FNotificationInfo Info(FText::FromString("Downloading..."));

	TWeakPtr<FFabDownloadRequest> WeakRequest = DownloadRequest;
	ProgressWidget = SNew(SNotificationProgressWidget)
		.ProgressText(FText::FromString("Downloading " + AssetName))
		.HasButton(true)
		.ButtonText(FText::FromString("Cancel"))
		.ButtonToolTip(FText::FromString("Cancel Pack Import"))
		.OnButtonClicked(
			FOnClicked::CreateLambda(
				[WeakRequest]()
				{
					FAB_LOG("Import Cancelled");
					if (TSharedPtr<FFabDownloadRequest> PinnedRequest = WeakRequest.Pin())
					{
						PinnedRequest->Cancel();
					}
					return FReply::Handled();
				}
			)
		);

	// Set up the notification properties
	Info.bFireAndForget                   = false; // We want to control when it disappears
	Info.FadeOutDuration                  = 1.0f;  // Duration of the fade-out
	Info.ExpireDuration                   = 0.0f;  // How long it stays on the screen
	Info.bUseThrobber                     = true;  // Adds a spinning throbber to the notification
	Info.bUseSuccessFailIcons             = true;  // Adds success/failure icons
	Info.bAllowThrottleWhenFrameRateIsLow = false; // Ensures it updates even if the frame rate is low
	Info.bUseLargeFont                    = false; // Uses the default font size
	Info.ContentWidget                    = ProgressWidget;

	DownloadProgressNotification = FSlateNotificationManager::Get().AddNotification(Info);

	if (DownloadProgressNotification.IsValid() && ProgressWidget)
	{
		DownloadProgressNotification->SetCompletionState(SNotificationItem::CS_Pending);
	}
}
