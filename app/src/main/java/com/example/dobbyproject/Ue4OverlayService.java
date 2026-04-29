package com.example.dobbyproject;

import android.app.usage.UsageEvents;
import android.app.usage.UsageStatsManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.Point;
import android.graphics.PixelFormat;
import android.graphics.SurfaceTexture;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.provider.Settings;
import android.util.Log;
import android.view.Display;
import android.view.Gravity;
import android.view.Surface;
import android.view.TextureView;
import android.view.View;
import android.view.WindowManager;
import android.view.WindowMetrics;

import androidx.core.app.NotificationCompat;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * 多游戏 Overlay 服务: 同一时刻只保留 1 个活动 overlay (TextureView + RenderServer),
 * 按前台游戏包动态切换.
 *
 * 为什么不能同时开 3 个 RenderServer:
 *   dear imgui 在同一进程内是全局单例 (GImGui + BackendRendererUserData),
 *   并发 Init 会触发 imgui_impl_opengl3.cpp:267 的 assert "Already initialized a renderer backend!"
 *   导致 SIGABRT 整个 service 进程.
 *
 * 端口分配 (与 PublicOverlayBridge / 各游戏 uestart.cpp 一致):
 *   PUBG = 16888
 *   DFM  = 16889
 *   NRC  = 16890
 * 游戏端 RenderClient 用各自端口连本机回环, 切换时旧 client 自然 ECONNREFUSED 重试.
 */
public class Ue4OverlayService extends Service {
    public static final String ACTION_START = "com.example.dobbyproject.action.START_UE4_OVERLAY";
    public static final String ACTION_STOP = "com.example.dobbyproject.action.STOP_UE4_OVERLAY";
    /** 可选 Intent extra: 明确指定要 overlay 的游戏包名。传入后会立即创建 overlay,
     *  不再需要 UsageStats 权限轮询。 */
    public static final String EXTRA_PACKAGE = "com.example.dobbyproject.extra.PACKAGE";

    private static final String TAG = "UE4OverlayService";
    private static final String CHANNEL_ID = "ue4_overlay_debug";
    private static final int NOTIFICATION_ID = 1107;
    private static final float TOUCH_PASSTHROUGH_ALPHA = 0.7f;
    private static final long FOREGROUND_POLL_INTERVAL_MS = 800L;

    /** package -> RenderServer 端口 */
    private static final Map<String, Integer> GAME_PORT_MAP = new LinkedHashMap<>();
    static {
        GAME_PORT_MAP.put("com.tencent.tmgp.pubgmhd", PublicOverlayBridge.PORT_PUBG);
        GAME_PORT_MAP.put("com.tencent.tmgp.dfm",     PublicOverlayBridge.PORT_DFM);
        GAME_PORT_MAP.put("com.tencent.nrc",          PublicOverlayBridge.PORT_NRC);
    }

    private WindowManager windowManager;
    private PowerManager.WakeLock wakeLock;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    /** 同一时刻最多 1 个 (ImGui 全局状态限制). null 表示当前前台不是已知游戏. */
    private GameOverlay activeOverlay;

