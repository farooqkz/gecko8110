/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const {classes: Cc, interfaces: Ci, utils: Cu} = Components;

Cu.import("resource://gre/modules/Services.jsm");
Cu.import("resource://gre/modules/KeyValueParser.jsm");
Cu.import("resource://gre/modules/XPCOMUtils.jsm");
Cu.import("resource://gre/modules/DeviceUtils.jsm");
Cu.importGlobalProperties(['File']);

XPCOMUtils.defineLazyModuleGetter(this, "PromiseUtils",
                                  "resource://gre/modules/PromiseUtils.jsm");

this.EXPORTED_SYMBOLS = [
  "CrashSubmit"
];

const STATE_START = Ci.nsIWebProgressListener.STATE_START;
const STATE_STOP = Ci.nsIWebProgressListener.STATE_STOP;

const SUCCESS = "success";
const FAILED  = "failed";
const SUBMITTING = "submitting";

var reportURL = null;
var strings = null;
var myListener = null;

function parseINIStrings(file) {
  var factory = Cc["@mozilla.org/xpcom/ini-parser-factory;1"].
                getService(Ci.nsIINIParserFactory);
  var parser = factory.createINIParser(file);
  var obj = {};
  var en = parser.getKeys("Strings");
  while (en.hasMore()) {
    var key = en.getNext();
    obj[key] = parser.getString("Strings", key);
  }
  return obj;
}

// Since we're basically re-implementing part of the crashreporter
// client here, we'll just steal the strings we need from crashreporter.ini
function getL10nStrings() {
  let dirSvc = Cc["@mozilla.org/file/directory_service;1"].
               getService(Ci.nsIProperties);
  let path = dirSvc.get("GreD", Ci.nsIFile);
  path.append("crashreporter.ini");
  if (!path.exists()) {
    // see if we're on a mac
    path = path.parent;
    path = path.parent;
    path.append("MacOS");
    path.append("crashreporter.app");
    path.append("Contents");
    path.append("Resources");
    path.append("crashreporter.ini");
    if (!path.exists()) {
      // very bad, but I don't know how to recover
      return;
    }
  }
  let crstrings = parseINIStrings(path);
  strings = {
    'crashid': crstrings.CrashID,
    'reporturl': crstrings.CrashDetailsURL
  };

  path = dirSvc.get("XCurProcD", Ci.nsIFile);
  path.append("crashreporter-override.ini");
  if (path.exists()) {
    crstrings = parseINIStrings(path);
    if ('CrashID' in crstrings)
      strings['crashid'] = crstrings.CrashID;
    if ('CrashDetailsURL' in crstrings)
      strings['reporturl'] = crstrings.CrashDetailsURL;
  }
}

function getDir(name) {
  let directoryService = Cc["@mozilla.org/file/directory_service;1"].
                         getService(Ci.nsIProperties);
  let dir = directoryService.get("UAppData", Ci.nsIFile);
  dir.append("Crash Reports");
  dir.append(name);
  return dir;
}

function writeFile(dirName, fileName, data) {
  let path = getDir(dirName);
  if (!path.exists())
    path.create(Ci.nsIFile.DIRECTORY_TYPE, parseInt('0700', 8));
  path.append(fileName);
  var fs = Cc["@mozilla.org/network/file-output-stream;1"].
           createInstance(Ci.nsIFileOutputStream);
  // open, write, truncate
  fs.init(path, -1, -1, 0);
  var os = Cc["@mozilla.org/intl/converter-output-stream;1"].
           createInstance(Ci.nsIConverterOutputStream);
  os.init(fs, "UTF-8", 0, 0x0000);
  os.writeString(data);
  os.close();
  fs.close();
}

function getPendingMinidump(id) {
  let pendingDir = getDir("pending");
  let dump = pendingDir.clone();
  let extra = pendingDir.clone();
  let memory = pendingDir.clone();
  dump.append(id + ".dmp");
  extra.append(id + ".extra");
  memory.append(id + ".memory.json.gz");
  return [dump, extra, memory];
}

