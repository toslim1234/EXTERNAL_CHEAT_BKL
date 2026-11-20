package com.microsoft.xboxone.smartglass;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Shader;
import android.util.TypedValue;
import android.view.Gravity;
import android.graphics.Typeface;
import android.graphics.Path;
import android.graphics.RectF;

import androidx.appcompat.widget.AppCompatTextView;

import com.microsoft.xboxone.smartglass.ui.Theme;

public class CustomFade extends AppCompatTextView {
    Paint paint = new Paint();
    Path path = new Path();
    RectF rect = new RectF();
    Paint bgPaint = new Paint();

    public CustomFade(Context context) {
        super(context);
        Theme.init(context);

        setTextColor(Theme.ON_SURFACE);
        setGravity(Gravity.CENTER);
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        if (Theme.medium != null) setTypeface(Theme.medium);

        bgPaint.setColor(Theme.SURFACE);
        bgPaint.setAntiAlias(true);
        paint.setAntiAlias(true);
    }

    @Override
    protected void onDraw(Canvas canvas) {

        int w = getWidth();
        int h = getHeight();

        LinearGradient gradient = new LinearGradient(
                0,0,w,0,
                new int[]{
                        Color.TRANSPARENT,
                        Theme.PRIMARY,
                        Color.TRANSPARENT
                },
                new float[]{0f,0.5f,1f},
                Shader.TileMode.CLAMP
        );

        paint.setShader(gradient);
        paint.setAlpha(120);

        rect.set(0,0,w,h);

        float r = Theme.dp(getContext(), 8);

        float[] radii = {
                r, r,
                r, r,
                0, 0,
                0, 0
        };

        path.reset();
        path.addRoundRect(rect, radii, Path.Direction.CW);

        canvas.drawPath(path, paint);

        super.onDraw(canvas);
    }
}
