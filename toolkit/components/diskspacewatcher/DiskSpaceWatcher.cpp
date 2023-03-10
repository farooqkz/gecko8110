/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "DiskSpaceWatcher.h"
#include "nsIObserverService.h"
#include "nsXULAppAPI.h"
#include "mozilla/Hal.h"
#include "mozilla/ModuleUtils.h"
#include "mozilla/Preferences.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/Services.h"

#define NS_DISKSPACEWATCHER_CID \
  { 0xab218518, 0xf197, 0x4fb4, { 0x8b, 0x0f, 0x8b, 0xb3, 0x4d, 0xf2, 0x4b, 0xf4 } }

using namespace mozilla;

#define WATCHER_PREF_FREE_SPACE_LOW "disk_space_watcher.free_space_low_threshold"
#define WATCHER_PREF_FREE_SPACE_HIGH "disk_space_watcher.free_space_high_threshold"

StaticRefPtr<DiskSpaceWatcher> gDiskSpaceWatcher;

NS_IMPL_ISUPPORTS(DiskSpaceWatcher, nsIDiskSpaceWatcher, nsIObserver)

uint64_t DiskSpaceWatcher::sFreeSpace = 0;
bool DiskSpaceWatcher::sIsDiskFull = false;
uint64_t DiskSpaceWatcher::sFreeSpaceLowThreshold = 0;
uint64_t DiskSpaceWatcher::sFreeSpaceHighThreshold = 0;

DiskSpaceWatcher::DiskSpaceWatcher()
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!gDiskSpaceWatcher);
  sFreeSpaceLowThreshold = Preferences::GetInt(WATCHER_PREF_FREE_SPACE_LOW, 50) * 1024 * 1024;
  sFreeSpaceHighThreshold = Preferences::GetInt(WATCHER_PREF_FREE_SPACE_HIGH, 52) * 1024 * 1024;
}

DiskSpaceWatcher::~DiskSpaceWatcher()
{
  MOZ_ASSERT(!gDiskSpaceWatcher);
}

already_AddRefed<DiskSpaceWatcher>
DiskSpaceWatcher::FactoryCreate()
{
  if (!XRE_IsParentProcess()) {
    return nullptr;
  }

  MOZ_ASSERT(NS_IsMainThread());

  if (!Preferences::GetBool("disk_space_watcher.enabled", false)) {
    return nullptr;
  }

  if (!gDiskSpaceWatcher) {
    gDiskSpaceWatcher = new DiskSpaceWatcher();
    ClearOnShutdown(&gDiskSpaceWatcher);
  }

  RefPtr<DiskSpaceWatcher> service = gDiskSpaceWatcher.get();
  return service.forget();
}

NS_IMETHODIMP
DiskSpaceWatcher::Observe(nsISupports* aSubject, const char* aTopic,
                          const char16_t* aData)
{
  MOZ_ASSERT(NS_IsMainThread());

  if (!strcmp(aTopic, "profile-after-change")) {
    nsCOMPtr<nsIObserverService> observerService =
      mozilla::services::GetObserverService();
    observerService->AddObserver(this, "profile-before-change", false);
    mozilla::hal::StartDiskSpaceWatcher();
    return NS_OK;
  }

  if (!strcmp(aTopic, "profile-before-change")) {
    nsCOMPtr<nsIObserverService> observerService =
      mozilla::services::GetObserverService();
    observerService->RemoveObserver(this, "profile-before-change");
    mozilla::hal::StopDiskSpaceWatcher();
    return NS_OK;
  }

  MOZ_ASSERT(false, "DiskSpaceWatcher got unexpected topic!");
  return NS_ERROR_UNEXPECTED;
}

NS_IMETHODIMP DiskSpaceWatcher::GetIsDiskFull(bool* aIsDiskFull)
{
  *aIsDiskFull = sIsDiskFull;
  return NS_OK;
}

// GetFreeSpace is a macro on windows, and that messes up with the c++
// compiler.
#ifdef XP_WIN
#undef GetFreeSpace
#endif
NS_IMETHODIMP DiskSpaceWatcher::GetFreeSpace(uint64_t* aFreeSpace)
{
  *aFreeSpace = sFreeSpace;
  return NS_OK;
}

// static
void DiskSpaceWatcher::UpdateState(bool aIsDiskFull, uint64_t aFreeSpace)
{
  MOZ_ASSERT(NS_IsMainThread());
  if (!gDiskSpaceWatcher) {
    return;
  }

  nsCOMPtr<nsIObserverService> observerService =
    mozilla::services::GetObserverService();

  sIsDiskFull = aIsDiskFull;
  sFreeSpace = aFreeSpace;

  if (!observerService) {
    return;
  }

  const char16_t stateFull[] = { 'f', 'u', 'l', 'l', 0 };
  const char16_t stateFree[] = { 'f', 'r', 'e', 'e', 0 };

  nsCOMPtr<nsISupports> subject;
  CallQueryInterface(gDiskSpaceWatcher.get(), getter_AddRefs(subject));
  MOZ_ASSERT(subject);
  observerService->NotifyObservers(subject,
                                   DISKSPACEWATCHER_OBSERVER_TOPIC,
                                   sIsDiskFull ? stateFull : stateFree);

  return;
}

// static
void DiskSpaceWatcher::UpdateFreeSpace(uint64_t aFreeSpace)
{
  MOZ_ASSERT(NS_IsMainThread());
  if (!gDiskSpaceWatcher) {
    return;
  }

  nsCOMPtr<nsIObserverService> observerService =
    mozilla::services::GetObserverService();

  if (!observerService) {
    return;
  }

  nsCOMPtr<nsISupports> subject;
  CallQueryInterface(gDiskSpaceWatcher.get(), getter_AddRefs(subject));
  MOZ_ASSERT(subject);
  if (aFreeSpace < sFreeSpaceLowThreshold) {
    observerService->NotifyObservers(subject,
                                     FREESPACELOW_OBSERVER_TOPIC,
                                     nullptr);
  }
  if (aFreeSpace >= sFreeSpaceHighThreshold) {
    observerService->NotifyObservers(subject,
                                     FREESPACEHIGH_OBSERVER_TOPIC,
                                     nullptr);
  }

  return;
}

NS_GENERIC_FACTORY_SINGLETON_CONSTRUCTOR(DiskSpaceWatcher,
                                         DiskSpaceWatcher::FactoryCreate)

NS_DEFINE_NAMED_CID(NS_DISKSPACEWATCHER_CID);

static const mozilla::Module::CIDEntry kDiskSpaceWatcherCIDs[] = {
  { &kNS_DISKSPACEWATCHER_CID, false, nullptr, DiskSpaceWatcherConstructor },
  { nullptr }
};

static const mozilla::Module::ContractIDEntry kDiskSpaceWatcherContracts[] = {
  { "@mozilla.org/toolkit/disk-space-watcher;1", &kNS_DISKSPACEWATCHER_CID },
  { nullptr }
};

static const mozilla::Module::CategoryEntry kDiskSpaceWatcherCategories[] = {
#ifdef MOZ_WIDGET_GONK
  { "profile-after-change", "Disk Space Watcher Service", DISKSPACEWATCHER_CONTRACTID },
#endif
  { nullptr }
};

static const mozilla::Module kDiskSpaceWatcherModule = {
  mozilla::Module::kVersion,
  kDiskSpaceWatcherCIDs,
  kDiskSpaceWatcherContracts,
  kDiskSpaceWatcherCategories
};

NSMODULE_DEFN(DiskSpaceWatcherModule) = &kDiskSpaceWatcherModule;
