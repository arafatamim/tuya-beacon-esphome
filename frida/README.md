# Extracting the localKey with Frida

The bulb's `localKey` is the only real secret you need. It's a 16-character per-device key that
**rotates on every re-pair**. Modern Tuya apps keep it in encrypted MMKV, so you read it from the
running app's heap rather than from files.

You do **not** need a Tuya IoT cloud subscription for this — it's a purely local extraction.

## Prerequisites

- A rooted device **or** a rootable emulator. A Google **APIs** (not Play Store) x86_64 AVD works
  and allows `adb root`. Avoid Play-Store images (no root).
- `frida-server` running on the device/emulator (matching your host `frida` version & the device
  ABI). Push it to `/data/local/tmp/` and run it as root. If attach fails with
  *"unable to write to process memory"*, set SELinux permissive: `adb shell setenforce 0`, and use
  spawn-suspend → attach → resume.
- The Tuya app (Smart Life / SSG / your vendor's app) **logged in, with the bulb already paired**
  so its DeviceBean exists.

## Run

Frida 17 removed the implicit `Java` global from raw scripts. Two options:

**A. Frida < 17** — delete the `import Java ...` line at the top of `dump_localkey.js`, then:

```bash
frida -U -f com.tuya.smartlife -l dump_localkey.js
```

**B. Frida 17+** — keep the import and compile the bridge in first:

```bash
npm init -y
npm i frida-java-bridge
npx frida-compile dump_localkey.js -o dump_compiled.js
frida -U -f <your.tuya.app.package> -l dump_compiled.js
```

Open the app's device list. Within ~10 s you'll see lines like:

```
[DeviceBean] devId=bf... name=Your Bulb
==== DEVICE Your Bulb : com.thingclips.smart.sdk.bean.DeviceBean ====
  localKey = XXXXXXXXXXXXXXXX
  ...
```

That `localKey` (16 chars, as ASCII) is your XXTEA key. Export it and you're done:

```bash
export TUYA_LOCALKEY=XXXXXXXXXXXXXXXX
```

> Keep your localKey private — it is the secret that authorizes control of your bulb. Never commit it.
