package com.vdl.kong520;

import android.text.TextUtils;
import android.util.Log;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * 加载模块自带的 native 库（libweibosdkcore.so）。
 *
 * 为什么不能只用 findLibrary()：
 *   · LSPosed 以「已安装的独立模块 APK」运行时，模块 ClassLoader 的
 *     findLibrary() 能给出 Android 安装器解压后的真实路径 —— 这是最稳的一条。
 *   · LSPatch 是把模块 dex / so 注入进目标 APK 再重新签名，模块 ClassLoader
 *     是拼接出来的，findLibrary() 往往返回 null。此时必须回退到
 *     「目标 APK 自己的 nativeLibraryDir」——库已经被打进宿主 APK 的 lib/ 里了。
 *
 * 所以这里按成功率从高到低依次尝试，全部失败才报错，并把每条路的结果写进
 * 日志，免得真机上一句 “加载失败” 看不出卡在哪一步。
 */
public final class NativeLibLoader {
    private static final String TAG = "NativeLibLoader";
    public static final String LIB_NAME = "weibosdkcore";
    public static final String FIND_LIB_METHOD = "findLibrary";

    /** 记录每一次尝试，(方式, 结果)。出错时原样写进日志。 */
    private static volatile String lastAttempts = "";
    private static volatile Throwable lastLoadError;

    private NativeLibLoader() {}

    public static boolean load(Class<?> moduleClass) {
        return load(moduleClass, null);
    }

    /**
     * @param hostContext 目标应用（游戏）的 Context。LSPatch 下需要它拿到
     *                    宿主 APK 的 nativeLibraryDir；传 null 则跳过这条路。
     */
    public static boolean load(Class<?> moduleClass, android.content.Context hostContext) {
        if (NativeCore.isNativeReady()) return true;
        lastLoadError = null;

        final List<String> attempts = new ArrayList<>();

        // ① 模块 ClassLoader 的 findLibrary()：LSPosed 正常路径。
        String path = findLibraryPath(moduleClass, attempts);
        if (!TextUtils.isEmpty(path) && NativeCore.loadNativePath(path)) {
            finish(attempts, "findLibrary");
            return true;
        }

        // ② 宿主 APK 的 nativeLibraryDir：LSPatch 注入后的正常路径。
        //    库已经被打进目标 APK，安装器把它解压到 <数据目录>/lib/<abi>/。
        if (hostContext != null) {
            final String dir = hostContext.getApplicationInfo().nativeLibraryDir;
            if (!TextUtils.isEmpty(dir)) {
                final String candidate = dir + "/" + System.mapLibraryName(LIB_NAME);
                if (new File(candidate).isFile() && NativeCore.loadNativePath(candidate)) {
                    finish(attempts, "host nativeLibraryDir");
                    return true;
                }
                attempts.add("host nativeLibraryDir: " + candidate + " -> 缺失或加载失败");
            }
        }

        // ③ 退一步：让宿主的 ClassLoader 直接 System.loadLibrary。
        //    LSPatch 补丁后的进程里，这个名字可能已经能从宿主路径解析到。
        try {
            System.loadLibrary(LIB_NAME);
            if (NativeCore.markLoaded()) {
                finish(attempts, "System.loadLibrary");
                return true;
            }
        } catch (Throwable error) {
            lastLoadError = error;
            attempts.add("System.loadLibrary: " + error);
        }

        lastAttempts = TextUtils.join("\n", attempts);
        Log.e(TAG, "native 库加载失败:\n" + lastAttempts, lastLoadError);
        return false;
    }

    /** 走反射调用 ClassLoader#findLibrary（protected）。 */
    private static String findLibraryPath(Class<?> moduleClass, List<String> attempts) {
        if (moduleClass == null) return null;
        try {
            final ClassLoader loader = moduleClass.getClassLoader();
            if (loader == null) {
                attempts.add("findLibrary: 模块 ClassLoader 为 null");
                return null;
            }
            final java.lang.reflect.Method method =
                    loader.getClass().getDeclaredMethod(FIND_LIB_METHOD, String.class);
            method.setAccessible(true);
            final Object result = method.invoke(loader, LIB_NAME);
            final String path = result instanceof String ? (String) result : null;
            if (TextUtils.isEmpty(path)) {
                attempts.add("findLibrary: 返回空（LSPatch 注入时属正常现象）");
                return null;
            }
            return path;
        } catch (Throwable error) {
            attempts.add("findLibrary: " + error);
            return null;
        }
    }

    private static void finish(List<String> attempts, String how) {
        attempts.add("成功：" + how);
        lastAttempts = TextUtils.join("\n", attempts);
        Log.d(TAG, "native 库已加载，方式=" + how);
    }

    public static Throwable getLastLoadError() {
        return lastLoadError;
    }

    /** 供诊断界面/日志展示：每条加载路径的结果。 */
    public static String getLastAttempts() {
        return lastAttempts;
    }
}
