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

    /** 注入触摸事件到指定 RenderServer (用于 scrcpy 投屏 / 软件鼠标 / 远程控制场景).
     *  这条路径绕过 /dev/input evdev, 直接把屏幕坐标喂进 ImGui 输入流.
     *  @param port   目标 RenderServer 端口
     *  @param action 0=Down, 1=Move, 2=Up, 3=Cancel
     *  @param x      显示坐标系 X (像素)
     *  @param y      显示坐标系 Y (像素)
     *  @return 是否成功转发 (端口未运行则返回 false)
     */
    public static boolean injectTouch(int port, int action, float x, float y) {
        if (port <= 0) return false;
        return nativeInjectTouch(port, action, x, y);
    }

    /** 查询 RenderServer 端缓存的最近一次菜单 rect 列表 (由游戏 RenderClient 自动上报).
     *  数组布局: out[0]=count, out[1..1+count*4-1] = (x0,y0,w0,h0, x1,y1,...)
     *  推荐传入长度 1 + 16*4 = 65 的数组.
     *  @return true = 已查询成功 (count 可能为 0); false = 端口未运行 / 数组太短
     */
    public static boolean getMenuRects(int port, int[] outBuf) {
        if (port <= 0 || outBuf == null || outBuf.length < 1) return false;
        return nativeGetMenuRect(port, outBuf);
    }

    private static native boolean nativeStartRenderer(Surface surface, int width, int height, int rotateTheta, int port);
    private static native void nativeStopRenderer(int port);
    private static native void nativeStopAllRenderers();
    private static native boolean nativeIsRendererRunning(int port);
    private static native boolean nativeInjectTouch(int port, int action, float x, float y);
    private static native boolean nativeGetMenuRect(int port, int[] outXYWH);
}
