package com.microsoft.xboxone.smartglass.ui;

import android.content.Context;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.view.View;
import android.view.ViewGroup;
import android.view.animation.Interpolator;
import android.view.animation.OvershootInterpolator;
import android.view.animation.PathInterpolator;
import android.widget.TextView;

import androidx.core.content.res.ResourcesCompat;

import com.microsoft.xboxone.smartglass.R;

public final class Theme {

    public static final int BG = 0xFF121217;
    public static final int SURFACE = 0xFF1A1A24;
    public static final int SURFACE_HIGH = 0xFF212130;
    public static final int OUTLINE = 0xFF2E2E40;
    public static final int PRIMARY = 0xFF7C3AED;
    public static final int PRIMARY_DIM = 0xFF5B8DEF;
    public static final int ON_PRIMARY = 0xFFFFFFFF;
    public static final int ON_SURFACE = 0xFFE8E8F2;
    public static final int MUTED = 0xFF9A9AAE;
    public static final int SUCCESS = 0xFF10B981;
    public static final int ERROR = 0xFFEF4444;
    public static final int SCRIM = 0xCC121217;
    public static final int PRIMARY_TRANSLUCENT = 0x4D7C3AED;
    public static final int SUCCESS_TRANSLUCENT = 0x3310B981;
    public static final int ERROR_TRANSLUCENT = 0x33EF4444;

    public static Typeface medium;
    public static Typeface semibold;
    public static Typeface bold;

    public static Interpolator emphasized;
    public static Interpolator emphasizedDecel;
    public static OvershootInterpolator overshoot;

    private static boolean ready = false;

    public static void init(Context ctx) {
        if (ready) return;
        medium = ResourcesCompat.getFont(ctx, R.font.rajdhani_medium);
        semibold = ResourcesCompat.getFont(ctx, R.font.rajdhani_semibold);
        bold = ResourcesCompat.getFont(ctx, R.font.rajdhani_bold);
        emphasized = new PathInterpolator(0.2f, 0f, 0f, 1f);
        emphasizedDecel = new PathInterpolator(0.05f, 0.7f, 0.1f, 1f);
        overshoot = new OvershootInterpolator(1.1f);
        ready = true;
    }

    public static int dp(Context ctx, int dp) {
        return (int) (dp * ctx.getResources().getDisplayMetrics().density + 0.5f);
    }

    public static GradientDrawable surface(Context ctx, int cornerDp) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(SURFACE);
        d.setCornerRadius(dp(ctx, cornerDp));
        d.setStroke(dp(ctx, 1), OUTLINE);
        return d;
    }

    public static GradientDrawable surfaceHigh(Context ctx, int cornerDp) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(SURFACE_HIGH);
        d.setCornerRadius(dp(ctx, cornerDp));
        return d;
    }

    public static GradientDrawable surfaceHighOutlined(Context ctx, int cornerDp) {
        GradientDrawable d = surfaceHigh(ctx, cornerDp);
        d.setStroke(dp(ctx, 1), OUTLINE);
        return d;
    }

    public static GradientDrawable primarySolid(Context ctx, int cornerDp) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(PRIMARY);
        d.setCornerRadius(dp(ctx, cornerDp));
        return d;
    }

    public static GradientDrawable primaryGradient(Context ctx, int cornerDp) {
        GradientDrawable d = new GradientDrawable(
                GradientDrawable.Orientation.LEFT_RIGHT,
                new int[]{PRIMARY, PRIMARY_DIM});
        d.setCornerRadius(dp(ctx, cornerDp));
        return d;
    }

    public static GradientDrawable solid(Context ctx, int color, int cornerDp) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(ctx, cornerDp));
        return d;
    }

    public static GradientDrawable outlined(Context ctx, int fill, int stroke, int cornerDp) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        d.setCornerRadius(dp(ctx, cornerDp));
        d.setStroke(dp(ctx, 1), stroke);
        return d;
    }

    public static void applyFont(TextView tv, Typeface tf) {
        if (tv == null) return;
        if (tf != null) tv.setTypeface(tf);
    }

    public static void bindPress(final View v) {
        bindPress(v, 0.97f);
    }

    public static void bindPress(final View v, final float scaleTo) {
        v.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View view, android.view.MotionEvent event) {
                switch (event.getAction()) {
                    case android.view.MotionEvent.ACTION_DOWN:
                        view.animate().scaleX(scaleTo).scaleY(scaleTo).setDuration(100).start();
                        break;
                    case android.view.MotionEvent.ACTION_UP:
                    case android.view.MotionEvent.ACTION_CANCEL:
                        view.animate().scaleX(1f).scaleY(1f).setDuration(140).setInterpolator(overshoot).start();
                        break;
                }
                return false;
            }
        });
    }

    public static void entrance(final View v, long delayMs) {
        v.setAlpha(0f);
        v.setTranslationY(dp(v.getContext(), 24));
        v.animate()
                .alpha(1f)
                .translationY(0f)
                .setStartDelay(delayMs)
                .setDuration(350)
                .setInterpolator(emphasized)
                .start();
    }

    public static void staggerEntrance(View... views) {
        for (int i = 0; i < views.length; i++) {
            entrance(views[i], i * 70L);
        }
    }

    public static void bounce(View v) {
        v.animate().cancel();
        v.animate()
                .scaleX(1.08f).scaleY(1.08f)
                .setDuration(120)
                .setInterpolator(emphasized)
                .withEndAction(new Runnable() {
                    @Override
                    public void run() {
                        v.animate().scaleX(1f).scaleY(1f).setDuration(180).setInterpolator(overshoot).start();
                    }
                })
                .start();
    }

    public static void fadeIn(View v, long delayMs, long durationMs) {
        v.setAlpha(0f);
        v.setVisibility(View.VISIBLE);
        v.animate().alpha(1f).setStartDelay(delayMs).setDuration(durationMs).setInterpolator(emphasized).start();
    }

    public static void fadeOutGone(final View v, long durationMs) {
        v.animate().alpha(0f).setDuration(durationMs).setInterpolator(emphasizedDecel)
                .withEndAction(new Runnable() {
                    @Override
                    public void run() {
                        v.setVisibility(View.GONE);
                        v.setAlpha(1f);
                    }
                }).start();
    }

    public static int childCountSafe(ViewGroup vg) {
        return vg == null ? 0 : vg.getChildCount();
    }
}
