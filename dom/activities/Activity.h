/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_activities_Activity_h
#define mozilla_dom_activities_Activity_h

#include "DOMRequest.h"
#include "mozilla/dom/BindingDeclarations.h"
#include "mozilla/dom/MozActivityBinding.h"
#include "nsIActivityProxy.h"
#include "mozilla/Preferences.h"
#include "nsPIDOMWindow.h"

namespace mozilla {
namespace dom {

class Activity : public DOMRequest
{
public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS_INHERITED(Activity, DOMRequest)

  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

  static already_AddRefed<Activity>
  Constructor(const GlobalObject& aOwner,
              const ActivityOptions& aOptions,
              ErrorResult& aRv)
  {
    nsCOMPtr<nsPIDOMWindowInner> window = do_QueryInterface(aOwner.GetAsSupports());
    if (!window) {
      aRv.Throw(NS_ERROR_UNEXPECTED);
      return nullptr;
    }

    RefPtr<Activity> activity = new Activity(window);
    aRv = activity->Initialize(window, aOwner.Context(), aOptions);
    return activity.forget();
  }

  explicit Activity(nsPIDOMWindowInner* aWindow);

  void Cancel();

protected:
  nsresult Initialize(nsPIDOMWindowInner* aWindow,
                      JSContext* aCx,
                      const ActivityOptions& aOptions);

  nsCOMPtr<nsIActivityProxy> mProxy;

  ~Activity();
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_activities_Activity_h
