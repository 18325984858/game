package com.example.dobbyproject;

import android.app.usage.UsageEvents;
import android.app.usage.UsageStatsManager;
import android.app.ActivityManager;
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

public class Ue4OverlayService extends Service {
    public static final String ACTION_START = "com.example.dobbyproject.action.START_UE4_OVERLAY";
    public static final String ACTION_STOP = "com.example.dobbyproject.action.STOP_UE4_OVERLAY";

    private static final String TAG = "UE4OverlayService";
    private static final String CHANNEL_ID = "ue4_overlay_debug";
    private static final int NOTIFICATION_ID = 1107;
    private static final float TOUCH_PASSTHROUGH_ALPHA = 0.7f;
    private static final long OVERLAY_LAYOUT_SYNC_INTERVAL_MS = 500L;
    private static final String[] GAME_PACKAGES = {
        "com.tencent.tmgp.pubgmhd",
        "com.tencent.tmgp.dfm",
        "com.tencent.nrc"
    };
    private static final long GAME_ALIVE_CHECK_INTERVAL_MS = 2000L;

    private WindowManager windowManager;
    private PowerManager.WakeLock wakeLock;
    private OverlayTextureView overlayTextureView;
    private Surface overlaySurface;
    private WindowManager.LayoutParams overlayLayoutParams;
    private int lastRendererWidth = 0;
    private int lastRendererHeight = 0;
    private int lastRendererRotation = -1;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final Runnable overlayLayoutSyncRunnable = new Runnable() {
        @Override
        public void run() {
            syncOverlayLayout();
            updateOverlayVisibility();
            if (overlayTextureView != null) {
                mainHandler.postDelayed(this, OVERLAY_LAYOUT_SYNC_INTERVAL_MS);
            }
        }
    };
    private final Runnable gameAliveCheckRunnable = new Runnable() {
        @Override
        public void run() {
            // 不检测游戏进程 — Android 14+ 无法可靠检测其他 UID 的进程
            // overlay 清理依赖: RenderClient 断线 → RenderServer 检测到 → 清屏
            // 用户手动 stopSelf 或系统回收
            mainHandler.postDelayed(this, GAME_ALIVE_CHECK_INTERVAL_MS);
        }
    };

    private boolean isGameRunning() {
        // Android 14+ getRunningAppProcesses() 只返回自己 UID 的进程，无法检测其他 app
        // 改用 /proc 扫描
        return isGameProcessRunning();
    }

