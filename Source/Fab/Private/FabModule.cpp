// Copyright Epic Games, Inc. All Rights Reserved.

#include "FabModule.h"

#include "Engine.h"

#include "FabActiveWorkflowRegistry.h"
#include "FabAuthentication.h"
#include "FabBrowser.h"
#include "FabDownloader.h"
#include "FabLog.h"
#include "FabSettingsCustomization.h"
#include "InterchangeManager.h"

#include "PropertyEditorModule.h"
#include "Engine/RendererSettings.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/ModuleManager.h"

#include "Pipelines/Factories/InterchangeInstancedFoliageTypeFactory.h"

#include "Runtime/Launch/Resources/Version.h"

#if (ENGINE_MAJOR_VERSION >=5 && ENGINE_MINOR_VERSION <=3)

#include "Settings/EditorExperimentalSettings.h"

#endif

DEFINE_LOG_CATEGORY(LogFab)

class FFabModule : public IFabModule
{
public:
	virtual void StartupModule() override
	{
		if (GIsEditor)
		{
			URendererSettings* RendererSettings                = GetMutableDefault<URendererSettings>();
			RendererSettings->bEnableVirtualTextureOpacityMask = true;
			RendererSettings->PostEditChange();

#if (ENGINE_MAJOR_VERSION >=5 && ENGINE_MINOR_VERSION <=3)
			{
				UEditorExperimentalSettings* EditorSettings    = GetMutableDefault<UEditorExperimentalSettings>();
				EditorSettings->bEnableAsyncTextureCompilation = false;
				EditorSettings->PostEditChange();
			}
#endif
		}
		
		if (GIsEditor && !IsRunningCommandlet())
		{
			// In UEFN, the Emporium plugin provides its own Fab UI and tab spawner. Skip registering
			// our entry points so Emporium's UI is the only one visible and all Fab invocations route
			// through Emporium's tab.
			const TSharedPtr<IPlugin> EmporiumPlugin = IPluginManager::Get().FindPlugin(TEXT("Emporium"));
			if (EmporiumPlugin.IsValid() && EmporiumPlugin->IsEnabled())
			{
				return;
			}

			FFabBrowser::Init();
			FabAuthentication::Init();

			FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
			PropertyModule.RegisterCustomClassLayout("FabSettings", FOnGetDetailCustomizationInstance::CreateStatic(&FFabSettingsCustomization::MakeInstance));

			auto RegisterItems = []()
			{
				UInterchangeManager::GetInterchangeManager().RegisterFactory(UInterchangeInstancedFoliageTypeFactory::StaticClass());
			};

			if (GEngine)
			{
				RegisterItems();
			}
			else
			{
				FCoreDelegates::GetOnPostEngineInit().AddLambda(RegisterItems);
			}

			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Fab"));

			if (Plugin.IsValid())
			{
				const FPluginDescriptor& Descriptor = Plugin->GetDescriptor();

				UE_LOGF(LogFab, Log,
					"Plugin initialized. VersionName: %ls (Version: %d) - Engine Version %ls",
					*Descriptor.VersionName,
					Descriptor.Version,
					*FEngineVersion::Current().ToString()
				);
			}
		}
	}

	virtual void ShutdownModule() override
	{
		// Ahead of the Emporium early-out below, which can return with workflows still in flight.
		FFabActiveWorkflowRegistry::Shutdown();

		if (GIsEditor && !IsRunningCommandlet())
		{
			const TSharedPtr<IPlugin> EmporiumPlugin = IPluginManager::Get().FindPlugin(TEXT("Emporium"));
			if (EmporiumPlugin.IsValid() && EmporiumPlugin->IsEnabled())
			{
				return;
			}

			if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
			{
				FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
				PropertyModule.UnregisterCustomClassLayout("FabSettings");
			}
			FabAuthentication::Shutdown();
			FFabBrowser::Shutdown();
			FFabDownloadRequest::ShutdownBpsModule();
		}
	}
};

IMPLEMENT_MODULE(FFabModule, Fab);