    private final Runnable foregroundPollRunnable = new Runnable() {
        @Override
        public void run() {
            try {
                syncActiveOverlayWithForeground();
            } catch (Throwable t) {
                Log.e(TAG, "foregroundPoll 异常", t);
            }
            mainHandler.postDelayed(this, FOREGROUND_POLL_INTERVAL_MS);
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        Log.w(TAG, "=== onCreate: overlay 服务启动 pid=" + android.os.Process.myPid() + " ===");
        windowManager = (WindowManager) getSystemService(WINDOW_SERVICE);

        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        if (pm != null) {
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "dobby:overlay_wakelock");
            wakeLock.acquire();
            Log.w(TAG, "=== WakeLock acquired ===");
        }

        createNotificationChannel();
        Notification notification = buildNotification();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else {
            startForeground(NOTIFICATION_ID, notification);
        }

        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M || Settings.canDrawOverlays(this)) {
            // 不在 onCreate 启动 polling; 等 onStartCommand 决定是显式指定还是 fallback 轮询
        } else {
            Log.e(TAG, "onCreate: 缺少悬浮窗权限");
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent != null ? intent.getAction() : ACTION_START;
        if (ACTION_STOP.equals(action)) {
            stopSelf();
            return START_NOT_STICKY;
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && !Settings.canDrawOverlays(this)) {
            Log.e(TAG, "缺少悬浮窗权限，停止服务");
            stopSelf();
            return START_NOT_STICKY;
        }

        // 优先使用 Intent 中的明确包名 (不依赖 UsageStats)
        String requestedPkg = intent != null ? intent.getStringExtra(EXTRA_PACKAGE) : null;
        if (requestedPkg != null && GAME_PORT_MAP.containsKey(requestedPkg)) {
            Log.w(TAG, "onStartCommand: 显式指定游戏 pkg=" + requestedPkg);
            // 关闭 UsageStats 轮询, 避免误判前台拆掉 overlay
            mainHandler.removeCallbacks(foregroundPollRunnable);
            switchActiveOverlayTo(requestedPkg);
        } else if (activeOverlay == null) {
            // 只有在完全没有明确请求且当前无 overlay 时才轮询
            startForegroundPolling();
        }
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        mainHandler.removeCallbacks(foregroundPollRunnable);
        teardownActiveOverlay();
        PublicOverlayBridge.stopAllRenderers();
        if (wakeLock != null && wakeLock.isHeld()) {
            wakeLock.release();
            Log.w(TAG, "=== WakeLock released ===");
        }
        stopForeground(STOP_FOREGROUND_REMOVE);
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void startForegroundPolling() {
        mainHandler.removeCallbacks(foregroundPollRunnable);
        mainHandler.post(foregroundPollRunnable);
    }

    /** 明确切换到指定游戏的 overlay (由 MainActivity 启动时调用). */
    private void switchActiveOverlayTo(String pkg) {
        Integer port = GAME_PORT_MAP.get(pkg);
        if (port == null) return;
        if (activeOverlay != null && activeOverlay.pkg.equals(pkg)) {
            return;
        }
        if (activeOverlay != null) {
            Log.w(TAG, "切换 overlay (显式): " + activeOverlay.pkg + " -> " + pkg);
            teardownActiveOverlay();
        }
        activeOverlay = createOverlayFor(pkg, port);
    }

    /** 检查前台 package, 必要时切换 active overlay. 同一时刻只保留 1 个. */
    private void syncActiveOverlayWithForeground() {
        String frontPkg = getForegroundPackage();
        Integer port = frontPkg != null ? GAME_PORT_MAP.get(frontPkg) : null;

        if (port == null) {
            // 前台不是任何已知游戏 → 销毁现有 overlay 释放 ImGui 全局状态
            if (activeOverlay != null) {
                Log.w(TAG, "前台非游戏 (" + frontPkg + "), 销毁 overlay pkg=" + activeOverlay.pkg);
                teardownActiveOverlay();
            }
            return;
        }

        if (activeOverlay != null && activeOverlay.pkg.equals(frontPkg)) {
            // 同一游戏继续保持; 顺便同步尺寸
            syncOverlayLayout();
            return;
        }

        // 切换到新游戏
        if (activeOverlay != null) {
            Log.w(TAG, "切换 overlay: " + activeOverlay.pkg + " -> " + frontPkg);
            teardownActiveOverlay();
        } else {
            Log.w(TAG, "前台游戏激活: " + frontPkg + " port=" + port);
        }
        activeOverlay = createOverlayFor(frontPkg, port);
    }

    private GameOverlay createOverlayFor(String pkg, int port) {
        Log.w(TAG, "createOverlayFor: pkg=" + pkg + " port=" + port);

        OverlayTextureView view;
        try {
            view = new OverlayTextureView(this, pkg, port);
        } catch (Exception e) {
            Log.e(TAG, "createOverlayFor: 创建 TextureView 失败 pkg=" + pkg, e);
            return null;
        }

        Point displaySize = getCurrentDisplaySize();
        int windowType = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                : WindowManager.LayoutParams.TYPE_PHONE;
        int flags = WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
                | WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                | WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED;
        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                displaySize.x, displaySize.y, windowType, flags, PixelFormat.TRANSLUCENT);
        lp.gravity = Gravity.TOP | Gravity.START;
        lp.alpha = TOUCH_PASSTHROUGH_ALPHA;
        lp.setTitle("UE4DebugOverlay-" + pkg);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        }
        view.setVisibility(View.VISIBLE);