function getAllPendingMinidumpsIDs() {
  let minidumps = [];
  let pendingDir = getDir("pending");

  if (!(pendingDir.exists() && pendingDir.isDirectory()))
    return [];
  let entries = pendingDir.directoryEntries;

  while (entries.hasMoreElements()) {
    let entry = entries.getNext().QueryInterface(Ci.nsIFile);
    if (entry.isFile()) {
      let matches = entry.leafName.match(/(.+)\.extra$/);
      if (matches)
        minidumps.push(matches[1]);
    }
  }

  return minidumps;
}

function pruneSavedDumps() {
  const KEEP = 10;

  let pendingDir = getDir("pending");
  if (!(pendingDir.exists() && pendingDir.isDirectory()))
    return;
  let entries = pendingDir.directoryEntries;
  let entriesArray = [];

  while (entries.hasMoreElements()) {
    let entry = entries.getNext().QueryInterface(Ci.nsIFile);
    if (entry.isFile()) {
      let matches = entry.leafName.match(/(.+)\.extra$/);
      if (matches)
        entriesArray.push(entry);
    }
  }

  entriesArray.sort(function(a,b) {
    let dateA = a.lastModifiedTime;
    let dateB = b.lastModifiedTime;
    if (dateA < dateB)
      return -1;
    if (dateB < dateA)
      return 1;
    return 0;
  });

  if (entriesArray.length > KEEP) {
    for (let i = 0; i < entriesArray.length - KEEP; ++i) {
      let extra = entriesArray[i];
      let matches = extra.leafName.match(/(.+)\.extra$/);
      if (matches) {
        let dump = extra.clone();
        dump.leafName = matches[1] + '.dmp';
        dump.remove(false);

        let memory = extra.clone();
        memory.leafName = matches[1] + '.memory.json.gz';
        if (memory.exists()) {
          memory.remove(false);
        }

        extra.remove(false);
      }
    }
  }
}

function addFormEntry(doc, form, name, value) {
  var input = doc.createElement("input");
  input.type = "hidden";
  input.name = name;
  input.value = value;
  form.appendChild(input);
}

function writeSubmittedReport(crashID, viewURL) {
  var data = strings.crashid.replace("%s", crashID);
  if (viewURL)
     data += "\n" + strings.reporturl.replace("%s", viewURL);

  writeFile("submitted", crashID + ".txt", data);
}

// the Submitter class represents an individual submission.
function Submitter(id, recordSubmission, noThrottle, extraExtraKeyVals) {
  this.id = id;
  this.recordSubmission = recordSubmission;
  this.noThrottle = noThrottle;
  this.additionalDumps = [];
  this.extraKeyVals = extraExtraKeyVals || {};
  this.deferredSubmit = PromiseUtils.defer();
}