    @Override
    public void onCreate() {
        super.onCreate();
        Log.w(TAG, "=== onCreate: overlay 服务启动 pid=" + android.os.Process.myPid() + " ===");
        windowManager = (WindowManager) getSystemService(WINDOW_SERVICE);

        // 获取 PARTIAL_WAKE_LOCK 防止进程被 freeze (保持 CPU 活跃, TCP listen socket 不被清理)
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
        // 尽早创建 overlay view, 不等 onStartCommand, 避免服务被冻结前未初始化
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M || Settings.canDrawOverlays(this)) {
            ensureOverlayView();
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

        ensureOverlayView();
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        PublicOverlayBridge.stopRenderer();
        mainHandler.removeCallbacks(overlayLayoutSyncRunnable);
        mainHandler.removeCallbacks(gameAliveCheckRunnable);
        removeOverlayView();
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

    private void ensureOverlayView() {
        if (overlayTextureView != null) {
            return;
        }

        Log.w(TAG, "ensureOverlayView: 开始创建 TextureView pid=" + android.os.Process.myPid());

        try {
            overlayTextureView = new OverlayTextureView(this);
        } catch (Exception e) {
            Log.e(TAG, "ensureOverlayView: 创建 OverlayTextureView 失败", e);
            return;
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

        WindowManager.LayoutParams layoutParams = new WindowManager.LayoutParams(
                displaySize.x,
                displaySize.y,
                windowType,
                flags,
                PixelFormat.TRANSLUCENT
        );
        layoutParams.gravity = Gravity.TOP | Gravity.START;
        layoutParams.alpha = TOUCH_PASSTHROUGH_ALPHA;
        layoutParams.setTitle("UE4DebugOverlay");
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            layoutParams.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        }
        overlayLayoutParams = layoutParams;

        try {
            windowManager.addView(overlayTextureView, layoutParams);
        } catch (Exception e) {
            Log.e(TAG, "ensureOverlayView: addView 失败", e);
            overlayTextureView = null;
            return;
        }
        startOverlayLayoutSync();
        startGameAliveCheck();
        Log.w(TAG, "=== Overlay TextureView 已添加 alpha=" + TOUCH_PASSTHROUGH_ALPHA + " size=" + displaySize.x + "x" + displaySize.y + " pid=" + android.os.Process.myPid() + " ===");
    }

    private void removeOverlayView() {
        if (overlayTextureView == null) {
            return;
        }

        PublicOverlayBridge.stopRenderer();
        releaseOverlaySurface();

        try {
            overlayTextureView.setVisibility(View.INVISIBLE);
            windowManager.removeViewImmediate(overlayTextureView);
        } catch (Exception e) {
            Log.e(TAG, "移除 Overlay TextureView 失败", e);
        }
        overlayTextureView = null;
        overlayLayoutParams = null;
    }

    private void startOverlayLayoutSync() {
        mainHandler.removeCallbacks(overlayLayoutSyncRunnable);
        mainHandler.post(overlayLayoutSyncRunnable);
    }

    private void syncOverlayLayout() {
        if (overlayTextureView == null || overlayLayoutParams == null) {
            return;
        }

        Point displaySize = getCurrentDisplaySize();
        if (displaySize.x <= 0 || displaySize.y <= 0) {
            return;
        }

        if (overlayLayoutParams.width == displaySize.x && overlayLayoutParams.height == displaySize.y) {
            return;
        }

        overlayLayoutParams.width = displaySize.x;
        overlayLayoutParams.height = displaySize.y;
        try {
            windowManager.updateViewLayout(overlayTextureView, overlayLayoutParams);
            Log.i(TAG, "Overlay 布局刷新为 " + displaySize.x + "x" + displaySize.y);
        } catch (Exception e) {
            Log.e(TAG, "刷新 Overlay 布局失败", e);
        }
    }

    private Point getCurrentDisplaySize() {
        Point size = new Point(1, 1);
        if (windowManager == null) {
            return size;
        }

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
        if (windowManager == null) {
            return 0;
        }

        Display display = windowManager.getDefaultDisplay();
        if (display == null) {
            return 0;
        }

        return display.getRotation() * 90;
    }

    private void releaseOverlaySurface() {
        if (overlaySurface == null) {
            return;
        }

        overlaySurface.release();
        overlaySurface = null;
    }

    private void updateOverlayVisibility() {
        if (overlayTextureView == null) return;
        boolean gameInFront = isGameInForeground();
        int desired = gameInFront ? View.VISIBLE : View.INVISIBLE;
        if (overlayTextureView.getVisibility() != desired) {
            overlayTextureView.setVisibility(desired);
        }
    }

    private boolean isGameInForeground() {
        try {
            UsageStatsManager usm = (UsageStatsManager) getSystemService(Context.USAGE_STATS_SERVICE);
            if (usm == null) return true;
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
            if (lastPkg == null) return true; // 无法判断时默认显示
            for (String pkg : GAME_PACKAGES) {
                if (pkg.equals(lastPkg)) return true;
            }
            return false;
        } catch (Exception e) {
            return true; // 异常时默认显示
        }
    }

    private Notification buildNotification() {
        return new NotificationCompat.Builder(this, CHANNEL_ID)
                .setSmallIcon(android.R.drawable.ic_menu_view)
                .setContentTitle("UE4 Overlay 调试中")
                .setContentText("公开 API 悬浮层已启动")
                .setOngoing(true)
                .setSilent(true)
                .build();
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return;
        }

        NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID,
                "UE4 Overlay",
                NotificationManager.IMPORTANCE_DEFAULT
        );
        channel.setDescription("UE4 公开 API 调试悬浮层");