        try {
            windowManager.addView(view, lp);
        } catch (Exception e) {
            Log.e(TAG, "createOverlayFor: addView 失败 pkg=" + pkg, e);
            return null;
        }

        Log.w(TAG, "=== Overlay 已添加 pkg=" + pkg + " port=" + port + " size=" + displaySize.x + "x" + displaySize.y + " ===");
        return new GameOverlay(pkg, port, view, lp);
    }

    private void teardownActiveOverlay() {
        if (activeOverlay == null) return;
        GameOverlay ov = activeOverlay;
        activeOverlay = null;
        // 1. 先停 RenderServer (关闭 ImGui context, 释放 BackendRendererUserData)
        PublicOverlayBridge.stopRenderer(ov.port);
        // 2. 再 removeView (会触发 onSurfaceTextureDestroyed, 但 RenderServer 已停, 不会重复)
        try {
            windowManager.removeViewImmediate(ov.view);
        } catch (Exception e) {
            Log.e(TAG, "removeView 失败 pkg=" + ov.pkg, e);
        }
        ov.releaseSurface();
        Log.w(TAG, "=== Overlay 已销毁 pkg=" + ov.pkg + " port=" + ov.port + " ===");
    }

    private void syncOverlayLayout() {
        if (activeOverlay == null) return;
        Point displaySize = getCurrentDisplaySize();
        if (displaySize.x <= 0 || displaySize.y <= 0) return;
        if (activeOverlay.layoutParams.width == displaySize.x
                && activeOverlay.layoutParams.height == displaySize.y) return;
        activeOverlay.layoutParams.width = displaySize.x;
        activeOverlay.layoutParams.height = displaySize.y;
        try {
            windowManager.updateViewLayout(activeOverlay.view, activeOverlay.layoutParams);
        } catch (Exception e) {
            Log.e(TAG, "刷新 Overlay 布局失败 pkg=" + activeOverlay.pkg, e);
        }
    }

    private Point getCurrentDisplaySize() {
        Point size = new Point(1, 1);
        if (windowManager == null) return size;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            WindowMetrics metrics = windowManager.getMaximumWindowMetrics();
            size.x = metrics.getBounds().width();
            size.y = metrics.getBounds().height();
            return size;
        }
        Display display = windowManager.getDefaultDisplay();
        display.getRealSize(size);
        return size;
    }

    private int getCurrentDisplayRotationDegrees() {
        if (windowManager == null) return 0;
        Display display = windowManager.getDefaultDisplay();
        if (display == null) return 0;
        return display.getRotation() * 90;
    }

    private String getForegroundPackage() {
        try {
            UsageStatsManager usm = (UsageStatsManager) getSystemService(Context.USAGE_STATS_SERVICE);
            if (usm == null) return null;
            long now = System.currentTimeMillis();
            UsageEvents events = usm.queryEvents(now - 2000, now);
            String lastPkg = null;
            while (events.hasNextEvent()) {
                UsageEvents.Event event = new UsageEvents.Event();
                events.getNextEvent(event);
                if (event.getEventType() == UsageEvents.Event.ACTIVITY_RESUMED) {
                    lastPkg = event.getPackageName();
                }
            }
            return lastPkg;
        } catch (Exception e) {
            return null;
        }
    }

    private Notification buildNotification() {
        return new NotificationCompat.Builder(this, CHANNEL_ID)
                .setSmallIcon(android.R.drawable.ic_menu_view)
                .setContentTitle("UE4 Overlay 调试中")
                .setContentText("公开 API 多游戏 overlay 已启动")
                .setOngoing(true)
                .setSilent(true)
                .build();
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return;
        NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID, "UE4 Overlay", NotificationManager.IMPORTANCE_DEFAULT);
        channel.setDescription("UE4 公开 API 调试悬浮层");
        NotificationManager manager = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        if (manager != null) manager.createNotificationChannel(channel);
    }

    // ---- 内部数据结构 -------------------------------------------------------

    private static final class GameOverlay {
        final String pkg;
        final int port;
        final OverlayTextureView view;
        final WindowManager.LayoutParams layoutParams;

        GameOverlay(String pkg, int port, OverlayTextureView view, WindowManager.LayoutParams lp) {
            this.pkg = pkg;
            this.port = port;
            this.view = view;
            this.layoutParams = lp;
        }

        void releaseSurface() {
            view.releaseSurface();
        }
    }

    private final class OverlayTextureView extends TextureView implements TextureView.SurfaceTextureListener {
        private final String pkg;
        private final int port;
        private Surface surface;
        private int lastW = 0, lastH = 0, lastR = -1;

        OverlayTextureView(Context context, String pkg, int port) {
            super(context);
            this.pkg = pkg;
            this.port = port;
            setOpaque(false);
            setSurfaceTextureListener(this);
        }

        void releaseSurface() {
            if (surface != null) {
                surface.release();
                surface = null;
            }
        }

        @Override
        public void onSurfaceTextureAvailable(SurfaceTexture surfaceTexture, int width, int height) {
            int rotateTheta = getCurrentDisplayRotationDegrees();
            Log.w(TAG, "=== onSurfaceTextureAvailable pkg=" + pkg + " port=" + port
                    + " width=" + width + " height=" + height + " rotate=" + rotateTheta + " ===");
            PublicOverlayBridge.stopRenderer(port);
            releaseSurface();
            surface = new Surface(surfaceTexture);
            boolean started = PublicOverlayBridge.startRenderer(surface, width, height, rotateTheta, port);
            Log.w(TAG, "=== RenderServer started pkg=" + pkg + " port=" + port + " ok=" + started + " ===");
            lastW = width; lastH = height; lastR = rotateTheta;
        }

        @Override
        public void onSurfaceTextureSizeChanged(SurfaceTexture surfaceTexture, int width, int height) {
            int rotateTheta = getCurrentDisplayRotationDegrees();
            if (width == lastW && height == lastH && rotateTheta == lastR) {
                return;
            }
            Log.i(TAG, "onSurfaceTextureSizeChanged pkg=" + pkg + " port=" + port
                    + " width=" + width + " height=" + height + " rotate=" + rotateTheta);
            PublicOverlayBridge.stopRenderer(port);
            releaseSurface();
            surface = new Surface(surfaceTexture);
            PublicOverlayBridge.startRenderer(surface, width, height, rotateTheta, port);
            lastW = width; lastH = height; lastR = rotateTheta;
        }

        @Override
        public boolean onSurfaceTextureDestroyed(SurfaceTexture surfaceTexture) {
            Log.i(TAG, "onSurfaceTextureDestroyed pkg=" + pkg + " port=" + port);
            PublicOverlayBridge.stopRenderer(port);
            releaseSurface();
            return true;
        }

        @Override
        public void onSurfaceTextureUpdated(SurfaceTexture surfaceTexture) { }
    }
}
