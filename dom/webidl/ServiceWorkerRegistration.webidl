/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origin of this IDL file is
 * http://slightlyoff.github.io/ServiceWorker/spec/service_worker/index.html
 *
 */

[Func="mozilla::dom::ServiceWorkerRegistrationVisible",
 Exposed=(Window,Worker)]
interface ServiceWorkerRegistration : EventTarget {
  [Unforgeable] readonly attribute ServiceWorker? installing;
  [Unforgeable] readonly attribute ServiceWorker? waiting;
  [Unforgeable] readonly attribute ServiceWorker? active;

  readonly attribute USVString scope;

  [Throws, NewObject]
  Promise<void> update();

  [Throws, NewObject]
  Promise<boolean> unregister();

  // event
  attribute EventHandler onupdatefound;
};

partial interface ServiceWorkerRegistration {
#ifdef MOZ_WEBPUSH
  [Throws, Exposed=(Window,Worker), Func="nsContentUtils::PushEnabled"]
  readonly attribute PushManager pushManager;
#endif
};
