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
import android.graphics.Color;
import android.graphics.Point;
import android.graphics.PixelFormat;
import android.graphics.SurfaceTexture;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.provider.Settings;
import android.util.Log;
import android.view.Display;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.TextureView;
import android.view.View;
import android.view.WindowManager;
import android.view.WindowMetrics;
import android.widget.TextView;

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

    /** 触摸转发状态: true = 菜单区域遮罩一个透明捕获窗, 仅该区域点击转发给 ImGui;
     *  false = 没有捕获窗, 所有点击都穿透到游戏.
     *  渲染用的 TextureView 始终 FLAG_NOT_TOUCHABLE 不变, 避免遮住游戏操作. */
    private boolean touchForwardEnabled = false;
    private View toggleButtonView;
    private WindowManager.LayoutParams toggleButtonLp;
    /** 多菜单捕获: 每个 ImGui 顶层菜单一个透明捕获窗, rect 由 RenderClient 自动上报.
     *  list 与 lpList 索引一一对应; 长度随菜单数量动态变化. */
    private final java.util.ArrayList<View> captureViews = new java.util.ArrayList<>();
    private final java.util.ArrayList<WindowManager.LayoutParams> captureLps = new java.util.ArrayList<>();
    /** 跟随菜单的 padding (向四周扩 N 像素, 让边缘也能命中 ImGui 的 resize handle). */
    private static final int CAPTURE_PADDING_PX = 12;
    /** 最多跟随多少个菜单 (与 native AImGui::kMaxMenuRects 同步). */
    private static final int MAX_MENU_RECTS = 16;
    /** 跟随轮询频率: 100ms. */
    private static final long MENU_FOLLOW_INTERVAL_MS = 100L;
    /** native 返回布局: [count, x0,y0,w0,h0, x1,...]. */
    private final int[] menuRectScratch = new int[1 + MAX_MENU_RECTS * 4];
    private final Runnable menuRectFollowRunnable = new Runnable() {
        @Override
        public void run() {
            try {
                followMenuRectsOnce();
            } catch (Throwable t) {
                Log.e(TAG, "menuRectFollow 异常", t);
            }
            if (touchForwardEnabled) {
                mainHandler.postDelayed(this, MENU_FOLLOW_INTERVAL_MS);
            }
        }
    };

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
            addToggleButton();
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
        removeTouchCaptureView();
        removeToggleButton();
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
        GameOverlay overlay = new GameOverlay(pkg, port, view, lp);

        // 渲染用 TextureView 永远不吞点击 (FLAG_NOT_TOUCHABLE), 避免遮住游戏操作.
        // 所有触摸都走独立的 capture views (只覆盖各菜单区域).
        // 如果触摸转发已经开启 (例如切换游戏前用户已开了 toggle), 重启 follow runnable
        // 让新 overlay 的 port 立刻被轮询.
        activeOverlay = overlay;  // 提前赋值, 让 addTouchCaptureView 能拿到 activeOverlay
        if (touchForwardEnabled) {
            mainHandler.removeCallbacks(menuRectFollowRunnable);
            mainHandler.post(menuRectFollowRunnable);
        }
        return overlay;
    }

    private void teardownActiveOverlay() {
        if (activeOverlay == null) return;
        GameOverlay ov = activeOverlay;
        activeOverlay = null;
        // 0. 先清理触摸捕获窗 (依赖 activeOverlay.port, 必须最早做)
        mainHandler.removeCallbacks(menuRectFollowRunnable);
        removeAllCaptureViews();
        // 1. 停 RenderServer (关闭 ImGui context, 释放 BackendRendererUserData)
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

    // ============================================================
    //  触摸转发 / 投屏支持
    // ============================================================

    /** 将 Java 端 MotionEvent 通过 JNI 喂给 RenderServer (ImGui).
     *  返回 true 表示该事件被消费, 不再下发给其他 view (避免被游戏抢走). */
    private boolean forwardTouchToServer(int port, MotionEvent event) {
        if (!touchForwardEnabled) return false;

        int action;
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                action = 0; break;
            case MotionEvent.ACTION_MOVE:
                action = 1; break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                action = 2; break;
            case MotionEvent.ACTION_CANCEL:
                action = 3; break;
            default:
                return false;
        }
        // 用 raw 坐标 (绝对 display 像素), 与 server 端 ImGui DisplaySize 同坐标系.
        // ACTION_MOVE 的 historicalSize > 0 时也按最新点送一次, 历史点丢弃 (避免抖动).
        float x = event.getRawX();
        float y = event.getRawY();
        PublicOverlayBridge.injectTouch(port, action, x, y);
        return true;
    }

    /** 切换某个 overlay 是否拦截触摸事件 (true = 菜单可点 / false = 点击穿透到游戏).
     *  @deprecated 已改为独立 touchCaptureView, 保留仅供嵌套/调试。 */
    @SuppressWarnings("unused")
    private void applyTouchableFlag(GameOverlay overlay, boolean touchable) {
        // no-op (遺留接口, 避免另外一个 path 调用报错)
    }

    private void setTouchForwardEnabled(boolean enabled) {
        if (touchForwardEnabled == enabled) return;
        touchForwardEnabled = enabled;
        if (enabled) {
            addTouchCaptureView();
        } else {
            removeTouchCaptureView();
        }
        if (toggleButtonView instanceof TextView) {
            mainHandler.post(() -> ((TextView) toggleButtonView).setText(enabled ? "🖱✓" : "🖱"));
        }
    }

    /** 添加始终可点击的小浮标按钮 (无论 overlay 是否吃事件, 这个按钮都收得到 click).
     *  它就是用户唯一能 "解锁/锁定" 触摸转发的入口. */
    private void addToggleButton() {
        if (toggleButtonView != null || windowManager == null) return;
        TextView btn = new TextView(this);
        btn.setText("🖱");
        btn.setTextColor(Color.WHITE);
        btn.setTextSize(18);
        btn.setGravity(Gravity.CENTER);
        GradientDrawable bg = new GradientDrawable();
        bg.setShape(GradientDrawable.OVAL);
        bg.setColor(Color.argb(180, 30, 30, 30));
        bg.setStroke(2, Color.argb(200, 80, 200, 255));
        btn.setBackground(bg);

        final int sizePx = (int) (56 * getResources().getDisplayMetrics().density);
        btn.setOnClickListener(v -> setTouchForwardEnabled(!touchForwardEnabled));
        // 长按拖动: 简单实现, 让用户能挪走按钮避免遮挡.
        btn.setOnTouchListener(new View.OnTouchListener() {
            float downX, downY;
            int origX, origY;
            boolean dragging = false;
            @Override
            public boolean onTouch(View v, MotionEvent event) {
                switch (event.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        downX = event.getRawX();
                        downY = event.getRawY();
                        origX = toggleButtonLp.x;
                        origY = toggleButtonLp.y;
                        dragging = false;
                        return false;  // 让 onClick 也能收到
                    case MotionEvent.ACTION_MOVE: {
                        float dx = event.getRawX() - downX;
                        float dy = event.getRawY() - downY;
                        if (!dragging && (Math.abs(dx) > 12 || Math.abs(dy) > 12)) {
                            dragging = true;
                        }
                        if (dragging) {
                            toggleButtonLp.x = origX + (int) dx;
                            toggleButtonLp.y = origY + (int) dy;
                            try {
                                windowManager.updateViewLayout(toggleButtonView, toggleButtonLp);
                            } catch (Exception ignored) {}
                            return true;
                        }
                        return false;
                    }
                    case MotionEvent.ACTION_UP:
                        return dragging;  // 拖动过则吞掉, 不触发 onClick
                }
                return false;
            }
        });

        int windowType = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                : WindowManager.LayoutParams.TYPE_PHONE;
        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                sizePx, sizePx, windowType,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.TRANSLUCENT);
        lp.gravity = Gravity.TOP | Gravity.START;
        Point displaySize = getCurrentDisplaySize();
        lp.x = Math.max(0, displaySize.x - sizePx - 20);
        lp.y = 200;
        lp.setTitle("UE4OverlayTouchToggle");
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        }
        try {
            windowManager.addView(btn, lp);
            toggleButtonView = btn;
            toggleButtonLp = lp;
            Log.w(TAG, "addToggleButton: 触摸切换按钮已添加 pos=(" + lp.x + "," + lp.y + ")");
        } catch (Exception e) {
            Log.e(TAG, "addToggleButton 失败", e);
        }
    }

    private void removeToggleButton() {
        if (toggleButtonView == null || windowManager == null) return;
        try {
            windowManager.removeViewImmediate(toggleButtonView);
        } catch (Exception ignored) {}
        toggleButtonView = null;
        toggleButtonLp = null;
    }

    /** 启动 / 重启 多菜单触摸捕获.
     *  一旦开启, menuRectFollowRunnable 会每 100ms 拉取 server 端最新 rect 列表,
     *  按需要新增 / 删除 / 更新捕获窗 (一窗一菜单). */
    private void addTouchCaptureView() {
        if (windowManager == null || activeOverlay == null) {
            Log.w(TAG, "addTouchCaptureView: 当前无 active overlay, 不创建捕获窗");
            return;
        }
        // 立刻同步一次 (后续由 runnable 周期跟随)
        followMenuRectsOnce();
        mainHandler.removeCallbacks(menuRectFollowRunnable);
        mainHandler.post(menuRectFollowRunnable);
    }

    /** 创建一个新的菜单捕获窗 (透明 + 蓝色细边). */
    private View createCaptureView(final int port) {
        View capture = new View(this) {
            @Override
            public boolean onTouchEvent(MotionEvent ev) {
                final int action;
                switch (ev.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                    case MotionEvent.ACTION_POINTER_DOWN:
                        action = 0; break;
                    case MotionEvent.ACTION_MOVE:
                        action = 1; break;
                    case MotionEvent.ACTION_UP:
                    case MotionEvent.ACTION_POINTER_UP:
                        action = 2; break;
                    case MotionEvent.ACTION_CANCEL:
                        action = 3; break;
                    default:
                        return true;
                }
                PublicOverlayBridge.injectTouch(port, action, ev.getRawX(), ev.getRawY());
                return true;
            }
        };
        GradientDrawable border = new GradientDrawable();
        border.setColor(Color.argb(20, 80, 200, 255));
        border.setStroke(1, Color.argb(140, 80, 200, 255));
        capture.setBackground(border);
        return capture;
    }

    /** 单次跟随: 查询 server 端最新 rect 列表, 增删/更新捕获窗集合. */
    private void followMenuRectsOnce() {
        if (activeOverlay == null || windowManager == null) return;

        if (!PublicOverlayBridge.getMenuRects(activeOverlay.port, menuRectScratch)) {
            // 端口没运行: 清空所有捕获窗
            removeAllCaptureViews();
            return;
        }
        final int count = Math.max(0, Math.min(menuRectScratch[0], MAX_MENU_RECTS));

        // 多了: 移除尾部多余
        while (captureViews.size() > count) {
            int idx = captureViews.size() - 1;
            View v = captureViews.remove(idx);
            captureLps.remove(idx);
            try { windowManager.removeViewImmediate(v); } catch (Exception ignored) {}
        }
        // 少了: 补足
        final int port = activeOverlay.port;
        int windowType = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                : WindowManager.LayoutParams.TYPE_PHONE;
        while (captureViews.size() < count) {
            View v = createCaptureView(port);
            WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                    1, 1, windowType,
                    WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                            | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                            | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                    PixelFormat.TRANSLUCENT);
            lp.gravity = Gravity.TOP | Gravity.START;
            lp.setTitle("UE4OverlayTouchCapture#" + captureViews.size());
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
            }
            try {
                windowManager.addView(v, lp);
                captureViews.add(v);
                captureLps.add(lp);
            } catch (Exception e) {
                Log.e(TAG, "addCaptureView 失败 idx=" + captureViews.size(), e);
                break;
            }
        }

        // 更新位置/尺寸 (抖动过滤)
        for (int i = 0; i < count && i < captureViews.size(); ++i) {
            int base = 1 + i * 4;
            int rx = menuRectScratch[base];
            int ry = menuRectScratch[base + 1];
            int rw = menuRectScratch[base + 2];
            int rh = menuRectScratch[base + 3];
            int newX = rx - CAPTURE_PADDING_PX;
            int newY = ry - CAPTURE_PADDING_PX;
            int newW = rw + CAPTURE_PADDING_PX * 2;
            int newH = rh + CAPTURE_PADDING_PX * 2;
            if (newW <= 0 || newH <= 0) continue;
            WindowManager.LayoutParams lp = captureLps.get(i);
            if (Math.abs(lp.x - newX) <= 2
                    && Math.abs(lp.y - newY) <= 2
                    && Math.abs(lp.width - newW) <= 2
                    && Math.abs(lp.height - newH) <= 2) {
                continue;
            }
            lp.x = newX;
            lp.y = newY;
            lp.width = newW;
            lp.height = newH;
            try {
                windowManager.updateViewLayout(captureViews.get(i), lp);
            } catch (Exception e) {
                Log.e(TAG, "follow 更新失败 idx=" + i, e);
            }
        }
    }

    private void removeAllCaptureViews() {
        for (View v : captureViews) {
            try { windowManager.removeViewImmediate(v); } catch (Exception ignored) {}
        }
        captureViews.clear();
        captureLps.clear();
    }

    private void removeTouchCaptureView() {
        mainHandler.removeCallbacks(menuRectFollowRunnable);
        if (windowManager == null) return;
        removeAllCaptureViews();
        Log.w(TAG, "removeTouchCaptureView: 所有捕获窗已移除");
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
