// Dump the localKey (and other fields) of every Tuya DeviceBean live in the app's heap.
//
// The Tuya / Smart Life / SSG apps store device credentials in encrypted MMKV, so a static
// dump of shared_prefs won't reveal the localKey on current versions. Instead we let the app
// load, then Java.choose the in-memory DeviceBean and read its fields directly.
//
// The localKey ROTATES on every re-pair -- re-run this after pairing/re-pairing a bulb.
//
// Frida 17 dropped the implicit `Java` global from raw scripts. Either run on Frida <17, or
// compile this with frida-java-bridge:  see frida/README.md.
//
// Usage (after pairing your bulb in the app, with frida-server running on the device/emulator):
//   frida -U -f com.tuya.smartlife -l dump_localkey.js          (Smart Life)
//   frida -U -f <your.tuya.app.package> -l dump_localkey.js
// then open the app and let the device list load.

import Java from 'frida-java-bridge'; // remove this line if running on Frida <17 (Java is global)

function dumpFields(obj, label) {
  try {
    var cls = obj.getClass();
    send('==== ' + label + ' : ' + cls.getName() + ' ====');
    while (cls && cls.getName().indexOf('java.lang.Object') < 0) {
      var fields = cls.getDeclaredFields();
      for (var i = 0; i < fields.length; i++) {
        var f = fields[i];
        f.setAccessible(true);
        try {
          var v = f.get(obj);
          var sv = (v === null) ? 'null' : v.toString();
          if (sv.length > 300) sv = sv.substring(0, 300);
          send('  ' + f.getName() + ' = ' + sv);
        } catch (e) {}
      }
      cls = cls.getSuperclass();
    }
  } catch (e) { send('dump err ' + e); }
}

// Tuya's SDK bean. Older apps: com.tuya.smart.sdk.bean.DeviceBean
// Newer (ThingClips-rebranded) apps: com.thingclips.smart.sdk.bean.DeviceBean
var CANDIDATES = [
  'com.thingclips.smart.sdk.bean.DeviceBean',
  'com.tuya.smart.sdk.bean.DeviceBean',
];

var tries = 0;
function attempt() {
  Java.perform(function () {
    var found = 0;
    CANDIDATES.forEach(function (cn) {
      try {
        Java.choose(cn, {
          onMatch: function (inst) {
            found++;
            try {
              var devId = inst.getDevId ? inst.getDevId() : null;
              var name = inst.getName ? inst.getName() : null;
              send('[DeviceBean] devId=' + devId + ' name=' + name);
              dumpFields(inst, 'DEVICE ' + name);
            } catch (e) { send('inst err ' + e); }
          },
          onComplete: function () {}
        });
      } catch (e) { /* class not present in this app build */ }
    });
    tries++;
    if (found === 0 && tries < 20) {
      setTimeout(attempt, 3000);
    } else {
      send('[FINISHED tries=' + tries + ' found=' + found + ']');
    }
  });
}

send('[*] agent up; waiting 8s for runtime, then polling for DeviceBean. Open the device list.');
setTimeout(attempt, 8000);
