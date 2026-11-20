package com.microsoft.xboxone.smartglass;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;
import java.util.Set;

public class Prefs {
    private static final String LENGTH = "_length";
    private static final String DEFAULT_STRING_VALUE = "";
    private static final int DEFAULT_INT_VALUE = -1;
    private static final double DEFAULT_DOUBLE_VALUE = -1d;
    private static final float DEFAULT_FLOAT_VALUE = -1f;
    private static final long DEFAULT_LONG_VALUE = -1L;
    private static final boolean DEFAULT_BOOLEAN_VALUE = false;
    private static Prefs prefsInstance;
    private SharedPreferences sharedPreferences;

    public static int dp(int dp, final Context _context) {
        final float scale = _context.getResources().getDisplayMetrics().density;
        return (int) (dp * scale + 0.5f);
    }

    public static void CustomToast(String name, Context mthis) {
        com.microsoft.xboxone.smartglass.ui.Theme.init(mthis);

        LinearLayout ln = new LinearLayout(mthis);
        ln.setLayoutParams(new LinearLayout.LayoutParams(-2, -2));
        ln.setPadding(dp(16, mthis), dp(12, mthis), dp(16, mthis), dp(12, mthis));
        ln.setAlpha(0.97f);
        ln.setGravity(android.view.Gravity.CENTER_VERTICAL);

        GradientDrawable mGrad = new GradientDrawable();
        mGrad.setColor(com.microsoft.xboxone.smartglass.ui.Theme.SURFACE_HIGH);
        mGrad.setCornerRadius(dp(12, mthis));
        mGrad.setStroke(dp(1, mthis), com.microsoft.xboxone.smartglass.ui.Theme.OUTLINE);
        ln.setBackground(mGrad);

        TextView tv = new TextView(mthis);
        tv.setLayoutParams(new LinearLayout.LayoutParams(-2, -2));
        tv.setText(name);
        tv.setTextColor(com.microsoft.xboxone.smartglass.ui.Theme.ON_SURFACE);
        tv.setTextSize(13);
        com.microsoft.xboxone.smartglass.ui.Theme.applyFont(tv, com.microsoft.xboxone.smartglass.ui.Theme.medium);

        ln.addView(tv);

        Toast toast = new Toast(mthis);
        toast.setDuration(Toast.LENGTH_SHORT);
        toast.setView(ln);
        toast.show();
    }

    private Prefs(Context context) {
        sharedPreferences = context.getApplicationContext().getSharedPreferences(
                context.getPackageName() + "_preferences",
                Context.MODE_PRIVATE
        );
    }

    private Prefs(Context context, String preferencesName) {
        sharedPreferences = context.getApplicationContext().getSharedPreferences(
                preferencesName,
                Context.MODE_PRIVATE
        );
    }

    public static Prefs with(Context context) {
        if (prefsInstance == null) {
            prefsInstance = new Prefs(context);
        }
        return prefsInstance;
    }

    public static Prefs with(Context context, boolean forceInstantiation) {
        if (forceInstantiation) {
            prefsInstance = new Prefs(context);
        }
        return prefsInstance;
    }

    public static Prefs with(Context context, String preferencesName) {
        if (prefsInstance == null) {
            prefsInstance = new Prefs(context, preferencesName);
        }
        return prefsInstance;
    }

    public static Prefs with(Context context, String preferencesName,
                             boolean forceInstantiation) {
        if (forceInstantiation) {
            prefsInstance = new Prefs(context, preferencesName);
        }
        return prefsInstance;
    }

    // String related methods

    public String read(String what) {
        return sharedPreferences.getString(what, DEFAULT_STRING_VALUE);
    }

    public String read(String what, String defaultString) {
        return sharedPreferences.getString(what, defaultString);
    }

    public void write(String where, String what) {
        sharedPreferences.edit().putString(where, what).apply();
    }

    // int related methods

    public int readInt(String what) {
        return sharedPreferences.getInt(what, DEFAULT_INT_VALUE);
    }

    public int readInt(String what, int defaultInt) {
        return sharedPreferences.getInt(what, defaultInt);
    }

    public void writeInt(String where, int what) {
        sharedPreferences.edit().putInt(where, what).apply();
    }

    // double related methods

    public double readDouble(String what) {
        if (!contains(what))
            return DEFAULT_DOUBLE_VALUE;
        return Double.longBitsToDouble(readLong(what));
    }

    public double readDouble(String what, double defaultDouble) {
        if (!contains(what))
            return defaultDouble;
        return Double.longBitsToDouble(readLong(what));
    }

    public void writeDouble(String where, double what) {
        writeLong(where, Double.doubleToRawLongBits(what));
    }

    // float related methods

    public float readFloat(String what) {
        return sharedPreferences.getFloat(what, DEFAULT_FLOAT_VALUE);
    }

    public float readFloat(String what, float defaultFloat) {
        return sharedPreferences.getFloat(what, defaultFloat);
    }

    public void writeFloat(String where, float what) {
        sharedPreferences.edit().putFloat(where, what).apply();
    }

    // long related methods

    public long readLong(String what) {
        return sharedPreferences.getLong(what, DEFAULT_LONG_VALUE);
    }

    public long readLong(String what, long defaultLong) {
        return sharedPreferences.getLong(what, defaultLong);
    }

    public void writeLong(String where, long what) {
        sharedPreferences.edit().putLong(where, what).apply();
    }

    // boolean related methods

    public boolean readBoolean(String what) {
        return readBoolean(what, DEFAULT_BOOLEAN_VALUE);
    }

    public boolean readBoolean(String what, boolean defaultBoolean) {
        return sharedPreferences.getBoolean(what, defaultBoolean);
    }

    public void writeBoolean(String where, boolean what) {
        sharedPreferences.edit().putBoolean(where, what).apply();
    }

    // String set methods

    public void putStringSet(final String key, final Set<String> value) {
        sharedPreferences.edit().putStringSet(key, value).apply();
    }

    public Set<String> getStringSet(final String key, final Set<String> defValue) {
        return sharedPreferences.getStringSet(key, defValue);
    }

    // end related methods

    public void remove(final String key) {
        if (contains(key + LENGTH)) {
            // Workaround for pre-HC's lack of StringSets
            int stringSetLength = readInt(key + LENGTH);
            if (stringSetLength >= 0) {
                sharedPreferences.edit().remove(key + LENGTH).apply();
                for (int i = 0; i < stringSetLength; i++) {
                    sharedPreferences.edit().remove(key + "[" + i + "]").apply();
                }
            }
        }
        sharedPreferences.edit().remove(key).apply();
    }

    public boolean contains(final String key) {
        return sharedPreferences.contains(key);
    }

    public void clear() {
        sharedPreferences.edit().clear().apply();
    }
}
