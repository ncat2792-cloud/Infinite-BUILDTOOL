package com.vdl.kong520;

import android.util.Log;

/** Native bridge intentionally limited to the independent building-tools API. */
public final class NativeCore {
    private static final String TAG = "BuildToolsNative";
    private static final Object SO_LOAD_LOCK = new Object();
    private static volatile boolean soLoaded;
    private static volatile Throwable soLoadError;

    private NativeCore() {}

    public static boolean loadNative() {
        if (soLoaded) return true;
        synchronized (SO_LOAD_LOCK) {
            if (soLoaded) return true;
            try {
                final String libraryName = DevConfig.SO_NAM;
                if (libraryName == null || libraryName.trim().isEmpty()) {
                    throw new IllegalStateException("Native library name is empty");
                }
                System.loadLibrary(libraryName);
                soLoaded = true;
                soLoadError = null;
            } catch (Throwable throwable) {
                soLoaded = false;
                soLoadError = throwable;
                Log.e(TAG, "Unable to load building-tools native runtime", throwable);
            }
            return soLoaded;
        }
    }

    /** Used by the optional injected host, where the module APK owns the .so. */
    static boolean loadNativePath(String absolutePath) {
        if (soLoaded) return true;
        synchronized (SO_LOAD_LOCK) {
            if (soLoaded) return true;
            try {
                System.load(absolutePath);
                soLoaded = true;
                soLoadError = null;
            } catch (Throwable throwable) {
                soLoaded = false;
                soLoadError = throwable;
                Log.e(TAG, "Unable to load building-tools native runtime", throwable);
            }
            return soLoaded;
        }
    }

    public static boolean isNativeReady() {
        return soLoaded;
    }

    /**
     * 标记 native 库「已由外部加载成功」。
     *
     * LSPatch 场景下库可能已被宿主 ClassLoader 加载（System.loadLibrary 成功），
     * 但那条路不经过 loadNative / loadNativePath，所以要显式同步这个标志，
     * 否则 isNativeReady() 一直是 false，每次进面板都会重复加载。
     *
     * 副作用是正确的：库确实已经在进程里了，后续 JNI 调用可以直接走。
     */
    static boolean markLoaded() {
        synchronized (SO_LOAD_LOCK) {
            soLoaded = true;
            soLoadError = null;
            return true;
        }
    }

    /** Returns the most recent native-loader error for injected-host diagnostics. */
    public static Throwable getNativeLoadError() {
        return soLoadError;
    }

    /**
     * Installs the hooks needed by import, projection and export.
     */
    public static native void ensureBuildToolsHooks();

    /** Claims one pending, validated anvil-window close request, or returns zero. */
    public static native long takePendingMapAnvilUiCloseRequest();

    /** Rechecks the claimed request against the current native anvil screen. */
    public static native boolean isMapAnvilUiCloseStillSafe(long ticket);

    /** Records that both Back key events were dispatched for the claimed request. */
    public static native void markMapAnvilUiCloseDispatched(long ticket);

    /** Claims the one pending tool-owned visible chest close request. */
    public static native long takePendingMapChestUiCloseRequest();

    /** Rechecks the exact captured chest before delivering Back. */
    public static native boolean isMapChestUiCloseStillSafe(long ticket);

    /** Records delivery of both Back key events for that chest. */
    public static native void markMapChestUiCloseDispatched(long ticket);
}
