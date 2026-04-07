package com.example.dobbyproject;

import android.view.Surface;

public final class PublicOverlayBridge {
    static {
        System.loadLibrary("dobbyproject");
    }

    private PublicOverlayBridge() {
    }

    public static boolean startRenderer(Surface surface, int width, int height, int rotateTheta) {
        if (surface == null || width <= 0 || height <= 0) {
            return false;
        }
        return nativeStartRenderer(surface, width, height, rotateTheta);
    }

    public static void stopRenderer() {
        nativeStopRenderer();
    }

    public static boolean isRendererRunning() {
        return nativeIsRendererRunning();
    }

    private static native boolean nativeStartRenderer(Surface surface, int width, int height, int rotateTheta);
    private static native void nativeStopRenderer();
    private static native boolean nativeIsRendererRunning();
}