Submitter.prototype = {
  reportSubmitSuccess: function Submitter_reportSubmitSuccess() {
    let deferred = PromiseUtils.defer();

    let appid, keyname, appkey;
    try {
      appid = Services.prefs.getCharPref("crashreport.token.appid");
      keyname = Services.prefs.getCharPref("crashreport.authorization.key");
    } catch(e) {
      dump("crash Submitter can't find crashreport.token.appid or crashreport.authorization.key");
      return deferred.reject();
    }
    try {
      appkey = Services.urlFormatter.formatURL(keyname);
    } catch (e) {
      dump("crash Submitter can't find app key");
      return deferred.reject();
    }

    if (typeof appkey !== 'string' ||
      appkey.length == 0 ||
      appkey == 'no-kaios-crashreporter-api-key') {
      deferred.reject();
      dump("crash Submitter without API Key");
      return;
    }

    let api = 'v1.0/crash_reports';
    let url = 'https://api.kaiostech.com/';
    try {
      url = Services.prefs.getCharPref("crashreport.api.url");
    } catch(e) {
      dump("crash Submitter can't find crashreport.api.url");
    }
    if(!url.endsWith('/')) {
      url += '/';
    }
    let urlApi = url + api;

    let xhr = Cc["@mozilla.org/xmlextras/xmlhttprequest;1"]
              .createInstance(Ci.nsIXMLHttpRequest);
    xhr.open("PUT", urlApi, true);
    xhr.setRequestHeader("Content-Type", "application/json");
    xhr.setRequestHeader('Authorization', 'Key ' + appkey);

    xhr.addEventListener("readystatechange", (evt) => {
      if (xhr.readyState == 4) {
        if(xhr.status == 200 || xhr.status == 201) {
          deferred.resolve();
          dump('crash Submitter reportSubmitSuccess ' +  xhr.status);
        } else {
          dump('crash Submitter reportSubmitSuccess '
            + xhr.status + ', xhr.responseText, ' + xhr.responseText);
          deferred.reject();
        }
      }
    }, false);

    xhr.addEventListener("error", () => {
      dump('crash Submitter reportSubmitSuccess NETWORK_ERROR');
      deferred.reject();
    }, false);

    xhr.timeout = 60000;
    xhr.addEventListener("timeout", () => {
      dump('crash Submitter reportSubmitSuccess NETWORK_TIMEOUT');
      deferred.reject();
    }, false);

    let payload = {};
    payload.crash_id = this.id;
    payload.application_id = appid;

    xhr.send(JSON.stringify(payload));

    return deferred.promise;
  },

  getSubmitUrl: function Submitter_getSubmitUrl() {
    let defererdSubmitUrl = PromiseUtils.defer();

    let appid, keyname, appkey;
    try {
      appid = Services.prefs.getCharPref("crashreport.token.appid");
      keyname = Services.prefs.getCharPref("crashreport.authorization.key");
    } catch(e) {
      dump("crash Submitter can't find crashreport.token.appid or crashreport.authorization.key");
      return defererdSubmitUrl.reject();
    }
    try {
      appkey = Services.urlFormatter.formatURL(keyname);
    } catch (e) {
      dump("crash Submitter can't find app key");
      return defererdSubmitUrl.reject();
    }

    if (typeof appkey !== 'string' ||
      appkey.length == 0 ||
      appkey == 'no-kaios-crashreporter-api-key') {
      defererdSubmitUrl.reject();
      dump("crash Submitter without API Key");
      return;
    }

    let api = 'v1.0/applications/' + appid + '/signed_urls';
    let url = 'https://api.kaiostech.com/';
    try {
      url = Services.prefs.getCharPref("crashreport.api.url");
    } catch(e) {
      dump("crash Submitter can't find crashreport.api.url");
    }
    if(!url.endsWith('/')) {
      url += '/';
    }
    let urlApi = url + api;

    let xhr = Cc["@mozilla.org/xmlextras/xmlhttprequest;1"]
              .createInstance(Ci.nsIXMLHttpRequest);
    xhr.open("POST", urlApi, true);
    xhr.setRequestHeader("Content-Type", "application/json");
    xhr.setRequestHeader('Authorization', 'Key ' + appkey);

    xhr.addEventListener("readystatechange", (evt) => {
      if (xhr.readyState == 4) {
        if(xhr.status == 200) {
          defererdSubmitUrl.resolve(xhr.responseText);
        } else {
          dump('crash Submitter url get xhr.status '
            + xhr.status + ', xhr.responseText, ' + xhr.responseText);
          defererdSubmitUrl.reject();
        }
      }
    }, false);

    xhr.addEventListener("error", () => {
      dump('crash Submitter NETWORK_ERROR');
      defererdSubmitUrl.reject();
    }, false);

    xhr.timeout = 60000;
    xhr.addEventListener("timeout", () => {
      dump('crash Submitter NETWORK_TIMEOUT');
      defererdSubmitUrl.reject();
    }, false);

    DeviceUtils.getTDeviceObject().then(device_info => {
      device_info.crash_id = this.id;
      for(var k in this.extraKeyVals) {
        if (k != "ServerURL") {
          device_info[k] = this.extraKeyVals[k];
        }
      }
      xhr.send(JSON.stringify(device_info));
    }, e => {
      dump('crash Submitter device_info can not get');
      defererdSubmitUrl.reject();
    });

    return defererdSubmitUrl.promise;
  },

  submitSuccess: function Submitter_submitSuccess(ret) {
    // Write out the details file to submitted/
    writeSubmittedReport(ret.CrashID, ret.ViewURL);

    // Delete from pending dir
    try {
      this.dump.remove(false);
      this.extra.remove(false);

      if (this.memory) {
        this.memory.remove(false);
      }

      for (let i of this.additionalDumps) {
        i.dump.remove(false);
      }
    }
    catch (ex) {
      // report an error? not much the user can do here.
    }

    this.notifyStatus(SUCCESS, ret);
    this.cleanup();
  },

  cleanup: function Submitter_cleanup() {
    // drop some references just to be nice
    this.iframe = null;
    this.dump = null;
    this.extra = null;
    this.memory = null;
    this.additionalDumps = null;
    // remove this object from the list of active submissions
    let idx = CrashSubmit._activeSubmissions.indexOf(this);
    if (idx != -1)
      CrashSubmit._activeSubmissions.splice(idx, 1);
  },

  submitForm: function Submitter_submitForm(serverURL) {
    // Override the submission URL from the environment
    var envOverride = Cc['@mozilla.org/process/environment;1'].
      getService(Ci.nsIEnvironment).get("MOZ_CRASHREPORTER_URL");
    if (envOverride != '') {
      serverURL = envOverride;
    }

    let xhr = Cc["@mozilla.org/xmlextras/xmlhttprequest;1"]
              .createInstance(Ci.nsIXMLHttpRequest);
    xhr.open("PUT", serverURL, true);

    let formData = Cc["@mozilla.org/files/formdata;1"]
                   .createInstance(Ci.nsIDOMFormData);
    // add the data
    for (let [name, value] in Iterator(this.extraKeyVals)) {
      if (name != "ServerURL") {
        formData.append(name, value);
      }
    }
    if (this.noThrottle) {
      // tell the server not to throttle this, since it was manually submitted
      formData.append("Throttleable", "0");
    }
    // add the minidumps
    formData.append("upload_file_minidump", new File(this.dump.path));
    if (this.memory) {
      formData.append("memory_report", new File(this.memory.path));
    }
    if (this.additionalDumps.length > 0) {
      let names = [];
      for (let i of this.additionalDumps) {
        names.push(i.name);
        formData.append("upload_file_minidump_"+i.name,
                        new File(i.dump.path));
      }
    }

    let manager = Services.crashmanager;
    let submissionID = manager.generateSubmissionID();

    let parseCrashID = function(url) {
      // https://kai-crash-na2.s3.amazonaws.com/
      // KaiOS2.5-B2G-qcom-20180806230748/jbhC_5sShd0WeI5K7wSl?xxxxx
      // the id is between first '/' and '?' in path
      let uri = Services.io.newURI(url, null, null)
      let path = uri.path;
      let idxEnd = path.indexOf('?');
      // skip first '/'
      let tmpStr = path.substring(1, idxEnd);

      return tmpStr.substring(tmpStr.indexOf('/') + 1);
    };

    xhr.addEventListener("readystatechange", (evt) => {
      if (xhr.readyState == 4) {
        let ret = {"CrashID": parseCrashID(serverURL), "ViewURL": ""};
        let submitted = xhr.status == 200;

        if (this.recordSubmission) {
          let result = submitted ? manager.SUBMISSION_RESULT_OK :
                                   manager.SUBMISSION_RESULT_FAILED;
          manager.addSubmissionResult(this.id, submissionID, new Date(),
                                      result);
          if (submitted) {
            manager.setRemoteCrashID(this.id, ret.CrashID);
          }
        }

        if (submitted) {
          this.reportSubmitSuccess().then(() => {
            this.submitSuccess(ret);
          }, () => {
            this.notifyStatus(FAILED);
            this.cleanup();
          });
        }
        else {
           this.notifyStatus(FAILED);
           this.cleanup();
        }
      }
    }, false);

    if (this.recordSubmission) {
      manager.addSubmissionAttempt(this.id, submissionID, new Date());
    }
    xhr.send(formData);
  },

  notifyStatus: function Submitter_notify(status, ret) {
    let propBag = Cc["@mozilla.org/hash-property-bag;1"].
                  createInstance(Ci.nsIWritablePropertyBag2);
    propBag.setPropertyAsAString("minidumpID", this.id);
    if (status == SUCCESS) {
      propBag.setPropertyAsAString("serverCrashID", ret.CrashID);
    }

    let extraKeyValsBag = Cc["@mozilla.org/hash-property-bag;1"].
                          createInstance(Ci.nsIWritablePropertyBag2);
    for (let key in this.extraKeyVals) {
      extraKeyValsBag.setPropertyAsAString(key, this.extraKeyVals[key]);
    }
    propBag.setPropertyAsInterface("extra", extraKeyValsBag);

    Services.obs.notifyObservers(propBag, "crash-report-status", status);

    switch (status) {
      case SUCCESS:
        this.deferredSubmit.resolve(ret.CrashID);
        break;
      case FAILED:
        this.deferredSubmit.reject();
        break;
      default:
        // no callbacks invoked.
    }
  },

  submit: function Submitter_submit() {
    let [dump, extra, memory] = getPendingMinidump(this.id);

    if (!dump.exists() || !extra.exists()) {
      this.notifyStatus(FAILED);
      this.cleanup();
      return this.deferredSubmit.promise;
    }
    this.dump = dump;
    this.extra = extra;

    // The memory file may or may not exist
    if (memory.exists()) {
      this.memory = memory;
    }

    let extraKeyVals = parseKeyValuePairsFromFile(extra);
    for (let key in extraKeyVals) {
      if (!(key in this.extraKeyVals)) {
        this.extraKeyVals[key] = extraKeyVals[key];
      }
    }

    let additionalDumps = [];
    if ("additional_minidumps" in this.extraKeyVals) {
      let names = this.extraKeyVals.additional_minidumps.split(',');
      for (let name of names) {
        let [dump, extra, memory] = getPendingMinidump(this.id + "-" + name);
        if (!dump.exists()) {
          this.notifyStatus(FAILED);
          this.cleanup();
          return this.deferredSubmit.promise;
        }
        additionalDumps.push({'name': name, 'dump': dump});
      }
    }

    this.notifyStatus(SUBMITTING);

    this.additionalDumps = additionalDumps;

    this.getSubmitUrl().then(url => {
      this.submitForm(url);
    }, () => {
      this.notifyStatus(FAILED);
      this.cleanup();
    });

    return this.deferredSubmit.promise;
  }
};

