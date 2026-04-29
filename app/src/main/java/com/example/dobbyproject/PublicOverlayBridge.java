package com.example.dobbyproject;

import android.view.Surface;

public final class PublicOverlayBridge {
    static {
        System.loadLibrary("dobbyproject");
    }

    // 端口分配 — 每个游戏占用独立端口, 避免单 RenderServer 抢占
    public static final int PORT_PUBG = 16888;
    public static final int PORT_DFM  = 16889;
    public static final int PORT_NRC  = 16890;

    private PublicOverlayBridge() {
    }

    /** 兼容旧接口: 不带端口默认 PUBG 端口 16888 */
    public static boolean startRenderer(Surface surface, int width, int height, int rotateTheta) {
        return startRenderer(surface, width, height, rotateTheta, PORT_PUBG);
    }

    public static boolean startRenderer(Surface surface, int width, int height, int rotateTheta, int port) {
        if (surface == null || width <= 0 || height <= 0 || port <= 0) {
            return false;
        }
        return nativeStartRenderer(surface, width, height, rotateTheta, port);
    }

    /** 兼容旧接口: 停止默认端口 */
    public static void stopRenderer() {
        stopRenderer(PORT_PUBG);
    }

    public static void stopRenderer(int port) {
        nativeStopRenderer(port);
    }

    /** 停止所有 renderer */
    public static void stopAllRenderers() {
        nativeStopAllRenderers();
    }

    public static boolean isRendererRunning() {
        return isRendererRunning(PORT_PUBG);
    }

    public static boolean isRendererRunning(int port) {
        return nativeIsRendererRunning(port);
    }

    private static native boolean nativeStartRenderer(Surface surface, int width, int height, int rotateTheta, int port);
    private static native void nativeStopRenderer(int port);
    private static native void nativeStopAllRenderers();
    private static native boolean nativeIsRendererRunning(int port);
}
