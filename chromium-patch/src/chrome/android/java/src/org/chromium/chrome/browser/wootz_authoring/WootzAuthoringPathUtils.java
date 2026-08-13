package org.chromium.chrome.browser.wootz_authoring;

import android.content.Context;

import org.jni_zero.CalledByNative;
import org.jni_zero.JNINamespace;

import org.chromium.base.ContextUtils;
import org.chromium.base.Log;

import java.io.File;

/** Utility class to provide storage paths for Wootz authoring sessions. */
@JNINamespace("wootz_authoring")
public class WootzAuthoringPathUtils {
    private static final String TAG = "WootzAuthoringPathUtils";
    private static final String AUTHORING_DIR = "WootzAuthoring";

    /**
     * Returns the app-specific external storage directory path for authoring data.
     * Path: /storage/emulated/0/Android/data/<package>/files/WootzAuthoring/
     */
    @CalledByNative
    public static String getAuthoringStoragePath() {
        try {
            Context context = ContextUtils.getApplicationContext();
            File externalFilesDir = context.getExternalFilesDir(null);
            if (externalFilesDir == null) {
                Log.e(TAG, "External storage not available");
                return null;
            }

            File authoringDir = new File(externalFilesDir, AUTHORING_DIR);
            if (!authoringDir.exists() && !authoringDir.mkdirs()) {
                Log.e(TAG, "Failed to create authoring directory");
                return null;
            }

            String path = authoringDir.getAbsolutePath();
            Log.i(TAG, "Authoring storage path: " + path);
            return path;
        } catch (Exception e) {
            Log.e(TAG, "Failed to get authoring storage path: " + e.getMessage());
            return null;
        }
    }
}