//===================================
// External API goes here
this.CrashSubmit = {
  /**
   * Submit the crash report named id.dmp from the "pending" directory.
   *
   * @param id
   *        Filename (minus .dmp extension) of the minidump to submit.
   * @param params
   *        An object containing any of the following optional parameters:
   *        - recordSubmission
   *          If true, a submission event is recorded in CrashManager.
   *        - noThrottle
   *          If true, this crash report should be submitted with
   *          an extra parameter of "Throttleable=0" indicating that
   *          it should be processed right away. This should be set
   *          when the report is being submitted and the user expects
   *          to see the results immediately. Defaults to false.
   *        - extraExtraKeyVals
   *          An object whose key-value pairs will be merged with the data from
   *          the ".extra" file submitted with the report.  The properties of
   *          this object will override properties of the same name in the
   *          .extra file.
   *
   *  @return a Promise that is fulfilled with the server crash ID when the
   *          submission succeeds and rejected otherwise.
   */
  submit: function CrashSubmit_submit(id, params) {
    params = params || {};
    let recordSubmission = false;
    let submitSuccess = null;
    let submitError = null;
    let noThrottle = false;
    let extraExtraKeyVals = null;

    if ('recordSubmission' in params)
      recordSubmission = params.recordSubmission;
    if ('noThrottle' in params)
      noThrottle = params.noThrottle;
    if ('extraExtraKeyVals' in params)
      extraExtraKeyVals = params.extraExtraKeyVals;

    let submitter = new Submitter(id, recordSubmission,
                                  noThrottle, extraExtraKeyVals);
    CrashSubmit._activeSubmissions.push(submitter);
    return submitter.submit();
  },

  /**
   * Delete the minidup from the "pending" directory.
   *
   * @param id
   *        Filename (minus .dmp extension) of the minidump to delete.
   */
  delete: function CrashSubmit_delete(id) {
    let [dump, extra, memory] = getPendingMinidump(id);
    dump.remove(false);
    extra.remove(false);
    if (memory.exists()) {
      memory.remove(false);
    }
  },

  /**
   * Get the list of pending crash IDs.
   *
   * @return an array of string, each being an ID as
   *         expected to be passed to submit()
   */
  pendingIDs: function CrashSubmit_pendingIDs() {
    return getAllPendingMinidumpsIDs();
  },

  /**
   * Prune the saved dumps.
   */
  pruneSavedDumps: function CrashSubmit_pruneSavedDumps() {
    pruneSavedDumps();
  },

  // List of currently active submit objects
  _activeSubmissions: []
};

// Run this when first loaded
getL10nStrings();