        NotificationManager manager = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        if (manager != null) {
            manager.createNotificationChannel(channel);
        }
    }

    private final class OverlayTextureView extends TextureView implements TextureView.SurfaceTextureListener {
        OverlayTextureView(Context context) {
            super(context);
            setOpaque(false);
            setSurfaceTextureListener(this);
        }

        @Override
        public void onSurfaceTextureAvailable(SurfaceTexture surfaceTexture, int width, int height) {
            int rotateTheta = getCurrentDisplayRotationDegrees();
            Log.w(TAG, "=== onSurfaceTextureAvailable width=" + width + " height=" + height + " rotate=" + rotateTheta + " pid=" + android.os.Process.myPid() + " ===");
            PublicOverlayBridge.stopRenderer();
            releaseOverlaySurface();
            overlaySurface = new Surface(surfaceTexture);
            boolean started = PublicOverlayBridge.startRenderer(overlaySurface, width, height, rotateTheta);
            Log.w(TAG, "=== RenderServer started=" + started + " ===");
            lastRendererWidth = width;
            lastRendererHeight = height;
            lastRendererRotation = rotateTheta;
        }

        @Override
        public void onSurfaceTextureSizeChanged(SurfaceTexture surfaceTexture, int width, int height) {
            int rotateTheta = getCurrentDisplayRotationDegrees();
            // 尺寸和旋转均未变化时跳过重建, 避免 layout sync 触发的无效重启闪烁
            if (width == lastRendererWidth && height == lastRendererHeight && rotateTheta == lastRendererRotation) {
                Log.d(TAG, "onSurfaceTextureSizeChanged same size/rotation, skip restart");
                return;
            }
            Log.i(TAG, "onSurfaceTextureSizeChanged width=" + width + " height=" + height + " rotate=" + rotateTheta);
            PublicOverlayBridge.stopRenderer();
            releaseOverlaySurface();
            overlaySurface = new Surface(surfaceTexture);
            PublicOverlayBridge.startRenderer(overlaySurface, width, height, rotateTheta);
            lastRendererWidth = width;
            lastRendererHeight = height;
            lastRendererRotation = rotateTheta;
        }

        @Override
        public boolean onSurfaceTextureDestroyed(SurfaceTexture surfaceTexture) {
            Log.i(TAG, "onSurfaceTextureDestroyed");
            PublicOverlayBridge.stopRenderer();
            releaseOverlaySurface();
            return true;
        }

        @Override
        public void onSurfaceTextureUpdated(SurfaceTexture surfaceTexture) {
        }
    }

    private void startGameAliveCheck() {
        mainHandler.removeCallbacks(gameAliveCheckRunnable);
        // 首次检查延迟 10 秒, 给游戏进程充足的启动时间
        mainHandler.postDelayed(gameAliveCheckRunnable, 10_000L);
    }

    private boolean isGameProcessRunning() {
        try {
            java.io.File procDir = new java.io.File("/proc");
            java.io.File[] entries = procDir.listFiles();
            if (entries == null) return true; // 无法访问 /proc, 假设存活
            for (java.io.File entry : entries) {
                if (!entry.isDirectory()) continue;
                try {
                    Integer.parseInt(entry.getName());
                } catch (NumberFormatException e) {
                    continue; // 非 PID 目录
                }
                java.io.File cmdline = new java.io.File(entry, "cmdline");
                try (java.io.BufferedReader reader = new java.io.BufferedReader(new java.io.FileReader(cmdline))) {
                    String line = reader.readLine();
                    if (line != null) {
                        for (String pkg : GAME_PACKAGES) {
                            if (line.contains(pkg)) {
                                return true;
                            }
                        }
                    }
                } catch (Exception ignored) {
                    // 无权限读取, 跳过
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "检查游戏进程失败", e);
            return true; // 检查失败时假设存活, 避免误杀
        }
        return false;
    }
}