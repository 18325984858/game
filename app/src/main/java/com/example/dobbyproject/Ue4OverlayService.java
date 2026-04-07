package com.example.dobbyproject;

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
import android.provider.Settings;
import android.util.Log;
import android.view.Display;
import android.view.Gravity;
import android.view.Surface;
import android.view.TextureView;
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

    private WindowManager windowManager;
    private OverlayTextureView overlayTextureView;
    private Surface overlaySurface;
    private WindowManager.LayoutParams overlayLayoutParams;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final Runnable overlayLayoutSyncRunnable = new Runnable() {
        @Override
        public void run() {
            syncOverlayLayout();
            if (overlayTextureView != null) {
                mainHandler.postDelayed(this, OVERLAY_LAYOUT_SYNC_INTERVAL_MS);
            }
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        windowManager = (WindowManager) getSystemService(WINDOW_SERVICE);
        createNotificationChannel();
        Notification notification = buildNotification();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else {
            startForeground(NOTIFICATION_ID, notification);
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
        removeOverlayView();
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

        overlayTextureView = new OverlayTextureView(this);
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

        windowManager.addView(overlayTextureView, layoutParams);
        startOverlayLayoutSync();
        Log.i(TAG, "Overlay TextureView 已添加 alpha=" + TOUCH_PASSTHROUGH_ALPHA + " size=" + displaySize.x + "x" + displaySize.y);
    }

    private void removeOverlayView() {
        if (overlayTextureView == null) {
            return;
        }

        PublicOverlayBridge.stopRenderer();
        releaseOverlaySurface();

        try {
            windowManager.removeView(overlayTextureView);
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
                NotificationManager.IMPORTANCE_MIN
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
            Log.i(TAG, "onSurfaceTextureAvailable width=" + width + " height=" + height + " rotate=" + rotateTheta);
            PublicOverlayBridge.stopRenderer();
            releaseOverlaySurface();
            overlaySurface = new Surface(surfaceTexture);
            PublicOverlayBridge.startRenderer(overlaySurface, width, height, rotateTheta);
        }

        @Override
        public void onSurfaceTextureSizeChanged(SurfaceTexture surfaceTexture, int width, int height) {
            int rotateTheta = getCurrentDisplayRotationDegrees();
            Log.i(TAG, "onSurfaceTextureSizeChanged width=" + width + " height=" + height + " rotate=" + rotateTheta);
            PublicOverlayBridge.stopRenderer();
            releaseOverlaySurface();
            overlaySurface = new Surface(surfaceTexture);
            PublicOverlayBridge.startRenderer(overlaySurface, width, height, rotateTheta);
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
}