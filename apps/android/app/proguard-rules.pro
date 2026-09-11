# isMinifyEnabled is true (see app/build.gradle.kts) - R8 shrinks/renames
# anything it cannot prove is reachable from an ordinary Kotlin/Java call
# site. Two classes are reachable a different way, invisible to that
# analysis, because native code (apps/android/app/src/main/cpp/ and
# src/audio/src/backend/android/passthrough.cpp) finds them by NAME at
# runtime rather than through a normal call graph edge:

# 1. Every `external fun` on NativeBridge is a JNI entry point resolved by
#    the JVM's own mangled-symbol convention
#    (Java_com_ac3forge_shield_NativeBridge_<method> - see
#    passthrough.cpp's header comment and jni_entry.cpp/live_cursor.cpp/
#    file_replay.cpp, which define exactly those symbols). Renaming the
#    class or any native method breaks that resolution with an
#    UnsatisfiedLinkError on the very first native call. The stock
#    proguard-android-optimize.txt template already protects classes with
#    native methods in general (`-keepclasseswithmembernames class * {
#    native <methods>; }`), but this app's entire native round trip depends
#    on that one line continuing to exist upstream, so it is pinned
#    explicitly here too rather than left solely to a template default.
-keepclasseswithmembernames class com.ac3forge.shield.NativeBridge {
    native <methods>;
}

# 2. PassthroughBridge is looked up the other way: native code holds
#    whatever instance Kotlin passed to registerPassthroughBridge(bridge:
#    Any) and calls env->GetObjectClass(bridge) +
#    env->GetMethodID(class, "<name>", "<signature>") for each method below
#    (passthrough.cpp's registerPassthroughBridge()). Because the Kotlin
#    parameter is typed `Any`, R8 cannot trace that call to
#    PassthroughBridge's methods - and `open`/`submit` specifically are
#    never called from Kotlin/Java at all, only from native - so without
#    this rule R8 is free to rename or strip them entirely. That fails
#    silently at runtime, not loudly: GetMethodID returns null,
#    registerPassthroughBridge logs "GetMethodID failed" and leaves the
#    bridge unregistered, and every PassthroughSink call after that
#    degrades to kNoBackend/kComFailure - the app runs, the UI looks normal,
#    and no audio ever reaches the receiver. Signatures are part of the JNI
#    ABI (see PassthroughBridge.kt's own class doc) - kept exactly as
#    declared there.
#
#    The class NAME (not just these members) is kept too - a plain
#    `-keepclassmembers` still leaves R8 free to rename the class itself
#    (confirmed: it became `H.q` before this line was `-keep`), which is
#    harmless for native's own lookup (GetObjectClass resolves the runtime
#    class of whatever instance it was handed, not by name) but breaks
#    src/androidTest/.../PassthroughBridgeInstrumentedTest.kt and
#    NativeBridgeInstrumentedTest.kt, both of which construct
#    `PassthroughBridge()` by that exact class name and would otherwise hit
#    ClassNotFoundException the moment either test APK runs against a
#    minified release build.
-keep class com.ac3forge.shield.PassthroughBridge {
    boolean isDirectPlaybackSupported(int, boolean);
    boolean isPcmSupported(int);
    boolean open(int, boolean);
    int submit(java.nio.ByteBuffer, int);
    void close();
}
