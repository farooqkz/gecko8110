/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


[Exposed=Worker]
interface WorkerNavigator {
};

WorkerNavigator implements NavigatorID;
WorkerNavigator implements NavigatorLanguage;
WorkerNavigator implements NavigatorOnLine;
WorkerNavigator implements NavigatorDataStore;
WorkerNavigator implements NavigatorConcurrentHardware;

#ifdef HAS_KOOST_MODULES
[Exposed=(Worker)]
partial interface WorkerNavigator {
    [Throws, Func="WorkerNavigator::HasExternalAPISupport"]
    readonly attribute ExternalAPI externalapi;
};
#endif
